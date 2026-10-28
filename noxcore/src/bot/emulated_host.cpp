// EmulatedHost (emulated_host.hpp): the rough stand-in server hosted bots play on in tests. It
// loads a map's walls, PlayerStarts and items, shows each bot what's near it, and applies its
// controls once a frame with simple, made-up rules (only a few numbers are the game's).
#include "noxcore/bot/emulated_host.hpp"

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace nox::bot {

namespace {

// The whole map's size in world units (the wall grid's cells times 23).
constexpr float kWorldSize = client::NavGrid::kCells * client::NavGrid::kCellSize;
constexpr std::uint32_t kRespawnFrames = 30;    ///< the dead press Action to come back after this

float distance(float ax, float ay, float bx, float by)
{
    return std::hypot(ax - bx, ay - by);
}

// Direction 0-255 as a unit vector (0 east, 64 south).
std::pair<float, float> unit(std::uint8_t direction)
{
    const double a = direction * 2.0 * std::numbers::pi / 256.0;
    return {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
}

} // namespace

// The controls a bot sets, kept in its Player's Controls for step() to apply.
class EmulatedHost::Body : public BotBody {
public:
    explicit Body(Controls& c)
        : c_(c)
    {
    }
    void setMove(std::optional<std::uint8_t> direction) override { c_.move = direction; }
    void setFacing(std::uint8_t direction) override { c_.facing = direction; }
    void setCursor(int x, int y) override { c_.cursor = {x, y}; }
    void setAction(bool held) override { c_.action = held; }
    void pressAction() override { c_.press = true; }
    void jump() override { c_.jump = true; }
    void useAbility(std::uint8_t ability) override { c_.abilities.push_back(ability); }
    void castSpells(std::span<const std::uint32_t>, bool) override {}
    void pickup(std::uint16_t netCode) override { c_.pickups.push_back(netCode); }
    void useItem(std::uint16_t netCode) override { c_.uses.push_back(netCode); }
    void equip(std::uint16_t netCode) override { c_.equips.push_back(netCode); }
    void say(std::string_view, bool) override {}
    void taunt() override {}
    void joinTeam(std::uint32_t) override {}
    void observe() override { c_.observe = true; }

private:
    Controls& c_;
};

EmulatedHost::Rules EmulatedHost::Rules::from(const things::GameData& gameData)
{
    // A deathmatch's numbers: the ARENA and unprefixed lines, resolved here once.
    const auto number = [&](std::string_view name) { return gameData.arena(name, 0); };
    Rules r;
    constexpr std::array<std::string_view, 6> kDelays{"", "BerserkerChargeDelay", "WarcryDelay", "HarpoonDelay",
                                                      "TreadLightlyDelay", "EyeOfTheWolfDelay"};
    for (std::size_t i = 1; i < kDelays.size(); ++i) {
        r.abilityDelay[i] = static_cast<std::uint32_t>(number(kDelays[i]));
    }
    r.chargeFrames = static_cast<std::uint32_t>(number("BerserkerChargeDuration"));
    r.chargeDamage = static_cast<std::uint16_t>(number("BerserkerDamage"));
    r.chargeStunFrames = static_cast<std::uint32_t>(number("BerserkerStunDuration"));
    r.chargePain = number("BerserkerPainRatio");
    r.harpoonRange = number("MaxHarpoonFlightDistance");
    r.harpoonDamage = static_cast<std::uint16_t>(number("HarpoonDamage"));
    r.maxHealth = static_cast<std::uint16_t>(number("WarriorMaxHealth"));
    return r;
}

EmulatedHost::EmulatedHost(const map::MapFile& map, const things::ThingDb& things, const things::GameData& gameData,
                           std::uint32_t seed, std::size_t playerLimit, const things::ModifierDb* modifiers)
    : rules_(Rules::from(gameData))
    , nav_(client::NavGrid::fromMap(map))
    , kinds_(things, modifiers, &gameData)
    , playerLimit_(playerLimit)
    , rng_(seed)
{
    // A thing type's id by name (thing.bin index + 1; 0 if unknown).
    const auto typeOf = [&](std::string_view name) -> std::uint16_t {
        const auto* t = things.findThing(name);
        return t ? static_cast<std::uint16_t>(t - things.things.data() + 1) : 0;
    };
    swordType_ = typeOf("Longsword");
    nav_.addObstacles(map, things); // solid objects stop players here too
    nav_.addFloor(map);             // and there's no walking off the floor
    // Walk the map's objects: PlayerStarts become spawns, anything a bot cares about an item.
    const ItemKinds& kinds = kinds_;
    const auto toc = map::decodeSection<map::ObjectTOC>(map);
    const auto objects = map::decodeSection<map::ObjectData>(map);
    std::map<std::uint16_t, std::string> names;
    for (const auto& e : toc.entries) {
        names[e.id] = e.name;
    }
    std::size_t inside = 0; // records that are the previous object's inventory
    for (const auto& rec : objects.records) {
        if (inside > 0) {
            --inside; // in a chest or a monster's pockets, not on the ground
            continue;
        }
        map::ObjectXfer x;
        try {
            x = map::ObjectXfer::parse(rec.data);
        } catch (const std::exception&) {
            continue;
        }
        inside = x.header.inventoryCount();
        const std::string& name = names[rec.tocId];
        if (name == "PlayerStart") {
            spawns_.emplace_back(x.header.x, x.header.y);
            continue;
        }
        const std::uint16_t type = typeOf(name);
        const ItemKind kind = kinds.kind(type);
        if (kind != ItemKind::Other) {
            items_.push_back({nextItemCode_++, type, kind, x.header.x, x.header.y, 0});
        }
    }
    // What a player who knows the map knows lies where (BotView::mapItems).
    for (const auto& i : items_) {
        const ItemInfo& info = kinds_.info(i.typeId);
        mapItems_.push_back({0, i.typeId, i.kind, i.x, i.y, kinds_.usableBy(i.typeId, 0), info.value, info.slot});
    }
}

EmulatedHost::~EmulatedHost() = default;

std::optional<std::uint16_t> EmulatedHost::join(std::string name, bool human)
{
    if (players_.size() >= playerLimit_) {
        return std::nullopt;
    }
    const std::uint16_t netCode = nextNetCode_++;
    Player& p = players_[netCode];
    p.netCode = netCode;
    p.name = std::move(name);
    p.human = human;
    p.maxHealth = rules_.maxHealth;
    if (!spawns_.empty()) {
        std::tie(p.x, p.y) = spawns_.front(); // watching from somewhere
    }
    bodies_[netCode] = std::make_unique<Body>(p.controls);
    if (human) {
        spawn(p); // a stand-in comes straight in
    }
    return netCode;
}

std::optional<std::uint16_t> EmulatedHost::addBot(const client::PlayerProfile& profile)
{
    return join(profile.name, false);
}

std::optional<std::uint16_t> EmulatedHost::addHuman(std::string name)
{
    return join(std::move(name), true);
}

void EmulatedHost::removeBot(std::uint16_t netCode)
{
    bodies_.erase(netCode);
    players_.erase(netCode);
}

BotBody& EmulatedHost::body(std::uint16_t netCode)
{
    return *bodies_.at(netCode);
}

std::size_t EmulatedHost::humans() const
{
    return static_cast<std::size_t>(std::ranges::count_if(players_, [](const auto& p) { return p.second.human; }));
}

std::set<std::string> EmulatedHost::names() const
{
    std::set<std::string> out;
    for (const auto& [_, p] : players_) {
        out.insert(p.name);
    }
    return out;
}

// What bot `netCode` would know as a network client: itself in full, its items with their
// ItemKinds facts, and the other players and floor items limited to its screen.
BotView EmulatedHost::view(std::uint16_t netCode) const
{
    const Player& me = players_.at(netCode);
    BotView v;
    v.numbers = kinds_.numbers();
    v.frame = frame_;
    v.inGame = true;
    v.netCode = netCode;
    v.x = me.x;
    v.y = me.y;
    v.placed = true;
    v.dead = me.dead;
    v.observer = me.observer;
    v.health = me.health;
    v.maxHealth = me.maxHealth;
    v.stamina = me.stamina;
    v.mapItems = &mapItems_;
    // Abilities 1-5 (0 is unused): ready once the cooldown is over and not mid-charge.
    for (std::size_t a = 1; a < v.abilityReady.size(); ++a) {
        v.abilityReady[a] = frame_ >= me.cooldownUntil[a] && frame_ >= me.chargeUntil;
        v.abilityLevel[a] = 3;
    }
    // Our items, filled in with what ItemKinds knows of their types.
    v.inventory = me.inventory;
    for (auto& i : v.inventory) {
        const ItemInfo& info = kinds_.info(i.typeId);
        i.usable = kinds_.usableBy(i.typeId, 0);
        i.value = info.value;
        i.slot = info.slot;
        i.blocks = info.blocks;
        i.reach = info.reach;
        i.swingStamina = info.swingStamina;
        i.twoHanded = info.twoHanded;
        i.ranged = info.ranged;
        i.crossbow = info.crossbow;
        i.wand = info.wand;
    }
    // Animations as a client sees them: DEAD, BERSERKER_CHARGE, SWORD_STRIKE for the first half
    // of a swing, else IDLE.
    const auto anim = [&](const Player& p) -> std::uint8_t {  // 2 DEAD, 27 SWORD_STRIKE, 0 IDLE
        if (p.dead) {
            return 2;
        }
        if (frame_ < p.chargeUntil) {
            return kChargeAnim;
        }
        return frame_ + kSwingFrames / 2 < p.nextSwing ? 27 : 0;
    };
    v.anim = anim(me);
    // Only what a client of a 640 x 480 view would be sent about (the server sends what's near
    // the player's screen), so a bot here knows no more than one over the network.
    const auto near = [&](float x, float y) {
        return std::abs(x - me.x) <= kViewHalfWidth && std::abs(y - me.y) <= kViewHalfHeight;
    };
    for (const auto& [code, p] : players_) {
        if (code == netCode) {
            continue;
        }
        SeenPlayer s;
        s.netCode = code;
        s.name = p.name;
        s.x = p.x;
        s.y = p.y;
        s.placed = !p.observer && near(p.x, p.y);
        s.dead = p.dead;
        s.observer = p.observer;
        s.anim = anim(p);
        v.players.push_back(std::move(s));
    }
    for (const auto& i : items_) {
        if (i.backAt == 0 && near(i.x, i.y)) {
            const ItemInfo& info = kinds_.info(i.typeId);
            v.items.push_back({i.netCode, i.typeId, i.kind, i.x, i.y, kinds_.usableBy(i.typeId, 0), info.value, info.slot});
        }
    }
    v.nav = &nav_;
    return v;
}

// Into play at a random PlayerStart, full health, with a Longsword if it has no weapon.
void EmulatedHost::spawn(Player& p)
{
    if (!spawns_.empty()) {
        std::tie(p.x, p.y) = spawns_[nox::util::uniform_int_distribution<std::size_t>(0, spawns_.size() - 1)(rng_)];
    }
    p.observer = false;
    p.dead = false;
    p.health = p.maxHealth;
    p.chargeUntil = 0;
    p.stunnedUntil = 0;
    p.airUntil = 0;
    p.stamina = 100;
    if (swordType_ != 0 && std::ranges::none_of(p.inventory, [](const OwnItem& i) { return i.kind == ItemKind::Weapon; })) {
        p.inventory.push_back({nextItemCode_++, swordType_, ItemKind::Weapon, true});  // equipped
    }
}

bool EmulatedHost::open(float x, float y) const
{
    if (x < 0 || y < 0 || x >= kWorldSize || y >= kWorldSize) {
        return false;
    }
    return nav_.empty() || !nav_.blocked(static_cast<int>(x) / client::NavGrid::kCellSize,
                                         static_cast<int>(y) / client::NavGrid::kCellSize);
}

void EmulatedHost::move(Player& p, std::uint8_t direction, float speed)
{
    const auto [ux, uy] = unit(direction);
    const float nx = p.x + ux * speed;
    const float ny = p.y + uy * speed;
    if (open(nx, ny)) {
        p.x = nx;
        p.y = ny;
    } else if (open(nx, p.y)) {
        p.x = nx; // slide along the wall
    } else if (open(p.x, ny)) {
        p.y = ny;
    }
}

// The nearest player in play within `reach` and `halfArc` direction units of p's facing.
EmulatedHost::Player* EmulatedHost::nearestInFront(const Player& p, float reach, int halfArc)
{
    Player* best = nullptr;
    float bestDistance = 0;
    for (auto& [code, o] : players_) {
        if (code == p.netCode || o.dead || o.observer) {
            continue;
        }
        const float d = distance(p.x, p.y, o.x, o.y);
        const int off = std::abs(static_cast<std::int8_t>(directionTo(p.x, p.y, o.x, o.y) - p.facing));  // the facing difference, wrapped to -128..127
        if (d <= reach && off <= halfArc && (!best || d < bestDistance)) {
            best = &o;
            bestDistance = d;
        }
    }
    return best;
}

// Takes `damage` off the victim; at 0 it dies and the kill is recorded (`by` null: a hazard).
void EmulatedHost::hurt(Player& victim, std::uint16_t damage, Player* by, bool charge)
{
    if (victim.dead || victim.observer) {
        return;
    }
    victim.health = static_cast<std::uint16_t>(victim.health > damage ? victim.health - damage : 0);
    if (victim.health > 0) {
        return;
    }
    victim.dead = true;
    victim.deadSince = frame_;
    victim.chargeUntil = 0;
    ++victim.deaths;
    if (by && by != &victim) {
        ++by->kills;
    }
    kills_.push_back({frame_, by ? by->netCode : std::uint16_t(0), victim.netCode, charge, victim.hazardFrames, victim.x,
                      victim.y});
}

// A blow at the nearest enemy in front: a fixed amount plus a part of the weapon's value.
void EmulatedHost::swing(Player& p)
{
    float value = 0;
    float reach = kReach;
    for (const auto& i : p.inventory) {
        if (i.kind == ItemKind::Weapon && i.equipped) {
            const ItemInfo& info = kinds_.info(i.typeId);
            value = info.value;
            reach = info.reach > 0 ? info.reach : kReach;
        }
    }
    // Within the weapon's reach and 60 degrees of the facing (cos > 0.5).
    if (Player* victim = nearestInFront(p, reach, 43)) {
        hurt(*victim, static_cast<std::uint16_t>(kSwingDamage + value * kSwingPerValue), &p, false);
    }
}

// Starts an ability's cooldown and does what this stand-in models of it (charge, harpoon).
void EmulatedHost::ability(Player& p, std::uint8_t which)
{
    if (which == 0 || which >= rules_.abilityDelay.size() || frame_ < p.cooldownUntil[which] || frame_ < p.chargeUntil) {
        return;
    }
    p.cooldownUntil[which] = frame_ + rules_.abilityDelay[which];
    if (which == Ability::BerserkerCharge) {
        p.chargeUntil = frame_ + rules_.chargeFrames;
    } else if (which == Ability::Harpoon) {
        // The first player near the line to the cursor, within the harpoon's flight, is dragged
        // in front of us.
        const float cx = static_cast<float>(p.controls.cursor.first);
        const float cy = static_cast<float>(p.controls.cursor.second);
        const float len = distance(p.x, p.y, cx, cy);
        if (len < 1) {
            return;
        }
        const float ux = (cx - p.x) / len;
        const float uy = (cy - p.y) / len;
        Player* hit = nullptr;
        float hitAlong = 0;
        for (auto& [code, o] : players_) {
            if (code == p.netCode || o.dead || o.observer) {
                continue;
            }
            // Distance along the line to the cursor, and off it to either side.
            const float along = (o.x - p.x) * ux + (o.y - p.y) * uy;
            const float across = std::abs((o.x - p.x) * uy - (o.y - p.y) * ux);
            if (along >= 20 && along <= rules_.harpoonRange && across <= 20 && (!hit || along < hitAlong)) {
                hit = &o;
                hitAlong = along;
            }
        }
        if (hit) {
            hit->x = p.x + ux * 40;
            hit->y = p.y + uy * 40;
            hurt(*hit, rules_.harpoonDamage, &p, false);
        }
    }
    // War Cry silences casters within 300 and the others are the player's own business: no
    // casters or buffs here, so they only start their cooldown.
}

void EmulatedHost::step()
{
    ++frame_;
    // Taken items whose respawn time has come lie there again.
    for (auto& i : items_) {
        if (i.backAt != 0 && frame_ >= i.backAt) {
            i.backAt = 0;
        }
    }
    for (auto& [code, p] : players_) {
        Controls& c = p.controls;
        if (p.human) {
            if (p.dead && frame_ - p.deadSince >= 90) {  // back 3 s (90 frames) after dying
                spawn(p);
            }
            continue; // stands still: target practice
        }
        // The bots: Observer Mode first, then Action to come back in (or respawn once dead long enough).
        if (std::exchange(c.observe, false)) {
            p.observer = true; // (Observer Mode: out of play until Action)
            p.dead = false;
        }
        if (std::exchange(c.press, false) && (p.observer || (p.dead && frame_ - p.deadSince >= kRespawnFrames))) {
            spawn(p);
        }
        const auto oneShots = [&] {
            c.abilities.clear();
            c.pickups.clear();
            c.uses.clear();
            c.equips.clear();
        };
        if (p.dead || p.observer) {
            oneShots();
            continue;
        }
        // Potions: health heals; any potion is used up.
        for (const auto nc : c.uses) {
            const auto it = std::ranges::find(p.inventory, nc, &OwnItem::netCode);
            if (it == p.inventory.end()) {
                continue;
            }
            if (it->kind == ItemKind::HealthPotion) {
                p.health = static_cast<std::uint16_t>(std::min<int>(p.maxHealth, p.health + kPotionHealth));
            }
            if (it->kind == ItemKind::HealthPotion || it->kind == ItemKind::ManaPotion || it->kind == ItemKind::CurePotion) {
                p.inventory.erase(it);
            }
        }
        // Equipping a weapon takes the others out of hand.
        for (const auto nc : c.equips) {
            const auto it = std::ranges::find(p.inventory, nc, &OwnItem::netCode);
            if (it == p.inventory.end() || (it->kind != ItemKind::Weapon && it->kind != ItemKind::Armor)) {
                continue; // a warrior can't use a staff
            }
            if (it->kind == ItemKind::Weapon) {
                for (auto& other : p.inventory) {
                    if (other.kind == ItemKind::Weapon) {
                        other.equipped = false;
                    }
                }
            }
            it->equipped = true;
        }
        // Pickups within 40 units; the item respawns kItemRespawnFrames later.
        for (const auto nc : c.pickups) {
            const auto it = std::ranges::find(items_, nc, &Item::netCode);
            if (it != items_.end() && it->backAt == 0 && distance(p.x, p.y, it->x, it->y) <= 40) {
                p.inventory.push_back({nextItemCode_++, it->typeId, it->kind, false});  // not equipped
                it->backAt = frame_ + kItemRespawnFrames;
            }
        }
        for (const auto a : c.abilities) {
            ability(p, a);
        }
        oneShots();
        // Back 100 / FPS a frame (updatePlayer).
        p.stamina = static_cast<std::uint8_t>(std::min(100, p.stamina + 3));
        if (std::exchange(c.jump, false) && frame_ >= p.airUntil && p.stamina >= kJumpStamina) {
            p.stamina = static_cast<std::uint8_t>(p.stamina - kJumpStamina);
            p.airUntil = frame_ + kJumpFrames;
        }
        const bool airborne = frame_ < p.airUntil;
        const bool inHazard = !airborne && !nav_.empty() &&
                              nav_.hazard(static_cast<int>(p.x) / client::NavGrid::kCellSize,
                                          static_cast<int>(p.y) / client::NavGrid::kCellSize);
        p.hazardFrames = inHazard ? p.hazardFrames + 1 : 0;
        if (inHazard && frame_ % 4 == 0) {  // every 4th frame
            hurt(p, kHazardDamage, nullptr, false); // flames, lava, spikes underfoot
            if (p.dead) {
                continue;
            }
        }
        if (frame_ < p.stunnedUntil) {
            continue;
        }
        if (airborne) {
            move(p, p.facing, kRunSpeed * 2); // a jump goes the way we face
            continue;
        }
        if (frame_ < p.chargeUntil) {
            // Charging: on along our facing until a wall (stunned, and hurt by a part of the
            // health left) or a player (hit hard).
            const auto [ux, uy] = unit(p.facing);
            const float nx = p.x + ux * kChargeSpeed;
            const float ny = p.y + uy * kChargeSpeed;
            if (!open(nx, ny)) { // a wall (or the map's edge)
                p.chargeUntil = 0;
                p.stunnedUntil = frame_ + rules_.chargeStunFrames;
                hurt(p, static_cast<std::uint16_t>(p.health * rules_.chargePain), &p, true);
                continue;
            }
            p.x = nx;
            p.y = ny;
            if (Player* victim = nearestInFront(p, 24, 64)) {  // within 24, up to a quarter turn (64 of 256) either side
                p.chargeUntil = 0;
                hurt(*victim, rules_.chargeDamage, &p, true);
            }
            continue;
        }
        if (c.move) {
            p.facing = *c.move;
            const bool run = distance(p.x, p.y, float(c.cursor.first), float(c.cursor.second)) > 100;  // the cursor over 100 away: running
            move(p, *c.move, run ? kRunSpeed : kWalkSpeed);
        } else {
            p.facing = c.facing;
        }
        if (c.action && frame_ >= p.nextSwing) {
            // A swing takes the weapon's stamina; without it, nothing (playerInputAttack).
            std::uint8_t cost = 0;
            for (const auto& i : p.inventory) {
                if (i.kind == ItemKind::Weapon && i.equipped) {
                    cost = kinds_.info(i.typeId).swingStamina;
                }
            }
            if (p.stamina >= cost) {
                p.stamina = static_cast<std::uint8_t>(p.stamina - cost);
                swing(p);
                p.nextSwing = frame_ + kSwingFrames;
            }
        }
    }
}

} // namespace nox::bot
