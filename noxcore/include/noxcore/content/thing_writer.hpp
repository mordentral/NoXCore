// thing.bin written back: every record type the reader (things/thing_db) reads, serialised the
// way the file stores it, in the file's own section order. A stock file read and written back is
// the same bytes (tested). The layout (which sections, in which order, and where a type's binary
// attributes sit among its text lines) comes from the file it was read from, since ThingDb keeps
// only the lists.
#pragma once

#include "noxcore/things/thing_db.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nox::content {

/// Which list a section's records go to.
enum class ThingList : std::uint8_t { Floor, Edge, Wall, Audio, Avnt, Spell, Ability, Image, Thing };

struct ThingSection {
    ThingList list = ThingList::Thing;
    std::size_t first = 0; ///< its first record in that list
    std::size_t count = 0; ///< how many (one, but for AUD, SPEL, ABIL, IMAG: their count)
};

/// Where a THNG's DRAW, MENUICON and PRETTYIMAGE lines sit among its text attributes, and how
/// each was written.
struct ThingBinaryAttr {
    std::size_t before = 0; ///< it comes before text attribute `before` (attributes.size(): last)
    std::string line;       ///< the attribute's own line as stored ("DRAW", "MENUICON"...)
    enum Kind : std::uint8_t { Draw, MenuIcon, PrettyImage } kind = Draw;
    /// A type giving the same one more than once: the reader keeps the last; an earlier one's
    /// value is kept here (it is written back as it was, and read over as the game reads it).
    std::optional<things::ThingDraw> earlierDraw;
    std::optional<things::ImageRef> earlierImage;
};

struct ThingLayout {
    std::vector<ThingSection> sections;
    std::vector<std::vector<ThingBinaryAttr>> binary; ///< by THNG index
    std::vector<std::vector<std::uint8_t>> avnt;      ///< each AVNT's record types in file order
    bool valid = false;
};

/// The layout of a decrypted thing.bin, checked against `db` (its parse): each section is
/// written again from `db` and must give the bytes the file has. Throws FormatError where not.
ThingLayout scanThingLayout(std::span<const std::uint8_t> plain, const things::ThingDb& db);

/// The decrypted bytes of `db` laid out as `layout` says (things beyond the layout's are added at
/// the end, each in a THNG section of its own; their binary attributes come after their text
/// ones), ended with the zero tag and padded to the cipher's blocks.
std::vector<std::uint8_t> writeThingDb(const things::ThingDb& db, const ThingLayout& layout);

/// writeThingDb, encrypted as the game's file.
std::vector<std::uint8_t> encryptThingDb(const things::ThingDb& db, const ThingLayout& layout);

} // namespace nox::content
