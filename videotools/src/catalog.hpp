// The browsable view of video.bag: categories of entries (a monster, a floor, a spell...), each a
// list of sequences (an animation, a set of tiles or icons), each a set of tracks (directions,
// variants) of frames that are video.bag image indices.
//
// thing.bin gives the structure: every THNG's DRAW payload, FLOR/EDGE/WALL images, spell and
// ability icons and IMAG named images. video.bag images nothing there references (GUI art the
// game loads by name, unused art) are grouped by name under "Other bag images", so every image
// in the bag is reachable from some entry.
#pragma once

#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/video/video_bag.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace videotools {

/// What a sequence's tracks stand for.
enum class TrackKind { None, Direction, Variant, Slot };

std::string_view trackKindName(TrackKind kind);

struct Track {
    std::string name;                  // "S", "v0", "icon"; empty for TrackKind::None
    std::vector<std::int32_t> frames;  // video.bag indices; -1 = a reference that didn't resolve
    /// Wall images only, per frame: the draw offset thing.bin stores beside the image.
    std::vector<std::array<std::int32_t, 2>> drawOffsets;
};

struct Sequence {
    std::string group; // e.g. a player state; empty for most sequences
    std::string name;  // e.g. "ATTACK", "NAKED", "tiles"
    std::string kind;  // playback mode (Loop, OneShot...); empty for stills
    int frameDelay = -1; // game frames per animation frame minus one; -1 when not an animation
    int state = -1;      // monster state number, AnimateState slot, conditional index
    std::string sound;
    TrackKind trackKind = TrackKind::None;
    std::vector<Track> tracks;
    /// Extra facts for the metadata, as (key, value) text pairs (floor cols/rows etc.).
    std::vector<std::pair<std::string, std::string>> info;

    /// "group/name", or "name".
    std::string path() const;
    std::size_t imageCount() const;
};

struct Entry {
    std::string name;
    std::string source;   // THNG, FLOR, EDGE, WALL, SPELL, ABILITY, IMAG or BAG
    std::string drawType; // THNG DRAW function name
    int thingId = 0;      // THNG network type id (index + 1); 0 when not a THNG
    std::vector<std::string> attributes; // THNG text attributes
    std::vector<Sequence> sequences;
    std::int32_t icon = -1; // a representative image

    std::size_t imageCount() const;
};

struct Category {
    std::string name;
    std::vector<Entry> entries; // sorted by name

    std::size_t imageCount() const;
};

struct Catalog {
    std::vector<Category> categories;
    std::size_t bagImages = 0;
    std::size_t referencedImages = 0; // distinct images thing.bin reaches
    std::size_t unresolvedRefs = 0;   // named references with no such image in the bag

    const Category* findCategory(std::string_view name) const;
    /// An entry by name (case-insensitive), optionally "Category/Entry"; nullptr if none or
    /// ambiguous (then `ambiguous` is set).
    const Entry* findEntry(std::string_view name, const Category** category = nullptr,
                           bool* ambiguous = nullptr) const;
};

Catalog buildCatalog(const nox::things::ThingDb& things, const nox::video::VideoBag& bag);

/// Lower-case copy (ASCII).
using nox::util::lowered;

} // namespace videotools
