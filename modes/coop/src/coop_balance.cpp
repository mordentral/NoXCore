// The co-op campaign's balance, keys and books (coop.hpp): the monsters' health and the damage
// players take by the party's size, one key ring for the party, and a book or guide put back for
// the others of the party who haven't had it yet.
#include "noxcore/coop/coop.hpp"

#include "noxcore/server/books.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace nox::coop {

namespace oc = things::ObjectClass;

// ---- difficulty

void CoopDirector::rescale(bool force)
{
    server::World& w = world();
    const int n = std::max<int>(1, static_cast<int>(party_.size()));
    // The host's difficulty: Normal the party's shares alone (the solo game's own numbers when
    // alone); the others start below or above it and add their own shares.
    const Difficulty& d = kDifficulties[static_cast<std::size_t>(std::clamp(options_.difficulty, 0, 3))];
    const bool normal = options_.difficulty == 1;
    const float perHealth = normal ? options_.healthPerPlayer : d.healthPerPlayer;
    const float perDamage = normal ? options_.damagePerPlayer : d.damagePerPlayer;
    const float health = options_.scaleDifficulty ? d.health + perHealth * static_cast<float>(n - 1) : 1.0f;
    const float damage = options_.scaleDifficulty ? d.damage + perDamage * static_cast<float>(n - 1) : 1.0f;
    // The damage players take: the quest's way of it (World::modeDamage; none: the game's own).
    w.modeDamage = damage == 1.0f ? 0.0f : damage;
    if (health == 1.0f && scaledHealth_ == 1.0f) {
        return; // (alone, and nothing scaled: the walk would change nothing)
    }
    if (!force && health == scaledHealth_) {
        // (The same party: only the monsters not seen yet, a generator's.)
        if (w.ticks - lastRescale_ < 5u * 30u) {
            return;
        }
    }
    lastRescale_ = w.ticks;
    applyScale(health);
}

void CoopDirector::applyScale(float health)
{
    server::World& w = world();
    scaledHealth_ = health;
    for (auto& [e, o] : w.objects()) {
        // The monsters and their generators (as the quest's rescale both): not one a player owns or
        // summoned.
        const bool monster = (o.classes & oc::Monster) && o.monster;
        const bool generator = (o.classes & oc::MonsterGenerator) != 0;
        if (!(monster || generator) || o.gone() || (o.flags & server::ObjFlag::Dead) || o.maxHealth == 0 || o.owner != 0 ||
            (o.monster && (o.monster->status & things::MonsterStatus::Summoned))) {
            continue;
        }
        // Its own health as the map made it (first seen), times the party's. (An extent can be
        // used again for a new thing: the base is that thing's only while its script id and type
        // are.)
        Base& b = baseHealth_[o.extent];
        if (b.own == 0 || b.scriptId != o.scriptId || b.type != o.type) {
            b = Base{o.scriptId, o.type, o.maxHealth};
        }
        const auto top = static_cast<std::uint16_t>(std::clamp(static_cast<int>(static_cast<float>(b.own) * health), 1, 0xFFFF));
        if (top == o.maxHealth) {
            continue;
        }
        // Whole stays whole; a hurt one keeps its share.
        const int now = o.health == o.maxHealth ? top : static_cast<int>(static_cast<std::int64_t>(o.health) * top / o.maxHealth);
        o.health = static_cast<std::uint16_t>(std::clamp(now, 1, static_cast<int>(top)));
        o.maxHealth = top;
        o.touch();
        if (o.monster) {
            o.monster->healthSeen.fill(o.health); // (as the quest's rescale: no delta reported)
        }
    }
    // (Those gone with their bases.)
    std::erase_if(baseHealth_, [&](const auto& kv) {
        const server::ServerObject* o = w.byExtent(kv.first);
        return !o || o->gone();
    });
}

// ---- experience

void CoopDirector::killExperienceGiven(server::Fight& fight, server::ServerObject& victim, server::ServerObject& earner)
{
    // The kill's worth to the rest of the party too, each its own award (unitGiveXP by its own
    // experience) and its "health.c:gainpoints": nobody falls behind the one who struck.
    if (!host_ || party_.size() < 2) {
        return;
    }
    const std::vector<server::ServerObject*> units = partyUnits();
    if (std::ranges::find(units, &earner) == units.end()) {
        return; // (a party member's kill only)
    }
    for (server::ServerObject* u : units) {
        if (u == &earner || !u->player) {
            continue;
        }
        const float gain = server::giveXP(fight, *u, victim.experience);
        if (0.0f < gain) {
            std::u16string n;
            for (const char c : std::to_string(static_cast<int>(gain))) {
                n.push_back(static_cast<char16_t>(c));
            }
            fight.world.textTo(*u, "health.c:gainpoints", n);
        }
    }
}

