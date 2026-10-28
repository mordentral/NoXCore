// The colours panels draw with themselves (messages, highlights, stars, the Chat tab, the Map
// panel's selection), picked for the theme in use so they stay readable on its panels. Set with
// the style (ImGuiHost::setStyle); read while drawing.
#pragma once

#include "editor/theme.hpp"

#include <imgui.h>

namespace noxsdl::editor {

struct ThemeColours {
    ImVec4 error;         ///< errors, clashes (red)
    ImVec4 warning;       ///< warnings and notices (yellow)
    ImVec4 caution;       ///< another editor has it (orange)
    ImVec4 info;          ///< what's running, what to pick (light blue)
    ImVec4 ok;            ///< "no problems", "compiles" (green)
    ImVec4 note;          ///< quoted game text (pale)
    ImVec4 param;         ///< a builtin's parameters (blue)
    ImVec4 unknown;       ///< a name the game doesn't know (amber)
    ImVec4 highlight;     ///< a picked entry's frame in a panel (gold; Dark: orange)
    ImVec4 highlightText; ///< a picked entry's text (the current quest picture)
    ImVec4 star;          ///< favourites' stars
    ImVec4 chatSystem;    ///< the server's lines in the Chat panel
    ImVec4 chatTeam;      ///< team lines' speakers
    ImVec4 chatAll;       ///< everyone lines' speakers
    ImVec4 chatTab;       ///< the Chat tab with unread lines (amber)
    ImVec4 chatTabDimmed;
    ImVec4 chatTabHovered;
    ImVec4 idle;          ///< a session editor doing nothing
    ImVec4 playing;       ///< a session editor in the game
    ImVec4 section;       ///< Properties' section headers (Dark: grey, not the selection's blue)
    ImVec4 sectionHovered;
    ImVec4 sectionActive;
    ImU32 viewSelection;      ///< the Map panel's selection outline
    ImU32 viewSelectionHot;   ///< ... where a drag moves it to
    ImU32 viewSelectionFaint; ///< ... where it came from
};

/// The theme the style was last made with.
Theme activeTheme();
/// Its panel colours.
const ThemeColours& themeColours();
/// A colour of the game's or a person's (an editor's, a team's) as text on the panels: as it is,
/// or darkened enough to read on the Light theme's pale panels.
ImVec4 onPanel(ImVec4 colour);
inline ImU32 onPanelU32(ImU32 colour)
{
    return ImGui::ColorConvertFloat4ToU32(onPanel(ImGui::ColorConvertU32ToFloat4(colour)));
}

} // namespace noxsdl::editor
