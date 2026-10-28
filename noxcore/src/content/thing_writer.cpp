// thing.bin written back (thing_writer.hpp): each record as things/thing_db.cpp reads it, the
// other way round.
#include "noxcore/content/thing_writer.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nox::content {

namespace {

using things::ThingDb;

// --- Writing -----------------------------------------------------------------------------

void tag(ByteWriter& w, std::string_view t)
{
    std::string s(t);
    std::reverse(s.begin(), s.end()); // (stored byte-reversed)
    w.rawString(s);
}

void string8(ByteWriter& w, std::string_view s)
{
    if (s.size() > 255) {
        throw std::length_error("thing.bin: a text longer than 255 bytes: " + std::string(s.substr(0, 40)) + "...");
    }
    w.u8(static_cast<std::uint8_t>(s.size()));
    w.rawString(s);
}

void string16(ByteWriter& w, std::string_view s)
{
    if (s.size() > 65535) {
        throw std::length_error("thing.bin: a description longer than 65535 bytes");
    }
    w.u16(static_cast<std::uint16_t>(s.size()));
    w.rawString(s);
}

void imageRef(ByteWriter& w, const things::ImageRef& r)
{
    w.i32(r.index);
    if (r.index == -1) {
        w.u8(r.type);
        string8(w, r.name);
    }
}

/// A count preceded by zeros to a multiple of 8 from the file's start.
void alignedCount(ByteWriter& w, std::size_t n)
{
    w.align(8);
    w.u64(n);
}

void floor(ByteWriter& w, const things::FloorDef& f)
{
    w.u32(1);
    string8(w, f.name);
    w.bytes(f.minimapColor);
    w.u32(f.material);
    w.u32(f.flags);
    w.u8(f.unused);
    w.u8(f.cols);
    w.u8(f.rows);
    w.u8(f.frames);
    w.u8(f.frameDelay);
    for (const auto& i : f.images) {
        imageRef(w, i);
    }
    tag(w, "END ");
}

void edge(ByteWriter& w, const things::EdgeDef& e)
{
    w.u32(1);
    string8(w, e.name);
    w.u32(e.material);
    w.u32(e.flags);
    w.u8(e.unused);
    w.u8(e.frames);
    w.u8(e.frameDelay);
    w.u8(e.unknown);
    w.u8(e.cols);
    w.u8(e.rows);
    for (const auto& i : e.images) {
        imageRef(w, i);
    }
    tag(w, "END ");
}

void wall(ByteWriter& w, const things::WallDef& d)
{
    w.u32(1);
    string8(w, d.name);
    w.u32(d.flags);
    w.i32(d.material);
    w.i32(d.unused);
    w.u8(d.unused2);
    w.u8(d.health);
    alignedCount(w, d.debris.size());
    for (const auto& s : d.debris) {
        string8(w, s);
    }
    string8(w, d.openSound);
    string8(w, d.closeSound);
    string8(w, d.breakSound);
    w.u8(d.randomVariations);
    for (const auto& dir : d.directions) {
        alignedCount(w, dir.size());
        for (const auto& variant : dir) {
            for (const auto& img : variant) {
                w.i32(img.x);
                w.i32(img.y);
                imageRef(w, img.image);
            }
        }
    }
    tag(w, "END ");
}

void audio(ByteWriter& w, const things::AudioDef& a)
{
    string8(w, a.name);
    w.i16(a.hearing);
    w.u8(a.volume);
    w.i16(a.range);
    w.i8(a.maxInstances);
    w.i8(a.pitchMin);
    w.i8(a.pitchMax);
    w.i8(a.priority);
    for (const auto& f : a.files) {
        string8(w, f);
    }
    w.u8(0);
}

/// An AVNT's records, in `order` (its record types as the file gave them; any it has that the
/// order lacks after them in type order).
void avnt(ByteWriter& w, const things::AvntDef& a, std::vector<std::uint8_t> order)
{
    for (std::uint8_t t = 1; t <= 10; ++t) {
        const bool has = (t == 1 && a.priority) || (t == 2 && a.flags) || (t == 3 && a.volume) || (t == 4 && a.maxInstances) || (t == 5 && a.loops) ||
                         (t == 6 && a.pitch) || (t == 7 && a.files) || (t == 8 && a.delay) || (t == 9 && a.range) || (t == 10 && a.hearing);
        if (has && std::ranges::find(order, t) == order.end()) {
            order.push_back(t);
        }
    }
    string8(w, a.name);
    for (const std::uint8_t t : order) {
        switch (t) {
        case 1: if (a.priority) { w.u8(1); w.u8(*a.priority); } break;
        case 2: if (a.flags) { w.u8(2); w.u8(*a.flags); } break;
        case 3: if (a.volume) { w.u8(3); w.u8(*a.volume); } break;
        case 4: if (a.maxInstances) { w.u8(4); w.u8(*a.maxInstances); } break;
        case 5: if (a.loops) { w.u8(5); w.u8(*a.loops); } break;
        case 6: if (a.pitch) { w.u8(6); w.i8(a.pitch->first); w.i8(a.pitch->second); } break;
        case 7:
            if (a.files) {
                w.u8(7);
                for (const auto& f : *a.files) {
                    string8(w, f);
                }
                w.u8(0);
            }
            break;
        case 8: if (a.delay) { w.u8(8); w.u32(a.delay->first); w.u32(a.delay->second); } break;
        case 9: if (a.range) { w.u8(9); w.i16(*a.range); } break;
        case 10: if (a.hearing) { w.u8(10); w.i16(*a.hearing); } break;
        default: break;
        }
    }
    w.u8(0);
}

void spell(ByteWriter& w, const things::SpellDef& s)
{
    string8(w, s.id);
    w.u8(s.manaCost);
    w.u16(s.price);
    w.u8(static_cast<std::uint8_t>(s.phonemes.size()));
    w.bytes(s.phonemes);
    imageRef(w, s.icon);
    imageRef(w, s.iconEnabled);
    w.u32(s.flags);
    string8(w, s.title);
    string16(w, s.description);
    string8(w, s.castSound);
    string8(w, s.onSound);
    string8(w, s.offSound);
}

void ability(ByteWriter& w, const things::AbilityDef& a)
{
    string8(w, a.name);
    w.u8(a.unknown);
    for (const auto& i : a.icons) {
        imageRef(w, i);
    }
    string8(w, a.title);
    string16(w, a.description);
    for (const auto& s : a.sounds) {
        string8(w, s);
    }
}

void image(ByteWriter& w, const things::ImageDef& d)
{
    string8(w, d.name);
    if (d.image) {
        w.i8(1);
        imageRef(w, *d.image);
    } else if (d.animation) {
        w.i8(2);
        w.u8(static_cast<std::uint8_t>(d.animation->frames.size()));
        w.u8(d.animation->frameDelay);
        string8(w, d.animation->kind);
        for (const auto& f : d.animation->frames) {
            imageRef(w, f);
        }
    } else {
        throw std::invalid_argument("thing.bin: named image " + d.name + " has neither an image nor an animation");
    }
}

void binaryAttr(ByteWriter& w, const things::ThingDef& t, const ThingBinaryAttr& b)
{
    string8(w, b.line);
    switch (b.kind) {
    case ThingBinaryAttr::Draw: {
        const things::ThingDraw& d = b.earlierDraw ? *b.earlierDraw : *t.draw;
        string8(w, d.type);
        alignedCount(w, d.data.size());
        w.bytes(d.data);
        break;
    }
    case ThingBinaryAttr::MenuIcon: imageRef(w, b.earlierImage ? *b.earlierImage : *t.menuIcon); break;
    case ThingBinaryAttr::PrettyImage: imageRef(w, b.earlierImage ? *b.earlierImage : *t.prettyImage); break;
    }
}

void thing(ByteWriter& w, const things::ThingDef& t, const std::vector<ThingBinaryAttr>* binary)
{
    // The binary attributes it has, where the layout put them (new ones after the text lines).
    const auto has = [&](ThingBinaryAttr::Kind kind) {
        return kind == ThingBinaryAttr::Draw ? t.draw.has_value() : kind == ThingBinaryAttr::MenuIcon ? t.menuIcon.has_value() : t.prettyImage.has_value();
    };
    std::vector<ThingBinaryAttr> bin;
    if (binary) {
        for (const auto& b : *binary) { // (in the file's order, earlier repeats of one included)
            if (has(b.kind)) {
                bin.push_back(b);
                bin.back().before = std::min(bin.back().before, t.attributes.size());
            }
        }
    }
    for (const auto& [kind, line] : {std::pair{ThingBinaryAttr::Draw, "DRAW"}, std::pair{ThingBinaryAttr::MenuIcon, "MENUICON"},
                                     std::pair{ThingBinaryAttr::PrettyImage, "PRETTYIMAGE"}}) {
        if (has(kind) && std::ranges::none_of(bin, [&](const ThingBinaryAttr& o) { return o.kind == kind; })) {
            bin.push_back({t.attributes.size(), line, kind, {}, {}}); // (new: after the text lines)
        }
    }
    std::ranges::stable_sort(bin, [](const ThingBinaryAttr& a, const ThingBinaryAttr& b) { return a.before < b.before; });
    string8(w, t.name);
    std::size_t next = 0;
    for (std::size_t i = 0; i <= t.attributes.size(); ++i) {
        while (next < bin.size() && bin[next].before == i) {
            binaryAttr(w, t, bin[next++]);
        }
        if (i < t.attributes.size()) {
            if (t.attributes[i].empty()) {
                throw std::invalid_argument("thing.bin: " + t.name + " has an empty attribute line (it would end the type)");
            }
            string8(w, t.attributes[i]);
        }
    }
    w.u8(0);
}

// --- Reading the layout --------------------------------------------------------------------

std::string readTag(ByteReader& r)
{
    std::string t = r.rawString(4);
    std::reverse(t.begin(), t.end());
    return t;
}

std::string readString8(ByteReader& r)
{
    return r.rawString(r.u8());
}

things::ImageRef readImageRef(ByteReader& r)
{
    things::ImageRef ref;
    ref.index = r.i32();
    if (ref.index == -1) {
        ref.type = r.u8();
        ref.name = readString8(r);
    }
    return ref;
}

/// A THNG's binary attributes and where they sit, read from the file (an earlier one of a kind
/// given again keeps its value: the reader keeps only the last).
std::vector<ThingBinaryAttr> scanThing(ByteReader& r)
{
    std::vector<ThingBinaryAttr> out;
    readString8(r); // (the name)
    std::size_t text = 0;
    for (;;) {
        std::string attr = readString8(r);
        if (attr.empty()) {
            break;
        }
        const std::string_view key = util::trim(std::string_view(attr).substr(0, attr.find('=')));
        if (util::equalsNoCase(key, "DRAW")) {
            ThingBinaryAttr b{text, attr, ThingBinaryAttr::Draw, {}, {}};
            things::ThingDraw d;
            d.type = readString8(r);
            r.align(8);
            const auto payload = r.bytes(static_cast<std::size_t>(r.u64()));
            d.data.assign(payload.begin(), payload.end());
            b.earlierDraw = std::move(d);
            out.push_back(std::move(b));
        } else if (util::equalsNoCase(key, "MENUICON") || util::equalsNoCase(key, "PRETTYIMAGE")) {
            ThingBinaryAttr b{text, attr, util::equalsNoCase(key, "MENUICON") ? ThingBinaryAttr::MenuIcon : ThingBinaryAttr::PrettyImage, {}, {}};
            b.earlierImage = readImageRef(r);
            out.push_back(std::move(b));
        } else {
            ++text;
        }
    }
    // The last of each kind is the type's own (written from it): only earlier ones keep a value.
    for (const auto kind : {ThingBinaryAttr::Draw, ThingBinaryAttr::MenuIcon, ThingBinaryAttr::PrettyImage}) {
        for (auto it = out.rbegin(); it != out.rend(); ++it) {
            if (it->kind == kind) {
                it->earlierDraw.reset();
                it->earlierImage.reset();
                break;
            }
        }
    }
    return out;
}

/// An AVNT's record types in file order.
std::vector<std::uint8_t> scanAvnt(ByteReader& r)
{
    std::vector<std::uint8_t> order;
    readString8(r);
    for (;;) {
        const std::uint8_t t = r.u8();
        if (t == 0) {
            return order;
        }
        order.push_back(t);
        switch (t) {
        case 6: r.skip(2); break;
        case 7:
            while (const std::uint8_t len = r.u8()) {
                r.skip(len);
            }
            break;
        case 8: r.skip(8); break;
        case 9:
        case 10: r.skip(2); break;
        default: r.skip(1); break;
        }
    }
}

struct Writer {
    const ThingDb& db;
    const ThingLayout* layout;
    std::vector<std::vector<std::uint8_t>> avntOrder; // (scan only)

