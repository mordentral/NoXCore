// Decoders and encoders for the type-specific tail of a map object record (ObjectXfer::extra).
// Each object type names an xfer function in thing.bin ("DoorXfer", "WeaponXfer"...); this file maps
// that name to a reader/writer pair for the struct in xfer.hpp. Monster, NPC and generator layouts
// are big enough to live in xfer_monster.cpp.
#include "noxcore/map/xfer.hpp"

#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include "section_io.hpp"
#include "xfer_monster.hpp"

namespace nox::map::xfer {

using namespace nox::map::detail;

namespace {

/// Typed decoding covers the versions shipped maps use; older records stay Raw.
constexpr std::uint16_t kMinTypedVersion = 0x3C;

// ---------------------------------------------------------------------------------------------
// Shared pieces

using monster::readModifiers;
using monster::writeModifiers;

// u16 count, then that many u8-length names (reward marker lists).
std::vector<std::string> readNames16(ByteReader& r)
{
    std::vector<std::string> out(r.u16());
    for (auto& s : out) {
        s = readString8(r);
    }
    return out;
}

void writeNames16(ByteWriter& w, const std::vector<std::string>& names)
{
    w.u16(countOf<std::uint16_t>(names.size(), "name list"));
    for (const auto& s : names) {
        writeString8(w, s);
    }
}

/// Wands and staves store charges: CLASS WAND with a staff subclass (XFerWeapon).
// v <= 0x28 never stores charges; before 0x3E the Oblivion Orb staves don't either.
bool weaponHasCharges(std::uint16_t v, const TypeInfo& t)
{
    const bool wand = (t.classes & things::ObjectClass::Wand) && (t.subclasses & things::ObjectSubclass::StaffWands);
    return v > 0x28 && wand && (v > 0x3D || !(t.subclasses & things::ObjectSubclass::StaffOblivionOrb));
}

// ---------------------------------------------------------------------------------------------
// Codecs: read(r, v, type) -> T, write(w, value, v, type)

// One known xfer function: its name in thing.bin, the newest version the game reads, and its reader.
struct Codec {
    std::string_view name;
    std::uint16_t maxVersion;
    Data (*read)(ByteReader&, std::uint16_t, const TypeInfo&); ///< null: the generator's (decode() reads it)
};

Data readDefault(ByteReader&, std::uint16_t, const TypeInfo&)
{
    return Default{};
}

Data readSpellPagePedestal(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    return SpellPagePedestal{r.u32()};
}

Data readReadable(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    return Readable{readString32(r)};
}

Data readExit(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    Exit x;
    x.mapName = readString32(r);
    x.x = r.f32();
    x.y = r.f32();
    return x;
}

Data readDoor(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    Door d;
    d.currentDirection = r.u32();
    d.lock = r.u32();
    d.direction = r.u32();
    return d;
}

// Layout: size x/y (i32), edge and back RGB (3 bytes each), u32, three script handlers,
// allowed/ignored class masks (u32), allowed/ignored team (u8), then state fields from v 0x3D.
Data readTrigger(ByteReader& r, std::uint16_t v, const TypeInfo&)
{
    Trigger t;
    t.sizeX = r.i32();
    t.sizeY = r.i32();
    readArray(r, t.edgeColor);
    readArray(r, t.backColor);
    t.unknown1 = r.u32();
    t.onPressed = readScriptHandler(r);
    t.onReleased = readScriptHandler(r);
    t.onCollided = readScriptHandler(r);
    t.allowedClasses = r.u32();
    t.ignoredClasses = r.u32();
    t.allowedTeam = r.u8();
    t.ignoredTeam = r.u8();
    if (v > 0x3C) {
        t.state = r.u8();
        t.unknown2 = r.u8();
        t.animFrame = r.u32();
    }
    return t;
}

Data readHole(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    Hole h;
    h.scriptActivated = r.u32();
    h.script = readScriptHandler(r);
    h.fallX = r.i32();
    h.fallY = r.i32();
    h.scriptTime = r.u32();
    h.scriptTimeout = r.u16();
    return h;
}

Data readTransporter(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    return Transporter{r.u32()};
}

Data readElevator(ByteReader& r, std::uint16_t v, const TypeInfo&)
{
    Elevator e;
    e.linkedExtent = r.u32();
    e.height = r.u32();
    if (v > 0x3C) {
        e.status = r.u8();
    }
    return e;
}

Data readElevatorShaft(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    return ElevatorShaft{r.u32()};
}

Data readMover(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    Mover m;
    m.movingSpeed = r.u32();
    m.waypoint = r.u32();
    m.movedExtent = r.u32();
    m.moveType = r.u8();
    m.waypointStart = r.u32();
    m.waypointEnd = r.u32();
    m.speedBase = r.f32();
    m.speedCurrent = r.f32();
    return m;
}

Data readGlyph(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    Glyph g;
    g.direction = r.u8();
    g.targetX = r.f32();
    g.targetY = r.f32();
    g.spells.resize(r.u8());
    for (auto& s : g.spells) {
        s = readString8(r);
    }
    return g;
}

// The drawable's light block: fixed size, no version-dependent fields.
Data readInvisibleLight(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    InvisibleLight l;
    l.type = r.u32();
    l.intensity = r.f32();
    l.radius = r.i32();
    l.intensityFx = r.i32();
    for (auto& c : l.color) {
        c = r.i32();
    }
    l.direction = r.u16();
    l.cone = r.u16();
    l.omni = r.u32();
    l.animFlags = r.u16();
    for (auto& key : l.colorKeys) {
        readArray(r, key);
    }
    readArray(r, l.intensityKeys);
    readArray(r, l.coneKeys);
    l.colorStep = r.u16();
    l.intensityStep = r.u16();
    l.coneStep = r.u16();
    l.target = r.u32();
    l.rotationSpeed = r.u16();
    l.rotationMax = r.u16();
    l.unused = r.u8();
    l.isAnti = r.u32();
    return l;
}

Data readSentry(ByteReader& r, std::uint16_t v, const TypeInfo&)
{
    Sentry s;
    s.basePosition = r.f32();
    s.rotateSpeed = r.f32();
    if (v > 0x3C) {
        s.currentPosition = r.f32();
    }
    return s;
}

Data readSpellReward(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    SpellReward s;
    s.spell = readString8(r);
    // Same name length limits as the exe's reader.
    if (s.spell.size() > 0x7F) {
        r.fail("spell name too long");
    }
    return s;
}

Data readAbilityReward(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    AbilityReward a;
    a.ability = readString8(r);
    if (a.ability.size() > 0x7F) {
        r.fail("ability name too long");
    }
    return a;
}

Data readFieldGuide(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    FieldGuide f;
    f.creature = readString8(r);
    if (f.creature.size() > 0x3F) {
        r.fail("creature name too long");
    }
    return f;
}

// Layout: 4 modifiers, charges (wands/staves only), u16 health, then a version-dependent tail.
Data readWeapon(ByteReader& r, std::uint16_t v, const TypeInfo& t)
{
    Weapon w;
    w.modifiers = readModifiers(r);
    if (weaponHasCharges(v, t)) {
        Weapon::Charges c;
        c.current = r.u8();
        c.max = r.u8();
        if (v > 0x3C) {
            c.unknown = r.u32();
        }
        w.charges = c;
    }
    w.health = r.u16();
    // Version 0x3F alone stores one byte here; from 0x40 a u32 takes its place.
    if (v == 0x3F) {
        w.unknown1 = r.u8();
    }
    if (v > 0x3F) {
        w.unknown2 = r.u32();
    }
    return w;
}

Data readArmor(ByteReader& r, std::uint16_t v, const TypeInfo&)
{
    Armor a;
    a.modifiers = readModifiers(r);
    a.health = r.u16();
    // Version 0x3D alone stores one byte here; from 0x3E a u32 takes its place.
    if (v == 0x3D) {
        a.unknown1 = r.u8();
    }
    if (v > 0x3D) {
        a.unknown2 = r.u32();
    }
    return a;
}

Data readAmmo(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    Ammo a;
    a.modifiers = readModifiers(r);
    a.current = r.u8();
    a.limit = r.u8();
    return a;
}

Data readTeam(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    return Team{readModifiers(r)};
}

Data readGold(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    return Gold{r.u32()};
}

Data readObelisk(ByteReader& r, std::uint16_t v, const TypeInfo&)
{
    Obelisk o;
    if (v > 0x3C) {
        o.manaPool = r.u32();
        o.minimapKnown = r.u8();
    }
    return o;
}

Data readToxicCloud(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    return ToxicCloud{r.u32()};
}

// Layout: reward type, u32, three u16-counted name lists, five u32, then chance/rarity by version.
Data readRewardMarker(ByteReader& r, std::uint16_t v, const TypeInfo&)
{
    RewardMarker m;
    m.rewardType = r.u32();
    m.unknown1 = r.u32();
    m.spells = readNames16(r);
    m.abilities = readNames16(r);
    m.creatures = readNames16(r);
    for (auto& x : m.unknown) {
        x = r.u32();
    }
    if (v > 0x3D) {
        m.activateChance = r.u32();
    }
    if (v > 0x3E) {
        m.rareOrSpecial = r.u8();
    }
    return m;
}

// Records older than kMinTypedVersion: keep everything that's left as bytes.
Data readRaw(ByteReader& r, std::uint16_t, const TypeInfo&)
{
    auto b = r.bytes(r.remaining());
    return Raw{{b.begin(), b.end()}};
}

// maxVersion is the newest version GAME.EXE accepts for that xfer; read() refuses anything newer.
const Codec kCodecs[] = {
    {"DefaultXfer", 0x3C, readDefault},
    {"SpellPagePedestalXfer", 0x3C, readSpellPagePedestal},
    {"ReadableXfer", 0x3C, readReadable},
    {"ExitXfer", 0x3C, readExit},
    {"DoorXfer", 0x3C, readDoor},
    {"TriggerXfer", 0x3D, readTrigger},
    {"HoleXfer", 0x3C, readHole},
    {"TransporterXfer", 0x3C, readTransporter},
    {"ElevatorXfer", 0x3D, readElevator},
    {"ElevatorShaftXfer", 0x3C, readElevatorShaft},
    {"MoverXfer", 0x3C, readMover},
    {"GlyphXfer", 0x3C, readGlyph},
    {"InvisibleLightXfer", 0x3C, readInvisibleLight},
    {"SentryXfer", 0x3D, readSentry},
    {"SpellRewardXfer", 0x3C, readSpellReward},
    {"AbilityRewardXfer", 0x3D, readAbilityReward},
    {"FieldGuideXfer", 0x3C, readFieldGuide},
    {"WeaponXfer", 0x40, readWeapon},
    {"ArmorXfer", 0x3E, readArmor},
    {"AmmoXfer", 0x3C, readAmmo},
    {"TeamXfer", 0x3C, readTeam},
    {"GoldXfer", 0x3C, readGold},
    {"ObeliskXfer", 0x3D, readObelisk},
    {"ToxicCloudXfer", 0x3D, readToxicCloud},
    {"RewardMarkerXfer", 0x3F, readRewardMarker},
    {"MonsterXfer", 0x40, [](ByteReader& r, std::uint16_t v, const TypeInfo& t) -> Data {
         return monster::readMonster(r, v, t);
     }},
    {"NPCXfer", 0x3E, [](ByteReader& r, std::uint16_t v, const TypeInfo&) -> Data { return monster::readNpc(r, v); }},
    {"MonsterGeneratorXfer", 0x3F, nullptr}, // needs the record offset; handled in decode()
};

// ---------------------------------------------------------------------------------------------
// Writers

// std::visit target that writes each Data alternative back in its stored order. `base` is the
// offset of the extra bytes in the record (for generator template alignment); `nested` re-encodes
// embedded records when the map CRC walk needs field-by-field calls.
struct Writer {
    ByteWriter& w;
    std::uint16_t v;
    const TypeInfo& t;
    std::size_t base;
    const RecordDataWriter& nested;

