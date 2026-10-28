// Typed .plr section codecs (structs in player_file.hpp). Each parse() reads one section's bytes
// completely and serialize() writes the same bytes back. Most sections after the u16 version have a
// "has" byte; when it is 0 nothing else follows.
#include "noxcore/player/player_file.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include <limits>

namespace nox::player {

namespace {

// u8 length, then that many 8-bit characters.
std::string readS8(ByteReader& r)
{
    return r.rawString(r.u8());
}

void writeS8(ByteWriter& w, const std::string& s)
{
    if (s.size() > 0xFF) {
        throw FormatError(".plr: string too long: " + s.substr(0, 32));
    }
    w.u8(static_cast<std::uint8_t>(s.size()));
    w.rawString(s);
}

/// u8 character count, then UTF-16LE.
std::u16string readW8(ByteReader& r)
{
    std::u16string s(r.u8(), u'\0');
    for (auto& c : s) {
        c = static_cast<char16_t>(r.u16());
    }
    return s;
}

void writeW8(ByteWriter& w, const std::u16string& s)
{
    if (s.size() > 0xFF) {
        throw FormatError(".plr: name too long");
    }
    w.u8(static_cast<std::uint8_t>(s.size()));
    for (auto c : s) {
        w.u16(static_cast<std::uint16_t>(c));
    }
}

template <std::size_t N>
void readBytes(ByteReader& r, std::array<std::uint8_t, N>& a)
{
    auto b = r.bytes(N);
    std::copy(b.begin(), b.end(), a.begin());
}

template <std::size_t N>
void writeBytes(ByteWriter& w, const std::array<std::uint8_t, N>& a)
{
    w.bytes(a);
}

// Five RGB triples, 15 bytes.
BodyColors readColors(ByteReader& r)
{
    BodyColors c;
    for (auto& rgb : c) {
        readBytes(r, rgb);
    }
    return c;
}

void writeColors(ByteWriter& w, const BodyColors& c)
{
    for (const auto& rgb : c) {
        writeBytes(w, rgb);
    }
}

CharacterClass readClass(ByteReader& r)
{
    const std::uint8_t c = r.u8();
    if (c > 2) {
        r.fail("unknown character class " + std::to_string(c));
    }
    return static_cast<CharacterClass>(c);
}

// Casts a container size to a count field, throwing if it doesn't fit.
template <class T>
T countOf(std::size_t n, std::string_view what)
{
    if (n > std::numeric_limits<T>::max()) {
        throw FormatError(std::string(".plr: too many ") + std::string(what));
    }
    return static_cast<T>(n);
}

std::vector<std::uint8_t> finish(ByteWriter& w)
{
    return std::move(w).take();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// 7 GUI

namespace {

// A grid of slots (the bars, the traps), row by row: each its name and a u8 flag.
template <class Grid>
void readSlots(ByteReader& r, Grid& grid)
{
    for (auto& row : grid) {
        for (auto& slot : row) {
            slot.name = readS8(r);
            slot.flag = r.u8();
        }
    }
}

template <class Grid>
void writeSlots(ByteWriter& w, const Grid& grid)
{
    for (const auto& row : grid) {
        for (const auto& slot : row) {
            writeS8(w, slot.name);
            w.u8(slot.flag);
        }
    }
}

} // namespace

// Layout: u16 version, u8 class, 5x5 bar slots (name + u8 flag each), 3x3 trap slots unless a
// warrior, then the active row/trap (v2) and summon order (v3).
Gui Gui::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "GUI");
    Gui g;
    g.version = r.u16();
    g.characterClass = readClass(r);
    readSlots(r, g.bars);
    if (g.characterClass != CharacterClass::Warrior) {
        readSlots(r, g.traps);
    }
    if (g.version >= 2) {
        g.activeRow = r.u8();
        g.activeTrap = r.u8();
    }
    if (g.version >= 3) {
        g.summonOrder = r.u8();
    }
    r.expectEnd();
    return g;
}

std::vector<std::uint8_t> Gui::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.u8(static_cast<std::uint8_t>(characterClass));
    writeSlots(w, bars);
    if (characterClass != CharacterClass::Warrior) {
        writeSlots(w, traps);
    }
    if (version >= 2) {
        w.u8(activeRow);
        w.u8(activeTrap);
    }
    if (version >= 3) {
        w.u8(summonOrder);
    }
    return finish(w);
}

