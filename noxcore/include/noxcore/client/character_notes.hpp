// (noxcpp) Where a saved character was last played, kept by NoX Core inside the character's own
// .plr, as a section of its own (player::SectionId::NoxCoreLabels, after the game's sections). The
// original game skips a section it doesn't know, by its size, wherever it reads a .plr, so it reads
// the file exactly as before; when it writes the file itself it leaves the section out. The section
// is written only by NoX Core's character writer (player/character_store.hpp), when it writes the
// character anyway.
//
// The section's data (little-endian, every length counted, nothing past the places read):
//   u16 version = 1
//   u8  places                 (at most kMaxPlaces, newest first)
//   each place:
//     u8  kind                 0 a NoX Core mode, 1 the co-op campaign
//     u8  id length, id        (the mode's id, or the co-op campaign mod's; may be empty: a remote
//                               host's mode is known by its name alone; the game's own campaign has
//                               none; at most kMaxIdBytes)
//     u8  title length, title  (the mode's name, or the campaign's title; the game's own co-op
//                               campaign has none; at most kMaxTitleBytes)
//     i64 last                 (when it was last played there: seconds since 1970, UTC)
//     u8  note length, note    (optional: the mode's own line, nox.characterNote; at most
//                               kMaxNoteBytes)
// The text is UTF-8, cleaned as it's read and written (cleanText). Read once when the character list
// is made, never while the game draws.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::client::character_notes {

/// The places a character's labels keep (the oldest let go past them).
inline constexpr std::size_t kMaxPlaces = 8;
/// A title's bytes at most (UTF-8, cut at a whole character).
inline constexpr std::size_t kMaxTitleBytes = 64;
/// A mode's line's bytes at most (nox.characterNote's).
inline constexpr std::size_t kMaxNoteBytes = 80;
/// An id's bytes at most.
inline constexpr std::size_t kMaxIdBytes = 32;
/// The section's version.
inline constexpr std::uint16_t kVersion = 1;

/// Where a character was played.
struct Place {
    enum class Kind : std::uint8_t { Mode, Coop };
    Kind kind = Kind::Mode;
    std::string id;
    std::string title;
    std::int64_t last = 0;
    std::string note;
    bool operator==(const Place&) const = default;
};

/// A character's labels.
struct Notes {
    std::vector<Place> places; ///< newest first
    bool operator==(const Notes&) const = default;
};

/// What a character screen is choosing a character for (none: no mode, a stock or arena game).
struct Target {
    Place::Kind kind = Place::Kind::Mode;
    std::string id;
    std::string title;
    bool any = false; ///< a mode or campaign at all
};

/// `text` as the labels may keep it: valid UTF-8 only (a broken sequence dropped), no control
/// characters (C0, DEL, C1, the line and paragraph separators, the direction overrides and
/// isolates), runs of spaces one, no space at either end, at most `maxBytes` (cut at a whole
/// character).
std::string cleanText(std::string_view text, std::size_t maxBytes);

/// The labels as their section's data (each text cleaned, places past kMaxPlaces let go). Empty:
/// no places (no section).
std::vector<std::uint8_t> encode(const Notes& notes);
/// A section's data read; none when it isn't one (another version, a length past its bound or
/// the data's end, a kind it doesn't know, more than player::character_store::kMaxLabelBytes).
/// Each text is cleaned; places are read newest first.
std::optional<Notes> decode(std::span<const std::uint8_t> data);

/// Whether `place` is where `target` plays: the same kind, and the same id when both have one,
/// else the same title (letters' case aside, the target's cleaned as the labels' are: cleanText;
/// the game's own co-op campaign: both empty).
bool matches(const Place& place, const Target& target);

/// `notes` with `place` put first (the same place as one it has is that one moved up, its line
/// kept when `place` brings none), played at `now`; past kMaxPlaces the oldest go.
void played(Notes& notes, Place place, std::int64_t now);

/// A character list's order for `target`: `of[i]` the labels of the list's i-th character (null:
/// none). The characters played at the target first, the most recent there first; then the rest
/// as the list had them, unless `onlyTarget` (then left out). With no target, the list as it is.
std::vector<std::size_t> order(const std::vector<const Notes*>& of, const Target& target, bool onlyTarget);

/// The place of `notes` a list shows first for `target`: the newest it was played at the target,
/// else its newest; null when it has none.
const Place* shownPlace(const Notes& notes, const Target& target);

} // namespace nox::client::character_notes
