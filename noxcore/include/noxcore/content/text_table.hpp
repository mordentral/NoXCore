// The game's text tables (gamedata.bin, monster.bin, soundset.bin, modifier.bin) as documents:
// the decrypted text kept as it is, with where each record and field sits in it, read the way
// each file's own reader reads it (things/game_data, monster_db, sound_sets, modifier). A field
// changed rewrites only its own bytes; everything else stays as the file had it, so a table read
// and written back unchanged is the stock file byte for byte.
//
// The same documents are content packs' patch files: a patch is a table in the same format
// holding only the records and fields that change (applyPatch, content/content_pack.hpp).
#pragma once

#include "noxcore/crypto/noxcrypt.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::content {

enum class TableFormat : std::uint8_t { GameData, Monster, SoundSet, Modifier };

inline constexpr TableFormat kTableFormats[] = {TableFormat::GameData, TableFormat::Monster, TableFormat::SoundSet, TableFormat::Modifier};

/// "gamedata.bin", "monster.bin", "soundset.bin", "modifier.bin".
const char* tableFileName(TableFormat format);
/// The Blowfish key the game reads it with.
crypto::FileKey tableKey(TableFormat format);

/// [begin, end) in the text.
struct TextSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t size() const { return end - begin; }
    bool empty() const { return end <= begin; }
};

/// Which kind of game a field is for: both, a solo game only (a SOLO line), a network (arena)
/// game only (ARENA). gamedata.bin and monster.bin have them; the others are always Both.
enum class GameKind : std::uint8_t { Both, Solo, Arena };

/// "", "SOLO", "ARENA".
const char* gameKindPrefix(GameKind kind);

struct TableField {
    GameKind kind = GameKind::Both;
    std::string key;   ///< as written (gamedata: the entry's name)
    std::string value; ///< as written, trimmed (gamedata / modifier: up to the ';'; monster / soundset: one word)
    TextSpan whole;    ///< the field from its key (gamedata: its prefix) to its value's end (modifier, gamedata: the ';')
    TextSpan prefixAt; ///< monster.bin: the SOLO / ARENA word it comes after on its line (several fields can share one)
    TextSpan valueAt;  ///< the value's text (empty and placed where it would go when the value is empty)
};

struct TableRecord {
    std::string name;    ///< gamedata: "" (the one record is the whole file)
    std::string section; ///< modifier.bin: WEAPON_DEFINITIONS, ARMOR_DEFINITIONS, EFFECTIVENESS, MATERIAL, ENCHANTMENT
    TextSpan nameAt;
    TextSpan whole;      ///< from its name to the end of its END (gamedata: the text before the padding)
    std::size_t insertAt = 0; ///< where a new field goes: its END's line (gamedata: the end of the text)
    std::vector<TableField> fields;

    /// The value a game of `kind` reads for `key` (the last of its lines that kind reads), or nullopt.
    /// Both: the value when solo and arena games read the same line.
    std::optional<std::string> effective(std::string_view key, GameKind kind) const;
    /// The index of that line, or nullopt.
    std::optional<std::size_t> effectiveIndex(std::string_view key, GameKind kind) const;
};

class TextTable {
public:
    /// Reads a decrypted table. Throws FormatError where its structure breaks (a record without
    /// END, modifier.bin's unknown section).
    static TextTable parse(TableFormat format, std::string text);
    /// Reads and decrypts a table file. Throws.
    static TextTable load(TableFormat format, std::span<const std::uint8_t> encrypted);
    /// Reads a patch file: the table's own format, plus '#' comment lines in every format (blanked
    /// before reading: monster.bin's reader would take one for a monster's name).
    static TextTable parsePatch(TableFormat format, std::string text);

    TableFormat format() const { return format_; }
    /// The text as it stands (with the cipher's zero padding at the end, as the file has it).
    const std::string& text() const { return text_; }
    const std::vector<TableRecord>& records() const { return records_; }
    /// A record by name as the game finds it (monster.bin: the first, case aside; soundset.bin:
    /// the last, exact; modifier.bin: the first in `section`, exact; gamedata.bin: the one
    /// record, any name). nullopt if none.
    std::optional<std::size_t> find(std::string_view name, std::string_view section = {}) const;

    // --- Edits (each one rewrites only its own bytes; the records are read again after it) ---

    /// Makes a game of `kind` read `value` for `key` in record `record`: the line it reads now
    /// gets the value if it is that kind's own (SOLO for Solo, ARENA for Arena, unprefixed for
    /// Both), else a line of that kind goes in after it (or at the record's end), so it wins.
    /// Both, where solo and arena games read different lines: each of them set. False (nothing
    /// changed) for a value the reader can't take back (valueFor).
    bool set(std::size_t record, std::string_view key, GameKind kind, std::string_view value);
    /// `value` as a table of `format` writes it so its reader takes it back whole, or nullopt if
    /// it can't be: monster.bin and soundset.bin values are one word (blanks only around a '+',
    /// which are dropped), never empty; gamedata.bin and modifier.bin ones one line without ';'.
    static std::optional<std::string> valueFor(TableFormat format, std::string_view value);
    /// Removes every field of `key` and exactly that `kind`. Returns how many.
    std::size_t remove(std::size_t record, std::string_view key, GameKind kind);
    /// Appends `recordText` (a whole record as the file writes one) at the end of the text,
    /// before the padding.
    void appendRecord(std::string_view recordText);

    /// The text encrypted as the game's file: padded with zeros to the cipher's 8-byte blocks
    /// (a text unchanged since it was read keeps the padding it came with).
    std::vector<std::uint8_t> encrypt() const;

    /// Where the game's reading of the file stops before its end, if it does (soundset.bin: an
    /// unknown key), as "record: key"; empty when it reads to the end.
    const std::string& stopped() const { return stopped_; }

    /// The line ending the file uses ("\r\n" or "\n").
    std::string_view newline() const { return crlf_ ? "\r\n" : "\n"; }

private:
    void reparse();
    void replace(TextSpan span, std::string_view with);
    std::string fieldLine(const TableRecord& r, GameKind kind, std::string_view key, std::string_view value) const;
    void insertField(std::size_t record, std::size_t at, GameKind kind, std::string_view key, std::string_view value);
    void setOne(std::size_t record, std::string_view key, GameKind kind, std::string_view value);

    TableFormat format_ = TableFormat::GameData;
    std::string text_;
    std::vector<TableRecord> records_;
    std::size_t contentEnd_ = 0; ///< where the zero padding starts
    bool crlf_ = false;
    bool changed_ = false;
    bool patch_ = false;  ///< read as a patch file (its "-KEY" lines and unknown keys kept, for checking)
    std::string stopped_;
};

/// What applying a patch did: each change as "Record.KEY" (with SOLO / ARENA), and what it
/// couldn't do.
struct PatchResult {
    std::vector<std::string> changed;
    std::vector<std::string> added;   ///< records the base didn't have, appended
    std::vector<std::string> removed;
    std::vector<std::string> problems;
};

/// Applies `patch` (a table of the same format holding the records and fields that change) to
/// `base`: each patch field set as `set` does (a key written "-KEY" removes that key's lines of
/// its kind instead); a record the base doesn't have is appended as the patch writes it.
PatchResult applyPatch(TextTable& base, const TextTable& patch);

} // namespace nox::content