    void section(ByteWriter& w, const ThingSection& s, std::size_t thingIndex) const
    {
        switch (s.list) {
        case ThingList::Floor: tag(w, "FLOR"); floor(w, db.floors.at(s.first)); break;
        case ThingList::Edge: tag(w, "EDGE"); edge(w, db.edges.at(s.first)); break;
        case ThingList::Wall: tag(w, "WALL"); wall(w, db.walls.at(s.first)); break;
        case ThingList::Audio:
            tag(w, "AUD ");
            w.i32(static_cast<std::int32_t>(s.count));
            for (std::size_t i = 0; i < s.count; ++i) audio(w, db.audio.at(s.first + i));
            break;
        case ThingList::Avnt:
            tag(w, "AVNT");
            avnt(w, db.avnts.at(s.first), s.first < avntOrder.size() ? avntOrder[s.first] : std::vector<std::uint8_t>{});
            break;
        case ThingList::Spell:
            tag(w, "SPEL");
            w.u32(static_cast<std::uint32_t>(s.count));
            for (std::size_t i = 0; i < s.count; ++i) spell(w, db.spells.at(s.first + i));
            break;
        case ThingList::Ability:
            tag(w, "ABIL");
            w.u32(static_cast<std::uint32_t>(s.count));
            for (std::size_t i = 0; i < s.count; ++i) ability(w, db.abilities.at(s.first + i));
            break;
        case ThingList::Image:
            tag(w, "IMAG");
            w.u32(static_cast<std::uint32_t>(s.count));
            for (std::size_t i = 0; i < s.count; ++i) image(w, db.images.at(s.first + i));
            break;
        case ThingList::Thing: {
            tag(w, "THNG");
            const auto* bin = layout && thingIndex < layout->binary.size() ? &layout->binary[thingIndex] : nullptr;
            thing(w, db.things.at(s.first), bin);
            break;
        }
        }
    }
};

void finish(ByteWriter& w)
{
    w.zeros(4); // (the zero tag)
    w.align(crypto::Blowfish::kBlockSize);
}

} // namespace