    void operator()(const Default&) const {}
    void operator()(const SpellPagePedestal& x) const { w.u32(x.spell); }
    void operator()(const Readable& x) const { writeString32(w, x.text); }
    void operator()(const Exit& x) const
    {
        writeString32(w, x.mapName);
        w.f32(x.x);
        w.f32(x.y);
    }
    void operator()(const Door& x) const
    {
        w.u32(x.currentDirection);
        w.u32(x.lock);
        w.u32(x.direction);
    }
    void operator()(const Trigger& x) const
    {
        w.i32(x.sizeX);
        w.i32(x.sizeY);
        writeBytewise(w, x.edgeColor); // one call per byte
        writeBytewise(w, x.backColor);
        w.u32(x.unknown1);
        writeScriptHandler(w, x.onPressed);
        writeScriptHandler(w, x.onReleased);
        writeScriptHandler(w, x.onCollided);
        w.u32(x.allowedClasses);
        w.u32(x.ignoredClasses);
        w.u8(x.allowedTeam);
        w.u8(x.ignoredTeam);
        if (v > 0x3C) {
            w.u8(x.state);
            w.u8(x.unknown2);
            w.u32(x.animFrame);
        }
    }
    void operator()(const Hole& x) const
    {
        w.u32(x.scriptActivated);
        writeScriptHandler(w, x.script);
        // The exe writes the fall point as one 8-byte call, which matters for the CRC.
        writeJoined(w, {static_cast<std::uint32_t>(x.fallX), static_cast<std::uint32_t>(x.fallY)});
        w.u32(x.scriptTime);
        w.u16(x.scriptTimeout);
    }
    void operator()(const Transporter& x) const { w.u32(x.targetExtent); }
    void operator()(const Elevator& x) const
    {
        w.u32(x.linkedExtent);
        w.u32(x.height);
        if (v > 0x3C) {
            w.u8(x.status);
        }
    }
    void operator()(const ElevatorShaft& x) const { w.u32(x.linkedExtent); }
    void operator()(const Mover& x) const
    {
        w.u32(x.movingSpeed);
        w.u32(x.waypoint);
        w.u32(x.movedExtent);
        w.u8(x.moveType);
        w.u32(x.waypointStart);
        w.u32(x.waypointEnd);
        w.f32(x.speedBase);
        w.f32(x.speedCurrent);
    }
    void operator()(const Glyph& x) const
    {
        w.u8(x.direction);
        w.f32(x.targetX);
        w.f32(x.targetY);
        w.u8(countOf<std::uint8_t>(x.spells.size(), "glyph spells"));
        for (const auto& s : x.spells) {
            writeString8(w, s);
        }
    }
    void operator()(const InvisibleLight& x) const
    {
        w.u32(x.type);
        w.f32(x.intensity);
        w.i32(x.radius);
        w.i32(x.intensityFx);
        writeJoined(w, {static_cast<std::uint32_t>(x.color[0]), static_cast<std::uint32_t>(x.color[1]),
                        static_cast<std::uint32_t>(x.color[2])});
        w.u16(x.direction);
        w.u16(x.cone);
        w.u32(x.omni);
        w.u16(x.animFlags);
        std::vector<std::uint8_t> keys; // 48 bytes in one call
        for (const auto& key : x.colorKeys) {
            keys.insert(keys.end(), key.begin(), key.end());
        }
        w.bytes(keys);
        writeArray(w, x.intensityKeys);
        writeArray(w, x.coneKeys);
        w.u16(x.colorStep);
        w.u16(x.intensityStep);
        w.u16(x.coneStep);
        w.u32(x.target);
        w.u16(x.rotationSpeed);
        w.u16(x.rotationMax);
        w.u8(x.unused);
        w.u32(x.isAnti);
    }
    void operator()(const Sentry& x) const
    {
        w.f32(x.basePosition);
        w.f32(x.rotateSpeed);
        if (v > 0x3C) {
            w.f32(x.currentPosition);
        }
    }
    void operator()(const SpellReward& x) const { writeString8(w, x.spell); }
    void operator()(const AbilityReward& x) const { writeString8(w, x.ability); }
    void operator()(const FieldGuide& x) const { writeString8(w, x.creature); }
    void operator()(const Weapon& x) const
    {
        writeModifiers(w, x.modifiers);
        // The type decides whether charges are stored, so a mismatch would produce bytes the game misreads.
        if (weaponHasCharges(v, t) != x.charges.has_value()) {
            throw FormatError("WeaponXfer: charges present/absent doesn't match the type");
        }
        if (x.charges) {
            w.u8(x.charges->current);
            w.u8(x.charges->max);
            if (v > 0x3C) {
                w.u32(x.charges->unknown);
            }
        }
        w.u16(x.health);
        if (v == 0x3F) {
            w.u8(x.unknown1);
        }
        if (v > 0x3F) {
            w.u32(x.unknown2);
        }
    }
    void operator()(const Armor& x) const
    {
        writeModifiers(w, x.modifiers);
        w.u16(x.health);
        if (v == 0x3D) {
            w.u8(x.unknown1);
        }
        if (v > 0x3D) {
            w.u32(x.unknown2);
        }
    }
    void operator()(const Ammo& x) const
    {
        writeModifiers(w, x.modifiers);
        w.u8(x.current);
        w.u8(x.limit);
    }
    void operator()(const Team& x) const { writeModifiers(w, x.modifiers); }
    void operator()(const Gold& x) const { w.u32(x.amount); }
    void operator()(const Obelisk& x) const
    {
        if (v > 0x3C) {
            w.u32(x.manaPool);
            w.u8(x.minimapKnown);
        }
    }
    void operator()(const ToxicCloud& x) const { w.u32(x.lifetime); }
    void operator()(const RewardMarker& x) const
    {
        w.u32(x.rewardType);
        w.u32(x.unknown1);
        writeNames16(w, x.spells);
        writeNames16(w, x.abilities);
        writeNames16(w, x.creatures);
        for (auto u : x.unknown) {
            w.u32(u);
        }
        if (v > 0x3D) {
            w.u32(x.activateChance);
        }
        if (v > 0x3E) {
            w.u8(x.rareOrSpecial);
        }
    }
    void operator()(const Monster& x) const { monster::writeMonster(w, x, v, t); }
    void operator()(const Npc& x) const { monster::writeNpc(w, x, v); }
    void operator()(const MonsterGenerator& x) const { monster::writeGenerator(w, x, v, base, nested); }
    void operator()(const Raw& x) const { w.bytes(x.bytes); }
};

// Linear search over the short codec table.
const Codec& findCodec(std::string_view xferName)
{
    for (const auto& codec : kCodecs) {
        if (codec.name == xferName) {
            return codec;
        }
    }
    throw FormatError("unknown xfer " + std::string(xferName));
}

} // namespace

// Version checks first: newer than the exe reads is refused, older than the typed layouts can't be
// streamed (there is no length to fall back on outside a map record).
Data read(ByteReader& r, std::string_view xferName, std::uint16_t xferVersion, const TypeInfo& type,
          std::size_t extraOffset)
{
    const Codec& codec = findCodec(xferName);
    if (xferVersion > codec.maxVersion) {
        r.fail(std::string(xferName) + " version " + std::to_string(xferVersion) + " is newer than the game reads");
    }
    if (xferVersion < kMinTypedVersion) {
        r.fail(std::string(xferName) + " version " + std::to_string(xferVersion) + " is too old to decode");
    }
    if (!codec.read) {
        return monster::readGenerator(r, xferVersion, extraOffset);
    }
    return codec.read(r, xferVersion, type);
}

Data decode(std::string_view xferName, std::uint16_t xferVersion, std::span<const std::uint8_t> extra,
            const TypeInfo& type, std::size_t extraOffset)
{
    // Old records: still check the xfer name is known, then keep the bytes untouched.
    ByteReader r(extra, xferName);
    if (xferVersion < kMinTypedVersion) {
        findCodec(xferName);
        return readRaw(r, xferVersion, type);
    }
    Data d = read(r, xferName, xferVersion, type, extraOffset);
    // The record length is known here, so every byte must be accounted for.
    r.expectEnd();
    return d;
}

std::vector<std::uint8_t> encode(const Data& data, std::uint16_t xferVersion, const TypeInfo& type,
                                 std::size_t extraOffset)
{
    ByteWriter w;
    write(w, data, xferVersion, type, extraOffset);
    return std::move(w).take();
}

void write(ByteWriter& w, const Data& data, std::uint16_t xferVersion, const TypeInfo& type,
           std::size_t extraOffset, const RecordDataWriter& nested)
{
    std::visit(Writer{w, xferVersion, type, extraOffset, nested}, data);
}

TypeXfer typeXferOf(const things::ThingDef& type)
{
    const auto x = type.attribute("XFER");
    return {x ? std::string(*x) : "DefaultXfer", {things::thingClasses(type), things::thingSubclasses(type)}};
}

} // namespace nox::map::xfer
