// The Westwood Online login (wolapi.wnd) and chat lobby (wolchat.wnd) in the game's default (small)
// face, and where the login's how-to text goes. No SDL here (the tests use it).
//
// Their windows mostly give no FONT, and a window with none is large while the shell is up (as the
// game's loader has it), so the buttons, labels and lists came out in the large face: too big for
// the screens' small boxes, which were laid out for the default face (the login list's 10-unit
// lines, six to its box; the chat's three lists and its title, FONT = default). The owner asked
// for them small (2026-11-10, vanilla_deviations.md VD-71).
#pragma once

#include "noxcore/gui/window_file.hpp"

#include <cstdint>
#include <string_view>

namespace noxsdl::wol_screens {

/// Every window in `def` that gives no FONT of its own gets "default"; a FONT given is kept.
inline void useDefaultFont(nox::gui::WindowDef& def)
{
    if (def.font.empty()) {
        def.font = "default";
    }
    for (auto& child : def.children) {
        useDefaultFont(child);
    }
}

/// (noxcpp) The login's how-to, drawn in the screen's empty right-hand panel (wolapi.wnd's window 0,
/// 260 x 380), wrapped `kLoginHelpInset` in from its sides and top.
inline constexpr std::string_view kLoginHelp =
    "Log in with your XWIS account. A new name becomes your account with the password you type; for a "
    "name registered at xwis.net, use the game password from xwis.net/ga. Skip Login plays without one.";
inline constexpr std::int32_t kLoginHelpPanel = 0;
inline constexpr int kLoginHelpInset = 12;

} // namespace noxsdl::wol_screens
