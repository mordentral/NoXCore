// The Westwood Online login (wolapi.wnd) and chat lobby (wolchat.wnd): their large text drawn smaller,
// and where the login's how-to text goes. No SDL here (the tests use it).
//
// Their windows mostly give no FONT, and a window with none is large while the shell is up (as the
// game's loader has it), so the buttons, labels and the login list are in the large font, on boxes
// too small for it at full size: "Ladder - WIP" wrapped under its button, the login's labels ran
// long. The owner asked for them smaller (2026-11-10, vanilla_deviations.md VD-71); the large font
// is TrueType, so it's drawn at kLargeScale of its size there (Window::largeScale), the faces kept.
// The chat's own lists and title give FONT = default and stay as they are.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace noxsdl::wol_screens {

/// The large font's share of its size on the two screens: its 16-unit line drawn 12.8 units, one
/// size for every label so they match, the largest that leaves each its margin ("Ladder - WIP" in
/// its 82-unit button is the tightest).
inline constexpr float kLargeScale = 0.8f;

/// How many lines of a list box show at once: its height less its title line (a font line and a
/// pixel, if it has a title), lines `lineHeight` and a pixel apart (as gui.cpp draws them).
constexpr int linesShown(int boxHeight, bool hasTitle, int fontLine, int lineHeight)
{
    const int room = boxHeight - (hasTitle ? fontLine + 1 : 0) + 1;
    return std::max(1, room / (lineHeight + 1));
}

/// (noxcpp) The login's how-to, drawn in the screen's empty right-hand panel (wolapi.wnd's window 0,
/// 260 x 380), wrapped `kLoginHelpInset` in from its sides and top, in the default font.
inline constexpr std::string_view kLoginHelp =
    "Log in with your XWIS account. A new name becomes your account with the password you type; for a "
    "name registered at xwis.net, use the game password from xwis.net/ga. Skip Login plays with no name (a "
    "game you host is listed under a generated one).";
inline constexpr std::int32_t kLoginHelpPanel = 0;
inline constexpr int kLoginHelpInset = 12;

} // namespace noxsdl::wol_screens