// ---------------------------------------------------------------------------------------------
// 1 File info

// Layout: u16 version, u32 flags, u16-length save path, u8-length unused string, SYSTEMTIME
// (8 u16), body colours, clothing, UTF-16 name, class, female, level, then map name (v > 10)
// and quest stage (v > 11).
FileInfo FileInfo::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "FileInfo");
    FileInfo f;
    f.version = r.u16();
    f.flags = r.u32();
    f.savePath = r.rawString(r.u16());
    f.unused = readS8(r);
    for (auto& t : f.time) {
        t = r.u16();
    }
    f.colors = readColors(r);
    readBytes(r, f.clothing);
    f.name = readW8(r);
    f.characterClass = readClass(r);
    f.isFemale = r.u8();
    f.level = r.u8();
    if (f.version > 10) {
        f.mapName = readS8(r);
    }
    if (f.version > 11) {
        f.questStage = r.u8();
    }
    r.expectEnd();
    return f;
}

std::vector<std::uint8_t> FileInfo::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.u32(flags);
    w.u16(countOf<std::uint16_t>(savePath.size(), "save path characters"));
    w.rawString(savePath);
    writeS8(w, unused);
    for (auto t : time) {
        w.u16(t);
    }
    writeColors(w, colors);
    writeBytes(w, clothing);
    writeW8(w, name);
    w.u8(static_cast<std::uint8_t>(characterClass));
    w.u8(isFemale);
    w.u8(level);
    if (version > 10) {
        writeS8(w, mapName);
    }
    if (version > 11) {
        w.u8(questStage);
    }
    return finish(w);
}

// ---------------------------------------------------------------------------------------------
// 12 Music

Music Music::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "Music");
    Music m;
    // Versions before 11 have no flag and always carry the state.
    m.version = r.u16();
    m.solo = 1;
    if (m.version >= 11) {
        m.solo = r.u8();
    }
    if (m.solo != 0) {
        // Four i32 for the current track, then an i32 count of stacked 4 x i32 entries.
        for (auto& v : m.current) {
            v = r.i32();
        }
        const std::int32_t n = r.i32();
        if (n < 0) {
            r.fail("negative music stack size");
        }
        m.stack.resize(r.count(static_cast<std::uint32_t>(n), 16));
        for (auto& e : m.stack) {
            for (auto& v : e) {
                v = r.i32();
            }
        }
    }
    r.expectEnd();
    return m;
}

std::vector<std::uint8_t> Music::serialize() const
{
    ByteWriter w;
    w.u16(version);
    if (version >= 11) {
        w.u8(solo);
    }
    if (solo != 0) {
        for (auto v : current) {
            w.i32(v);
        }
        w.i32(countOf<std::int32_t>(stack.size(), "music stack entries"));
        for (const auto& e : stack) {
            for (auto v : e) {
                w.i32(v);
            }
        }
    }
    return finish(w);
}

// ---------------------------------------------------------------------------------------------
// 2 Attrib

Attrib Attrib::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "Attrib");
    Attrib a;
    // Layout: game mode (v5), UTF-16 name, speed, strength, max mana, max health (u32 each), class,
    // female, body colours, clothing (v2), u8, extra lives (v3, plus nine unused u32 in v3 only),
    // highest quest stage (v4).
    a.version = r.u16();
    if (a.version >= 5) {
        a.gameMode = r.u32();
    }
    a.name = readW8(r);
    a.speed = r.u32();
    a.strength = r.u32();
    a.maxMana = r.u32();
    a.maxHealth = r.u32();
    a.characterClass = readClass(r);
    a.isFemale = r.u8();
    a.colors = readColors(r);
    if (a.version >= 2) {
        readBytes(r, a.clothing);
    }
    a.unknown88 = r.u8();
    if (a.version >= 3) {
        a.extraLives = r.u32();
        if (a.version == 3) {
            for (auto& u : a.unusedV3) {
                u = r.u32();
            }
        }
    }
    if (a.version >= 4) {
        a.highestQuestStage = r.u32();
    }
    r.expectEnd();
    return a;
}

