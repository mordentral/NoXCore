// Monster, NPC and monster generator xfer layouts (see xfer_monster.hpp). These are the longest
// object records: script events, spell lists, the AI state block (ActionData), shop inventories,
// active enchantments, and for generators whole embedded object records.
#include "xfer_monster.hpp"

#include "ai_actions.hpp"
#include "section_io.hpp"

#include "noxcore/map/objects.hpp"
#include "noxcore/things/object_class.hpp"

namespace nox::map::xfer::monster {

using nox::map::detail::countOf;
using nox::map::detail::findAiAction;
using nox::map::detail::kAiActions;
using nox::map::detail::readArray;
using nox::map::detail::readScriptHandler;
using nox::map::detail::readString8;
using nox::map::detail::writeArray;
using nox::map::detail::writeBytewise;
using nox::map::detail::writeScriptHandler;
using nox::map::detail::writeJoined;
using nox::map::detail::writeOptString8;
using nox::map::detail::writeString8;

namespace {

// Enchantment names, GAME.EXE table. Buffs refer to them by name.
constexpr std::string_view kEnchantments[] = {
    "ENCHANT_INVISIBLE",          "ENCHANT_MOONGLOW",           "ENCHANT_BLINDED",
    "ENCHANT_CONFUSED",           "ENCHANT_SLOWED",             "ENCHANT_HELD",
    "ENCHANT_DETECTING",          "ENCHANT_ETHEREAL",           "ENCHANT_RUN",
    "ENCHANT_HASTED",             "ENCHANT_VILLAIN",            "ENCHANT_AFRAID",
    "ENCHANT_BURNING",            "ENCHANT_VAMPIRISM",          "ENCHANT_ANCHORED",
    "ENCHANT_LIGHT",              "ENCHANT_DEATH",              "ENCHANT_PROTECT_FROM_FIRE",
    "ENCHANT_PROTECT_FROM_POISON", "ENCHANT_PROTECT_FROM_MAGIC", "ENCHANT_PROTECT_FROM_ELECTRICITY",
    "ENCHANT_INFRAVISION",        "ENCHANT_SHOCK",              "ENCHANT_INVULNERABLE",
    "ENCHANT_TELEKINESIS",        "ENCHANT_FREEZE",             "ENCHANT_SHIELD",
    "ENCHANT_REFLECTIVE_SHIELD",  "ENCHANT_CHARMING",           "ENCHANT_ANTI_MAGIC",
    "ENCHANT_CROWN",              "ENCHANT_SNEAK",
};
constexpr std::string_view kEnchantShield = "ENCHANT_SHIELD"; // index 26: carries shield health

bool isEnchantment(std::string_view name)
{
    for (auto e : kEnchantments) {
        if (e == name) {
            return true;
        }
    }
    return false;
}

void readU32s(ByteReader& r, std::span<std::uint32_t> xs)
{
    for (auto& x : xs) {
        x = r.u32();
    }
}

// u32 list whose count was read by the caller; `max` is the size of the exe's fixed buffer.
std::vector<std::uint32_t> readU32List(ByteReader& r, std::size_t count, std::size_t max, std::string_view what)
{
    if (count > max) {
        r.fail(std::string(what) + ": more entries than the game's buffer holds");
    }
    std::vector<std::uint32_t> out(count);
    readU32s(r, out);
    return out;
}

// Each u32 its own call.
void writeU32s(ByteWriter& w, std::span<const std::uint32_t> xs)
{
    for (auto x : xs) {
        w.u32(x);
    }
}

// ---------------------------------------------------------------------------------------------
// ActionData and its stack entries

// One AI stack entry: u8-length action name, u8 argument count (<= 2), each argument's bytes
// (size from the action table's argument type), then a u32.
AiActionEntry readActionEntry(ByteReader& r)
{
    AiActionEntry e;
    e.action = readString8(r);
    const int index = findAiAction(e.action);
    if (index < 0) {
        r.fail("unknown AI action " + e.action);
    }
    const auto& def = kAiActions[index];
    const std::uint8_t argc = r.u8();
    if (argc > 2) {
        r.fail("AI action with more than two arguments");
    }
    for (std::uint8_t i = 0; i < argc; ++i) {
        AiActionEntry::Arg a;
        a.type = def.argTypes[i];
        const std::size_t size = nox::map::detail::aiArgSize(a.type);
        if (size == 0) {
            r.fail("AI action argument of unknown type");
        }
        auto b = r.bytes(size);
        // Arguments are little-endian; pack them into the low bytes of `value`.
        for (std::size_t k = 0; k < size; ++k) {
            a.value |= std::uint64_t{b[k]} << (8 * k);
        }
        e.args.push_back(a);
    }
    e.unknown = r.u32();
    return e;
}

void writeActionEntry(ByteWriter& w, const AiActionEntry& e)
{
    const int index = findAiAction(e.action);
    if (index < 0 || e.args.size() > 2) {
        throw FormatError("ActionData: unknown action or too many arguments");
    }
    writeString8(w, e.action);
    w.u8(static_cast<std::uint8_t>(e.args.size()));
    for (std::size_t i = 0; i < e.args.size(); ++i) {
        const auto& a = e.args[i];
        if (a.type != kAiActions[index].argTypes[i]) {
            throw FormatError("ActionData: argument type doesn't match " + e.action);
        }
        const std::size_t size = nox::map::detail::aiArgSize(a.type);
        std::vector<std::uint8_t> arg(size); // one call per argument
        for (std::size_t k = 0; k < size; ++k) {
            arg[k] = static_cast<std::uint8_t>(a.value >> (8 * k));
        }
        w.bytes(arg);
    }
    w.u32(e.unknown);
}

// Layout follows the struct order in xfer.hpp. Version 1 has no `present` byte and always
// carries a body; maps usually store version 4 with present = 0.
ActionData readActionData(ByteReader& r)
{
    ActionData a;
    a.version = r.u16();
    if (a.version > 4) {
        r.fail("unsupported ActionData version");
    }
    a.present = 1;
    if (a.version > 1) {
        a.present = r.u8();
        if (a.present == 0) {
            return a;
        }
    }
    // Body fields in file order; most are opaque AI state carried through unchanged.
    ActionData::Body b;
    b.saveFrame = r.u32();
    b.unknown1 = r.u32();
    const std::uint32_t n = r.u32();
    // Counts are checked against the exe's buffer sizes (32, 16, 24 stack entries, 16, 8).
    if (n > 32) {
        r.fail("ActionData: list larger than the game's buffer");
    }
    b.unknownList1.resize(n);
    for (auto& e : b.unknownList1) {
        readArray(r, e);
    }
    b.unknown2 = r.u32();
    readArray(r, b.unknown3);
    b.frame1 = r.u32();
    b.unknown4 = r.u8();
    b.waypoints = readU32List(r, r.u32(), 16, "ActionData waypoints");
    b.unknown5 = r.u32();
    readArray(r, b.unknown6);
    b.unknown7 = r.u32();
    readArray(r, b.unknown8);
    b.sound = readString8(r);
    readArray(r, b.unknown9);
    b.frame2 = r.u32();
    readArray(r, b.unknown10);
    // Versions 1-2 only: a u32 that version 3 dropped.
    if (a.version < 3) {
        b.unknown11 = r.u32();
    }
    b.frame3 = r.u32();
    readArray(r, b.unknown12);
    b.unknown13 = r.u32();
    b.frame4 = r.u32();
    b.frame5 = r.u32();
    // The stack is stored as its top index (-1 when empty), so entries = top + 1.
    const std::int8_t top = r.i8();
    if (top >= 24) {
        r.fail("ActionData: action stack larger than the game's buffer");
    }
    for (int i = 0; i <= top; ++i) {
        b.stack.push_back(readActionEntry(r));
    }
    // u8 count, then that many u32s.
    b.unknownList2 = readU32List(r, r.u8(), 16, "ActionData list");
    b.unknown14 = r.u32();
    b.frame6 = r.u32();
    b.unknown15 = r.u32();
    b.unknown16 = r.u32();
    b.unknown17 = r.u8();
    b.unknown18 = r.u8();
    b.unknownName = readString8(r);
    // Version 4 appends a tail of script ids and frame stamps.
    if (a.version > 3) {
        ActionData::Body::V4 v4;
        readU32s(r, v4.unknown1);
        v4.scriptId1 = r.u32();
        v4.unknown2 = r.u32();
        readU32s(r, v4.frames1);
        readU32s(r, v4.frames2);
        v4.unknown3 = r.u32();
        v4.frame3 = r.u32();
        v4.unknown4 = r.u8();
        v4.scriptId2 = r.u32();
        readU32s(r, v4.frames4);
        v4.scriptId3 = r.u32();
        v4.scriptIds = readU32List(r, r.u8(), 8, "ActionData script ids");
        b.v4 = std::move(v4);
    }
    a.body = std::move(b);
    return a;
}

// Writes ActionData back in readActionData's order. Each ByteWriter call is one step of the map
// CRC, so fields are grouped into calls as the game writes them (the "one call" notes below).
// Throws FormatError when the fields don't fit the version.
void writeActionData(ByteWriter& w, const ActionData& a)
{
    w.u16(a.version);
    if (a.version > 1) {
        w.u8(a.present);
        if (a.present == 0) {
            if (a.body) {
                throw FormatError("ActionData: body present but flagged absent");
            }
            return;
        }
    }
    if (!a.body) {
        throw FormatError("ActionData: missing body");
    }
    const auto& b = *a.body;
    // A v4 tail exactly when version > 3, and no more stack entries than the game's 24.
    if ((a.version > 3) != b.v4.has_value() || b.stack.size() > 24) {
        throw FormatError("ActionData: fields don't match the version");
    }
    w.u32(b.saveFrame);
    w.u32(b.unknown1);
    w.u32(countOf<std::uint32_t>(b.unknownList1.size(), "ActionData list"));
    std::vector<std::uint8_t> list1; // one call
    for (const auto& e : b.unknownList1) {
        list1.insert(list1.end(), e.begin(), e.end());
    }
    w.bytes(list1);
    w.u32(b.unknown2);
    writeArray(w, b.unknown3);
    w.u32(b.frame1);
    w.u8(b.unknown4);
    w.u32(countOf<std::uint32_t>(b.waypoints.size(), "ActionData waypoints"));
    writeU32s(w, b.waypoints);
    w.u32(b.unknown5);
    writeArray(w, b.unknown6);
    w.u32(b.unknown7);
    writeArray(w, b.unknown8);
    writeString8(w, b.sound);
    writeArray(w, b.unknown9);
    w.u32(b.frame2);
    writeBytewise(w, b.unknown10); // one call per byte
    if (a.version < 3) {
        w.u32(b.unknown11);
    }
    w.u32(b.frame3);
    writeArray(w, b.unknown12);
    w.u32(b.unknown13);
    w.u32(b.frame4);
    w.u32(b.frame5);
    // Top index, not count: an empty stack writes -1.
    w.i8(static_cast<std::int8_t>(static_cast<int>(b.stack.size()) - 1));
    for (const auto& e : b.stack) {
        writeActionEntry(w, e);
    }
    w.u8(countOf<std::uint8_t>(b.unknownList2.size(), "ActionData list"));
    writeU32s(w, b.unknownList2);
    w.u32(b.unknown14);
    w.u32(b.frame6);
    w.u32(b.unknown15);
    w.u32(b.unknown16);
    w.u8(b.unknown17);
    w.u8(b.unknown18);
    writeString8(w, b.unknownName);
    // Version 4 tail, same order as read.
    if (b.v4) {
        const auto& v4 = *b.v4;
        writeU32s(w, v4.unknown1);
        w.u32(v4.scriptId1);
        w.u32(v4.unknown2);
        writeU32s(w, v4.frames1);
        writeU32s(w, v4.frames2);
        w.u32(v4.unknown3);
        w.u32(v4.frame3);
        w.u8(v4.unknown4);
        w.u32(v4.scriptId2);
        writeU32s(w, v4.frames4);
        w.u32(v4.scriptId3);
        w.u8(countOf<std::uint8_t>(v4.scriptIds.size(), "ActionData script ids"));
        writeU32s(w, v4.scriptIds);
    }
}

// ---------------------------------------------------------------------------------------------
// Buffs, shop (XFer_ReadShopItem), shared monster pieces

// u16 version (1..2), u8 count, then per buff: name, u8 power, u32 timer, and for
// ENCHANT_SHIELD in version 2 a u32 shield health.
Buffs readBuffs(ByteReader& r)
{
    Buffs b;
    b.version = r.u16();
    if (b.version < 1 || b.version > 2) {
        r.fail("unsupported buff version");
    }
    b.buffs.resize(r.u8());
    for (auto& buff : b.buffs) {
        buff.enchantment = readString8(r);
        if (!isEnchantment(buff.enchantment)) {
            r.fail("unknown enchantment " + buff.enchantment);
        }
        buff.power = r.u8();
        buff.timer = r.u32();
        if (buff.enchantment == kEnchantShield && b.version > 1) {
            buff.shieldHealth = r.u32();
        }
    }
    return b;
}

void writeBuffs(ByteWriter& w, const Buffs& b)
{
    w.u16(b.version);
    w.u8(countOf<std::uint8_t>(b.buffs.size(), "buffs"));
    for (const auto& buff : b.buffs) {
        writeString8(w, buff.enchantment);
        w.u8(buff.power);
        w.u32(buff.timer);
        if (buff.enchantment == kEnchantShield && b.version > 1) {
            w.u32(buff.shieldHealth);
        }
    }
}

} // namespace

Modifiers readModifiers(ByteReader& r)
{
    Modifiers m;
    for (auto& s : m) {
        s = readString8(r);
    }
    return m;
}

void writeModifiers(ByteWriter& w, const Modifiers& m)
{
    for (const auto& s : m) {
        writeOptString8(w, s); // unset modifiers are null pointers: length only
    }
}

namespace {

// Shop: u32 (+ u32 from v 0x3D), u8-length name, u8 item count, then per item: u8 count, type name,
// parameter name (book contents), 4 modifiers.
Shop readShop(ByteReader& r, std::uint16_t v)
{
    Shop s;
    s.unknown1 = r.u32();
    if (v > 0x3C) {
        s.unknown2 = r.u32();
    }
    s.name = readString8(r);
    s.items.resize(r.u8());
    for (auto& item : s.items) {
        item.count = r.u8();
        item.type = readString8(r);
        item.parameter = readString8(r);
        item.modifiers = readModifiers(r);
    }
    return s;
}

void writeShop(ByteWriter& w, const Shop& s, std::uint16_t v)
{
    w.u32(s.unknown1);
    if (v > 0x3C) {
        w.u32(s.unknown2);
    }
    writeString8(w, s.name);
    w.u8(countOf<std::uint8_t>(s.items.size(), "shop items"));
    for (const auto& item : s.items) {
        w.u8(item.count);
        writeString8(w, item.type);
        writeOptString8(w, item.parameter);
        writeModifiers(w, item.modifiers);
    }
}

/// Script events; lostEnemy is present when the version is above `lostEnemyAfter`.
MonsterScripts readScripts(ByteReader& r, std::uint16_t v, std::uint16_t lostEnemyAfter)
{
    MonsterScripts s;
    s.enemySighted = readScriptHandler(r);
    s.death = readScriptHandler(r);
    s.unknown305 = r.u16();
    s.lookingForEnemy = readScriptHandler(r);
    s.changeFocus = readScriptHandler(r);
    s.isHit = readScriptHandler(r);
    s.retreat = readScriptHandler(r);
    s.collision = readScriptHandler(r);
    s.hearEnemy = readScriptHandler(r);
    s.endOfWaypoint = readScriptHandler(r);
    if (v > lostEnemyAfter) {
        s.lostEnemy = readScriptHandler(r);
    }
    return s;
}

void writeScripts(ByteWriter& w, const MonsterScripts& s, std::uint16_t v, std::uint16_t lostEnemyAfter)
{
    writeScriptHandler(w, s.enemySighted);
    writeScriptHandler(w, s.death);
    w.u16(s.unknown305);
    writeScriptHandler(w, s.lookingForEnemy);
    writeScriptHandler(w, s.changeFocus);
    writeScriptHandler(w, s.isHit);
    writeScriptHandler(w, s.retreat);
    writeScriptHandler(w, s.collision);
    writeScriptHandler(w, s.hearEnemy);
    writeScriptHandler(w, s.endOfWaypoint);
    if (v > lostEnemyAfter) {
        writeScriptHandler(w, s.lostEnemy);
    }
}

// u32 count (at most 0x89, the number of spells), then name + u32 flags per spell.
std::vector<MonsterSpell> readSpells(ByteReader& r)
{
    const std::uint32_t n = r.u32();
    if (n > 0x89) {
        r.fail("more monster spells than exist");
    }
    std::vector<MonsterSpell> out(n);
    for (auto& s : out) {
        s.name = readString8(r);
        s.flags = r.u32();
    }
    return out;
}

void writeSpells(ByteWriter& w, const std::vector<MonsterSpell>& spells)
{
    w.u32(countOf<std::uint32_t>(spells.size(), "monster spells"));
    for (const auto& s : spells) {
        writeString8(w, s.name);
        w.u32(s.flags);
    }
}

// Six RGB triples.
NpcColors readColors(ByteReader& r)
{
    NpcColors c;
    for (auto& rgb : c) {
        readArray(r, rgb);
    }
    return c;
}

void writeColors(ByteWriter& w, const NpcColors& c)
{
    for (const auto& rgb : c) {
        writeArray(w, rgb);
    }
}

bool hasSubclass(const TypeInfo& t, std::uint32_t bit)
{
    return (t.subclasses & bit) != 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// MonsterXfer

// Optional parts depend on the type's SUBCLASS (shopkeeper, female NPC, wounded NPC) as well as
// the version, which is why the TypeInfo is needed.
Monster readMonster(ByteReader& r, std::uint16_t v, const TypeInfo& t)
{
    Monster m;
    m.direction[0] = r.i32();
    m.direction[1] = r.i32();
    // lostEnemy is stored from version 0x34 for monsters (0x32 for NPCs).
    m.scripts = readScripts(r, v, 0x33);
    m.unknown1 = r.u32();
    m.unknown333 = r.u8();
    m.statusFlags = r.u32();
    m.unknown338 = r.u32();
    m.retreatLevel = r.u32();
    m.resumeLevel = r.u32();
    m.sightRange = r.u32();
    m.aggression = r.u32();
    m.unknown341 = readString8(r);
    m.spells = readSpells(r);
    for (auto& d : m.spellDelays) {
        d = r.u16();
    }
    m.unknown329 = r.u32();
    m.unknown510 = r.u32();
    m.unknown330 = r.u32();
    for (auto& s : m.defaultSpells) {
        s = readString8(r);
    }
    m.action = readString8(r);
    m.actionData = readActionData(r);
    m.unknown361 = r.u8();
    // Shopkeepers carry their shop inventory here.
    if (hasSubclass(t, things::ObjectSubclass::Shopkeeper)) {
        m.shop = readShop(r, v);
    }
    m.unknown0 = r.u32();
    m.subclassBits = r.u32();
    m.health = r.u16();
    readArray(r, m.unknown337);
    // Female NPCs carry their own colors and voice set.
    if (hasSubclass(t, things::ObjectSubclass::FemaleNpc)) {
        m.colors = readColors(r);
        m.voiceSet = readString8(r);
    }
    // Later file versions append buffs (0x3E), a second voice set for wounded NPCs (0x3F) and poison (0x40).
    if (v > 0x3D) {
        m.buffs = readBuffs(r);
    }
    if (v > 0x3E && hasSubclass(t, things::ObjectSubclass::WoundedNpc)) {
        m.voiceSet2 = readString8(r);
    }
    if (v >= 0x40) {
        m.poison = r.u8();
    }
    return m;
}

void writeMonster(ByteWriter& w, const Monster& m, std::uint16_t v, const TypeInfo& t)
{
    const bool shop = hasSubclass(t, things::ObjectSubclass::Shopkeeper);
    const bool female = hasSubclass(t, things::ObjectSubclass::FemaleNpc);
    const bool wounded = v > 0x3E && hasSubclass(t, things::ObjectSubclass::WoundedNpc);
    // The type decides which optional parts exist; refuse data that would not round-trip.
    if (shop != m.shop.has_value() || female != m.colors.has_value() || female != m.voiceSet.has_value() ||
        wounded != m.voiceSet2.has_value()) {
        throw FormatError("MonsterXfer: optional parts don't match the type");
    }
    writeJoined(w, {static_cast<std::uint32_t>(m.direction[0]), static_cast<std::uint32_t>(m.direction[1])});
    writeScripts(w, m.scripts, v, 0x33);
    w.u32(m.unknown1);
    w.u8(m.unknown333);
    w.u32(m.statusFlags);
    w.u32(m.unknown338);
    w.u32(m.retreatLevel);
    w.u32(m.resumeLevel);
    w.u32(m.sightRange);
    w.u32(m.aggression);
    writeString8(w, m.unknown341);
    writeSpells(w, m.spells);
    for (auto d : m.spellDelays) {
        w.u16(d);
    }
    w.u32(m.unknown329);
    w.u32(m.unknown510);
    w.u32(m.unknown330);
    for (const auto& s : m.defaultSpells) {
        writeString8(w, s);
    }
    writeString8(w, m.action);
    writeActionData(w, m.actionData);
    w.u8(m.unknown361);
    if (m.shop) {
        writeShop(w, *m.shop, v);
    }
    w.u32(m.unknown0);
    w.u32(m.subclassBits);
    w.u16(m.health);
    writeBytewise(w, m.unknown337); // one call per byte
    if (female) {
        writeColors(w, *m.colors);
        writeOptString8(w, *m.voiceSet);
    }
    if (v > 0x3D) {
        writeBuffs(w, m.buffs);
    }
    if (wounded) {
        writeOptString8(w, *m.voiceSet2);
    }
    if (v >= 0x40) {
        w.u8(m.poison);
    }
}

// ---------------------------------------------------------------------------------------------
// NPCXfer

// Same building blocks as MonsterXfer, in a different order and with no subclass-dependent parts.
Npc readNpc(ByteReader& r, std::uint16_t v)
{
    Npc n;
    n.direction[0] = r.i32();
    n.direction[1] = r.i32();
    n.scripts = readScripts(r, v, 0x31);
    n.unknown1 = r.u32();
    n.colors = readColors(r);
    n.unknown333 = r.u8();
    n.statusFlags = r.u32();
    n.unknown338 = r.u32();
    n.retreatLevel = r.u32();
    n.resumeLevel = r.u32();
    n.sightRange = r.u32();
    n.health = r.u16();
    n.aggression = r.u32();
    n.unknown341 = readString8(r);
    n.spells = readSpells(r);
    for (auto& d : n.spellDelays) {
        d = r.u16();
    }
    n.unknown329 = r.u32();
    n.unknown510 = r.u32();
    n.unknown331 = r.u8();
    n.unknown332 = r.u32();
    n.unknown330 = r.u32();
    for (auto& s : n.defaultSpells) {
        s = readString8(r);
    }
    n.action = readString8(r);
    n.actionData = readActionData(r);
    n.unknown361 = r.u8();
    n.unknown0 = r.u32();
    n.healthMax = r.u32();
    n.subclassBits = r.u32();
    n.healthCurrent = r.u16();
    n.experience = r.u32();
    n.voiceSet = readString8(r);
    // Buffs from version 0x3D and poison from 0x3E, earlier than in MonsterXfer.
    if (v > 0x3C) {
        n.buffs = readBuffs(r);
    }
    if (v >= 0x3E) {
        n.poison = r.u8();
    }
    return n;
}

void writeNpc(ByteWriter& w, const Npc& n, std::uint16_t v)
{
    writeJoined(w, {static_cast<std::uint32_t>(n.direction[0]), static_cast<std::uint32_t>(n.direction[1])});
    writeScripts(w, n.scripts, v, 0x31);
    w.u32(n.unknown1);
    writeColors(w, n.colors);
    w.u8(n.unknown333);
    w.u32(n.statusFlags);
    w.u32(n.unknown338);
    w.u32(n.retreatLevel);
    w.u32(n.resumeLevel);
    w.u32(n.sightRange);
    w.u16(n.health);
    w.u32(n.aggression);
    writeString8(w, n.unknown341);
    writeSpells(w, n.spells);
    for (auto d : n.spellDelays) {
        w.u16(d);
    }
    w.u32(n.unknown329);
    w.u32(n.unknown510);
    w.u8(n.unknown331);
    w.u32(n.unknown332);
    w.u32(n.unknown330);
    for (const auto& s : n.defaultSpells) {
        writeString8(w, s);
    }
    writeString8(w, n.action);
    writeActionData(w, n.actionData);
    w.u8(n.unknown361);
    w.u32(n.unknown0);
    w.u32(n.healthMax);
    w.u32(n.subclassBits);
    w.u16(n.healthCurrent);
    w.u32(n.experience);
    writeOptString8(w, n.voiceSet);
    if (v > 0x3C) {
        writeBuffs(w, n.buffs);
    }
    if (v >= 0x3E) {
        w.u8(n.poison);
    }
}

// ---------------------------------------------------------------------------------------------
// MonsterGeneratorXfer

namespace {

// Zero bytes needed to bring an offset (counted from the file start) to a multiple of 8.
std::size_t padTo8(std::size_t absolute)
{
    return (8 - absolute % 8) % 8;
}

/// One record as written by the game's object saver (xfer_saveObj) inside the generator data.
// Embedded record: u16 object TOC id, zero padding to 8-byte alignment, size as two u32 (low, high),
// then `size` bytes of record data.
ObjectRecord readEmbeddedRecord(ByteReader& r, std::size_t base)
{
    ObjectRecord rec;
    rec.tocId = r.u16();
    auto pad = r.bytes(padTo8(base + r.offset()));
    rec.padding.assign(pad.begin(), pad.end());
    rec.size = r.u32();
    rec.sizeHi = r.u32();
    auto data = r.bytes(rec.size);
    rec.data.assign(data.begin(), data.end());
    return rec;
}

/// A record followed by its inventory records (nested), in stream order.
void readRecordWithInventory(ByteReader& r, std::size_t base, std::vector<ObjectRecord>& out)
{
    out.push_back(readEmbeddedRecord(r, base));
    // The header's inventory count says how many records follow (each may carry its own inventory).
    const auto items = ObjectXfer::parse(out.back().data).header.inventoryCount();
    for (std::uint8_t i = 0; i < items; ++i) {
        readRecordWithInventory(r, base, out);
    }
}

/// `offset` is where the writer currently is within the 8-aligned record data. Padding and size
/// go below the CRC; the data goes through `nested`.
void writeEmbeddedRecord(ByteWriter& w, const ObjectRecord& rec, std::size_t offset,
                         const RecordDataWriter& nested)
{
    if (rec.size != rec.data.size()) {
        throw FormatError("MonsterGenerator: template size doesn't match its data");
    }
    w.u16(rec.tocId);
    // offset + 2: the padding starts after the u16 toc id. Original padding bytes are kept when the
    // alignment still matches, so unchanged files write back byte for byte.
    const std::size_t pad = padTo8(offset + 2);
    if (rec.padding.size() == pad) {
        w.uncountedBytes(rec.padding);
    } else {
        w.zeros(pad);
    }
    w.uncountedU32(rec.size);
    w.uncountedU32(rec.sizeHi);
    if (nested) {
        nested(w, rec);
    } else {
        w.bytes(rec.data);
    }
}

std::vector<std::uint8_t> readCountedBytes(ByteReader& r, std::size_t max)
{
    const std::uint8_t n = r.u8();
    if (n > max) {
        r.fail("MonsterGenerator: more bytes than the game's buffer holds");
    }
    auto b = r.bytes(n);
    return {b.begin(), b.end()};
}

} // namespace

// Layout: up to 3 counted bytes, two u8, u32, 4 script handlers, up to 3 template groups of up to 4
// templates (type name + embedded record + its inventory), then version-dependent extras.
MonsterGenerator readGenerator(ByteReader& r, std::uint16_t v, std::size_t base)
{
    MonsterGenerator g;
    g.unknown1 = readCountedBytes(r, 3);
    g.unknown2 = r.u8();
    g.unknown3 = r.u8();
    g.unknown4 = r.u32();
    for (auto& s : g.scripts) {
        s = readScriptHandler(r);
    }
    // Up to 3 groups, each of up to 4 templates (the game's fixed arrays).
    const std::uint8_t groups = r.u8();
    if (groups > 3) {
        r.fail("MonsterGenerator: more than three template groups");
    }
    g.groups.resize(groups);
    for (auto& group : g.groups) {
        const std::uint8_t count = r.u8();
        if (count > 4) {
            r.fail("MonsterGenerator: more than four templates in a group");
        }
        for (std::uint8_t i = 0; i < count; ++i) {
            MonsterGenerator::Template t;
            t.type = readString8(r);
            // A template is its type name, then its own record followed by its inventory records.
            std::vector<ObjectRecord> records;
            readRecordWithInventory(r, base, records);
            t.record = std::move(records.front());
            t.inventory.assign(std::make_move_iterator(records.begin() + 1),
                               std::make_move_iterator(records.end()));
            group.push_back(std::move(t));
        }
    }
    // Version-dependent extras at the end.
    if (v > 0x3D) {
        g.unknown5 = readCountedBytes(r, 3);
    }
    if (v > 0x3E) {
        g.unknown6 = r.u32();
    }
    return g;
}

void writeGenerator(ByteWriter& w, const MonsterGenerator& g, std::uint16_t v, std::size_t base,
                    const RecordDataWriter& nested)
{
    if (g.unknown1.size() > 3 || g.unknown5.size() > 3 || g.groups.size() > 3) {
        throw FormatError("MonsterGenerator: more entries than the game holds");
    }
    const std::size_t start = w.size();
    // Absolute position in the record data, needed for the embedded records' 8-byte alignment.
    auto offset = [&] { return base + (w.size() - start); };
    w.u8(static_cast<std::uint8_t>(g.unknown1.size()));
    writeBytewise(w, g.unknown1); // one call per byte
    w.u8(g.unknown2);
    w.u8(g.unknown3);
    w.u32(g.unknown4);
    for (const auto& s : g.scripts) {
        writeScriptHandler(w, s);
    }
    w.u8(static_cast<std::uint8_t>(g.groups.size()));
    for (const auto& group : g.groups) {
        if (group.size() > 4) {
            throw FormatError("MonsterGenerator: more than four templates in a group");
        }
        w.u8(static_cast<std::uint8_t>(group.size()));
        for (const auto& t : group) {
            writeString8(w, t.type);
            writeEmbeddedRecord(w, t.record, offset(), nested);
            // Each record is aligned against the absolute offset at the point it is written.
            for (const auto& item : t.inventory) {
                writeEmbeddedRecord(w, item, offset(), nested);
            }
        }
    }
    if (v > 0x3D) {
        w.u8(static_cast<std::uint8_t>(g.unknown5.size()));
        writeBytewise(w, g.unknown5); // one call per byte
    }
    if (v > 0x3E) {
        w.u32(g.unknown6);
    }
}

} // namespace nox::map::xfer::monster
