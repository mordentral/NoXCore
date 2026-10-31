// Writes and reads a player's .plr on the server: one writer and one reader per server section
// (Attrib, Status, Inventory, FieldGuide, Spellbook, Enchantment, Journal, GameData, Pad), plus
// the SAVEDATA / SAVE_SERVER byte streams. player_save.hpp lists what each section holds;
// player/player_file.hpp has the file layout itself.
#include "noxcore/server/player_save.hpp"

#include "noxcore/client/spell_bars.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/server/abilities.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/object_links.hpp"
#include "noxcore/server/object_save.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/quest_status.hpp"
#include "noxcore/server/rewards.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

constexpr std::uint32_t kGameFps = 30; // (gameFPS__)
constexpr std::size_t kName = 0x00;     // the info block's fields (PlayerControl::info, the join's PlayerInfo)
constexpr std::size_t kMaxMana = 0x3A;
constexpr std::size_t kMaxHealth = 0x3E;
constexpr std::size_t kFemale = 0x43;
constexpr std::size_t kColors = 0x44;
constexpr std::size_t kClothing = 0x53;
constexpr std::size_t kInfo58 = 0x58;

using util::hi;
using util::lo;

void tell(World& world, const ServerObject& unit, std::vector<std::uint8_t> bytes)
{
    world.broadcasts().push_back({std::move(bytes), true, unit.extent});
}

// A little-endian u32 in the info block, read or written.
std::uint32_t infoU32(const PlayerControl& p, std::size_t at)
{
    return util::u32At(p.info, at);
}