std::vector<std::uint8_t> Attrib::serialize() const
{
    ByteWriter w;
    w.u16(version);
    if (version >= 5) {
        w.u32(gameMode);
    }
    writeW8(w, name);
    w.u32(speed);
    w.u32(strength);
    w.u32(maxMana);
    w.u32(maxHealth);
    w.u8(static_cast<std::uint8_t>(characterClass));
    w.u8(isFemale);
    writeColors(w, colors);
    if (version >= 2) {
        writeBytes(w, clothing);
    }
    w.u8(unknown88);
    if (version >= 3) {
        w.u32(extraLives);
        if (version == 3) {
            for (auto u : unusedV3) {
                w.u32(u);
            }
        }
    }
    if (version >= 4) {
        w.u32(highestQuestStage);
    }
    return finish(w);
}

// ---------------------------------------------------------------------------------------------
// 3 Status

Status Status::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "Status");
    Status s;
    s.version = r.u16();
    // "has" byte, then the values block.
    if (r.u8() != 0) {
        Values v;
        v.maxHealth = r.u16();
        v.maxMana = r.u16();
        v.health = r.u16();
        v.mana = r.u16();
        v.poison = r.u8();
        v.unknown541 = r.u8();
        v.unknown542 = r.u16();
        v.experience = r.f32();
        if (s.version >= 2) {
            v.direction = r.u16();
        }
        s.values = v;
    }
    r.expectEnd();
    return s;
}

std::vector<std::uint8_t> Status::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.u8(values ? 1 : 0);
    if (values) {
        const auto& v = *values;
        w.u16(v.maxHealth);
        w.u16(v.maxMana);
        w.u16(v.health);
        w.u16(v.mana);
        w.u8(v.poison);
        w.u8(v.unknown541);
        w.u16(v.unknown542);
        w.f32(v.experience);
        if (version >= 2) {
            w.u16(v.direction);
        }
    }
    return finish(w);
}

// ---------------------------------------------------------------------------------------------
// 4 Inventory

Inventory Inventory::parse(std::span<const std::uint8_t> data, const things::ThingDb& things)
{
    ByteReader r(data, "Inventory");
    Inventory inv;
    inv.version = r.u16();
    // "has" byte; then gold, a u32 item count and the items, the equipped script ids, and the tail.
    if (r.u8() != 0) {
        Contents c;
        c.gold = r.u32();
        c.items.resize(r.count(r.u32()));
        for (auto& item : c.items) {
            item.type = readS8(r);
            const things::ThingDef* t = things.findThing(item.type);
            if (!t) {
                r.fail("unknown item type " + item.type);
            }
            const auto xferName = t->attribute("XFER");
            const map::xfer::TypeInfo info{things::thingClasses(*t), things::thingSubclasses(*t)};
            // The object's xfer has no size: read it through to find where it ends.
            const std::size_t start = r.offset();
            const std::uint16_t xferVersion = r.u16();
            const map::ObjectHeader header = map::readObjectHeader(r);
            // Nested inventories (items inside an item) aren't handled by this reader.
            if (header.inventoryCount() != 0) {
                r.fail("inventory items that carry items aren't supported");
            }
            // Parsed only to find the end; the bytes are kept as they are.
            map::xfer::read(r, xferName ? *xferName : "DefaultXfer", xferVersion, info, r.offset() - start);
            item.xfer.assign(data.begin() + static_cast<std::ptrdiff_t>(start),
                             data.begin() + static_cast<std::ptrdiff_t>(r.offset()));
        }
        c.equipped.resize(r.u8());
        for (auto& id : c.equipped) {
            id = r.u32();
        }
        c.secondaryWeapon = r.u32();
        if (inv.version >= 2) {
            c.lastQuiver = r.u32();
        }
        if (inv.version >= 3) {
            c.trapCount = r.u8();
        }
        inv.contents = std::move(c);
    }
    r.expectEnd();
    return inv;
}

std::vector<std::uint8_t> Inventory::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.u8(contents ? 1 : 0);
    if (contents) {
        const auto& c = *contents;
        w.u32(c.gold);
        w.u32(countOf<std::uint32_t>(c.items.size(), "items"));
        for (const auto& item : c.items) {
            writeS8(w, item.type);
            w.bytes(item.xfer);
        }
        w.u8(countOf<std::uint8_t>(c.equipped.size(), "equipped items"));
        for (auto id : c.equipped) {
            w.u32(id);
        }
        w.u32(c.secondaryWeapon);
        if (version >= 2) {
            w.u32(c.lastQuiver);
        }
        if (version >= 3) {
            w.u8(c.trapCount);
        }
    }
    return finish(w);
}

