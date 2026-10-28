// thing.bin parser (layout overview in thing_db.hpp). Walks the tagged sections in file order and
// fills one ThingDb list per tag; THNG attributes stay as text except the binary ones.
#include "noxcore/things/thing_db.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "thing_io.hpp"

#include <algorithm>
#include <cctype>

namespace nox::things {

namespace {

using detail::equalsNoCase;
using detail::readAnimation;
using detail::trim;
using detail::readImageRef;
using detail::readImageRefs;
using detail::readString8;

// u16 length, then the bytes (spell and ability descriptions).
std::string readString16(ByteReader& r)
{
    return r.rawString(r.u16());
}

/// Section tags are stored byte-reversed.
std::string readTag(ByteReader& r)
{
    std::string tag = r.rawString(4);
    std::reverse(tag.begin(), tag.end());
    return tag;
}

void expectEndTag(ByteReader& r)
{
    if (readTag(r) != "END ") {
        r.fail("expected END tag");
    }
}

template <std::size_t N>
void readBytes(ByteReader& r, std::array<std::uint8_t, N>& out)
{
    auto b = r.bytes(N);
    std::copy(b.begin(), b.end(), out.begin());
}

/// u64 count preceded by padding to a multiple of 8 from the file start.
std::size_t readAlignedCount(ByteReader& r)
{
    r.align(8);
    const std::uint64_t n = r.u64();
    if (n > r.remaining()) {
        r.fail("implausible count " + std::to_string(n));
    }
    return static_cast<std::size_t>(n);
}

/// Every FLOR/EDGE/WALL record starts with a u32 the game skips; it is always 1.
void expectRecordOne(ByteReader& r)
{
    if (r.u32() != 1) {
        r.fail("record header is not 1");
    }
}

// Layout: record u32 (1), name, RGB minimap colour, u32 material, u32 flags, five u8 (unused, cols,
// rows, frames, frame delay), frames * cols * rows image refs, END tag.
FloorDef readFloor(ByteReader& r)
{
    FloorDef f;
    expectRecordOne(r);
    f.name = readString8(r);
    readBytes(r, f.minimapColor);
    f.material = r.u32();
    f.flags = r.u32();
    f.unused = r.u8();
    f.cols = r.u8();
    f.rows = r.u8();
    f.frames = r.u8();
    f.frameDelay = r.u8();
    f.images = readImageRefs(r, std::size_t{f.frames} * f.cols * f.rows);
    expectEndTag(r);
    return f;
}

// Layout: record u32 (1), name, u32 material, u32 flags, six u8 (unused, frames, frame delay,
// unknown, cols, rows), 2 * (cols + rows) * frames image refs, END tag.
EdgeDef readEdge(ByteReader& r)
{
    EdgeDef e;
    expectRecordOne(r);
    e.name = readString8(r);
    e.material = r.u32();
    e.flags = r.u32();
    e.unused = r.u8();
    e.frames = r.u8();
    e.frameDelay = r.u8();
    e.unknown = r.u8();
    e.cols = r.u8();
    e.rows = r.u8();
    e.images = readImageRefs(r, 2 * (std::size_t{e.cols} + e.rows) * e.frames);
    expectEndTag(r);
    return e;
}

// Layout: record u32 (1), name, u32 flags, i32 material, i32, u8, u8 health, aligned count + debris
// names, open/close/break sounds, u8 variations, then for each of the 15 directions an aligned
// count of variants, 4 images per variant (i32 x, i32 y, image ref), END tag.
WallDef readWall(ByteReader& r)
{
    WallDef w;
    expectRecordOne(r);
    w.name = readString8(r);
    w.flags = r.u32();
    w.material = r.i32();
    w.unused = r.i32();
    w.unused2 = r.u8();
    w.health = r.u8();
    w.debris.resize(readAlignedCount(r));
    for (auto& d : w.debris) {
        d = readString8(r);
    }
    w.openSound = readString8(r);
    w.closeSound = readString8(r);
    w.breakSound = readString8(r);
    w.randomVariations = r.u8();
    for (auto& dir : w.directions) {
        dir.resize(readAlignedCount(r));
        for (auto& variant : dir) {
            for (auto& img : variant) {
                img.x = r.i32();
                img.y = r.i32();
                img.image = readImageRef(r);
            }
        }
    }
    expectEndTag(r);
    return w;
}

// One AUD section holds every event: i32 count; per event the fixed fields, then file names until
// an empty one.
void readAudio(ByteReader& r, std::vector<AudioDef>& out)
{
    const std::int32_t n = r.i32();
    for (std::int32_t i = 0; i < n; ++i) {
        AudioDef a;
        a.name = readString8(r);
        a.hearing = r.i16();
        a.volume = r.u8();
        a.range = r.i16();
        a.maxInstances = r.i8();
        a.pitchMin = r.i8();
        a.pitchMax = r.i8();
        a.priority = r.i8();
        for (;;) {
            std::string file = readString8(r);
            if (file.empty()) {
                break;
            }
            a.files.push_back(std::move(file));
        }
        out.push_back(std::move(a));
    }
}

// One AVNT section is one event: a name, then (type byte, value) records until type 0.
AvntDef readAvnt(ByteReader& r)
{
    AvntDef a;
    a.name = readString8(r);
    for (;;) {
        switch (const std::uint8_t type = r.u8()) {
        case 0:
            return a;
        case 1:
            a.priority = r.u8();
            break;
        case 2:
            a.flags = r.u8();
            break;
        case 3:
            a.volume = r.u8();
            break;
        case 4:
            a.maxInstances = r.u8();
            break;
        case 5:
            a.loops = r.u8();
            break;
        case 6: {
            const std::int8_t lo = r.i8();
            a.pitch = std::pair{lo, r.i8()};
            break;
        }
        case 7: {
            auto& files = a.files.emplace();
            // File names until a zero length.
            while (const std::uint8_t len = r.u8()) {
                files.push_back(r.rawString(len));
            }
            break;
        }
        case 8: {
            const std::uint32_t lo = r.u32();
            a.delay = std::pair{lo, r.u32()};
            break;
        }
        case 9:
            a.range = r.i16();
            break;
        case 10:
            a.hearing = r.i16();
            break;
        default:
            r.fail("unknown AVNT record type " + std::to_string(type));
        }
    }
}

// u32 count; per spell: id, u8 mana cost, u16 price, u8 phoneme count + phonemes (0..8), two image
// refs, u32 flags, title, u16-length description, three sound names.
void readSpells(ByteReader& r, std::vector<SpellDef>& out)
{
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n; ++i) {
        SpellDef s;
        s.id = readString8(r);
        s.manaCost = r.u8();
        s.price = r.u16();
        auto phonemes = r.bytes(r.u8());
        s.phonemes.assign(phonemes.begin(), phonemes.end());
        for (auto p : s.phonemes) {
            if (p > 8) {
                r.fail("invalid phoneme " + std::to_string(p));
            }
        }
        s.icon = readImageRef(r);
        s.iconEnabled = readImageRef(r);
        s.flags = r.u32();
        s.title = readString8(r);
        s.description = readString16(r);
        s.castSound = readString8(r);
        s.onSound = readString8(r);
        s.offSound = readString8(r);
        out.push_back(std::move(s));
    }
}

// u32 count; per ability: name, u8, three icons, title, u16-length description, three sound names.
void readAbilities(ByteReader& r, std::vector<AbilityDef>& out)
{
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n; ++i) {
        AbilityDef a;
        a.name = readString8(r);
        a.unknown = r.u8();
        for (auto& icon : a.icons) {
            icon = readImageRef(r);
        }
        a.title = readString8(r);
        a.description = readString16(r);
        for (auto& s : a.sounds) {
            s = readString8(r);
        }
        out.push_back(std::move(a));
    }
}

