// NetBot (net_bot.hpp): a bot over the network. viewOf() turns what a HeadlessClient's World
// knows into a BotView; NetBot::tick() adds what the map tells (item spots, flag bases,
// obelisks, elevators) and lets the brain think once per input frame before the client runs.
#include "noxcore/bot/net_bot.hpp"

#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <cmath>

namespace nox::bot {

namespace {
/// A player's drawable in its DIE (1) or DEAD (2) animation (PlayerDraw's states: IDLE, DIE,
/// DEAD, ...). Arena games send no MSG_PLAYER_DIED, so this is how a client tells the fallen.
bool fallen(const client::Drawable* d)
{
    return d && (d->animState == 1 || d->animState == 2);
}
// The player list's status bits (0x1 observing, 0x400 poisoned) and the invisibility enchantment.
constexpr std::uint32_t kObserver = 0x1;
constexpr std::uint32_t kPoisoned = 0x400;
constexpr std::uint32_t kInvisible = 1u << 0; ///< enchantment 0

/// A summoning's price and its creature's size (thing.bin gives summonings no cost): the
/// price the exe reads from its summon price table (things::summonManaCost, the one the server
/// charges from), the size from the creature's SUBCLASS (field-guide creature n = spell - 0x4A).
void summonFacts(const things::ThingDb& db, BotView::KnownSpell& s)
{
    if (!things::isSummonSpell(s.id)) {
        return;
    }
    s.manaCost = static_cast<decltype(s.manaCost)>(things::summonManaCost(s.id));
    s.creatureSize = static_cast<std::uint8_t>(things::guideSize(db, static_cast<int>(s.id) - 0x4A));
}
} // namespace

std::vector<BotView::KnownSpell> classSpells(const things::ThingDb& db, std::uint8_t playerClass,
                                             const std::array<std::uint8_t, 20>& allowed)
{
    std::vector<BotView::KnownSpell> out;
    if (playerClass != 1 && playerClass != 2) {
        return out;
    }
    // thing.bin spell flags: bit 24 any caster's book, bits 25 and 26 the wizard's and conjurer's.
    const std::uint32_t classBit = playerClass == 1 ? 0x2000000 : 0x4000000;
    const bool all = std::ranges::all_of(allowed, [](std::uint8_t b) { return b == 0; });
    for (std::uint32_t id = 1; id < things::kSpellCount; ++id) {
        const auto* def = db.findSpell(id);
        // `allowed` is a bitmap by spell id: byte id / 8, bit id % 8.
        if (!def || !(def->flags & (0x1000000 | classBit)) ||
            !(all || (id / 8 < allowed.size() && ((allowed[id / 8] >> (id % 8)) & 1)))) {
            continue;
        }
        out.push_back({id, 3, def->manaCost});
        summonFacts(db, out.back());
    }
    return out;
}

namespace {

/// `v` as a fresh view, every field its first value, its lists emptied but their room kept.
void freshView(BotView& v)
{
    auto inventory = std::move(v.inventory);
    auto players = std::move(v.players);
    auto creatures = std::move(v.creatures);
    auto items = std::move(v.items);
    auto manaSources = std::move(v.manaSources);
    auto elevatorCars = std::move(v.elevatorCars);
    auto flags = std::move(v.flags);
    auto escorts = std::move(v.escorts);
    auto struggling = std::move(v.struggling);
    auto missiles = std::move(v.missiles);
    auto magicWalls = std::move(v.magicWalls);
    auto spells = std::move(v.spells);
    v = BotView{};
    const auto keep = [](auto& list, auto& into) {
        list.clear();
        into = std::move(list);
    };
    keep(inventory, v.inventory);
    keep(players, v.players);
    keep(creatures, v.creatures);
    keep(items, v.items);
    keep(manaSources, v.manaSources);
    keep(elevatorCars, v.elevatorCars);
    keep(flags, v.flags);
    keep(escorts, v.escorts);
    keep(struggling, v.struggling);
    keep(missiles, v.missiles);
    keep(magicWalls, v.magicWalls);
    keep(spells, v.spells);
}

} // namespace

BotView viewOf(const client::HeadlessClient& client, const ItemKinds& kinds)
{
    BotView v;
    ClassSpellsCache spells;
    viewInto(v, client, kinds, spells);
    return v;
}

void viewInto(BotView& v, const client::HeadlessClient& client, const ItemKinds& kinds, ClassSpellsCache& spells)
{
    using Stage = client::HeadlessClient::Stage;
    const auto& w = client.world();
    freshView(v);
    v.numbers = kinds.numbers();
    v.frame = client.frame();
    v.gameFlags = w.game().gameFlags;
    v.inGame = client.stage() == Stage::InGame;
    v.netCode = client.netCode();
    v.nav = &client.nav();
    for (const auto& [cell, wall] : w.game().magicWalls) {
        v.magicWalls.emplace_back(cell.first, cell.second);
    }
    v.viewHalfWidth = static_cast<float>(client.profile().screenWidth) / 2; // (its screen, as a player's)
    v.viewHalfHeight = static_cast<float>(client.profile().screenHeight) / 2;
    // Where we are: our drawable if the server has sent it, else the client's own position.
    const auto* ours = w.drawable(v.netCode);
    if (ours) {
        v.x = static_cast<float>(ours->x);
        v.y = static_cast<float>(ours->y);
        v.placed = true;
        v.anim = ours->animState;
        v.buffs = ours->buffs;
    } else if (const auto p = client.position()) {
        v.x = static_cast<float>(p->first);
        v.y = static_cast<float>(p->second);
        v.placed = true;
    }
    if (const auto* me = w.player(v.netCode)) {
        v.playerClass = me->playerClass;
        v.dead = me->dead || fallen(ours);
        v.observer = (me->status & kObserver) != 0;
        v.poisoned = (me->status & kPoisoned) != 0;
        v.team = me->team;
        v.carryingFlag = (me->weaponEquip & 1) != 0; // (the flag is weapon class bit 0)
        v.lagFrames = static_cast<float>(me->latency) / 2 / (1000.0f / 30); // (MSG_REPORT_ALL_LATENCY: ms)
    }
    const auto& self = w.self();
    v.health = self.health;
    v.maxHealth = self.maxHealth;
    v.mana = self.mana;
    v.maxMana = self.maxMana;
    v.stamina = self.staminaAt(client.frame()); // (the server reports only what's spent)
    v.abilityReady = self.abilityReady;
    // The spells: in a quest those awarded, else every one the class's book and the game allow.
    if (const auto* db = client.things()) {
        if (v.gameFlags & 0x1000) {
            // A quest: the spells we've been given (MSG_REPORT_SPELL_AWARD).
            for (const auto& [id, level] : self.spellLevels) {
                if (const auto* def = db->findSpell(id); def && level != 0) {
                    v.spells.push_back({id, level, def->manaCost});
                    summonFacts(*db, v.spells.back());
                }
            }
        } else {
            const auto& allowed = w.game().allowedSpells;
            if (spells.db != db || spells.playerClass != v.playerClass || spells.allowed != allowed) {
                spells.db = db;
                spells.playerClass = v.playerClass;
                spells.allowed = allowed;
                spells.spells = classSpells(*db, v.playerClass, allowed);
            }
            v.spells = spells.spells;
        }
    }
    // Our creatures' cells in the cage (a small one 1, a medium 2, a large 4, of 4).
    int caged = 0;
    for (const auto& s : self.summons) {
        caged += s ? s->size : 0;
    }
    v.summonRoom = static_cast<std::uint8_t>(std::max(0, 4 - caged));
    v.abilityLevel = self.abilityLevel;
    for (const auto& [netCode, item] : self.inventory) {
        const ItemInfo& info = kinds.info(item.typeId);
        v.inventory.push_back({netCode, item.typeId, info.kind, item.equipped, kinds.usableBy(item.typeId, v.playerClass),
                               info.value, info.slot, info.blocks, info.reach, info.swingStamina, info.twoHanded,
                               info.ranged, info.crossbow, item.charges, info.wand});
    }
    const bool seesInvisible = ours && (ours->buffs & (1u << 21));  // enchantment 21: Infravision
    for (const auto& [netCode, p] : w.players()) {
        if (netCode == v.netCode) {
            continue;
        }
        SeenPlayer s;
        s.netCode = netCode;
        s.name = p.name;
        s.team = p.team;
        s.playerClass = p.playerClass;
        s.dead = p.dead;
        s.observer = (p.status & kObserver) != 0;
        s.hasFlag = (p.weaponEquip & 1) != 0;
        s.poisoned = (p.status & kPoisoned) != 0;
        if (const auto* d = w.drawable(netCode)) {
            s.x = static_cast<float>(d->x);
            s.y = static_cast<float>(d->y);
            s.placed = true;
            // Unseen only while they stand: moving, the invisible show as a ghost,
            // and enchantment 21 (Infravision) shows them anyway.
            s.invisible = (d->buffs & kInvisible) != 0 && !d->movingVisibly() && !seesInvisible;
            s.dead = s.dead || fallen(d);
            s.anim = d->animState;
            s.buffs = d->buffs;
        }
        v.players.push_back(std::move(s));
    }
    // A thing's team: flags and creatures are members of their team, as players are (the map
    // draws them so).
    const auto teamOf = [&](std::uint16_t netCode) {
        std::uint32_t team = 0;
        for (const auto& [id, t] : w.teams()) {
            if (t.members.contains(netCode)) {
                team = id;
            }
        }
        return team;
    };
    const auto ourCreature = [&](std::uint16_t netCode) {
        return w.isFriend(netCode) || std::ranges::any_of(self.summons, [&](const auto& s) { return s && s->netCode == netCode; });
    };
    // Every drawable we're sent: a flag, an enemy creature, a missile, or an item lying about.
    for (const auto& [netCode, d] : w.drawables()) {
        const ItemInfo& info = kinds.info(d.typeId);
        if (info.flag && d.enabled) {
            // A flag's team, as FlagDraw finds it: its material modifier is its team's
            // colour (MaterialTeamRed...), and its team the one of that colour.
            std::uint32_t team = teamOf(netCode);
            if (const std::uint8_t color = d.modifiers ? kinds.teamColorOf((*d.modifiers)[1]) : 0; team == 0 && color != 0) {
                for (const auto& [id, t] : w.teams()) {
                    if (t.color == color) {
                        team = id;
                        break;
                    }
                }
            }
            v.flags.push_back({team, static_cast<float>(d.x), static_cast<float>(d.y)});
        } else if (info.creature && d.enabled && !ourCreature(netCode)) {
            SeenPlayer c;
            c.netCode = netCode;
            c.creature = true;
            c.typeId = d.typeId;
            c.x = static_cast<float>(d.x);
            c.y = static_cast<float>(d.y);
            c.placed = true;
            c.team = teamOf(netCode);
            c.dead = d.animState == 9 || d.animState == 10; // (MonsterDraw's DIE, DEAD)
            c.invisible = (d.buffs & kInvisible) != 0 && !d.movingVisibly() && !seesInvisible;
            c.buffs = d.buffs;
            c.poisoned = d.poison != 0;
            if (v.team == 0 || c.team != v.team) {
                v.creatures.push_back(std::move(c));
            }
        } else if (info.missile) {
            v.missiles.push_back({netCode, d.typeId, static_cast<float>(d.x), static_cast<float>(d.y)});
        } else if (info.kind != ItemKind::Other && d.enabled && !self.inventory.contains(netCode)) {
            v.items.push_back({netCode, d.typeId, info.kind, static_cast<float>(d.x), static_cast<float>(d.y),
                               kinds.usableBy(d.typeId, v.playerClass), info.value, info.slot});
        }
    }
}

NetBot::NetBot(client::PlayerProfile profile, const things::ThingDb& things, const ItemKinds& kinds,
               std::filesystem::path mapsDir, std::filesystem::path mapCache, std::unique_ptr<Brain> brain)
    : client_(std::move(profile), &things, std::move(mapsDir), std::move(mapCache))
    , body_(client_)
    , brain_(std::move(brain))
    , kinds_(kinds)
{
}

void NetBot::tick(std::uint32_t nowMs)
{
    // The brain thinks once for each input frame the client sends (not on every tick).
    if (client_.inputFrames() != thoughtAt_) {
        thoughtAt_ = client_.inputFrames();
        if (client_.mapLoads() != mapItemsLoad_) {
            // What a player who knows the map knows lies where: its weapons, armour and potions.
            mapItemsLoad_ = client_.mapLoads();
            mapItems_.clear();
            flagBases_.clear();
            obelisks_.clear();
            elevatorCars_.clear();
            const std::uint8_t playerClass = client_.profile().playerClass;
            const auto* things = client_.things();
            for (const auto& o : client_.mapObjects()) {
                const ItemInfo& info = kinds_.info(o.typeId);
                if (info.kind != ItemKind::Other && !info.missile) {
                    mapItems_.push_back({0, o.typeId, info.kind, o.x, o.y, kinds_.usableBy(o.typeId, playerClass),
                                         info.value, info.slot});
                }
                const std::uint32_t classes = things && o.typeId <= things->things.size()
                                                  ? things::thingClasses(things->things[o.typeId - 1u])
                                                  : 0;
                // Each team's flag where it starts: its base.
                if (o.team != 0 && (classes & things::ObjectClass::Flag)) {
                    flagBases_.push_back({o.team, o.x, o.y});
                }
                if (info.obelisk) {
                    obelisks_.push_back({static_cast<std::uint16_t>(o.extent & 0x7FFF),  // the statics key: bit 15, the static marker, off
                                         {o.x, o.y, o.team, info.refills, std::nullopt}});
                }
                if (classes & things::ObjectClass::Elevator) {
                    elevatorCars_.push_back({static_cast<std::uint16_t>(o.extent & 0x7FFF), {o.x, o.y, true}});
                }
            }
        }
        BotView& view = view_;
        viewInto(view, client_, kinds_, spells_);
        view.standDown = standDown_;
        view.mapItems = &mapItems_;
        view.flagBases = &flagBases_;
        const auto& statics = client_.world().game().statics;
        for (const auto& [extent, source] : obelisks_) {
            auto& s = view.manaSources.emplace_back(source);
            // Its light is charge * 80 / 50 (CliNet_OnPacket), sent as the charge passes each 8th.
            if (const auto it = statics.find(extent); it != statics.end() && it->second.lightIntensity) {
                s.charge = static_cast<std::uint8_t>(std::clamp(std::lround(*it->second.lightIntensity * 50 / 80), 0L, 50L));
            }
        }
        for (const auto& [extent, car] : elevatorCars_) {
            auto& c = view.elevatorCars.emplace_back(car);
            // Its height / 4 (SrvNet_SendSpriteState's MSG_DRAW_FRAME); stepped onto at 20 or less (updateElevator).
            if (const auto it = statics.find(extent); it != statics.end() && it->second.frame) {
                c.down = *it->second.frame <= 5;
            }
        }
        view.role = role_;
        view.leader = leader_;
        view.escorts = escorts_;
        view.struggling = struggling_;
        brain_->think(view, body_);
        flagsSeen_ = view.flags;
    }
    client_.tick(nowMs);
}

} // namespace nox::bot
