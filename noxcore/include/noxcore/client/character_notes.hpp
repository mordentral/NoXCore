// (noxcpp) Where a saved character was last played, kept by NoX Core in a small file beside it:
// Save\<name>.plr's is Save\<name>.plr.noxcore. The .plr is the game's and is never written for
// it, so the original game reads it exactly as before; nothing that lists *.plr sees the note
// (its extension is ".noxcore"), and the original game's own listing never opens it.
//
// The file is plain text, one "key = value" a line (UTF-8, no control characters in a value):
//   noxcore-character-notes = 1
//   name = Jack                       (the character's name and class as its .plr has them:
//   class = 0                          a note left by a deleted character of the same file name
//                                      is passed over)
//   [place]                           (each place it was played, newest first, at most kMaxPlaces)
//   kind = mode | coop                 (a NoX Core mode, or the co-op campaign)
//   id = deep_delve                    (the mode's id, or the co-op campaign mod's; may be empty:
//                                       a remote host's mode is known by its name alone; the
//                                       game's own co-op campaign has none)
//   title = Deep Delve                 (the mode's name, or the campaign's title; the game's own
//                                       co-op campaign has none)
//   last = 1763600000                  (when it was last played there: seconds since 1970, UTC)
//   note = level 12, act 3             (optional: the mode's own line, nox.characterNote)
// Keys it doesn't know are passed over. Read once when the character list is made, never while
// the game draws.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::client::character_notes {

/// The places a note keeps (the oldest let go past them).
inline constexpr std::size_t kMaxPlaces = 8;
/// A title's bytes at most (UTF-8, cut at a whole character).
inline constexpr std::size_t kMaxTitleBytes = 64;
/// A mode's line's bytes at most (nox.characterNote's).
inline constexpr std::size_t kMaxNoteBytes = 80;
/// An id's bytes at most.
inline constexpr std::size_t kMaxIdBytes = 32;
/// The most of a file that's read (a bigger one is passed over).
inline constexpr std::uintmax_t kMaxFileBytes = 8 * 1024;

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

/// A character's note.
struct Notes {
    std::string name;       ///< (UTF-8)
    int characterClass = 0; ///< 0 warrior, 1 wizard, 2 conjurer
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

/// `plr`'s note file: the same path with ".noxcore" after it.
std::filesystem::path fileOf(const std::filesystem::path& plr);

/// `text` as a note may keep it: valid UTF-8 only (a broken sequence dropped), no control
/// characters (C0, DEL, C1, the line and paragraph separators, the direction overrides and
/// isolates), runs of spaces one, no space at either end, at most `maxBytes` (cut at a whole
/// character).
std::string cleanText(std::string_view text, std::size_t maxBytes);

/// The note's text (what `write` puts in the file).
std::string format(const Notes& notes);
/// A note's text read; none when it isn't one (the first line's key, or too long). Each value is
/// cleaned (cleanText) and places past kMaxPlaces let go.
std::optional<Notes> parse(std::string_view text);

/// `plr`'s note, when there's one for the character it is: `name` (UTF-8) and `characterClass` as
/// its .plr has them (a note naming another is left where it is, and passed over).
std::optional<Notes> read(const std::filesystem::path& plr, std::string_view name, int characterClass);
/// `notes` written beside `plr` (to a scratch file first, then put in place). False: not written.
bool write(const std::filesystem::path& plr, const Notes& notes);
/// `plr`'s note gone (a character deleted); nothing when there's none.
void remove(const std::filesystem::path& plr);
/// `from`'s note moved to `to`'s (a character renamed), replacing one there; when `from` has none,
/// one at `to` goes (it was another character's).
void move(const std::filesystem::path& from, const std::filesystem::path& to);

/// Whether `place` is where `target` plays: the same kind, and the same id when both have one,
/// else the same title (letters' case aside; the game's own co-op campaign: both empty).
bool matches(const Place& place, const Target& target);

/// `notes` with `place` put first (the same place as one it has is that one moved up, its line
/// kept when `place` brings none), played at `now`; past kMaxPlaces the oldest go.
void played(Notes& notes, Place place, std::int64_t now);

/// A character list's order for `target`: `of[i]` the note of the list's i-th character (null:
/// none). The characters played at the target first, the most recent there first; then the rest
/// as the list had them, unless `onlyTarget` (then left out). With no target, the list as it is.
std::vector<std::size_t> order(const std::vector<const Notes*>& of, const Target& target, bool onlyTarget);

/// The place of `notes` a list shows first for `target`: the newest it was played at the target,
/// else its newest; null when it has none.
const Place* shownPlace(const Notes& notes, const Target& target);

} // namespace nox::client::character_notes
