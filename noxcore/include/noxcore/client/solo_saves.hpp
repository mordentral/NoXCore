// A solo game's saved games as the host's own client sees them (GUISave.c),
// and the client's part of a save and a load (Plr_LoadClientPlayerData).
//
//   slots      AUTOSAVE and SAVE0001-SAVE0013 under <game dir>\Save. The save menu reads
//              each one's Player.plr with the File Info reader: a slot is there when the file opens and its
//              File Info section (if any) reads; its lines then show the character ("%s the %s",
//              GUISave.c:the, with nox.str:<Class>), " - " and the place its stage names (or " "
//              and GUISave.c:AutoSaveLabel for AUTOSAVE), and the save's date and time
//              ("%S, %S ": GetDateFormatA DATE_SHORTDATE, GetTimeFormatA without seconds or marker,
//              24 hours), in one colour; an empty one GUISave.c:EmptySlot (AUTOSAVE's
//              " "), grey. The newest is selected (savegame_findLatestSave).
//   the save   the client appends its sections to the Player.plr the server wrote: the
//              spell bars (GUI 7), the File Info (1: parseFileInfoData's write, the
//              slot's header) and the music (12), then the closing 0 id, the close filling the
//              cipher block.
//   a quest's  a network client keeps the SAVEDATA its host sends as its character's file
//              (keepSaveData), then appends the same sections.
#pragma once

#include "noxcore/player/player_file.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::client::solo_saves {

/// The slots the save menu lists (14).
inline constexpr int kSlots = 14;

/// AUTOSAVE for 0, else SAVE%04d.
std::string slotName(int index);

struct Slot {
    std::string name;         ///< AUTOSAVE, SAVE0001...
    std::filesystem::path plr; ///< Save\<name>\Player.plr
    bool exists = false;      ///< the File Info reader read it (the exe's path copied in)
    bool infoRead = false;    ///< a File Info section read (copied into the record)
    player::FileInfo info;    ///< its File Info (zeros without one)
};

/// Save\<name>\Player.plr read for its File Info.
Slot read(const std::filesystem::path& gameDir, int index);
/// The File Info reader on any .plr (the character list's Save\*.plr too): `exists` when it opened and
/// its File Info (if any) read; `name` left empty.
Slot readPlr(const std::filesystem::path& plr);
/// Each of the 14.
std::vector<Slot> list(const std::filesystem::path& gameDir);
/// savegame_findLatestSave: the slot saved last (the first of equals), -1 for none.
int latest(const std::vector<Slot>& slots);
/// A slot's lines' palette colour: 10 for a File Info flagged 8, else 13 for AUTOSAVE
/// and 14 for the others; 3 an empty slot's.
int lineColor(const Slot& slot);
/// The save menu's place by the save's stage (1-11) and class: a string id.
std::string_view stageLabel(int stage, int characterClass);
/// The class table (Warrior, Wizard, Conjurer), for "nox.str:%s".
std::string_view className(int characterClass);

/// The slot's date and time as the save menu shows them, "%S, %S ": GetDateFormatA
/// (LOCALE_SYSTEM_DEFAULT, DATE_SHORTDATE) and GetTimeFormatA (no seconds, no time marker, 24
/// hours); off Windows "M/D/YYYY, H:MM ".
std::string whenText(const std::array<std::uint16_t, 8>& time);

/// The local time as GetLocalTime's SYSTEMTIME (year, month, weekday, day, hour, minute, second,
/// milliseconds).
std::array<std::uint16_t, 8> localTime();

/// What parseFileInfoData writes (as `held`): out of the network
/// (game flag 0x2000 off in `gameFlags`) flags 1 set, 2 / 4 off (cleared 8 just
/// before); in it, 1 off and 4 set in a quest (game flag 0x1000), else 2 (8 kept); the .plr's
/// path, the unused string kept, the time now, the colours, clothes, name and class held, female
/// 0 (it's zeroed before the write), `level`, the map's name and `stage`; version 12.
player::FileInfo saveFileInfo(player::FileInfo held, const std::string& plrPath, const std::array<std::uint16_t, 8>& now,
                              std::uint8_t level, const std::string& mapName, std::uint8_t stage,
                              std::uint32_t gameFlags = 0);

/// A SAVEDATA transfer (the server's sections, plain: the received-file handler's action
/// 2) kept as the character's file `plr`. A client that isn't the host refuses one shorter than
/// kMinSaveData (Wol.c:WolApierror). Else `saveDir` is made (CreateDirectoryA:
/// <work dir>\Save\), the file written anew enciphered (key 0x1B: the size's low 16 bits of
/// bytes, binfile_close filling the last block with zeros). The client's own sections
/// follow (appendClientSections), then "CharacterSaved".
inline constexpr std::size_t kMinSaveData = 700;
enum class KeptSave : std::uint8_t {
    Written,
    TooShort,  ///< refused: Wol.c:WolApierror on the console (12) and in the middle
    Unwritten, ///< the file couldn't be opened (only the network log says so)
};
KeptSave keepSaveData(const std::filesystem::path& saveDir, const std::filesystem::path& plr,
                      std::span<const std::uint8_t> data, bool host);

/// The client's own sections, plain: each section (its id, zeros to the block, its 64-bit size, its
/// data) in the table's order (g_plrClientSections: GUI, File Info, Music), then the 0 id and zeros to the
/// block.
std::vector<std::uint8_t> clientSectionBytes(const player::Gui& gui, const player::FileInfo& info, const player::Music& music);
/// The same, enciphered and appended to `plr` (which ends on a block: the server's sections);
/// false when it couldn't be written.
bool appendClientSections(const std::filesystem::path& plr, const player::Gui& gui, const player::FileInfo& info,
                          const player::Music& music);

} // namespace nox::client::solo_saves