// ---- keys

server::ServerObject* CoopDirector::sharedKey(server::Fight& fight, server::ServerObject& door, server::ServerObject& toucher,
                                              std::string_view keyType)
{
    (void)fight;
    (void)door;
    if (!host_ || party_.size() < 2) {
        return nullptr;
    }
    const std::vector<server::ServerObject*> units = partyUnits();
    if (std::ranges::find(units, &toucher) == units.end()) {
        return nullptr; // (a party member's touch only)
    }
    // One ring: another player's key of the kind, used up as the toucher's would be.
    for (server::ServerObject* u : units) {
        if (u == &toucher) {
            continue;
        }
        for (const std::uint32_t e : u->inventory) {
            server::ServerObject* k = world().byExtent(e);
            if (k && !k->pendingDelete && (k->classes & oc::Key) && k->typeName == keyType) {
                return k;
            }
        }
    }
    return nullptr;
}

void CoopDirector::keysToHost(server::ServerObject& unit)
{
    // A leaving player's keys to the host (as a quest's go to the others), "GainedKey" and its
    // sound; none to give to: left about it.
    server::ServerObject* h = server::hostPlayer(world());
    server::Fight f = host_->server().fight();
    if (!f.things) {
        return;
    }
    server::Items items{world(), *f.things, f.modifiers, f.rng, f.frame};
    for (const std::uint32_t e : keysOf(unit.inventory)) {
        server::ServerObject* key = world().byExtent(e);
        if (!key || !(key->classes & oc::Key) || key->pendingDelete) {
            continue;
        }
        items.removeFromInventory(unit, *key);
        if (h && h != &unit && !(h->flags & server::ObjFlag::Dead)) {
            items.receive(*h, *key);
            world().broadcasts().push_back({server::informStringBytes("GeneralPrint:GainedKey"), true, h->extent});
            server::playSound(world(), 0x334, *h);
        } else {
            const auto [x, y] = server::clearPointNear(world(), rng_, unit.x, unit.y, 60.0f);
            items.place(*key, x, y);
        }
    }
}

void CoopDirector::keysGone(server::ServerObject& unit)
{
    // (StartupScreen's: the host's keys gone at a chapter's start; every player's in a party.)
    server::Fight f = host_->server().fight();
    if (!f.things) {
        return;
    }
    server::Items items{world(), *f.things, f.modifiers, f.rng, f.frame};
    for (const std::uint32_t e : keysOf(unit.inventory)) {
        if (server::ServerObject* item = world().byExtent(e); item && !item->pendingDelete && (item->classes & oc::Key)) {
            items.removeFromInventory(unit, *item);
            item->pendingDelete = true;
        }
    }
}

// ---- books and guides

bool CoopDirector::isRewardBook(const server::ServerObject& item)
{
    return item.useFn == "SpellRewardUse" || item.useFn == "AbilityRewardUse" || item.useFn == "FieldGuideUse";
}

