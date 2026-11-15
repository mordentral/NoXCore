// What a few of the front end's windows decide, kept apart from their SDL windows so the tests can
// run it: the solo save menu's buttons and message box (cinematics.cpp), the in-game menu's
// AutoSave (quit_menu.cpp), the Westwood Online chat's lobby list and users (menus_wolchat.cpp),
// the shell's Options / Back bar (menus.cpp) and where the menus' pointer sparks go (main.cpp).
// All but the bar were original bugs the owner decided to fix (2026-11-04; original_bugs.md, the
// entry's "Fixed" line; vanilla_deviations.md VD-64); the bar is as the game's.
#pragma once

#include <algorithm>
#include <cstdint>

namespace noxsdl {

/// The shell's Options / Back bar (OptsBack.wnd) on a screen, by its state code: Back's text,
/// "Quit" on the main menu (100) and "Back" elsewhere, Options itself (300) included; the Options
/// button greyed while Options is up (game_showOptions), on again once it's gone.
struct OptsBackLook {
    const char* backText;
    bool optionsOn;
};
constexpr OptsBackLook optsBackLook(int state)
{
    return {state == 100 ? "OptsBack.wnd:Quit" : "OptsBack.wnd:Back", state != 300};
}

/// A message box's answer closes it when its flags say so (0x20) and it's still the box answered:
/// one the answer's own callback put up (a failed load's error box) stays. (The exe closes
/// whatever box is up by then: OB-147.)
constexpr bool closesAfterAnswer(std::uint32_t flags, std::uint32_t answeredSerial, std::uint32_t serialNow)
{
    return (flags & 0x20u) != 0 && answeredSerial == serialNow;
}

/// The save menu's Cancel (Quit after a briefing or a death) and Load: Cancel always on, Load with
/// a saved game. (The exe turns Cancel off too with no saved game, so the menu couldn't be left:
/// OB-148.)
struct LoadMenuButtons {
    bool cancel = true;
    bool load = false;
};
constexpr LoadMenuButtons loadMenuButtons(int savedGames)
{
    return {true, savedGames > 0};
}

/// The in-game menu's AutoSave as the menu opens: turned off by a press while dead, on again once
/// the player isn't (after a load). (The exe's stays off for the rest of the game: OB-149.)
constexpr bool autoSaveOn(bool wasOn, bool dead)
{
    return wasOn || !dead;
}

/// A user leaving the lobby warns (WolUser.c:LostSelection) when it was picked for private
/// messages and no one picked is left. (The exe's test never holds: OB-190.)
constexpr bool lostSelection(bool leaverWasPicked, bool anyStillPicked)
{
    return leaverWasPicked && !anyStillPicked;
}

/// The lobby a click on the lobby list joins: the one clicked, from none or from another; none
/// (-1) when it's the one we're in. (The exe joins 0, nothing, from none and leaves "Updating
/// channel list..." up: OB-191.)
constexpr int lobbyToJoin(int clicked, int current, bool sameAsCurrent)
{
    if (current < 0) {
        return clicked;
    }
    return sameAsCurrent ? -1 : clicked;
}

/// The menus' 640 x 480 scaled whole into a `width` x `height` output (the smaller ratio), where
/// the pointer's sparks go; 0 for an output with no area (a minimised window's 0 x 0), when they
/// skip that frame (the pointer's place in them would be a division by 0 made an int).
constexpr float menuSparkScale(float width, float height)
{
    const float s = std::min(width / 640.0f, height / 480.0f);
    return s > 0 ? s : 0;
}

} // namespace noxsdl
