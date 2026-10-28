// thing.bin: the game database of floor tiles, edges, walls, sounds, spells, abilities, images
// and object types ("things").
//
// Decrypted layout: a sequence of sections, each introduced by a 4-character tag stored
// byte-reversed (a little-endian u32 of 'FLOR' reads "ROLF"), ended by a zero tag and zero
// padding. Alignment ("align8") is counted from the start of the file.
//
// Sources: OpenNox libs `things` (MIT) and NoXLib ThingDb.cs, cross-checked by parsing the
// retail file with every byte accounted for. Fields whose meaning isn't known yet are named
// unknownN; draw payloads are kept undecoded until the renderer needs them.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nox::things {

/// Reference to a sprite: an index into video.bag, or (index -1) a named image.
struct ImageRef {
    std::int32_t index = -1;
    std::uint8_t type = 0; // named form only
    std::string name;      // named form only
};

/// An animation's playback mode (its `kind` text), named once as it's read; Other for any
/// other text.
enum class AnimPlayback : std::uint8_t { Other, OneShot, OneShotRemove, Loop, LoopAndFade, Random, Slave };

/// The playback mode its kind text names.
inline AnimPlayback animPlayback(std::string_view kind)
{
    using enum AnimPlayback;
    return kind == "OneShot"         ? OneShot
           : kind == "OneShotRemove" ? OneShotRemove
           : kind == "Loop"          ? Loop
           : kind == "LoopAndFade"   ? LoopAndFade
           : kind == "Random"        ? Random
           : kind == "Slave"         ? Slave
                                     : Other;
}

/// Frame sequence. `kind` is the playback mode as text: OneShot, OneShotRemove, Loop,
/// LoopAndFade, Random or Slave.
struct Animation {
    std::uint8_t frameDelay = 0; // game frames per animation frame
    std::string kind;
    std::vector<ImageRef> frames; // at most 255
    AnimPlayback playback = AnimPlayback::Other; ///< `kind`, named
};

// FLOR, EDGE and WALL layouts are verified against the 1.2b loaders (thing_read_FLOR, thing_read_EDGE,
// thing_read_WALL_def). Each record starts with a u32 that is always 1 (skipped by the game).

/// Object material flags, shared with THNG MATERIAL. Picks footstep and impact sounds
/// (8 wood, 0x40 earth, 0x80 liquid, 0x400 snow, 0x800 mud, 2/0x4000 silent, else stone).
using MaterialFlags = std::uint32_t;

/// FLOR: one floor tile type. Map FloorTile::image indexes this list.
struct FloorDef {
    std::string name;
    std::array<std::uint8_t, 3> minimapColor{}; // RGB; FF00FF means none
    MaterialFlags material = 0;
    std::uint32_t flags = 0; // only Lava sets 0x1000; unused by 1.2b code
    std::uint8_t unused = 0;
    std::uint8_t cols = 0;
    std::uint8_t rows = 0;
    std::uint8_t frames = 0;     // animated tiles, e.g. water = 6
    std::uint8_t frameDelay = 0; // unused by 1.2b code
    std::vector<ImageRef> images; // frames * cols * rows
};

/// EDGE: one tile-edge type. Map FloorEdge::edge indexes this list.
struct EdgeDef {
    std::string name;
    MaterialFlags material = 0;
    std::uint32_t flags = 0;
    std::uint8_t unused = 0;
    std::uint8_t frames = 0;
    std::uint8_t frameDelay = 0;
    std::uint8_t unknown = 0; // the game refuses to load the edge if this is 1
    std::uint8_t cols = 0;
    std::uint8_t rows = 0;
    std::vector<ImageRef> images; // 2 * (cols + rows) * frames
};

struct WallImage {
    std::int32_t x = 0; // draw offset
    std::int32_t y = 0;
    ImageRef image;
};

/// Four images per variant.
using WallVariant = std::array<WallImage, 4>;

