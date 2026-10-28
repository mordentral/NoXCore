// What the client shows for MSG_INFORM (0xA9), as CliNet_OnInform does: the words it
// prints with Gui_PrintCenteredMessage, the sound it plays, the message box of 0x11.
// The kill feed (0x0E) is the HeadlessClient's Kill event, not here.
//
//   0     a spell's result, plyrspel.c's label from the table
//   1     plyrspel.c:SpellCastSuccess with the spell's title (Spell_GetTitle)
//   2     an ability's result, from the table
//   3     netserv.c:PlayerTimeout (a player)                    4  objcoll.c:FlagRetrieveNotice, 0x131
//   5     objcoll.c:FlagCaptureNotice (player, colour), 0x132   6  objcoll.c:FlagPickupNotice, 0x12F
//   7     drop.c:FlagDropNotice, 0x130                          8  update.c:FlagRespawnNotice (colour), 0x131
//   9     objcoll.c:FlagBallNotice (player, team) or FlagBallUnknownNotice (team)
//   0x0A  pickup.c:PickUpCrown / PickUpTeamCrown               0x0B drop.c:DropCrown / DropTeamCrown
//   0x0C  Netserv.c:InObservationMode, then Netserv.c:PressJump when it may join
//   0x0D  atckexec.c:PlayerStunned / PlayerConfused / PlayerPoisoned, player.c:TooHeavy
//   0x0F  a string's label (after a byte: a sign's, "use.c:SignSays" round it), in a game only
//   0x10  pickup.c:WrongTeam (colour)                          0x11 the message box: guiserv.c:Notice,
//                                                                    Noxworld.c:ErrChangedClass
//   0x12  objcoll.c:PlayerExited  0x13 objcoll.c:PlayerExitedWarp  0x14 GeneralPrint:SecretFoundOther
//   0x15  GeneralPrint:AdvanceToStage1 (a number) inside use.c:SignSays
// A player or team the client doesn't know prints nothing (Plr_GetByNetCode / Team_GetByID null).
// "Colour" is Team_GetColorTitle: the team colour's title, team.c:NoTeam for one the
// table lacks.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::client::inform {

struct Lookups {
    /// Str_GetString: a label's text (one of its variants), the label itself when it's missing.
    std::function<std::string(std::string_view label)> text;
    /// Plr_GetByNetCode's name; nullopt for no such player.
    std::function<std::optional<std::string>(std::uint32_t netCode)> playerName;
    /// Team_GetByID's name; nullopt for no such team.
    std::function<std::optional<std::string>(std::uint32_t team)> teamName;
    /// Spell_GetTitle: the spell's title (empty for none).
    std::function<std::string(std::uint32_t spell)> spellTitle;
    /// Game_GetCreatureGuideTitle / Abil_GetTitle: a beast scroll's or an ability's title.
    std::function<std::string(std::uint32_t guide)> guideTitle;
    std::function<std::string(std::uint32_t ability)> abilityTitle;
    /// g_cliLocalPlayer's net code (an award to us or to another).
    std::uint32_t localNetCode = 0;
    /// Game_GetPlayState() == 3: in a game (0x0F prints only then).
    bool inGame = true;
};

struct Notice {
    std::vector<std::string> centred; ///< Gui_PrintCenteredMessage's, in order
    std::uint32_t sound = 0;          ///< Snd_PlaySound(id, 100); 0 none
    bool dialog = false;              ///< Gui_Dialog_Create (0x21: OK, closed after)
    std::string dialogTitle;
    std::string dialogText;
};

/// CliNet_OnInform for one MSG_INFORM (`m` from its 0xA9).
Notice decode(std::span<const std::uint8_t> m, const Lookups& lookups);

/// CliNet_OnPacket's MSG 0xF0 sub-codes that print centred: 0x1E a spell awarded (plyrspel.c:AwardSpell,
/// or AwardSpellToOther with the player's name), 0x1F a beast scroll (PlyrGide.c:AwardGuide...), 0x20 an
/// ability (ComAblty.c:AwardAbility...): {id, u16 net code}, nothing for a player the client doesn't
/// know; 0x21 a lock's notice: the label at +2 with the key's name (objcoll.c:SilverKey... by the
/// byte at +0x33).
Notice decodeReport(std::span<const std::uint8_t> m, const Lookups& lookups);

/// The formatting the game's strings use: %s, %S and %d filled in order from
/// `values`, %% a percent sign.
std::string format(std::string_view fmt, std::initializer_list<std::string_view> values);

/// Team_GetColorTitle's label for a team colour (g_teamColorTitles: modifier.db:MaterialTeam<Colour>Desc),
/// team.c:NoTeam for one outside the table.
std::string colorTitleLabel(std::uint32_t color);

} // namespace nox::client::inform
