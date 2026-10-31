// PlayerDraw's name over a player: drawn over every player (us too) in default.fnt, its top 64
// units above the feet, grey or the team's text colour, with a dark shadow; but not while the
// player has a chat bubble up (the client's chat bubble table), not over an invisible player we can't
// see through (showsOverhead), and never in a campaign game (game flag 0x800: the solo game, where
// the host plays alone). A network game of any kind draws them: an arena's, and a quest's (flag
// 0x1000 without 0x800), so a stock client in our co-op campaign, told the quest's flags, always
// drew the party's names. The exe has no option that turns them off (noxsdl's console
// "nameplates" is our own).
//
// noxcpp: the co-op campaign (its mode's client side greeted us: App::partyNames) draws them as a
// network game does. The host's client and a NoX Core guest's are told the solo game's flags
// (0x800), which had hidden every member's name there.
#pragma once

#include <cstdint>

namespace noxsdl {

/// Whether PlayerDraw puts the player's name over it, in a game of flags `gameFlags` (the client's,
/// with soloGame's 0x800): the player has a name (`named`), no chat bubble up (`bubbleUp`) and its
/// overhead things shown (`overhead`: showsOverhead). Not in a campaign game (0x800) but the co-op
/// campaign's (`coopCampaign`).
inline bool namePlateShown(std::uint32_t gameFlags, bool named, bool bubbleUp, bool overhead, bool coopCampaign = false)
{
    constexpr std::uint32_t kSolo = 0x800;
    if (!named || bubbleUp || !overhead) {
        return false;
    }
    return !(gameFlags & kSolo) || coopCampaign;
}

} // namespace noxsdl