/// WALL: one wall material. Map Wall::material indexes this list.
struct WallDef {
    std::string name;
    /// 1 blocks sight, 2 ignored by the map line trace, 4 see-through and sprite-sorted
    /// (fences, cages, magic walls).
    std::uint32_t flags = 0;
    std::int32_t material = 0; // MaterialFlags; impact sounds
    std::int32_t unused = 0;
    std::uint8_t unused2 = 0;
    std::uint8_t health = 0;         // destructible walls
    std::vector<std::string> debris; // up to 8 object types spawned when broken
    std::string openSound;
    std::string closeSound;
    std::string breakSound;
    std::uint8_t randomVariations = 0; // modulus when auto-placing straight walls
    std::array<std::vector<WallVariant>, 15> directions;
};

/// AUD: a sound event and the sound files it can play (read by client and server). The
/// event's name is one of the exe's sound names (audio/sound_names.hpp); the game sets its flags
/// to 2 (a random file each time).
struct AudioDef {
    std::string name;
    std::int16_t hearing = 0;     ///< kept by both sides (event +8); no client code reads it
    std::uint8_t volume = 0;      ///< percent (stored x163, 0x4000 = full)
    std::int16_t range = 0;       ///< in 15-unit steps; 0 or less keeps the default 600 units
    std::int8_t maxInstances = 0; ///< voices of it at once, 0 = no limit
    std::int8_t pitchMin = 0;     ///< playback-rate percent added to 100, a random pick in
    std::int8_t pitchMax = 0;     ///< [pitchMin, pitchMax] per play
    std::int8_t priority = 0;     ///< 0-2 (the game refuses the file otherwise)
    std::vector<std::string> files; ///< Audio.bag sample names, extension included
};

/// AVNT: a sound event given field by field (client thing_read_AVNT, and the server). Each record is a
/// type byte and its value; a field not given keeps its default (or an AUD record's value).
struct AvntDef {
    std::string name;
    std::optional<std::uint8_t> priority;     ///< 1
    std::optional<std::uint8_t> flags;        ///< 2: 1 loop, 2 random file, 4 play the files in
                                              ///< sequence, 8 wait a random delay between plays
    std::optional<std::uint8_t> volume;       ///< 3: percent
    std::optional<std::uint8_t> maxInstances; ///< 4
    std::optional<std::uint8_t> loops;        ///< 5: times through the files when looping, 0 = forever
    std::optional<std::pair<std::int8_t, std::int8_t>> pitch;  ///< 6: min, max
    std::optional<std::vector<std::string>> files;             ///< 7
    std::optional<std::pair<std::uint32_t, std::uint32_t>> delay; ///< 8: min, max milliseconds
    std::optional<std::int16_t> range;   ///< 9: in 15-unit steps
    std::optional<std::int16_t> hearing; ///< 10
};

struct SpellDef {
    std::string id;
    std::uint8_t manaCost = 0;
    std::uint16_t price = 0;
    std::vector<std::uint8_t> phonemes; // 0..8
    ImageRef icon;
    ImageRef iconEnabled;
    std::uint32_t flags = 0;
    std::string title;       // string table id
    std::string description; // string table id
    std::string castSound;   // "NULL" for none
    std::string onSound;
    std::string offSound;
};

struct AbilityDef {
    std::string name;
    std::uint8_t unknown = 0;
    std::array<ImageRef, 3> icons;
    std::string title;
    std::string description;
    std::array<std::string, 3> sounds;
};

/// IMAG: a named image, either a single sprite or an animation.
struct ImageDef {
    std::string name;
    std::optional<ImageRef> image;
    std::optional<Animation> animation;
};

/// A THNG's DRAW attribute: the draw function's name and its payload, undecoded.
struct ThingDraw {
    std::string type;
    std::vector<std::uint8_t> data;
};