void setInfoU32(PlayerControl& p, std::size_t at, std::uint32_t v)
{
    for (std::size_t i = 0; i < 4; ++i) {
        p.info[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

/// Math_FloatToInt (chop mode).
using util::chop;

bool solo(const World& world) { return (world.gameFlags & GameFlag::Solo) != 0; }
bool quest(const World& world) { return (world.gameFlags & GameFlag::Quest) != 0; }
bool network(const World& world) { return (world.gameFlags & GameFlag::Network) != 0; }

/// The inventory's, field guide's and spellbook's "has": not in a network game but a quest.
bool hasBook(const World& world) { return !(network(world) && !quest(world)); }

int abilityByName(std::string_view name)
{
    // (abilityNameToN: 0 when unknown)
    for (std::size_t i = 1; i < client::kAbilityNames.size(); ++i) {
        if (name == client::kAbilityNames[i]) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

std::string_view abilityName(int ability)
{
    return 0 <= ability && ability < static_cast<int>(client::kAbilityNames.size()) ? client::kAbilityNames[static_cast<std::size_t>(ability)]
                                                                                   : std::string_view{};
}

int guideByName(std::string_view name)
{
    // (Game_GetCreatureGuideIndex: 0 when unknown)
    for (std::size_t i = 0; i < things::kGuideNames.size(); ++i) {
        if (name == things::kGuideNames[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

/// The Shield spell on `unit` (the lasting-spell lookup (0x33, unit): its record not cancelled, its target).
DurationSpell* shieldSpell(World& world, const ServerObject& unit)
{
    for (DurationSpell& s : world.durationSpells()) {
        if (!s.cancelled && s.spell == 0x33 && s.target != 0 && s.target == unit.extent) {
            return &s;
        }
    }
    return nullptr;
}

ActiveAbility* activeAbility(World& world, const ServerObject& unit, int ability)
{
    for (ActiveAbility& a : world.activeAbilities()) {
        if (a.unit == unit.extent && a.ability == ability) {
            return &a;
        }
    }
    return nullptr;
}

std::string xferNameOf(const things::ThingDef* t)
{
    const auto x = t ? t->lastAttribute("XFER") : std::nullopt;
    return x ? std::string(*x) : std::string("DefaultXfer");
}

// ---------------------------------------------------------------------------------------------
// Writing (the read / write mode 0)

/// An inventory item's record (its type's xfer writing it: the header, mapReadWriteObjData,
/// then its own fields): the map save's writer (object_save.hpp objectRecord), its owned things,
/// pickup handler and file extent included.
std::vector<std::uint8_t> itemRecord(Fight& fight, const ServerObject& item)
{
    return objectRecord(fight.world, *fight.things, fight.modifiers, fight.frame, item);
}

// Attrib: name, numbers, class, sex, colours, clothing, lives and highest stage. Fails on a
// name over 24 letters or more lives than MaxExtraLives allows.
std::optional<std::vector<std::uint8_t>> writeAttrib(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    const PlayerControl& p = *unit.player;
    player::Attrib a;
    a.version = 5;
    a.gameMode = quest(world) ? 4u : solo(world) ? 1u : 2u;
    if (0x18 < p.name.size()) {
        return std::nullopt;
    }
    a.name = p.name;
    a.speed = p.speed;
    a.strength = p.strength;
    a.maxMana = infoU32(p, kMaxMana);
    a.maxHealth = infoU32(p, kMaxHealth);
    a.characterClass = static_cast<player::CharacterClass>(p.classId);
    a.isFemale = p.info[kFemale];
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            a.colors[i][c] = p.info[kColors + i * 3 + c];
        }
    }
    a.clothing = p.clothing;
    a.unknown88 = p.info[kInfo58];
    a.extraLives = p.questLives;
    if (chop(world.number("MaxExtraLives")) < static_cast<std::int32_t>(p.questLives)) {
        return std::nullopt;
    }
    a.highestQuestStage = p.questHighestStage;
    return a.serialize();
}

// Status: a solo game's health, mana, poison, experience and facing; else only the "has" byte.
std::optional<std::vector<std::uint8_t>> writeStatus(Fight& fight, ServerObject& unit)
{
    player::Status s;
    s.version = 2;
    if (solo(fight.world)) {
        const PlayerControl& p = *unit.player;
        player::Status::Values v;
        v.maxHealth = unit.maxHealth;
        v.maxMana = p.maxMana;
        v.health = unit.health;
        v.mana = p.mana;
        v.poison = unit.poison;
        v.unknown541 = unit.slowed;
        v.unknown542 = static_cast<std::uint16_t>(unit.poisonTimer);
        v.experience = unit.experience;
        v.direction = unit.direction;
        s.values = v;
    }
    return s.serialize();
}

// Inventory: gold, each item's record, the worn items' script ids, the host window's weapon
// picks and the trap count. Fails outside solo and quest games, or for a quest player carrying
// too much (questInventoryAllowed).
std::optional<std::vector<std::uint8_t>> writeInventory(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    const PlayerControl& p = *unit.player;
    player::Inventory inv;
    inv.version = 3;
    if (hasBook(world)) {
        if (!solo(world) && !quest(world)) {
            return std::nullopt;
        }
        player::Inventory::Contents c;
        c.gold = p.gold;
        // Its things, head first (a solo host's own: its inventory window's order when it shows
        // them all; none on a server); a quest's but its keys and glyphs.
        std::vector<const ServerObject*> held;
        for (const std::uint32_t e : unit.inventory) {
            const ServerObject* o = world.byExtent(e);
            if (o && !o->pendingDelete && !o->gone()) {
                held.push_back(o);
            }
        }
        // (Obj_FindInventoryItemByNetCode: one of its things by net code.)
        const auto byNetCode = [&](std::uint16_t nc) -> const ServerObject* {
            for (const ServerObject* o : held) {
                if (netCodeOf(*o) == nc) {
                    return o;
                }
            }
            return nullptr;
        };
        const HostClient::InventoryWindow window = world.hostClient ? world.hostClient->inventoryWindow() : HostClient::InventoryWindow{};
        std::size_t count = 0;
        // Count what will be written, to see whether the host's window lists every one of them.
        for (const ServerObject* o : held) {
            if (!(quest(world) && ((o->classes & oc::Key) || o->typeName == "Glyph"))) {
                ++count;
            }
        }
        // (noxcpp: the host's own window is the host's things; a solo game's guest's are listed as
        // held. In a solo game alone the one player is the host: as the exe.)
        if (solo(world) && window.cells.size() == count && unit.player && (unit.player->index & 31u) == 0x1F) {
            // Every one in the host's window: in its cells' order. One not held
            // fails the section.
            for (const std::uint16_t nc : window.cells) {
                const ServerObject* o = byNetCode(nc);
                if (!o) {
                    return std::nullopt;
                }
                c.items.push_back({o->typeName, itemRecord(fight, *o)});
            }
        } else {
            for (const ServerObject* o : held) {
                if (quest(world) && ((o->classes & oc::Key) || o->typeName == "Glyph")) {
                    continue;
                }
                c.items.push_back({o->typeName, itemRecord(fight, *o)});
            }
        }
        for (const ServerObject* o : held) {
            if (o->flags & ObjFlag::Equipped) {
                c.equipped.push_back(static_cast<std::uint32_t>(o->scriptId));
            }
        }
        // The host's window's secondary weapon and last quiver, by its net codes, as the script
        // ids of its things (in any game; none found: 0).
        const auto scriptIdOf = [&](std::uint16_t nc) -> std::uint32_t {
            const ServerObject* o = nc != 0 ? byNetCode(nc) : nullptr;
            return o ? static_cast<std::uint32_t>(o->scriptId) : 0u;
        };
        c.secondaryWeapon = scriptIdOf(window.secondaryWeapon);
        c.lastQuiver = scriptIdOf(window.lastQuiver);
        c.trapCount = p.curTraps; // (CurTraps)
        inv.contents = std::move(c);
    }
    if (quest(world) && !questInventoryAllowed(world, unit)) {
        return std::nullopt;
    }
    tell(world, unit, {0x71}); // (netMsgInventoryLoaded, as the exe on either way)
    return inv.serialize();
}

// FieldGuide: the names of the creatures it knows.
std::optional<std::vector<std::uint8_t>> writeFieldGuide(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    player::FieldGuide g;
    g.version = 1;
    if (hasBook(world)) {
        if (!solo(world) && !quest(world)) {
            return std::nullopt;
        }
        std::vector<std::string> names;
        for (int i = 1; i <= 0x28; ++i) {
            if (unit.player->fieldGuide[static_cast<std::size_t>(i)]) {
                names.emplace_back(things::kGuideNames[static_cast<std::size_t>(i - 1)]);
            }
        }
        g.creatures = std::move(names);
    }
    return g.serialize();
}

// Spellbook: each known spell's name and level; a warrior's abilities instead, by name.
std::optional<std::vector<std::uint8_t>> writeSpellbook(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    const PlayerControl& p = *unit.player;
    player::Spellbook s;
    s.version = 3;
    if (hasBook(world)) {
        if (!solo(world) && !quest(world)) {
            return std::nullopt;
        }
        std::vector<player::Spellbook::Entry> entries;
        const int last = p.classId == 0 ? 5 : 0x88;  // a warrior: abilities 1-5; others: spells 1-0x88
        for (int i = 1; i <= last; ++i) {
            const std::uint8_t level = p.spellLevels[static_cast<std::size_t>(i)];
            if (level != 0) {
                entries.push_back({std::string(p.classId == 0 ? abilityName(i) : things::spellName(static_cast<std::uint32_t>(i))), level});
            }
        }
        s.entries = std::move(entries);
    }
    return s.serialize();
}

// Enchantment: each enchantment on it (kEnchantOrder) with frames left and power, then a
// warrior's ability state. Solo games only (empty in a network game).
std::optional<std::vector<std::uint8_t>> writeEnchantment(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    const PlayerControl& p = *unit.player;
    player::Enchantment e;
    e.version = 5;
    if (!network(world)) {
        if (!solo(world)) {
            return std::nullopt;
        }
        std::vector<player::Enchantment::Buff> buffs;
        for (const int b : kEnchantOrder) {
            if (!enchanted(unit, b)) {
                continue;
            }
            player::Enchantment::Buff buff;
            buff.name = std::string(kEnchantNames[b]);
            buff.framesLeft = unit.buffTime[static_cast<std::size_t>(b)];
            buff.power = unit.buffPower[static_cast<std::size_t>(b)];
            if (b == Enchant::Shield) {
                const DurationSpell* s = shieldSpell(world, unit);
                buff.shieldHealth = s ? static_cast<std::uint32_t>(s->value) : 100u;
            }
            buffs.push_back(std::move(buff));
        }
        e.buffs = std::move(buffs);
        if (p.classId == 0) {
            // A warrior's: Berserker and Tread Lightly under way (playerIsAbilityActive),
            // the latter's frames left (-1: none), the cool-downs (from 2 while
            // charging).
            player::Enchantment::Abilities a;
            a.berserkActive = abilityActive(world, unit, 1) ? 1 : 0;
            a.treadLightlyActive = abilityActive(world, unit, 4) ? 1 : 0;
            const ActiveAbility* tread = activeAbility(world, unit, 4);
            a.treadLightlyTimer = tread ? tread->end - fight.frame : 0xFFFFFFFFu;
            for (int i = a.berserkActive == 1 ? 2 : 1; i < 6; ++i) {
                a.cooldowns.push_back(static_cast<std::uint32_t>(p.abilityCooldown[static_cast<std::size_t>(i)]));
            }
            e.abilities = std::move(a);
        }
    }
    return e.serialize();
}

// Journal: the entries, oldest first. Solo games only (empty in a network game).
std::optional<std::vector<std::uint8_t>> writeJournal(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    player::Journal j;
    j.version = 1;
    if (!network(world)) {
        if (!solo(world)) {
            return std::nullopt;
        }
        std::vector<player::Journal::Entry> entries;
        for (auto it = unit.player->journal.rbegin(); it != unit.player->journal.rend(); ++it) { // (oldest first)
            entries.push_back({it->name, it->type});
        }
        j.entries = std::move(entries);
    }
    return j.serialize();
}

// GameData: the next script id, the map's name, quest variables, magic walls and the
// briefing byte; nothing at all in a network game.
std::optional<std::vector<std::uint8_t>> writeGameData(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    if (network(world)) {
        return std::vector<std::uint8_t>{}; // (nothing at all)
    }
    PlayerControl& p = *unit.player;
    player::GameData g;
    g.version = 5;
    g.lastObjectScriptId = static_cast<std::uint32_t>(world.nextScriptId());
    // The map's name into savedMapName (strcpy), then u16 its length and twice that many bytes
    // from there (what follows the name in the 32 bytes; past them, ours zeros).
    const std::string& name = world.mapName;
    const std::size_t copied = std::min<std::size_t>(name.size() + 1, p.savedMapName.size());
    std::copy_n(name.begin(), std::min(name.size(), copied), p.savedMapName.begin());
    if (name.size() < p.savedMapName.size()) {
        p.savedMapName[name.size()] = '\0';
    }
    const std::size_t len = strnlen(p.savedMapName.data(), p.savedMapName.size());
    for (std::size_t i = 0; i < 2 * len; ++i) {
        g.mapNameBytes.push_back(i < p.savedMapName.size() ? static_cast<std::uint8_t>(p.savedMapName[i]) : 0);
    }
    // plr_writeQuestVars: a solo game's list, newest first (else none).
    g.questVariablesVersion = 1;
    if (solo(world)) {
        for (const World::QuestVariable& q : world.questVariables) {
            g.questVariables.push_back({q.name, q.type, q.value});
        }
    }
    // plr_rwMagicWalls: the Wall spell's walls, newest first: whether it changed a
    // wall, where (its x / y), what was there (material, shape, look), the wall's material,
    // look, shape and health.
    g.magicWallsVersion = 1;
    for (auto it = world.magicWalls().rbegin(); it != world.magicWalls().rend(); ++it) {
        player::GameData::MagicWall m;
        m.unknown = it->replaced ? 1u : 0u;
        const WallCell* cell = world.wall(it->x, it->y);
        m.fields = {static_cast<std::uint8_t>(it->x),
                    static_cast<std::uint8_t>(it->y),
                    it->oldMaterial,
                    it->oldDirection,
                    it->oldVariant,
                    cell ? cell->material : std::uint8_t{0},
                    cell ? cell->variant : std::uint8_t{0},
                    cell ? cell->direction : std::uint8_t{0},
                    cell ? cell->health : std::uint8_t{0}};
        g.magicWalls.push_back(m);
    }
    g.questStage = world.briefingByte;
    return g.serialize();
}

std::optional<std::vector<std::uint8_t>> writePad(Fight&, ServerObject&)
{
    // Its version, then zeros to the 8-byte block (the section's data starts on one).
    return player::Pad{1, 6}.serialize();
}

// ---------------------------------------------------------------------------------------------
// Reading (the read / write mode 1)

// What the section readers share during one load.
struct Load {
    Fight& fight;
    ServerObject& unit;
    bool* disconnect;
    std::uint16_t health = 0; ///< what its health is to be at the end
    std::uint16_t mana = 0;   ///< and its mana
    bool playerOnly = false;  ///< (noxcpp: a solo game's guest: what it touches its own alone)
};

/// Its level (a network game's 10; else by its experience, XPTable) and its
/// numbers anew (plrReadVals, untold).
void levelByExperience(World& world, ServerObject& unit)
{
    PlayerControl& p = *unit.player;
    if (network(world)) {
        p.level = 10;
    } else {
        int level = 0;
        // Count the XPTable entries its experience has reached (at most 11); the level is one less.
        while (level < 0xB && !(unit.experience < world.number("XPTable", static_cast<std::size_t>(level)))) {
            ++level;
        }
        p.level = static_cast<std::uint8_t>(level - 1);
    }
    readVals(world, unit, false);
}

/// spellAwardAll1 / All2 / All3: its field guide, spells and (a
/// warrior's) abilities emptied, or (engine flag 0x10, god mode) all known; a quest's first
/// spells given (Spell_GrantToPlayer, told).
void resetGuide(World& world, ServerObject& unit)
{
    auto& g = unit.player->fieldGuide;
    for (std::size_t i = 1; i < 0x29; ++i) {
        g[i] = world.godMode;
    }
}

void resetSpells(Fight& fight, ServerObject& unit, bool questStart = true)
{
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    for (std::size_t i = 1; i < 0x89; ++i) {
        p.spellLevels[i] = world.godMode ? 3 : 0;
    }
    if (questStart && !world.godMode && quest(world) && fight.things) {
        if (p.classId == 1) {
            grantSpell(world, *fight.things, unit, 0x1B, true, true, 1);
        } else if (p.classId == 2) {
            grantSpell(world, *fight.things, unit, 9, true, true, 1);
            grantSpell(world, *fight.things, unit, 0x29, true, true, 1);
        }
    }
}

void resetAbilities(World& world, ServerObject& unit)
{
    PlayerControl& p = *unit.player;
    if (p.classId == 0) {
        for (std::size_t i = 1; i < 6; ++i) {
            p.spellLevels[i] = world.godMode ? 5 : 0;
        }
    }
}

/// A player made anew for its .plr. (`questStart` false: noxcpp's co-op guest bringing a character
/// no solo game saved: a new solo character's book, not a quest's first spells, whatever its
/// client is told.)
void resetForLoad(Fight& fight, ServerObject& unit, bool questStart = true)
{
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    resetGuide(world, unit);
    resetSpells(fight, unit, questStart);
    p.level = 1; // (Player->Level)
    cancelAbilities(world, unit); // (playerCancelAbils)
    readVals(world, unit, false);
    resetAbilities(world, unit);
    p.mana = p.maxMana;
    p.trapSpells = {}; // (TrapSpells)
    p.trapCount = 0;
    unit.health = unit.maxHealth; // (unitHPsetOnMax)
    unit.slowed = 0;
    unit.flags &= 0xFFEB3FE7u;
    setPlayerState(world, unit, 0xD, fight.frame);
    clearEnchantments(unit); // (unitClearBuffs)
    for (DurationSpell& s : world.durationSpells()) { // (playerCancelSpells)
        if (s.caster == unit.extent && !s.cancelled) {
            cancelDurationSpell(world, s);
        }
    }
    fight.removePoison(unit);
    // (Its custom waypoints: not kept here.) Its whole health and mana told it
    // (SrvNet_SendReportTotalHealth, netReportTotalMana: the host's report).
    p.healthSent = -1;
    p.manaSent = -1;
    unit.hurtBy = 0;
    p.polygon = kPolygonUnknown;
    unit.touch();
}

/// A section's reader failed: the exe's returns 0.
struct Fail {};

// A reader's "return 0": unwinds to applyPlayerFile, which fails the load.
void require(bool ok)
{
    if (!ok) {
        throw Fail{};
    }
}

// A string with its u8 length before it.
std::string readS8(ByteReader& r)
{
    return r.rawString(r.u8());
}

// Attrib: the reverse of writeAttrib, also filling the info block (the name as UTF-16 at 0,
// speed at 0x32, strength 0x36, max mana and health, class 0x42, then sex, colours, clothing).
void readAttrib(Load& L, ByteReader& r)
{
    World& world = L.fight.world;
    PlayerControl& p = *L.unit.player;
    const std::uint16_t version = r.u16();
    require(version <= 5);
    if (4 < version) {  // version 5 on: the game mode the file was saved in
        // The game the file is of must be a quest's in a quest, and not one elsewhere: else the
        // player is dropped (Plr_DisconnectByIndex, 1). (The host's own, index 0x1F: none here.)
        const std::uint32_t mode = r.u32();
        if (quest(world) ? mode != 4 : mode == 4) {
            if (L.disconnect) {
                *L.disconnect = true;
            }
            throw Fail{};
        }
    }
    const std::uint8_t len = r.u8();
    require(len <= 0x18);  // at most 24 UTF-16 letters
    std::u16string name(len, u'\0');
    for (auto& c : name) {
        c = static_cast<char16_t>(r.u16());
    }
    p.name = name; // (the info's, and Player->name)
    std::fill_n(p.info.begin() + kName, 50, std::uint8_t{0});
    for (std::size_t i = 0; i < name.size(); ++i) {
        p.info[kName + 2 * i] = lo(name[i]);
        p.info[kName + 2 * i + 1] = hi(name[i]);
    }
    p.speed = r.u32();
    setInfoU32(p, 0x32, p.speed);
    p.strength = r.u32();
    setInfoU32(p, 0x36, p.strength);
    setInfoU32(p, kMaxMana, r.u32());
    setInfoU32(p, kMaxHealth, r.u32());
    p.classId = r.u8(); // (info + 0x42 is Player->playerClass)
    p.info[0x42] = p.classId;
    p.info[kFemale] = r.u8();
    for (std::size_t i = 0; i < 15; ++i) {
        p.info[kColors + i] = r.u8();
    }
    if (1 < version) {  // version 2 on: the 5 clothing bytes
        for (std::size_t i = 0; i < 5; ++i) {
            p.clothing[i] = r.u8();
            p.info[kClothing + i] = p.clothing[i];
        }
    }
    // (Plr_InitColors: the host's drawing of it.)
    p.info[kInfo58] = r.u8();
    if (2 < version) {  // version 3 on: extra lives (at most MaxExtraLives)
        p.questLives = r.u32();
        require(!(chop(world.number("MaxExtraLives")) < static_cast<std::int32_t>(p.questLives)));
        if (version == 3) {  // a version 3 file has 36 more bytes here, skipped
            r.skip(9 * 4);
        }
    }
    // Its quest tallies anew.
    p.questTimeUps = p.questTally1230 = p.questDeaths = 0;
    p.questKills = p.questGenerators = p.questSecrets = 0;
    p.questSecretsMap = p.questSecretsCredited = p.questTally124C = 0;
    p.questJoinStage = static_cast<std::uint32_t>(world.questStage);
    p.questFlags = 0x3F;
    if (3 < version) {  // version 4 on: the highest quest stage reached
        p.questHighestStage = r.u32();
        // SrvNet_SendGauntletHighestStage: MSG_GAUNTLET 0x1D {u16 stage}.
        tell(world, L.unit, {0xF0, 0x1D, lo(p.questHighestStage), hi(p.questHighestStage)});
    }
}

// Status: its numbers from the file; the health and mana it is to end with wait in L.
void readStatus(Load& L, ByteReader& r)
{
    World& world = L.fight.world;
    ServerObject& unit = L.unit;
    PlayerControl& p = *unit.player;
    const std::uint16_t version = r.u16();
    require(version <= 2);
    if (r.u8() == 0) {
        return;
    }
    require(solo(world));
    unit.maxHealth = unit.health = r.u16(); // (unitSetMaxHP, unitSetHP)
    p.maxMana = p.mana = r.u16();           // (playerSetMaxMana, playerManaRefresh)
    L.health = r.u16();
    L.mana = r.u16();
    L.fight.setPoison(unit, r.u8()); // (setSomePoisonData)
    unit.slowed = r.u8();
    unit.poisonTimer = static_cast<std::int16_t>(r.u16());
    unit.experience = r.f32();
    // SrvNet_SendReportExperience: MSG_REPORT_EXPERIENCE, truncated.
    const auto xp = static_cast<std::uint32_t>(static_cast<std::int32_t>(unit.experience));
    tell(world, unit, {0x6E, lo(xp), hi(xp), static_cast<std::uint8_t>(xp >> 16), static_cast<std::uint8_t>(xp >> 24)});
    if (1 < version) {
        unit.direction = r.u16(); // (Direction1, Direction2 with it)
    }
    unit.touch();
}

// Inventory: what it carries goes, then each item in the file is made, placed and picked up;
// the worn ones are worn again and its client told the weapon picks.
void readInventory(Load& L, ByteReader& r, std::span<const std::uint8_t> section)
{
    Fight& fight = L.fight;
    World& world = fight.world;
    ServerObject& unit = L.unit;
    PlayerControl& p = *unit.player;
    Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    levelByExperience(world, unit);
    const std::uint16_t version = r.u16();
    require(version <= 3);
    if (r.u8() != 0) {
        require(solo(world) || quest(world));
        // Its gold the file's (playerSubGold all, playerAddGold).
        const std::uint32_t gold = r.u32();
        subGold(unit, p.gold);
        addGold(unit, static_cast<std::int32_t>(gold));
        // What it carried goes (delayedDeleteObject: out of its inventory).
        for (const std::uint32_t e : keysOf(unit.inventory)) {
            if (ServerObject* o = world.byExtent(e)) {
                items.removeFromInventory(unit, *o);
                o->flags |= ObjFlag::Destroyed;
                o->pendingDelete = true;
            }
        }
        const std::uint32_t count = r.u32();
        require(!(quest(world) && 0xA00 < count));
        for (std::uint32_t i = 0; i < count; ++i) {
            // newObjectByTypeID, then its type's xfer reading it.
            const std::string type = readS8(r);
            const things::ThingDef* t = fight.things->findThing(type);
            require(t != nullptr);
            const std::string xferName = xferNameOf(t);
            const map::xfer::TypeInfo info{things::thingClasses(*t), things::thingSubclasses(*t)};
            const std::size_t start = r.offset();
            const std::uint16_t xferVersion = r.u16();
            const map::ObjectHeader header = map::readObjectHeader(r);
            require(header.inventoryCount() == 0); // (nested items: not here)
            map::xfer::read(r, xferName, xferVersion, info, r.offset() - start);
            const map::ObjectXfer x = map::ObjectXfer::parse(section.subspan(start, r.offset() - start));
            // (Its xfer's own fields, the charges, health and entries, read with it: World's
            // readItemXfer; its script ids taken there, as the exe's object read and mapReadWriteObjData do.)
            ServerObject o = objectFromRecord(world, x, type, *fight.things, fight.modifiers);
            o.touch();
            // At (2944, 2944) (PosVec), checked in a quest, put in the world
            // (servMapLoadPlaceObj -> createAt: not a type the
            // host turned off), then picked up (Obj_TryPickup).
            require(!quest(world) || questItemValid(o, fight.modifiers));
            ServerObject* item = nullptr;
            if (!world.disabledTypes.contains(o.typeName)) {
                o.extent = world.newExtent();
                clearLinks(o); // (a thing of its own: held by nothing, holding nothing, naming nothing)
                o.x = o.frameStartX = o.newX = o.lastX = o.homeX = 2944.0f;
                o.y = o.frameStartY = o.newY = o.lastY = o.homeY = 2944.0f;
                o.flags = (o.flags & 0x35E9FEDBu) | ObjFlag::Active;
                o.born = o.timer = fight.frame;
                // The class mask: food, books, wands, weapons and armour.
                if (network(world) && !quest(world) && (o.typeName == "Gold" || (o.classes & 0x3001110u)) && !(o.classes & oc::Missile)) {
                    o.flags |= ObjFlag::NoCollide;
                }
                const std::uint32_t extent = o.extent;
                world.adopt(std::move(o));
                item = world.byExtent(extent);
            }
            if (!item || !items.pickUp(unit, *item, true)) {
                require(quest(world));
                if (item) {
                    item->flags |= ObjFlag::Destroyed;
                    item->pendingDelete = true;
                }
            }
            if (item && !(item->flags & ObjFlag::Destroyed) && (item->flags & ObjFlag::Equipped)) {
                items.tryDequip(unit, *item); // (Plr_TryDequip: the file's EQUIPPED, undone)
            }
        }
        // The worn ones worn again (Plr_TryEquip), by script id.
        const std::uint8_t worn = r.u8();
        for (std::uint8_t i = 0; i < worn; ++i) {
            const std::int32_t id = r.i32();
            for (const std::uint32_t e : keysOf(unit.inventory)) {
                if (ServerObject* o = world.byExtent(e); o && o->scriptId == id) {
                    items.tryEquip(unit, *o);
                }
            }
        }
        // A solo game's: the host's inventory window's secondary weapon and quiver cleared
        // (Gui_Inv_SetSecondaryWeapon(0, 0), Gui_Inv_SetLastQuiver(0)). Its client told the file's
        // (MSG_REPORT_SECONDARY_WEAPON 0xE0, netMsgLastQuiver 0xE1).
        if (solo(world) && world.hostClient) {
            world.hostClient->clearInventoryWeapons();
        }
        const auto findById = [&](std::int32_t id) -> ServerObject* {
            for (const std::uint32_t e : unit.inventory) {
                if (ServerObject* o = world.byExtent(e); o && o->scriptId == id) {
                    return o;
                }
            }
            return nullptr;
        };
        if (const std::int32_t id = r.i32(); id != 0) {
            if (const ServerObject* o = findById(id)) {
                const std::uint16_t code = netCodeOf(*o);
                tell(world, unit, {0xE0, lo(code), hi(code), 0});
            }
        }
        if (1 < version) {
            if (const std::int32_t id = r.i32(); id != 0) {
                if (const ServerObject* o = findById(id)) {
                    const std::uint16_t code = netCodeOf(*o);
                    tell(world, unit, {0xE1, lo(code), hi(code)});
                }
            }
        }
        // A quest's things numbered afresh (and their Extent their net code: ours already is).
        if (quest(world)) {
            for (const std::uint32_t e : unit.inventory) {
                if (ServerObject* o = world.byExtent(e)) {
                    o->scriptId = world.newScriptId();
                }
            }
        }
        p.curTraps = version < 3 ? 0 : r.u8(); // (CurTraps)
        if (quest(world)) {
            p.curTraps = 0;
        }
    }
    require(!quest(world) || questInventoryAllowed(world, unit));
    tell(world, unit, {0x71}); // (netMsgInventoryLoaded)
}

// FieldGuide: each named creature awarded, untold.
void readFieldGuide(Load& L, ByteReader& r)
{
    World& world = L.fight.world;
    const std::uint16_t version = r.u16();
    require(version <= 1);
    if (r.u8() == 0) {
        return;
    }
    require(solo(world) || quest(world));
    const std::uint8_t count = r.u8();
    require(count <= 0x29);
    for (std::uint8_t i = 0; i < count; ++i) {
        const int creature = guideByName(readS8(r));
        require(!quest(world) || questGuideValid(creature));
        awardFieldGuide(world, L.unit, creature, false);
    }
}

// Spellbook: each spell granted at its level (3 before version 2); a warrior's entries (from
// version 3) are abilities.
void readSpellbook(Load& L, ByteReader& r)
{
    World& world = L.fight.world;
    ServerObject& unit = L.unit;
    const PlayerControl& p = *unit.player;
    const std::uint16_t version = r.u16();
    require(version <= 3);
    if (r.u8() == 0) {
        return;
    }
    require(solo(world) || quest(world));
    const std::uint8_t count = r.u8();
    require(count <= 0x89);
    for (std::uint8_t i = 0; i < count; ++i) {
        const std::string name = readS8(r);
        std::int32_t level = 3;
        if (1 < version) {
            level = r.i32();
        }
        const int spell = static_cast<int>(things::spellId(name).value_or(0)); // (spellNameToN)
        require(!(quest(world) && 3 < level && p.classId != 0));
        require(!(quest(world) && p.classId != 0 && !questSpellValid(spell)));
        if (version < 3 || p.classId != 0) {
            grantSpell(world, *L.fight.things, unit, spell, false, false, level); // (Spell_GrantToPlayer)
        } else {
            grantPlayerAbility(world, unit, abilityByName(name), false); // (Abil_GrantToPlayer)
        }
    }
}

void readAbilityState(Load& L, ByteReader& r)
{
    // A warrior's (its class 0): Berserker charging (then run
    // again by the solo frame), Tread Lightly under way (Abil_TryExecute, its end the file's
    // frames on), the cool-downs (one not over told: 0xCE not ready).
    Fight& fight = L.fight;
    World& world = fight.world;
    ServerObject& unit = L.unit;
    PlayerControl& p = *unit.player;
    const std::uint8_t berserk = r.u8();
    if (berserk == 1) {
        world.pendingAbility = 1;
    }
    const std::uint8_t tread = r.u8();
    const std::uint32_t timer = r.u32();
    if (tread == 1 && fight.things) {
        Abilities{fight, *fight.things}.tryExecute(unit, 4);
        if (ActiveAbility* a = activeAbility(world, unit, 4)) {
            a->end = fight.frame + timer;
        }
    }
    for (int i = berserk == 1 ? 2 : 1; i < 6; ++i) {
        const std::uint32_t v = r.u32();
        p.abilityCooldown[static_cast<std::size_t>(i)] = static_cast<int>(v);
        if (v != 0) {
            tell(world, unit, {0xCE, static_cast<std::uint8_t>(i), 0}); // (SrvNet_SendReportAbilityState)
        }
    }
}

// Enchantment: each saved enchantment's spell cast on it again, then its frames left restored.
void readEnchantment(Load& L, ByteReader& r)
{
    Fight& fight = L.fight;
    World& world = fight.world;
    ServerObject& unit = L.unit;
    const PlayerControl& p = *unit.player;
    const std::uint16_t version = r.u16();
    require(version <= 5);
    Spells spells{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    if (solo(world) && L.playerOnly) {
        // (noxcpp: a solo game's guest: its own lasting spells forgotten, freed not ended; the
        // world's others go on.)
        std::erase_if(world.durationSpells(), [&](const DurationSpell& s) { return s.caster == unit.extent; });
    } else if (solo(world)) {
        // The lasting spells' pass (spellCastByPlayer), then every one forgotten
        // (the lasting spells cleared with 0: freed, not ended).
        spells.updateLasting();
        world.durationSpells().clear();
    }
    if (r.u8() != 0) {
        require(solo(world));
        const std::uint8_t count = r.u8();
        for (std::uint8_t i = 0; i < count; ++i) {
            const std::string name = readS8(r);
            // enchantByName (32 when unknown: its frames go past the table,
            // an original bug; ours drop them).
            int e = 0;
            while (e < 32 && kEnchantNames[e] != name) {
                ++e;
            }
            std::uint32_t frames = r.u16();
            const int power = 1 < version ? r.u8() : 2;
            // Its spell cast on itself (spellAccept with getEnchantSpell's).
            if (e < 32) {
                if (const std::uint32_t spell = enchantmentSpell(e); spell != 0) {
                    spells.cast(spell, unit, &unit, unit.x, unit.y, power);
                }
            }
            // A saved 0 frames left becomes a second.
            if (static_cast<std::uint16_t>(frames) == 0) {
                frames = kGameFps;
            }
            if (e < 32) {
                unit.buffTime[static_cast<std::size_t>(e)] = static_cast<std::uint16_t>(frames);
            }
            if (e == Enchant::Shield && 2 < version) {
                const std::uint32_t strength = r.u32();
                if (DurationSpell* s = shieldSpell(world, unit)) {
                    s->value = static_cast<int>(strength);
                }
            }
        }
        if (4 < version && p.classId == 0) {
            readAbilityState(L, r);
        }
    }
    // A version 4 file has the warrior's ability state even without the "has" byte set.
    if (version == 4 && p.classId == 0) {
        readAbilityState(L, r);
    }
}

// Journal: the file's entries replace its journal.
void readJournal(Load& L, ByteReader& r)
{
    World& world = L.fight.world;
    const std::uint16_t version = r.u16();
    require(version <= 1);
    if (r.u8() == 0) {
        return;
    }
    require(solo(world));
    const std::uint16_t count = r.u16();
    removeJournalEntries(world, L.unit, 0xFFFF);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::string name = readS8(r); // (into 64 bytes: a longer one runs over, an original bug)
        const std::uint16_t type = r.u16();
        addJournalEntry(world, L.unit, name, type);
    }
}

// GameData: the map's name, the quest variables and the magic walls to raise again.
void readGameData(Load& L, ByteReader& r)
{
    World& world = L.fight.world;
    PlayerControl& p = *L.unit.player;
    if (network(world)) {
        return; // (nothing read)
    }
    const std::uint16_t version = r.u16();
    require(version <= 5);
    if (4 < version) {
        r.u32(); // (read and dropped)
    }
    // The map's name into savedMapName (twice its length read: past the 32 bytes the exe runs
    // into the next fields, an original bug; ours drops them), ended at its length.
    const std::uint16_t len = r.u16();
    const auto bytes = r.bytes(2 * std::size_t{len});
    for (std::size_t i = 0; i < bytes.size() && i < p.savedMapName.size(); ++i) {
        p.savedMapName[i] = static_cast<char>(bytes[i]);
    }
    if (len < p.savedMapName.size()) {
        p.savedMapName[len] = '\0';
    }
    if (1 < version) {
        // plr_readQuestVars: every quest variable gone ("*:*"), then the
        // file's (a type but 0 or 1 has no value).
        world.questVariables.clear();
        require(r.u16() < 2);
        const std::uint32_t n = r.u32();
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::string name = readS8(r);
            const std::uint32_t type = r.u32();
            if (type == 0 || type == 1) {
                setQuestVariable(world, name, type, r.u32());
            }
        }
    }
    if (2 < version) {
        // plr_rwMagicWalls: the walls kept for the map (15 at most).
        // Some: begins a Wall for the player with no functions (a lasting spell's start (0x84,
        // it, it, it, {it, its cursor}, 3, none): its sound, no end), the list emptied first;
        // They're raised on it in the tick (Server::tickEnd, restoreMagicWalls).
        const std::uint16_t wallsVersion = r.u16();
        require(wallsVersion < 2 && 0 < wallsVersion);
        const std::uint8_t n = r.u8();
        if (n != 0) {
            const float cx = L.unit.player ? static_cast<float>(static_cast<int>(L.unit.player->cursorX)) : 0.0f;
            const float cy = L.unit.player ? static_cast<float>(static_cast<int>(L.unit.player->cursorY)) : 0.0f;
            if (L.fight.things) {
                Spells{world, *L.fight.things, L.fight.modifiers, L.fight.rng, L.fight.frame}.startLasting(0x84, &L.unit, &L.unit, &L.unit, &L.unit,
                                                                                                         cx, cy, 3, 0, true);
            }
            world.pendingMagicWalls.clear();
        }
        for (std::uint8_t i = 0; i < n; ++i) {
            World::PendingWall w;
            w.replaced = r.u32();
            w.x = r.u8();
            w.y = r.u8();
            w.oldMaterial = r.u8();
            w.oldDirection = r.u8();
            w.oldVariant = r.u8();
            w.material = r.u8();
            w.variant = r.u8();
            w.direction = r.u8();
            w.health = r.u8();
            if (world.pendingMagicWalls.size() < 15) {
                world.pendingMagicWalls.push_back(w);
            }
        }
    }
    world.briefingByte = 3 < version ? r.u8() : 0;
}

void readPad(Load&, ByteReader& r)
{
    require(r.u16() <= 1); // (then its zeros)
}

/// The end of the transfer (the player's transfer flag 0): the MSG_INFORM strings to it made meanwhile
/// were never sent (SrvNet_SendInformString's).
void endTransfer(World& world, ServerObject& unit, std::size_t mark)
{
    unit.player->transfer = false;
    auto& b = world.broadcasts();
    const auto first = b.begin() + static_cast<std::ptrdiff_t>(std::min(mark, b.size()));
    b.erase(std::remove_if(first, b.end(),
                           [&](const World::Broadcast& m) { return m.to == unit.extent && 2 <= m.bytes.size() && m.bytes[0] == 0xA9 && m.bytes[1] == 0x0F; }),
            b.end());
}

} // namespace

void relevel(World& world, ServerObject& player)
{
    if (player.player) {
        levelByExperience(world, player);
    }
}

void setGod(Fight& fight, bool on)
{
    World& world = fight.world;
    if (!(world.gameFlags & GameFlag::Solo)) {
        return;
    }
    world.godMode = on; // (engine flags 0x30)
    // (Plr_GetFirst's order: by index)
    for (ServerObject* o : playersInOrder(world, [](const ServerObject& u) { return !u.gone(); })) {
        resetGuide(world, *o);
        resetSpells(fight, *o);
        resetAbilities(world, *o);
    }
}

bool questInventoryAllowed(const World& world, const ServerObject& player)
{
    if (!(player.classes & oc::Player)) {
        return true;
    }
    const auto count = [&](std::string_view type) {
        return std::ranges::count_if(player.inventory, [&](std::uint32_t e) {
            const auto it = world.objects().find(e);
            return it != world.objects().end() && it->second.typeName == type && !(it->second.flags & ObjFlag::Destroyed);
        });
    };
    static constexpr std::string_view kPotions[] = {"RedPotion",          "BluePotion",       "CurePoisonPotion",   "HastePotion",
                                                    "InvisibilityPotion", "ShieldPotion",     "VampirismPotion",    "FireProtectPotion",
                                                    "ShockProtectPotion", "PoisonProtectPotion", "InvulnerabilityPotion", "InfravisionPotion"};
    for (const std::string_view potion : kPotions) {
        if (10 <= count(potion)) {
            return false;
        }
    }
    return count("InfinitePainWand") <= chop(world.number("ForceOfNatureStaffLimit"));
}

std::optional<player::PlayerFile> buildPlayerFile(Fight& fight, ServerObject& player)
{
    if (!player.player || !fight.things) {
        return std::nullopt; // ("SaveServerPlayerData: NULL player")
    }
    using Writer = std::optional<std::vector<std::uint8_t>> (*)(Fight&, ServerObject&);
    struct Entry {
        player::SectionId id;
        Writer write;
    };
    // The server section table (g_plrServerSections), in its order.
    static constexpr Entry kSections[] = {
        {player::SectionId::Attrib, writeAttrib},           {player::SectionId::Status, writeStatus},
        {player::SectionId::Inventory, writeInventory},     {player::SectionId::FieldGuide, writeFieldGuide},
        {player::SectionId::Spellbook, writeSpellbook},     {player::SectionId::Enchantment, writeEnchantment},
        {player::SectionId::Journal, writeJournal},         {player::SectionId::GameData, writeGameData},
        {player::SectionId::Pad, writePad},
    };
    player::PlayerFile file;
    for (const Entry& e : kSections) {
        auto data = e.write(fight, player);
        if (!data) {
            return std::nullopt; // ("SavePlayerData: Error saving player data '%s'")
        }
        file.sections.push_back({static_cast<std::uint32_t>(e.id), std::move(*data)});
    }
    return file;
}

std::optional<player::PlayerFile> buildQuestFile(Fight& fight, ServerObject& player)
{
    // (noxcpp) A character of another game written as a quest's (the world's flags already the
    // quest's: GameFlagsAs) with only what a quest's read takes: its things the quest's item check
    // passes (no keys, food but potions, nothing off the quest's tables), at most 9 of a potion and
    // the quest's Infinite Pain wands; a wizard's or conjurer's spells of the quest's table at most
    // at level 3; the guide's creatures of its table. The player itself as it was after.
    if (!player.player) {
        return std::nullopt;
    }
    World& world = fight.world;
    PlayerControl& p = *player.player;
    const std::vector<ObjectRef> inventory = player.inventory;
    const auto spells = p.spellLevels;
    const auto guide = p.fieldGuide;
    std::map<std::string, int> held;
    const int pains = chop(world.number("ForceOfNatureStaffLimit"));
    std::erase_if(player.inventory, [&](std::uint32_t e) {
        const ServerObject* o = world.byExtent(e);
        if (!o || (o->flags & ObjFlag::Destroyed) || !questItemValid(*o, fight.modifiers)) {
            return true;
        }
        const int n = ++held[o->typeName];
        return ((o->classes & oc::Food) && 9 < n) || (o->typeName == "InfinitePainWand" && pains < n);
    });
    if (p.classId != 0) {
        for (std::size_t s = 0; s < p.spellLevels.size(); ++s) {
            if (p.spellLevels[s] != 0 && !questSpellValid(static_cast<int>(s))) {
                p.spellLevels[s] = 0;
            }
            p.spellLevels[s] = std::min<std::uint8_t>(p.spellLevels[s], 3);
        }
    }
    for (std::size_t c = 0; c < p.fieldGuide.size(); ++c) {
        if (p.fieldGuide[c] && !questGuideValid(static_cast<int>(c))) {
            p.fieldGuide[c] = false;
        }
    }
    auto file = buildPlayerFile(fight, player);
    player.inventory = inventory;
    p.spellLevels = spells;
    p.fieldGuide = guide;
    return file;
}

std::vector<std::uint8_t> savePlayerData(Fight& fight, ServerObject& player)
{
    const auto file = buildPlayerFile(fight, player);
    if (!file) {
        return {};
    }
    auto bytes = player::serializePlayerFile(*file);
    crypto::cipherForKey(crypto::FileKey::PlayerFile).encrypt(bytes);
    return bytes;
}

namespace {

/// applyPlayerFile's work; `fresh` (noxcpp: a solo game's guest bringing a character no solo game
/// saved): the quest's first spells left out of the reset.
bool applyFile(Fight& fight, ServerObject& unit, const player::PlayerFile& file, bool* disconnect, bool playerOnly, bool fresh);

} // namespace

bool applyPlayerFile(Fight& fight, ServerObject& unit, const player::PlayerFile& file, bool* disconnect, bool playerOnly)
{
    return applyFile(fight, unit, file, disconnect, playerOnly, false);
}

namespace {

bool applyFile(Fight& fight, ServerObject& unit, const player::PlayerFile& file, bool* disconnect, bool playerOnly, bool fresh)
{
    if (disconnect) {
        *disconnect = false;
    }
    if (!unit.player || !fight.things) {
        return false;
    }
    World& world = fight.world;
    if (playerOnly) {
        // (noxcpp: a solo game's guest: its own book, guide and abilities anew, the world's god
        // mode and the others' as they are.)
        if (world.gameFlags & GameFlag::Solo) {
            resetGuide(world, unit);
            resetSpells(fight, unit);
            resetAbilities(world, unit);
        }
    } else {
        setGod(fight, false); // (a solo game's god mode off: every player's book anew)
    }
    Load L{fight, unit, disconnect};
    L.playerOnly = playerOnly;
    L.health = unit.health;      // (unitGetHP)
    L.mana = unit.player->mana;  // (unitGetOldMana)
    resetForLoad(fight, unit, !fresh);
    unit.player->transfer = true; // (the transfer flag 1)
    const std::size_t mark = world.broadcasts().size();
    try {
        for (const player::RawSection& s : file.sections) {
            ByteReader r(s.data, "plr section");
            switch (static_cast<player::SectionId>(s.id)) {
            case player::SectionId::Attrib: readAttrib(L, r); break;
            case player::SectionId::Status: readStatus(L, r); break;
            case player::SectionId::Inventory: readInventory(L, r, s.data); break;
            case player::SectionId::FieldGuide: readFieldGuide(L, r); break;
            case player::SectionId::Spellbook: readSpellbook(L, r); break;
            case player::SectionId::Enchantment: readEnchantment(L, r); break;
            case player::SectionId::Journal: readJournal(L, r); break;
            case player::SectionId::GameData: readGameData(L, r); break;
            case player::SectionId::Pad: readPad(L, r); break;
            default: break; // (skipped by its size)
            }
        }
    } catch (const Fail&) {
        // A section failed: what it carries goes (delayedDeleteObject).
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        for (const std::uint32_t e : keysOf(unit.inventory)) {
            if (ServerObject* o = world.byExtent(e)) {
                items.removeFromInventory(unit, *o);
                o->flags |= ObjFlag::Destroyed;
                o->pendingDelete = true;
            }
        }
        endTransfer(world, unit, mark);
        return false;
    } catch (const std::exception&) {
        // (A section shorter than its fields: the exe reads on into the next; here it fails.)
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        for (const std::uint32_t e : keysOf(unit.inventory)) {
            if (ServerObject* o = world.byExtent(e)) {
                items.removeFromInventory(unit, *o);
                o->flags |= ObjFlag::Destroyed;
                o->pendingDelete = true;
            }
        }
        endTransfer(world, unit, mark);
        return false;
    }
    // The end: its level and numbers anew, its health and mana down to the
    // file's (or what it had: unitDamageClear, playerManaSub),
    // its health noted (playerHP), out of transfer.
    levelByExperience(world, unit);
    fight.takeHealth(unit, static_cast<int>(unit.maxHealth) - static_cast<int>(L.health));
    PlayerControl& p = *unit.player;
    if (!world.godMode) {
        const int amount = static_cast<int>(p.maxMana) - static_cast<int>(L.mana);
        p.mana = amount < static_cast<int>(p.mana) ? static_cast<std::uint16_t>(p.mana - amount) : std::uint16_t{0};
    }
    p.hurtLookHealth = unit.health;
    endTransfer(world, unit, mark);
    return true;
}

} // namespace

std::vector<std::uint8_t> saveDataSent(const player::PlayerFile& file)
{
    std::vector<std::uint8_t> plain = player::serializePlayerFile(file);
    // computeServerPlayerDataBufferSize: the server sections (id, its padding, the
    // size, the data) one after another, and a zero id's 4 bytes when one follows them.
    static constexpr std::uint32_t kServer[] = {2, 3, 4, 8, 5, 6, 9, 10, 11};
    std::size_t pos = 0;
    std::size_t total = 0;
    while (pos + 4 <= plain.size()) {
        const std::uint32_t id = util::u32At(plain, pos);
        if (id == 0) {
            total += 4;
            break;
        }
        const std::size_t sizeAt = (pos + 4 + 7) / 8 * 8;  // the size sits at the next 8-byte boundary
        if (sizeAt + 8 > plain.size()) {
            break;
        }
        const std::uint32_t size = util::u32At(plain, sizeAt);
        const std::size_t end = sizeAt + 8 + size;
        if (std::ranges::find(kServer, id) != std::end(kServer)) {
            total += end - pos;
        }
        pos = end;
    }
    // SrvNet_SendSaveDataToClient keeps the count's low 16 bits (TEST SI,SI; AND ESI,0xFFFF: an
    // original bug, a save of 64 KiB or more goes cut short, one of a multiple of it not at all).
    total &= 0xFFFFu;
    plain.resize(std::min(total, plain.size()));
    return plain;
}

std::optional<player::PlayerFile> readLoadedSections(std::span<const std::uint8_t> plain)
{
    // cliPlrInfoLoadFromFile's walk: a u32 id (0: the end), then the size, read by
    // fileCryptReadCrcMB (binfile_fread_align: the rest of the 8-byte
    // block skipped, 8 bytes read, the low 4 kept), then the section's data.
    const auto u32At = [&](std::size_t at) { return util::u32At(plain, at); };
    player::PlayerFile file;
    std::size_t pos = 0;
    while (pos + 4 <= plain.size()) {
        const std::uint32_t id = u32At(pos);
        if (id == 0) {
            return file;
        }
        const std::size_t sizeAt = (pos + 4 + 7) / 8 * 8;
        if (sizeAt + 8 > plain.size()) {
            return std::nullopt;
        }
        const std::uint32_t size = u32At(sizeAt);
        if (size > plain.size() - (sizeAt + 8)) {
            return std::nullopt;
        }
        const auto data = plain.subspan(sizeAt + 8, size);
        file.sections.push_back({id, std::vector<std::uint8_t>(data.begin(), data.end())});
        pos = sizeAt + 8 + size;
    }
    return file;
}

namespace {

/// (noxcpp: a solo game's guest) the file less its GameData: the game's state is the host's. A
/// character no solo game saved (its GameData none or empty: a network game's character, or a
/// quest's, whose saves write none) also less its book and field guide: it comes in with a new
/// solo character's (nothing known; a warrior's abilities by its level as it gains them), as
/// the campaign is the solo game's. Returns whether it was one of those (`fresh`).
bool forGuest(player::PlayerFile& file)
{
    const auto id = [](const player::RawSection& s) { return static_cast<player::SectionId>(s.id); };
    const bool fresh = std::ranges::none_of(file.sections, [&](const player::RawSection& s) { return id(s) == player::SectionId::GameData && !s.data.empty(); });
    std::erase_if(file.sections, [&](const player::RawSection& s) {
        return id(s) == player::SectionId::GameData || (fresh && (id(s) == player::SectionId::Spellbook || id(s) == player::SectionId::FieldGuide));
    });
    return fresh;
}

} // namespace

bool loadPlayerData(Fight& fight, ServerObject& player, std::span<const std::uint8_t> file, bool* disconnect, bool playerOnly)
{
    if (disconnect) {
        *disconnect = false;
    }
    if (file.empty() || file.size() % crypto::Blowfish::kBlockSize != 0) {
        return false; // (cryptOpen fails)
    }
    std::vector<std::uint8_t> plain(file.begin(), file.end());
    crypto::cipherForKey(crypto::FileKey::PlayerFile).decrypt(plain);
    auto parsed = readLoadedSections(plain);
    if (!parsed) {
        return false;
    }
    const bool fresh = playerOnly && forGuest(*parsed);
    return applyFile(fight, player, *parsed, disconnect, playerOnly, fresh);
}

bool loadSentPlayerData(Fight& fight, ServerObject& player, std::span<const std::uint8_t> sent, bool* disconnect, bool playerOnly)
{
    if (disconnect) {
        *disconnect = false;
    }
    // SavePlayerDataFromClient: the bytes written out as they came, the last block
    // filled with zeros by the close (binfile_close); the load (cliPlrInfoLoadFromFile) reads them back
    // as they were. (A stream that ends without a 0 id, which no client sends, ends here; the
    // exe's walk would go on with its last id.)
    std::vector<std::uint8_t> plain(sent.begin(), sent.end());
    plain.resize((plain.size() + crypto::Blowfish::kBlockSize - 1) / crypto::Blowfish::kBlockSize * crypto::Blowfish::kBlockSize, 0);
    auto parsed = readLoadedSections(plain);
    if (!parsed) {
        return false;
    }
    const bool fresh = playerOnly && forGuest(*parsed);
    return applyFile(fight, player, *parsed, disconnect, playerOnly, fresh);
}

std::vector<std::uint8_t> saveServerSent(std::span<const std::uint8_t> file)
{
    // (binfile_open / cryptSet(0x1B): a file read in whole 8-byte blocks; none,
    // computeServerPlayerDataBufferSize's 0: malloc(4), and the copy fails at its open and copies
    // nothing.)
    const std::size_t whole = file.size() / crypto::Blowfish::kBlockSize * crypto::Blowfish::kBlockSize;
    if (whole == 0) {
        return std::vector<std::uint8_t>(4, 0);
    }
    std::vector<std::uint8_t> plain(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(whole));
    crypto::cipherForKey(crypto::FileKey::PlayerFile).decrypt(plain);
    static constexpr std::uint32_t kServer[] = {2, 3, 4, 8, 5, 6, 9, 10, 11}; // (g_plrServerSections)
    const auto u32At = [&](std::size_t at) { return util::u32At(plain, at); };
    // Each id read (its 4 bytes), then the size (binfile_fread_align: the rest of the
    // 8-byte block skipped, 8 bytes read, the low 4 kept); a server section's id, its size and its
    // data copied each at its place in the section as the file has it; the others skipped.
    // (computeServerPlayerDataBufferSize stops at the file's end without the 0 id; the copy doesn't
    // look: a file without one isn't one either writes.)
    std::vector<std::uint8_t> out;
    std::size_t pos = 0;
    while (pos + 4 <= plain.size()) {
        const std::uint32_t id = u32At(pos);
        if (id == 0) {
            out.insert(out.end(), 4, 0);
            break;
        }
        const std::size_t sizeAt = (pos + 4 + 7) / 8 * 8;
        if (sizeAt + 8 > plain.size()) {
            break;
        }
        const std::uint32_t size = u32At(sizeAt);
        const std::size_t end = sizeAt + 8 + size;
        if (end > plain.size()) {
            break;
        }
        if (std::ranges::find(kServer, id) != std::end(kServer)) {
            // Only the id, the size's low 4 bytes and the data are copied; the gaps stay zero.
            const std::size_t at = out.size();
            out.resize(at + (end - pos), 0);
            std::copy_n(plain.begin() + static_cast<std::ptrdiff_t>(pos), 4, out.begin() + static_cast<std::ptrdiff_t>(at));
            std::copy_n(plain.begin() + static_cast<std::ptrdiff_t>(sizeAt), 4, out.begin() + static_cast<std::ptrdiff_t>(at + (sizeAt - pos)));
            std::copy_n(plain.begin() + static_cast<std::ptrdiff_t>(sizeAt + 8), size, out.begin() + static_cast<std::ptrdiff_t>(at + (sizeAt + 8 - pos)));
        }
        pos = end;
    }
    out.insert(out.end(), 4, 0); // (malloc(count + 4): the last 4 never written)
    return out;
}

} // namespace nox::server
