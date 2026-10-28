// The content model's schema: for each field of the text tables (monster.bin, soundset.bin,
// modifier.bin, gamedata.bin), its type, range, what it does, which side of a game reads it (from
// NoX Core's own readers) and so how far a change reaches. The forms, the checks (Problems) and
// the patch files all come from it.
#pragma once

#include "noxcore/content/text_table.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace videotools::content {

/// Which side of a game reads a field (side.hpp's meaning: the host's gameplay, the player's
/// drawing, sounds and texts, or both).
enum class ReadBy : std::uint8_t { Unknown, Server, Client, Both };

/// How far a change to it reaches.
///  - ServerOnly: takes effect for everyone in a game hosted with the pack (stock clients too).
///  - ClientVisible: only players who installed the pack see it; nobody else is affected.
///  - ServerShown: gameplay on the host, and clients without the pack show the old figure
///    (a cooldown bar, an identify page) where they show it at all.
///  - Structural: changes ids the network carries (thing.bin's type list, modifier.bin's
///    modifier order): every player needs the same pack.
enum class Reach : std::uint8_t { ServerOnly, ClientVisible, ServerShown, Structural };

const char* readByLabel(ReadBy r);   ///< "server", "client", "both", "?"
const char* reachLabel(Reach r);     ///< "server-only", "client-visible", "server, shown by clients", "structural"
const char* reachDocs(Reach r);      ///< a sentence for tooltips
Reach reachOf(ReadBy r);
/// The stronger of two reaches (the pack's overall reach is the strongest of its changes).
Reach stronger(Reach a, Reach b);

enum class FieldType : std::uint8_t {
    Int,       ///< whole number (atoi)
    Float,     ///< number (atof)
    Numbers,   ///< gamedata.bin: up to 255 numbers
    Text,      ///< free text, one word where the file reads words
    StringId,  ///< a nox.csf label ("modifier.db:LongswordDesc")
    Thing,     ///< a thing.bin type name
    Sound,     ///< a sound name (audio/sound_names.hpp)
    Flags,     ///< names joined by '+'
    Enum,      ///< one of `names`
    Rgb,       ///< "r g b", 0-255 each
    Slot,      ///< COLOR1 .. COLOR6
    Function,  ///< an engine function's name (DIE_FUNCTION...): must be one the engine has
    Effect,    ///< a modifier effect: a function and its number
    Allowed,   ///< ALLOWED_WEAPONS / ALLOWED_ARMOR: names joined by + and -
};

struct FieldSpec {
    std::string_view key;
    FieldType type = FieldType::Int;
    ReadBy readBy = ReadBy::Unknown;
    std::string_view docs;
    double min = 0, max = 0;            ///< for numbers: the range the game reads sensibly (both 0: none)
    std::span<const std::string_view> names; ///< Flags / Enum
    bool gameKinds = false;             ///< has SOLO / ARENA lines (monster.bin, gamedata.bin)
    std::string_view readers;           ///< where NoX Core reads it (for the curious: file:line)
    bool structural = false;            ///< both sides must have the same value (thing.bin's CLASS...)
    bool exactCase = false;             ///< Enum / Flags names compared exactly (modifier.bin's reader)
};

/// The fields a table's records take, in the order the forms list them. modifier.bin's depend on
/// the record's section.
std::span<const FieldSpec> tableFields(nox::content::TableFormat format, std::string_view section = {});
/// One by key (case-insensitive), or nullptr for a key the game doesn't read.
const FieldSpec* tableField(nox::content::TableFormat format, std::string_view key, std::string_view section = {});

/// gamedata.bin's names NoX Core reads, each with its spec (Numbers, the side, docs). A name
/// not here is read by nothing (a mod's own name, read through nox.gameNumber, is server-only).
std::span<const FieldSpec> gameDataNames();
const FieldSpec* gameDataName(std::string_view name);

/// thing.bin's object-type attributes NoX Core reads (THNG: CLASS, HEALTH, DRAW, MENUICON...),
/// with who reads each. Some decide which map objects a client loads itself and which the
/// server sends (CLASS, FLAGS, COLLIDE, XFER): those reach Structural (every player needs the
/// same pack).
std::span<const FieldSpec> thingFields();
const FieldSpec* thingField(std::string_view key);
/// A field's reach (Structural where the schema says so).
Reach reachOf(const FieldSpec& spec);

/// A value checked against its spec: empty when it's fine, else why not.
std::string checkValue(const FieldSpec& spec, std::string_view value);

} // namespace videotools::content