void CoopDirector::rewardToParty(server::ServerObject& taker, const RewardBook& book)
{
    if (!host_ || party_.size() < 2 || !taker.player) {
        return;
    }
    const std::vector<server::ServerObject*> units = partyUnits();
    if (std::ranges::find(units, &taker) == units.end()) {
        return;
    }
    const bool spell = book.useFn == "SpellRewardUse";
    const bool ability = book.useFn == "AbilityRewardUse";
    const int creature = book.useFn == "FieldGuideUse" ? server::guideCreatureIndex(book.guideCreature) : 0;
    // What the taker has of it now (nothing: its pickup didn't take, and nobody else gets one).
    const std::uint8_t level = spell && book.bookEntry < taker.player->spellLevels.size() ? taker.player->spellLevels[book.bookEntry] : 0;
    const bool has = spell     ? level != 0
                     : ability ? book.bookEntry < 6 && taker.player->abilityLevel(book.bookEntry) != 0
                               : creature > 0 && taker.player->fieldGuide[static_cast<std::size_t>(creature)];
    if (!has) {
        return;
    }
    server::Fight f = host_->server().fight();
    if (!f.things) {
        return;
    }
    server::World& w = world();
    server::Items items{w, *f.things, f.modifiers, f.rng, f.frame};
    for (server::ServerObject* u : units) {
        if (u == &taker || !u->player) {
            continue;
        }
        // Once each: below the taker's level of a spell (a second book of a spell is its next
        // level), an ability or creature it doesn't know.
        const bool wanted = spell     ? book.bookEntry < u->player->spellLevels.size() && u->player->spellLevels[book.bookEntry] < level
                            : ability ? book.bookEntry < 6 && u->player->abilityLevel(book.bookEntry) == 0
                                      : creature > 0 && !u->player->fieldGuide[static_cast<std::size_t>(creature)];
        if (!wanted) {
            continue;
        }
        server::ServerObject* copy = w.createAt(host_->server().things(), book.type, u->x, u->y, host_->server().clock().frame());
        if (!copy) {
            continue;
        }
        copy->bookEntry = static_cast<decltype(copy->bookEntry)>(book.bookEntry);
        copy->guideCreature = book.guideCreature;
        copy->subclasses = book.subclasses;
        items.use(*u, *copy); // (the book's USE: the award told and shown as the taker's was)
        if (!copy->pendingDelete && !copy->gone()) {
            copy->pendingDelete = true; // (not taken: none left lying about)
        }
    }
}

bool CoopDirector::isSharedReward(const server::ServerObject& item)
{
    if ((item.classes & (oc::Player | oc::Monster)) || isRewardBook(item) || item.pickup.empty()) {
        return false; // (not a thing to carry, or a book: rewardToParty's)
    }
    if ((item.classes & oc::Key) || item.typeName == "SponsorshipLetter" || item.typeName.starts_with("Oblivion")) {
        return false; // (the party's one: shared by the key ring, or a later talk's, or unique)
    }
    return true;
}

void CoopDirector::itemToParty(server::ServerObject& taker, const RewardItem& item)
{
    if (!host_ || party_.size() < 2) {
        return;
    }
    const std::vector<server::ServerObject*> units = partyUnits();
    if (std::ranges::find(units, &taker) == units.end()) {
        return;
    }
    server::Fight f = host_->server().fight();
    if (!f.things) {
        return;
    }
    server::World& w = world();
    server::Items items{w, *f.things, f.modifiers, f.rng, f.frame};
    for (server::ServerObject* u : units) {
        if (u == &taker || !u->player) {
            continue;
        }
        server::ServerObject* copy = w.createAt(host_->server().things(), item.type, u->x, u->y, host_->server().clock().frame());
        if (!copy) {
            continue;
        }
        // The same thing: its modifiers (a weapon's, armour's or staff's), its charges, its gold.
        items.setModifiers(*copy, item.modifiers);
        if (item.maxCharges != 0) {
            copy->maxCharges = item.maxCharges;
            copy->charges = item.charges;
        }
        if (item.goldAmount != 0) {
            copy->goldAmount = item.goldAmount;
        }
        // Its own pickup, as the script's Pickup gives the taker it (Obj_TryPickup: gold's amount
        // and line, a potion drunk when it doesn't overfill, the class's gear). (The host's client's
        // grid check, Plr_DropCheapestItemIfFull, is the host's alone: a guest's grid isn't the
        // server's to see, as in the game's network games.)
        const bool took = items.pickUp(*u, *copy, true);
        if (!took && !copy->pendingDelete && !copy->gone()) {
            copy->pendingDelete = true; // (refused, too heavy: "carrying too much" told; none left lying about)
        }
    }
}