// u32 count; per image: name, then type 1 (one image ref) or type 2 (an animation).
void readImages(ByteReader& r, std::vector<ImageDef>& out)
{
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n; ++i) {
        ImageDef img;
        img.name = readString8(r);
        const std::int8_t type = r.i8();
        if (type == 1) {
            img.image = readImageRef(r);
        } else if (type == 2) {
            img.animation = readAnimation(r);
        } else {
            r.fail("invalid image type " + std::to_string(type));
        }
        out.push_back(std::move(img));
    }
}

/// Attribute key: the text before '=', trimmed.
std::string_view attributeKey(std::string_view attr)
{
    return trim(attr.substr(0, attr.find('=')));
}

/// Attribute value: the text after '=', trimmed (empty with no '=').
std::string_view attributeValue(std::string_view attr)
{
    const auto eq = attr.find('=');
    return eq == std::string_view::npos ? std::string_view{} : trim(attr.substr(eq + 1));
}

// Name, then u8-length attribute lines until an empty one. DRAW is followed by the draw function's
// name and an aligned u64-sized payload; MENUICON and PRETTYIMAGE by an image ref.
ThingDef readThing(ByteReader& r)
{
    ThingDef t;
    t.name = readString8(r);
    for (;;) {
        std::string attr = readString8(r);
        if (attr.empty()) {
            break;
        }
        const std::string_view key = attributeKey(attr);
        if (equalsNoCase(key, "DRAW")) {
            ThingDraw d;
            d.type = readString8(r);
            auto payload = r.bytes(readAlignedCount(r));
            d.data.assign(payload.begin(), payload.end());
            t.draw = std::move(d);
        } else if (equalsNoCase(key, "MENUICON")) {
            t.menuIcon = readImageRef(r);
        } else if (equalsNoCase(key, "PRETTYIMAGE")) {
            t.prettyImage = readImageRef(r);
        } else {
            t.attributes.push_back(std::move(attr));
        }
    }
    return t;
}

} // namespace

