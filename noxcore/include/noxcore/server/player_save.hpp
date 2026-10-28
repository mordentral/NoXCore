// A player's .plr on the server: the sections the server writes (Plr_SaveServerPlayerData)
// and applies (cliPlrInfoLoadFromFile), as GAME.EXE does them.
//
//   - Saving: the server section table (g_plrServerSections: Attrib 2, Status 3, Inventory 4, FieldGuide 8,
//     Spellbook 5, Enchantment 6, Journal 9, GameData 10, Pad 11) in order, each writer's
//     section into a file crypted with key 27 (player_file.hpp's layout). A writer that fails
//     fails the whole save. What each takes (the "has" byte: a section's state follows it only
//     when set):
//       Attrib v5: the game (4 a quest, 1 a solo game, else 2), the info block's
//         name (at most 24 letters), speed, strength, max mana and health, class, sex,
//         colours, clothing and +0x58; its lives (fails past MaxExtraLives); its highest stage.
//       Status v2: a solo game's health and mana (most, now), poison, slowness and
//         its timer, experience, facing.
//       Inventory v3 (none in a network game but a quest; fails outside solo and
//         quest games): gold, the items (a quest's but keys and glyphs) each as its type's name
//         and its xfer (WeaponXfer 0x40, ArmorXfer 0x3E, AbilityRewardXfer 0x3D, the rest
//         0x3C, each the object header 0x40 of mapReadWriteObjData first), the
//         worn ones' script ids, the secondary weapon and last quiver (the host's own
//         inventory window's: none on a server), its glyphs out; a quest's fails with ten of a
//         potion or more of the Infinite Pain wand than ForceOfNatureStaffLimit; then
//         MSG_INVENTORY_LOADED (0x71) to its client.
//       FieldGuide v1, Spellbook v3 (a warrior's abilities, with their
//         levels): as the inventory's "has".
//       Enchantment v5, Journal v1: a solo game's (else empty; failing
//         outside one): the enchantments on it in the order with their frames left,
//         power and a shield's strength (0x64 without its spell); a warrior's Berserker and
//         Tread Lightly under way, the latter's frames left, the cool-downs (but a charging
//         Berserker's); the journal oldest first.
//       GameData v5: nothing in a network game; else the next script id, the map's
//         name (twice its length written: the bug player_file.hpp keeps), the scripts' quest
//         variables (a solo game's), the magic walls (newest first), the host's briefing byte.
//       Pad: to the 8-byte block.
//   - Loading: a solo game's god mode off (every player's book, guide and abilities reset); its
//     health and mana kept (its book, guide and abilities reset, level 1, numbers
//     anew, full, no enchantments, spells or poison, idle); its .plr "in transfer" (no INFORMs to
//     it, quest sounds from it, item uses, weapons worn on pickup...); each server section in the
//     file's order (others skipped), applied by its reader; on a failure the inventory's things
//     go and it's false. Then its level by its experience and its numbers anew,
//     its health and mana to what the file (or it before) had, out of transfer.
#pragma once

#include "noxcore/player/player_file.hpp"
#include "noxcore/server/combat.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nox::server {

/// noxcpp (a solo game's guest told another game's flags, GameHost::Options::clientGameFlags):
/// its .plr written or read as that game's (`flags` the world's game flags for the while), the
/// world's own put back after.
class GameFlagsAs {
public:
    GameFlagsAs(World& world, std::uint32_t flags)
        : world_(world)
        , was_(world.gameFlags)
    {
        world_.gameFlags = flags;
    }
    ~GameFlagsAs() { world_.gameFlags = was_; }
    GameFlagsAs(const GameFlagsAs&) = delete;
    GameFlagsAs& operator=(const GameFlagsAs&) = delete;

private:
    World& world_;
    std::uint32_t was_;
};