ThingLayout scanThingLayout(std::span<const std::uint8_t> plain, const ThingDb& db)
{
    ThingLayout layout;
    ByteReader r(plain, "thing.bin");
    ByteWriter w;
    Writer writer{db, &layout, {}};
    std::size_t counts[9] = {};
    for (;;) {
        const std::size_t start = r.offset();
        if (r.remaining() < 4) {
            r.fail("no end tag");
        }
        const std::string t = readTag(r);
        if (t == std::string(4, '\0')) {
            break;
        }
        ThingSection s;
        const auto one = [&](ThingList list) {
            s.list = list;
            s.first = counts[static_cast<int>(list)]++;
            s.count = 1;
        };
        const auto group = [&](ThingList list, std::size_t n) {
            s.list = list;
            s.first = counts[static_cast<int>(list)];
            s.count = n;
            counts[static_cast<int>(list)] += n;
        };
        if (t == "FLOR") {
            one(ThingList::Floor);
        } else if (t == "EDGE") {
            one(ThingList::Edge);
        } else if (t == "WALL") {
            one(ThingList::Wall);
        } else if (t == "AUD ") {
            group(ThingList::Audio, static_cast<std::size_t>(r.i32()));
        } else if (t == "AVNT") {
            one(ThingList::Avnt);
            writer.avntOrder.resize(s.first + 1);
            writer.avntOrder[s.first] = scanAvnt(r);
        } else if (t == "SPEL") {
            group(ThingList::Spell, r.u32());
        } else if (t == "ABIL") {
            group(ThingList::Ability, r.u32());
        } else if (t == "IMAG") {
            group(ThingList::Image, r.u32());
        } else if (t == "THNG") {
            one(ThingList::Thing);
            layout.binary.push_back(scanThing(r));
        } else {
            r.fail("unknown section tag");
        }
        // The section written from `db` must be the file's own bytes; the reader goes on from there.
        writer.section(w, s, s.list == ThingList::Thing ? s.first : 0);
        if (w.size() > plain.size() || std::memcmp(w.data().data() + start, plain.data() + start, w.size() - start) != 0) {
            std::size_t at = start;
            while (at < w.size() && at < plain.size() && w.data()[at] == plain[at]) {
                ++at;
            }
            const auto hex = [&](const std::uint8_t* p, std::size_t n) {
                std::string out;
                for (std::size_t i = 0; i < n; ++i) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += kHex[p[i] >> 4];
                    out += kHex[p[i] & 15];
                    out += ' ';
                }
                return out;
            };
            const std::size_t from = at >= 8 ? at - 8 : 0;
            const std::size_t n = std::min<std::size_t>({24, w.size() - from, plain.size() - from});
            throw FormatError("thing.bin: section " + t + " at " + std::to_string(start) + " doesn't write back as it was read (from byte " +
                              std::to_string(at - start) + " of it; file " + hex(plain.data() + from, n) + "/ written " + hex(w.data().data() + from, n) +
                              ")");
        }
        r.skip(w.size() - r.offset());
        layout.sections.push_back(s);
    }
    layout.valid = true;
    layout.avnt = std::move(writer.avntOrder);
    return layout;
}

std::vector<std::uint8_t> writeThingDb(const ThingDb& db, const ThingLayout& layout)
{
    ByteWriter w;
    Writer writer{db, &layout, layout.avnt};
    std::size_t things = 0;
    for (const ThingSection& s : layout.sections) {
        if (s.list == ThingList::Thing && s.first >= db.things.size()) {
            continue; // (a type the db no longer has)
        }
        writer.section(w, s, s.first);
        if (s.list == ThingList::Thing) {
            things = std::max(things, s.first + 1);
        }
    }
    // Types beyond the layout's (appended), each a section of its own.
    for (std::size_t i = things; i < db.things.size(); ++i) {
        writer.section(w, ThingSection{ThingList::Thing, i, 1}, i);
    }
    finish(w);
    return std::move(w).take();
}

std::vector<std::uint8_t> encryptThingDb(const ThingDb& db, const ThingLayout& layout)
{
    auto bytes = writeThingDb(db, layout);
    crypto::cipherForKey(crypto::FileKey::ThingBin).encrypt(bytes);
    return bytes;
}

} // namespace nox::content