// ---------------------------------------------------------------------------------------------
// 8 Field guide, 5 Spellbook, 9 Journal

FieldGuide FieldGuide::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "FieldGuide");
    FieldGuide f;
    f.version = r.u16();
    // "has" byte, then a u8 count of creature names.
    if (r.u8() != 0) {
        std::vector<std::string> names(r.u8());
        for (auto& n : names) {
            n = readS8(r);
        }
        f.creatures = std::move(names);
    }
    r.expectEnd();
    return f;
}

std::vector<std::uint8_t> FieldGuide::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.u8(creatures ? 1 : 0);
    if (creatures) {
        w.u8(countOf<std::uint8_t>(creatures->size(), "creatures"));
        for (const auto& n : *creatures) {
            writeS8(w, n);
        }
    }
    return finish(w);
}

Spellbook Spellbook::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "Spellbook");
    Spellbook s;
    s.version = r.u16();
    // "has" byte, then a u8 count of names (+ u32 level from v2).
    if (r.u8() != 0) {
        std::vector<Entry> entries(r.u8());
        for (auto& e : entries) {
            e.name = readS8(r);
            if (s.version >= 2) {
                e.level = r.u32();
            }
        }
        s.entries = std::move(entries);
    }
    r.expectEnd();
    return s;
}

std::vector<std::uint8_t> Spellbook::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.u8(entries ? 1 : 0);
    if (entries) {
        w.u8(countOf<std::uint8_t>(entries->size(), "spells"));
        for (const auto& e : *entries) {
            writeS8(w, e.name);
            if (version >= 2) {
                w.u32(e.level);
            }
        }
    }
    return finish(w);
}

Journal Journal::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "Journal");
    Journal j;
    j.version = r.u16();
    // "has" byte, then a u16 count of (name, u16 type) entries.
    if (r.u8() != 0) {
        std::vector<Entry> entries(r.u16());
        for (auto& e : entries) {
            e.name = readS8(r);
            e.type = r.u16();
        }
        j.entries = std::move(entries);
    }
    r.expectEnd();
    return j;
}

std::vector<std::uint8_t> Journal::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.u8(entries ? 1 : 0);
    if (entries) {
        w.u16(countOf<std::uint16_t>(entries->size(), "journal entries"));
        for (const auto& e : *entries) {
            writeS8(w, e.name);
            w.u16(e.type);
        }
    }
    return finish(w);
}

// ---------------------------------------------------------------------------------------------
// 6 Enchantment

namespace {

// Cooldowns for abilities 1..5; the first isn't stored while berserkActive is set.
Enchantment::Abilities readAbilities(ByteReader& r)
{
    Enchantment::Abilities a;
    a.berserkActive = r.u8();
    a.treadLightlyActive = r.u8();
    a.treadLightlyTimer = r.u32();
    for (int i = a.berserkActive ? 2 : 1; i <= 5; ++i) {
        a.cooldowns.push_back(r.u32());
    }
    return a;
}

void writeAbilities(ByteWriter& w, const Enchantment::Abilities& a)
{
    if (a.cooldowns.size() != (a.berserkActive ? 4u : 5u)) {
        throw FormatError("Enchantment: cooldown count doesn't match berserkActive");
    }
    w.u8(a.berserkActive);
    w.u8(a.treadLightlyActive);
    w.u32(a.treadLightlyTimer);
    for (auto c : a.cooldowns) {
        w.u32(c);
    }
}

constexpr std::string_view kEnchantShield = "ENCHANT_SHIELD";

} // namespace

Enchantment Enchantment::parse(std::span<const std::uint8_t> data, CharacterClass characterClass)
{
    ByteReader r(data, "Enchantment");
    Enchantment e;
    e.version = r.u16();
    const bool warrior = characterClass == CharacterClass::Warrior;
    // "has" byte; then per buff: name, u16 frames left, u8 power (v2), shield health (v3, shield only).
    if (r.u8() != 0) {
        std::vector<Buff> buffs(r.u8());
        for (auto& b : buffs) {
            b.name = readS8(r);
            b.framesLeft = r.u16();
            if (e.version >= 2) {
                b.power = r.u8();
            }
            if (b.name == kEnchantShield && e.version >= 3) {
                b.shieldHealth = r.u32();
            }
        }
        e.buffs = std::move(buffs);
        // Warrior ability state sits inside the "has" block from v5 but after it in v4.
        if (warrior && e.version >= 5) {
            e.abilities = readAbilities(r);
        }
    }
    if (warrior && e.version == 4) {
        e.abilities = readAbilities(r);
    }
    r.expectEnd();
    return e;
}