/// Plr_SaveServerPlayerData's sections for `player` (a player's unit) as a file; nullopt where
/// the exe's save fails (a section's writer returns 0). Not encrypted.
std::optional<player::PlayerFile> buildPlayerFile(Fight& fight, ServerObject& player);
/// noxcpp: a character of another game written as a quest's (the world's flags a quest's for the
/// while: GameFlagsAs), with only what a quest's read takes (its items, potions, spells and
/// guide's creatures by the quest's checks); the player as it was after.
std::optional<player::PlayerFile> buildQuestFile(Fight& fight, ServerObject& player);
/// Plr_SaveServerPlayerData: the whole file as the exe writes it (the sections,
/// padded, encrypted with key 27); empty when it fails.
std::vector<std::uint8_t> savePlayerData(Fight& fight, ServerObject& player);
/// SrvNet_SendSaveDataToClient's "SAVEDATA" for a file Plr_SaveServerPlayerData wrote: its
/// decrypted bytes as far as computeServerPlayerDataBufferSize counts (its server
/// sections), that count's low 16 bits (an original bug); empty, nothing sent.
std::vector<std::uint8_t> saveDataSent(const player::PlayerFile& file);

/// cliPlrInfoLoadFromFile: `file` (a whole .plr, encrypted) applied to `player`.
/// False where the exe's fails (a section's reader fails; and a file that isn't a .plr), with
/// `disconnect` set when the exe also drops the player (Plr_DisconnectByIndex: a quest's file in
/// another game or the other way round).
/// `playerOnly` (noxcpp: a solo game's guest): its GameData section skipped (the quest variables,
/// the magic walls, the briefing byte and the saved map's name are the host's game's). A character
/// no solo game saved (no GameData of its own: a network or quest character) comes in with a new
/// solo character's book and field guide (nothing known, not a quest's first spells): its
/// Spellbook and FieldGuide sections skipped too.
bool loadPlayerData(Fight& fight, ServerObject& player, std::span<const std::uint8_t> file, bool* disconnect = nullptr,
                    bool playerOnly = false);
/// A received SAVE_SERVER (action 3) on the server: a client's saved player as it sent it
/// (CliNet_SendSaveServerXfer: the server sections as its copy has them, decrypted; the
/// bytes it never writes, those after each id up to the size's block, the size's high half and
/// the last 4, whatever its buffer held), written out (SavePlayerDataFromClient)
/// and loaded (cliPlrInfoLoadFromFile) as `loadPlayerData`.
bool loadSentPlayerData(Fight& fight, ServerObject& player, std::span<const std::uint8_t> sent, bool* disconnect = nullptr,
                        bool playerOnly = false);
/// CliNet_SendSaveServerXfer: what a client sends of its .plr `file` (as on disk) as
/// SAVE_SERVER: computeServerPlayerDataBufferSize's count (the server sections,
/// each its id, its size's block and its data, then the 0 id's 4) and 4 more, the client's copy
/// of them decrypted in it. The bytes the copy never writes (after each id up to its size's
/// block, the size's high half, the last 4: the malloc'd buffer's) zero here. A file that can't be
/// read: computeServerPlayerDataBufferSize's 0, the 4 bytes alone (nothing copied).
std::vector<std::uint8_t> saveServerSent(std::span<const std::uint8_t> file);
/// The sections as cliPlrInfoLoadFromFile walks a decrypted .plr: ids up to a 0
/// (or the end), each size the low half of the 8 bytes at the block after its id (the bytes
/// between and the high half not looked at); nullopt when a size runs past the end.
std::optional<player::PlayerFile> readLoadedSections(std::span<const std::uint8_t> plain);
/// The same for a parsed file. `playerOnly` (noxcpp: a solo game's guest): what the load resets is
/// that player's alone (its book, guide and abilities, its lasting spells), not every player's.
bool applyPlayerFile(Fight& fight, ServerObject& player, const player::PlayerFile& file, bool* disconnect = nullptr,
                     bool playerOnly = false);
/// noxcpp: a player's level by the world's own rules (a network game's 10; else by its
/// experience, XPTable) and its numbers anew: a character read as another game's (GameFlagsAs).
void relevel(World& world, ServerObject& player);

/// Whether a quest player carries fewer than ten of each potion and no more Infinite Pain
/// wands than ForceOfNatureStaffLimit (true for anything but a player).
bool questInventoryAllowed(const World& world, const ServerObject& player);
/// set_god: in a solo game only, god mode (engine flags 0x30) on or off and every
/// player's field guide, spells and (a warrior's) abilities all known or emptied
/// (spellAwardAll1 / All2 / All3; a quest's first spells given).
/// The exe's arrays are its host client's too: a remote client isn't told (host-local).
void setGod(Fight& fight, bool on);

} // namespace nox::server