std::optional<std::string_view> ThingDef::attribute(std::string_view key) const
{
    for (const auto& attr : attributes) {
        if (equalsNoCase(attributeKey(attr), key)) {
            return attributeValue(attr);
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> ThingDef::lastAttribute(std::string_view key) const
{
    for (auto it = attributes.rbegin(); it != attributes.rend(); ++it) {
        if (equalsNoCase(attributeKey(*it), key)) {
            return attributeValue(*it);
        }
    }
    return std::nullopt;
}

void ThingDb::reindex()
{
    thingIndex.clear();
    thingIndex.reserve(things.size());
    for (std::size_t i = 0; i < things.size(); ++i) {
        thingIndex.try_emplace(things[i].name, static_cast<std::uint32_t>(i)); // (the first of a name kept)
    }
    thingIndexOf = things.size();
    for (ThingDef& t : things) {
        t.bits = detail::readBits(t);
    }
    // Each id's first record by name, as findSpell's walk finds it; each record's id as spellId has it.
    spellIndex.assign(kSpellCount, -1);
    for (std::uint32_t id = 1; id < kSpellCount; ++id) {
        const std::string_view name = spellName(id);
        for (std::size_t i = 0; i < spells.size(); ++i) {
            if (spells[i].id == name) {
                spellIndex[id] = static_cast<std::int32_t>(i);
                break;
            }
        }
    }
    spellNumbers.clear();
    spellNumbers.reserve(spells.size());
    for (const SpellDef& s : spells) {
        spellNumbers.push_back(spellId(s.id).value_or(0));
    }
    spellIndexOf = spells.size();
    ++reindexCount;
}

const ThingDef* ThingDb::findThing(std::string_view name) const
{
    if (thingIndexOf == things.size()) {
        const auto it = thingIndex.find(name);
        return it == thingIndex.end() ? nullptr : &things[it->second];
    }
    for (const auto& t : things) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

const SpellDef* ThingDb::findSpell(std::uint32_t id) const
{
    const std::string_view name = spellName(id);
    // Id 0 is SPELL_INVALID.
    if (id == 0 || name.empty()) {
        return nullptr;
    }
    if (spellIndexOf == spells.size()) {
        const std::int32_t i = spellIndex[id];
        return i < 0 ? nullptr : &spells[static_cast<std::size_t>(i)];
    }
    for (const auto& s : spells) {
        if (s.id == name) {
            return &s;
        }
    }
    return nullptr;
}

std::uint32_t ThingDb::spellNumber(std::size_t index) const
{
    return spellIndexOf == spells.size() ? spellNumbers[index] : spellId(spells[index].id).value_or(0);
}

ThingDb parseThingDb(std::span<const std::uint8_t> plain)
{
    ByteReader r(plain, "thing.bin");
    ThingDb db;
    // One tag per record or record group; a zero tag ends the data.
    while (!r.atEnd()) {
        const std::string tag = readTag(r);
        if (tag == "FLOR") {
            db.floors.push_back(readFloor(r));
        } else if (tag == "EDGE") {
            db.edges.push_back(readEdge(r));
        } else if (tag == "WALL") {
            db.walls.push_back(readWall(r));
        } else if (tag == "AUD ") {
            readAudio(r, db.audio);
        } else if (tag == "AVNT") {
            db.avnts.push_back(readAvnt(r));
        } else if (tag == "SPEL") {
            readSpells(r, db.spells);
        } else if (tag == "ABIL") {
            readAbilities(r, db.abilities);
        } else if (tag == "IMAG") {
            readImages(r, db.images);
        } else if (tag == "THNG") {
            db.things.push_back(readThing(r));
        } else if (tag == std::string(4, '\0')) {
            // End of data: the rest is zero padding to the cipher block.
            auto rest = r.bytes(r.remaining());
            // At most 7 padding bytes can follow, since the file is a whole number of cipher blocks.
            if (rest.size() >= 8 || std::any_of(rest.begin(), rest.end(), [](std::uint8_t b) { return b != 0; })) {
                r.fail("unexpected data after the end tag");
            }
        } else {
            r.fail("unknown section tag");
        }
    }
    db.reindex();
    return db;
}

ThingDb loadThingDb(const std::filesystem::path& path)
{
    auto data = readFileBytes(path);
    crypto::cipherForKey(crypto::FileKey::ThingBin).decrypt(data);
    ThingDb db = parseThingDb(data);
    content::applyContentPacksToThings(db); // (the installed content packs' changes to types, once, as it loads; none installed: nothing)
    return db;
}

} // namespace nox::things