/// THNG: one object type.
struct ThingDef {
    std::string name;
    /// Text attributes in file order, as stored ("CLASS = MONSTER | IMMOBILE" etc.), except
    /// DRAW, MENUICON and PRETTYIMAGE, which carry binary data and are decoded below.
    std::vector<std::string> attributes;
    std::optional<ThingDraw> draw;
    std::optional<ImageRef> menuIcon;
    std::optional<ImageRef> prettyImage;

    /// Value of the first attribute named `key` (case-insensitive), trimmed, or nullopt.
    /// Attributes with no "=" have an empty value.
    std::optional<std::string_view> attribute(std::string_view key) const;
    /// Value of the last attribute named `key`: the game's parsers set their field for each
    /// line in turn, so a repeated one (the mine crystals' two LIGHTCOLORs) ends as the last.
    std::optional<std::string_view> lastAttribute(std::string_view key) const;

    /// (thingClasses, thingSubclasses and thingFlags' answers from `attributes`, made by
    /// ThingDb::reindex: a cache of what was loaded, stale once `attributes` change until
    /// reindex runs again. Without it they read the attributes.)
    struct Bits {
        std::uint32_t classes = 0;
        std::uint32_t subclasses = 0;
        std::uint32_t flags = 0;
    };
    std::optional<Bits> bits;
};

/// All of thing.bin. Each list keeps file order, which is what the game's indices refer to.
struct ThingDb {
    std::vector<FloorDef> floors;
    std::vector<EdgeDef> edges;
    std::vector<WallDef> walls;
    std::vector<AudioDef> audio;
    std::vector<AvntDef> avnts;
    std::vector<SpellDef> spells;
    std::vector<AbilityDef> abilities;
    std::vector<ImageDef> images;
    std::vector<ThingDef> things;

    /// First thing with this name (case-sensitive, as the game compares), or nullptr. (By the
    /// names' index when it's of `things` as they stand, else by walking the list: the same
    /// answer either way.)
    const ThingDef* findThing(std::string_view name) const;
    /// The one rebuild of every index and cache of this table: the names' index (each name's
    /// first entry), each thing's class bits (ThingDef::bits) and the spells' index (each spell
    /// id's first SPEL record, each record's spell id). parseThingDb calls it as the db is loaded;
    /// ANYTHING that changes `things` or `spells` after load (an overlay, an editor) must call it
    /// again. The size checks below only notice entries added or removed: an in-place change of
    /// the same size (a renamed thing, an attribute, a SPEL record's id) leaves the caches stale
    /// until this is called. Nothing of thing.bin's in the code: a cache of what was loaded.
    void reindex();

    /// (findThing's cache, reindex's: each name's first entry in `things`, and how many entries it
    /// was made from; any other count, and findThing walks the list instead.)
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, std::uint32_t, NameHash, std::equal_to<>> thingIndex;
    std::size_t thingIndexOf = static_cast<std::size_t>(-1);
    std::uint32_t reindexCount = 0; ///< (counted by each reindex: what's cached from `things` is made afresh)
    /// The SPEL record for a spell id (spell_ids.hpp), or nullptr: the first whose name is the
    /// id's. (By reindex's index when it's of `spells` as they stand, else by walking the list.)
    const SpellDef* findSpell(std::uint32_t id) const;
    /// The spell id SPEL record `index` names (spellId of its name), 0 when it names none.
    std::uint32_t spellNumber(std::size_t index) const;

    /// (findSpell's and spellNumber's cache, reindex's: each spell id's first record (-1 none),
    /// each record's spell id, and how many records they were made from.)
    std::vector<std::int32_t> spellIndex;
    std::vector<std::uint32_t> spellNumbers;
    std::size_t spellIndexOf = static_cast<std::size_t>(-1);
};

/// Parses decrypted thing.bin bytes. Throws FormatError.
ThingDb parseThingDb(std::span<const std::uint8_t> plain);

/// Reads and decrypts thing.bin.
ThingDb loadThingDb(const std::filesystem::path& path);

} // namespace nox::things
