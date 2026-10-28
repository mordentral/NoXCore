// The shared bot pieces (bot.hpp): what each thing type is to a bot (ItemKinds), swing stamina,
// the skill levels, Nox directions and bot names.
#include "noxcore/bot/bot.hpp"

#include "noxcore/things/game_data.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <numbers>

namespace nox::bot {

GameNumbers GameNumbers::from(const things::GameData& gameData)
{
    // (Bots play network games: the ARENA and unprefixed lines.)
    GameNumbers n;
    n.drainRange = gameData.arena("ManaDrainRange", 0);
    n.harpoonFlight = gameData.arena("MaxHarpoonFlightDistance", 0);
    return n;
}

ItemKinds::ItemKinds(const things::ThingDb& things, const things::ModifierDb* modifiers, const things::GameData* gameData)
    : numbers_(gameData ? GameNumbers::from(*gameData) : GameNumbers{})
{
    namespace oc = things::ObjectClass;
    // Armour pieces are numbered from 1 in the order their SUBCLASS first turns up.
    std::map<std::string, std::uint8_t, std::less<>> slots; // armour SUBCLASS -> slot
    infos_.reserve(things.things.size());
    // One ItemInfo for each thing.bin type, in order (type id = index + 1).
    for (const auto& t : things.things) {
        ItemInfo info;
        const things::EquipDef* weapon = modifiers ? modifiers->findWeapon(t.name) : nullptr;
        const things::EquipDef* armor = modifiers && !weapon ? modifiers->findArmor(t.name) : nullptr;
        const things::EquipDef* equip = weapon ? weapon : armor;
        if (equip && equip->classUse) {
            info.classUse = *equip->classUse;
        }
        const std::uint32_t classes = things::thingClasses(t);
        const std::string_view sub = t.attribute("SUBCLASS").value_or("");
        const auto has = [&](std::string_view s) { return sub.find(s) != std::string_view::npos; };
        info.missile = (classes & oc::Missile) != 0;
        info.flag = (classes & oc::Flag) != 0;
        info.creature = (classes & oc::Monster) && !(classes & oc::Immobile);
        info.obelisk = has("OBELISK");
        info.refills = info.obelisk && t.attribute("UPDATE").value_or("") == "ObeliskUpdate";
        if (classes & oc::Wand) {
            info.kind = ItemKind::Staff; // staffs: wizards' and conjurers' (CLASS WAND)
            // How good a staff's shot is, roughly as players rank them; the wooden one only
            // strikes (and the Missile, Summoning, Web and Poison wands are monsters'). Any bow
            // with ammunition is held over all of them (CasterBrain::tendGear), the Force of
            // Nature's too: its ball bounces back at whoever shot it.
            static constexpr std::pair<std::string_view, float> kStaffs[] = {
                {"STAFF_DEATH_RAY", 60.0f},        {"STAFF_TRIPLE_FIREBALL", 55.0f}, {"STAFF_FORCE_OF_NATURE", 55.0f},
                {"STAFF_LIGHTNING", 50.0f},        {"STAFF_OBLIVION_ORB", 50.0f},    {"STAFF_SULPHOROUS_SHOWER", 45.0f},
                {"STAFF_SULPHOROUS_FLARE", 40.0f}, {"STAFF_FIREBALL", 35.0f},
            };
            info.value = has("STAFF") ? 10.0f : 0.0f;
            for (const auto& [sub2, v] : kStaffs) {
                if (has(sub2)) {
                    info.value = v;
                    info.wand = true;
                    break;
                }
            }
        } else if ((classes & oc::Weapon) && !info.missile && has("QUIVER")) {
            info.kind = ItemKind::Quiver; // a bow's or a crossbow's ammunition
        } else if ((classes & oc::Weapon) && !info.missile) {
            info.kind = ItemKind::Weapon;
            // Melee damage at 100 strength (calcBoltDamage's terms); thrown
            // (SHURIKEN: the chakrams) and shot ones are no use to a melee brain.
            const bool melee = !has("SHURIKEN") && !has("BOW") && !has("QUIVER") && !has("FLAG");
            if (weapon && melee) {
                info.value = static_cast<float>(weapon->damageMin) + weapon->damageCoefficient * 100;
                // The hammer strikes round a point 35 ahead; the rest round the
                // wielder. Both players' radius (10) on top.
                info.reach = has("HAMMER") ? 35 + weapon->range + 10 : weapon->range + 20;
            }
            info.blocks = has("GREAT_SWORD");
            info.twoHanded = has("GREAT_SWORD") || has("HAMMER");
            info.ranged = has("BOW"); // (BOW and CROSSBOW)
            info.crossbow = has("CROSSBOW");
            if (info.ranged) {
                info.value = has("CROSSBOW") ? 50.0f : 40.0f; // (a conjurer's; warriors can't use them)
            }
            info.swingStamina = swingStamina(things::thingSubclasses(t));
        } else if (classes & oc::Armor) {
            info.kind = ItemKind::Armor;
            if (armor) {
                info.value = armor->armorValue * 1000;
            }
            info.blocks = has("SHIELD");
            // The piece is the SUBCLASS's first word (it may list more after a space or '+').
            const std::string piece(sub.substr(0, sub.find_first_of(" +")));
            if (!piece.empty()) {
                info.slot = slots.try_emplace(piece, static_cast<std::uint8_t>(slots.size() + 1)).first->second;
            }
        } else if (classes & oc::Food) {
            // The potions are FOOD with a POTION subclass naming what they do.
            if (has("CURE_POISON_POTION")) {
                info.kind = ItemKind::CurePotion;
            } else if (has("HEALTH_POTION")) {
                info.kind = ItemKind::HealthPotion;
            } else if (has("MANA_POTION")) {
                info.kind = ItemKind::ManaPotion;
            }
        }
        infos_.push_back(info);
    }
    if (modifiers) {
        // The team colours by material modifier name, as FlagDraw reads them.
        static constexpr std::array<std::pair<std::string_view, std::uint8_t>, 9> kTeamMaterials{{
            {"MaterialTeamRed", 1},
            {"MaterialTeamBlue", 2},
            {"MaterialTeamGreen", 3},
            {"MaterialTeamCyan", 4},
            {"MaterialTeamYellow", 5},
            {"MaterialTeamViolet", 6},
            {"MaterialTeamBlack", 7},
            {"MaterialTeamWhite", 8},
            {"MaterialTeamOrange", 9},
        }};
        for (const auto& m : modifiers->modifiers) {
            for (const auto& [name, color] : kTeamMaterials) {
                if (m.name == name) {
                    teamColors_[m.id] = color;
                }
            }
        }
    }
}

// By the weapon's SUBCLASS bits (the player's WeaponEquip), checked in this order.
std::uint8_t swingStamina(std::uint32_t bits)
{
    if (bits == 0) {
        return 0; // bare hands: free (Plr_SetState(1) without a cost)
    }
    if (bits & 0x200) {
        return 70; // Longsword
    }
    if (bits & (0x4000 | 0x2000)) {
        return 100; // WarHammer, OgreAxe
    }
    if (bits & 0x800) {
        return 50; // MorningStar
    }
    if (bits & 0x100) {
        return 45; // Sword
    }
    if (bits & 0x1000) {
        return 75; // BattleAxe
    }
    if (bits & 0x7ff8000) {  // any of the staff bits
        return 45; // the staffs
    }
    return (bits & 0x400) ? 75 : 10; // GreatSword; the rest
}

bool isStrike(std::uint8_t anim)
{
    // PUNCH_LEFT .. STAFF_STRIKE, STAFF_THRUST, AXE_STRIKE, GREAT_SWORD_STRIKE, HAMMER_STRIKE
    return (anim >= 23 && anim <= 29) || anim == 32 || anim == 35 || anim == 37 || anim == 39;
}

// Easy: slow to react, poor aim, few abilities and dodges; goes easy on strugglers.
Skill Skill::easy()
{
    Skill s;
    s.reactionFrames = 18;
    s.aimError = 24;
    s.shotError = 4;
    s.retreatHealth = 0.25f;
    s.abilityChance = 20;
    s.dodgeChance = 15;
    s.gentle = true;
    return s;
}

// The easing a gentle bot puts on for someone struggling (Skill::gentle).
Skill Skill::gently() const
{
    Skill s = *this;
    s.reactionFrames += 12;
    s.aimError += 16;
    s.shotError += 4;
    s.abilityChance /= 2;
    s.dodgeChance /= 2;
    s.slowdown += 15;
    return s;
}

// Normal: the Skill defaults.
Skill Skill::normal()
{
    return Skill{};
}

// Hard: quick, accurate, abilities and dodges nearly always.
Skill Skill::hard()
{
    Skill s;
    s.reactionFrames = 4;
    s.aimError = 3;
    s.shotError = 0;
    s.retreatHealth = 0.45f;
    s.abilityChance = 90;
    s.dodgeChance = 90;
    return s;
}

std::uint8_t directionTo(float fromX, float fromY, float toX, float toY)
{
    // Nox's angle in turns of the circle (y grows southward), then 256 steps a turn.
    const double turns = std::atan2(double(toY - fromY), double(toX - fromX)) / (2.0 * std::numbers::pi);
    return static_cast<std::uint8_t>(static_cast<int>(std::lround(turns * 256.0)) & 0xFF);
}

namespace {

// At most 18 characters each, so "[BOT] " and the name fit the game's 24.
constexpr std::array<std::string_view, 40> kNames{
    "Grimwald the Unlit", "Morgrin Ashfist",  "Hesta Coldwater",  "Ulric of Dun Mir", "Brannoc the Grey",
    "Sable Wyrmtongue",   "Torvald Ironside", "Kestrel Nightjar", "Old Man Gorrin",   "Aldric the Bold",
    "Fenna Thornwick",    "Horvath Grimm",    "Isolde Brightaxe", "Jarek the Lame",   "Lyssa Moonwell",
    "Magnus Redbeard",    "Nessa Galecall",   "Osric the Wary",   "Perrin Ashdown",   "Quill Mossback",
    "Rurik Stonejaw",     "Selka Duskvane",   "Thane Ogrebane",   "Ysolde the Fair",  "Varek Hollowmane",
    "Wulfric Tallow",     "Dorn the Quiet",   "Elric Cinderhand", "Garrick Mudfoot",  "Hilda Bonecrush",
    "Bram Whitlock",      "Cormac the Lost",  "Edda Flintheart",  "Finnick Larkspur", "Gwyn of Galava",
    "Ivo the Unready",    "Kara Ironwood",    "Lothar Grimsby",   "Mira Swiftbrook",  "Nils Hammerfall",
};

using util::lowered;

} // namespace

std::string botName(std::mt19937& rng, const std::set<std::string>& taken)
{
    std::set<std::string> used;
    for (const auto& name : taken) {
        used.insert(lowered(name));
    }
    std::vector<std::string> free;
    for (const auto name : kNames) {
        std::string full = "[BOT] " + std::string(name);
        if (!used.contains(lowered(full))) {
            free.push_back(std::move(full));
        }
    }
    if (!free.empty()) {
        return free[nox::util::uniform_int_distribution<std::size_t>(0, free.size() - 1)(rng)];
    }
    for (int n = 1;; ++n) {
        std::string full = "[BOT] Bot " + std::to_string(n);
        if (!used.contains(lowered(full))) {
            return full;
        }
    }
}

} // namespace nox::bot