void CoopDirector::partyBookTo(server::ServerObject& unit)
{
    if (!host_ || !unit.player) {
        return;
    }
    server::Fight f = host_->server().fight();
    if (!f.things) {
        return;
    }
    // The best of each the others know (same class: what any of them learnt, it can).
    std::array<std::uint8_t, 0x89> spells{};
    std::array<bool, 6> abilities{};
    std::array<bool, 0x29> creatures{};
    for (const server::ServerObject* u : partyUnits()) {
        if (u == &unit || !u->player) {
            continue;
        }
        for (std::size_t s = 1; s < spells.size() && s < u->player->spellLevels.size(); ++s) {
            spells[s] = std::max(spells[s], u->player->spellLevels[s]);
        }
        for (std::size_t a = 1; a < abilities.size() && u->player->usesAbilities(); ++a) {
            abilities[a] = abilities[a] || u->player->abilityLevel(a) != 0;
        }
        for (std::size_t c = 1; c < creatures.size() && c < u->player->fieldGuide.size(); ++c) {
            creatures[c] = creatures[c] || u->player->fieldGuide[c];
        }
    }
    server::World& w = world();
    server::PlayerControl& p = *unit.player;
    // Told as a .plr's load tells them (no sound, no flight: MSG_REPORT_SPELL_AWARD, the
    // ability's and the guide's awards), so its book has them.
    if (p.classId != 0 || p.multiclass) {
        for (std::size_t s = 1; s < spells.size(); ++s) {
            if (spells[s] != 0 && s < p.spellLevels.size() && p.spellLevels[s] < spells[s]) {
                server::grantSpell(w, *f.things, unit, static_cast<int>(s), false, false, spells[s]);
            }
        }
    }
    for (std::size_t a = 1; a < abilities.size() && p.usesAbilities(); ++a) {
        if (abilities[a] && p.abilityLevel(a) == 0) {
            server::grantPlayerAbility(w, unit, static_cast<int>(a), false);
        }
    }
    for (std::size_t c = 1; c < creatures.size(); ++c) {
        if (creatures[c] && c < p.fieldGuide.size() && !p.fieldGuide[c]) {
            server::awardFieldGuide(w, unit, static_cast<int>(c), false);
        }
    }
}

void CoopDirector::pickedUp(server::World& w, server::ServerObject& taker, server::ServerObject& item, float x, float y)
{
    if (!host_ || party_.size() < 2 || !(item.pendingDelete || item.gone()) || scriptedReward_) {
        return; // (alone, or not used up: as the game; a script's reward: every player's already)
    }
    const bool spell = item.useFn == "SpellRewardUse";
    const bool ability = item.useFn == "AbilityRewardUse";
    const bool guide = item.useFn == "FieldGuideUse";
    if (!spell && !ability && !guide) {
        return;
    }
    const std::vector<server::ServerObject*> units = partyUnits();
    if (std::ranges::find(units, &taker) == units.end()) {
        return;
    }
    copies_.erase(item.extent);
    // Another of the party who hasn't had it yet (all of the campaign's class: what the taker
    // could use, they can). A spell book's: below the level the taker has now (a second book of
    // a spell is its next level).
    const int creature = guide ? server::guideCreatureIndex(item.guideCreature) : 0;
    const std::uint8_t level = spell && taker.player && item.bookEntry < taker.player->spellLevels.size() ? taker.player->spellLevels[item.bookEntry] : 1;
    const bool wanted = std::ranges::any_of(units, [&](const server::ServerObject* u) {
        if (u == &taker || !u->player) {
            return false;
        }
        if (spell) {
            return item.bookEntry < u->player->spellLevels.size() && u->player->spellLevels[item.bookEntry] < level;
        }
        if (ability) {
            return item.bookEntry < 6 && u->player->abilityLevel(item.bookEntry) == 0;
        }
        return creature > 0 && !u->player->fieldGuide[static_cast<std::size_t>(creature)];
    });
    if (!wanted) {
        return;
    }
    // The same book where it lay (the map's own reward, not new content).
    server::ServerObject* copy = w.createAt(host_->server().things(), item.typeName, x, y, host_->server().clock().frame());
    if (copy) {
        copy->bookEntry = item.bookEntry;
        copy->guideCreature = item.guideCreature;
        copy->subclasses = item.subclasses;
        if (spell) {
            copies_[copy->extent] = level; // (for those below it only: mayPickUp)
        }
    }
}

bool CoopDirector::mayPickUp(server::World& w, server::ServerObject& taker, server::ServerObject& item)
{
    (void)w;
    // A spell book put back for the party: not for one already at its level (it would take it
    // past the book's: the same book read twice).
    const auto it = copies_.find(item.extent);
    if (it == copies_.end() || !taker.player || item.bookEntry >= taker.player->spellLevels.size()) {
        return true;
    }
    return taker.player->spellLevels[item.bookEntry] < it->second;
}

} // namespace nox::coop
