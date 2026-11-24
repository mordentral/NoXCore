// NPC dialog on the server (the campaign's), and freezing a unit.
//
//   - A map script gives an NPC (a monster) its dialog: SetDialog its
//     start and end functions, its type (NORMAL 0, YESNO 1, NEXT 2, YESNONEXT 3, by name) and
//     xStatus 0x10 (clients show it as one to talk to); CancelDialog takes them away; StoryPic its portrait.
//   - A player's click on it (MSG_DIALOG 1, SrvNet_OnPacket) or the script's StartDialog
//     runs the start function (SrvNet_OnDialogStart: the player as caller, the
//     NPC as trigger). The start function's TellStory (-> SrvNet_OpenNpcDialog)
//     opens the dialog on the player's screen: the player talks to the NPC (DialogWith), MSG_DIALOG
//     3 (135 bytes: the text's id, the NPC's name, the speech's sound, the portrait, the type), the
//     player frozen.
//   - Its answer (MSG_DIALOG 2, SrvNet_OnDialogAnswer) unfreezes it and, while the NPC
//     still has its functions, closes the dialog (MSG_DIALOG 4), keeps a YESNO answer for
//     GetAnswer and runs the end function. IsTalking: the host in a dialog.
//   - While talking, a player doesn't leave by an exit (solo.hpp's mayAct), pick up,
//     drop or work things (SrvNet_OnPacket 0x72 / 0x73 / 0x7B).
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/monsters.hpp" // (freezeUnit / unfreezeUnit: the dialog's freeze)
#include "noxcore/server/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::server {

/// A dialog type by its name (case counts); 0 for an unknown one.
std::uint8_t dialogType(std::string_view name);
/// SetDialog: a monster's dialog, when both functions are given (-1: not).
void setDialog(ServerObject& npc, std::uint8_t type, std::int32_t start, std::int32_t end);
/// CancelDialog: a monster's functions -1, xStatus 0x10 off.
void cancelDialog(ServerObject& npc);
/// StoryPic: a monster's portrait.
void storyPic(ServerObject& npc, std::string_view picture);
/// The label of an NPC's name in the string table: "NPC:" + its script name after any ':' without
/// '_', or its type's.
std::string npcNameLabel(const ServerObject& npc);
/// MSG_DIALOG 3's name at +0x22 written as npcDialogMessage writes it (31 UTF-16 letters, the rest
/// zeros); nothing for a shorter message.
void putDialogTitle(std::vector<std::uint8_t>& m, std::u16string_view name);
/// (noxcpp) A mode's name for that title (ModeHooks::playerText's answer) cut to its 31 UTF-16 letters
/// at a whole character: a surrogate pair the 31st letter would split is left out, as nox.tellStory's
/// speaker (strings::fromUtf8Cut) is. The string table's names are written as they are. `letters`:
/// another field's length (the shop window's title, 24).
std::u16string fitDialogTitle(std::u16string name, std::size_t letters = 31);
/// `text` written as a field of `letters` UTF-16 letters at `at` in `m` (wcsncpy's: its letters up
/// to its first NUL, then zeros), `m` grown to hold it where it's shorter.
void putWideField(std::vector<std::uint8_t>& m, std::size_t at, std::u16string_view text, std::size_t letters);
/// (noxcpp) A NoX Core mode's own text for an NPC's name label (npcNameLabel) for this player
/// (World::modeHooks' playerText), fitted to `letters` (fitDialogTitle); none without a mode, or
/// when it has none.
std::optional<std::u16string> modeNpcTitle(const World& world, const ServerObject& player, std::string_view label, std::size_t letters = 31);
/// (noxcpp) The NPC's name as a window opened for this player titles it: modeNpcTitle, else the
/// string table's (World::text of its label), as the game's.
std::u16string npcTitleFor(const World& world, const ServerObject& player, const ServerObject& npc, std::size_t letters = 31);
/// MSG_DIALOG 3 as SrvNet_OpenNpcDialog builds it (135 bytes; what the exe leaves of its stack
/// as zeros): its text's id at +2, the NPC's name at +0x22 (Str_GetString("NPC:" + its script
/// name after any ':' without '_', or its type's), 31 UTF-16 letters), 0 at +0x60, the sound at
/// +0x62, the portrait at +0x66 ("ShopKeeperPic" for none), the type at +0x86. The strings are
/// copied as the game copies it, unbounded: a long text id runs on under what's written after it, a
/// portrait of 32 letters or more over the type (an original bug, kept).
/// (noxcpp: `name`, a script mode's nox.tellStory's speaker, in place of the NPC's string-table
/// name; null: the NPC's, as TellStory has it.)
std::vector<std::uint8_t> npcDialogMessage(const World& world, const ServerObject& npc, std::uint32_t sound, std::string_view textId,
                                           const std::u16string* name = nullptr);
/// SrvNet_OpenNpcDialog (TellStory): a player talks to `npc` (DialogWith), sent MSG_DIALOG 3 and
/// frozen (monsters.hpp's freezeUnit, lock 0). (`name` as npcDialogMessage's. noxcpp: with none
/// given, a NoX Core mode's own text for the NPC's name label for this player first: World::modeHooks'
/// playerText; without a mode, or when it has none, the string table's.)
void openNpcDialog(Fight& fight, ServerObject* player, ServerObject* npc, std::uint32_t sound, std::string_view textId,
                   const std::u16string* name = nullptr);
/// SrvNet_OnDialogStart (MSG_DIALOG 1, StartDialog): a player not dead nor gone, a monster with
/// xStatus 0x10 and both functions: its start function runs (caller the player, trigger the NPC).
void dialogStart(Fight& fight, ServerObject& player, ServerObject& npc);
/// SrvNet_OnDialogAnswer (MSG_DIALOG 2): the player unfrozen (lock 0); its NPC with both functions:
/// MSG_DIALOG 4 to it, no longer talking, the answer kept (a YESNO's; else 0), the end function
/// run.
void dialogAnswer(Fight& fight, ServerObject& player, std::uint8_t answer);
/// GetAnswer: a monster's kept answer, else 0.
std::uint8_t dialogAnswerOf(const ServerObject* o);

} // namespace nox::server