std::vector<std::uint8_t> Enchantment::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.u8(buffs ? 1 : 0);
    if (buffs) {
        w.u8(countOf<std::uint8_t>(buffs->size(), "enchantments"));
        for (const auto& b : *buffs) {
            writeS8(w, b.name);
            w.u16(b.framesLeft);
            if (version >= 2) {
                w.u8(b.power);
            }
            if (b.name == kEnchantShield && version >= 3) {
                w.u32(b.shieldHealth);
            }
        }
        if (abilities && version >= 5) {
            writeAbilities(w, *abilities);
        }
    }
    if (abilities && version == 4) {
        writeAbilities(w, *abilities);
    }
    return finish(w);
}

// ---------------------------------------------------------------------------------------------
// 10 Game data, 11 Pad

std::string GameData::mapName() const
{
    // Only the first half of the stored bytes is the name (see GameData::mapNameBytes).
    const std::size_t len = mapNameBytes.size() / 2;
    std::string s(mapNameBytes.begin(), mapNameBytes.begin() + static_cast<std::ptrdiff_t>(len));
    return s.substr(0, s.find('\0'));
}

GameData GameData::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "GameData");
    GameData g;
    g.version = r.u16();
    if (g.version >= 5) {
        g.lastObjectScriptId = r.u32();
    }
    // The u16 is the name length, but the game writes twice that many bytes, kept as they are.
    auto name = r.bytes(2 * std::size_t{r.u16()});
    g.mapNameBytes.assign(name.begin(), name.end());
    if (g.version >= 2) {
        g.questVariablesVersion = r.u16();
        g.questVariables.resize(r.count(r.u32(), 9));
        for (auto& q : g.questVariables) {
            q.name = readS8(r);
            q.type = r.u32();
            q.value = r.u32();
        }
    }
    if (g.version >= 3) {
        g.magicWallsVersion = r.u16();
        // u8 count; each magic wall is a u32 and 9 bytes of wall fields.
        g.magicWalls.resize(r.u8());
        for (auto& m : g.magicWalls) {
            m.unknown = r.u32();
            readBytes(r, m.fields);
        }
    }
    if (g.version >= 4) {
        g.questStage = r.u8();
    }
    r.expectEnd();
    return g;
}

std::vector<std::uint8_t> GameData::serialize() const
{
    if (mapNameBytes.size() % 2 != 0) {
        throw FormatError("GameData: map name bytes must be twice the stored length");
    }
    ByteWriter w;
    w.u16(version);
    if (version >= 5) {
        w.u32(lastObjectScriptId);
    }
    w.u16(countOf<std::uint16_t>(mapNameBytes.size() / 2, "map name characters"));
    w.bytes(mapNameBytes);
    if (version >= 2) {
        w.u16(questVariablesVersion);
        w.u32(countOf<std::uint32_t>(questVariables.size(), "quest variables"));
        for (const auto& q : questVariables) {
            writeS8(w, q.name);
            w.u32(q.type);
            w.u32(q.value);
        }
    }
    if (version >= 3) {
        w.u16(magicWallsVersion);
        w.u8(countOf<std::uint8_t>(magicWalls.size(), "magic walls"));
        for (const auto& m : magicWalls) {
            w.u32(m.unknown);
            writeBytes(w, m.fields);
        }
    }
    if (version >= 4) {
        w.u8(questStage);
    }
    return finish(w);
}

Pad Pad::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "Pad");
    Pad p;
    p.version = r.u16();
    auto rest = r.bytes(r.remaining());
    if (std::any_of(rest.begin(), rest.end(), [](std::uint8_t b) { return b != 0; })) {
        r.fail("non-zero padding");
    }
    p.zeros = rest.size();
    return p;
}

std::vector<std::uint8_t> Pad::serialize() const
{
    ByteWriter w;
    w.u16(version);
    w.zeros(zeros);
    return finish(w);
}

} // namespace nox::player
