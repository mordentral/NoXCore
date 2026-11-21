// (noxcpp) The one way NoX Core writes a character file: every .plr it writes (a network character,
// a new one, a quest's or the co-op campaign's SAVEDATA kept, the client's sections after it, a solo
// save's Player.plr and its slot copies, the co-op host's party files, the content editor's) goes
// through here, and nothing else opens a .plr for writing (a test checks the source for it).
//
//   - Each write is whole: the new bytes go to "<file>.tmp" beside it, then take the file's place
//     (a rename over it). A failed write leaves the file as it was; the list of characters never
//     sees the scratch file (its extension isn't .plr).
//   - The bytes of the game's own sections are exactly the caller's (the same as a plain write of
//     them).
//   - The character's labels (where it was last played: client/character_notes.hpp) ride along.
//     The client keeps them in memory for the file of the character it plays (setLabels) and they
//     go into that file, as a section of NoX Core's own (SectionId::NoxCoreLabels, last before
//     the end), whenever the file is written whole (write) or its client sections are written
//     (appendSections). Nothing else writes them, save the game's end for a file that no write
//     carried them to (flushLabels), and that only in a game whose end writes the character in the
//     original game too (a network game that isn't a quest: gameEndWrites); in any other game
//     labels no write took are dropped. The original game skips a section it doesn't know, by its
//     size, everywhere it reads a .plr; it drops it when it writes the file itself.
#pragma once

#include "noxcore/player/player_file.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace nox::player::character_store {

/// A label section's data at most (a bigger one is passed over when read, and never written).
inline constexpr std::size_t kMaxLabelBytes = 4096;

/// `plain` (the file's bytes, unenciphered: a whole number of cipher blocks) enciphered as a .plr
/// is (Blowfish, key 27).
void encipher(std::vector<std::uint8_t>& plain);

/// `file` as the whole of `path` (serializePlayerFile, enciphered), with the labels kept for `path`
/// put in. Throws std::system_error when it can't be written (the file left as it was).
void write(const std::filesystem::path& path, const PlayerFile& file);
/// `enciphered` (a whole file as it goes on disk) as the whole of `path`. False: not written.
bool writeEnciphered(const std::filesystem::path& path, std::span<const std::uint8_t> enciphered);
/// `plain` filled with zeros to the cipher's block, enciphered, as the whole of `path`. False: not
/// written.
bool writePlain(const std::filesystem::path& path, std::span<const std::uint8_t> plain);
/// `plain` (whole cipher blocks) enciphered after what `path` holds (which must be there). False:
/// not written (nor when there's no file).
bool appendPlain(const std::filesystem::path& path, std::span<const std::uint8_t> plain);
/// The client's sections (`sections`, each its id, zeros to the block, its 64-bit size and its
/// data) after what `path` holds, then the labels kept for it, then the 0 id and zeros to the
/// block. False: not written (nor when there's no file).
bool appendSections(const std::filesystem::path& path, const std::vector<RawSection>& sections);
/// The bytes of `sections` as appendSections writes them (plain), with `labels`' section among
/// them when it's given.
std::vector<std::uint8_t> sectionBytes(const std::vector<RawSection>& sections, const std::vector<std::uint8_t>* labels = nullptr);
/// `from`'s bytes as the whole of `to`. False: not copied.
bool copy(const std::filesystem::path& from, const std::filesystem::path& to);

/// The label section's data in `file` (the last there, when it's one kMaxLabelBytes holds).
std::optional<std::vector<std::uint8_t>> labelsOf(const PlayerFile& file);
/// `file` with `labels` as its label section (none when empty), put last; the end's 0 id kept
/// after it.
void putLabels(PlayerFile& file, const std::vector<std::uint8_t>& labels);

/// The labels to go into `path` at its next write (their section's data; empty: none). They wait
/// in memory until a write of `path` carries them (write, appendSections) or flushLabels.
void setLabels(const std::filesystem::path& path, std::vector<std::uint8_t> labels);
/// Whether labels set for `path` wait for a write.
bool labelsWaiting(const std::filesystem::path& path);
/// Whether the end of a game with `gameFlags` (the game's flags as its client has them, 0x800 for a
/// solo or co-op campaign game) writes the player's character, as the original game's end does: a
/// network game's (0x2000), not a quest's (0x1000) nor a solo or campaign game's (0x800).
bool gameEndWrites(std::uint32_t gameFlags);
/// The game over for `path` (a game with `gameFlags`): when its end writes the character
/// (gameEndWrites), labels that no write carried written into it now (the file read, its labels
/// put in, written whole: its other sections byte for byte as they were); in any other game they
/// are dropped, the file left as it is. Forgotten either way. Nothing written when none wait, when
/// the file isn't there, or when it doesn't read back as it is. Returns whether it wrote.
bool flushLabels(const std::filesystem::path& path, std::uint32_t gameFlags);
/// Labels kept for `path` forgotten (unwritten ones let go).
void forgetLabels(const std::filesystem::path& path);
/// Every path's let go (tests).
void forgetAllLabels();

} // namespace nox::player::character_store
