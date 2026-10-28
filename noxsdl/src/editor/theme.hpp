// The map editor's themes, by name (no ImGui here: the settings and their tests use it). The
// colours are imgui_host.cpp's; what panels draw themselves follows them through theme_colours.hpp.
#pragma once

#include <string_view>

namespace noxsdl::editor {

/// Dark: near-black panels with blue accents (the default). Medium: the NoX Core stone greys and
/// gold (the editor's first dark theme). Light: its light variant.
enum class Theme : int { Dark = 0, Medium = 1, Light = 2 };

inline constexpr int kThemeCount = 3;

/// As saved ("dark", "medium", "light").
const char* themeKey(Theme theme);
/// As shown ("Dark", "Medium", "Light").
const char* themeLabel(Theme theme);
/// A saved name read back (any case); false (and `out` left alone) for anything else.
bool themeFromKey(std::string_view key, Theme& out);

/// The order the Theme drop-downs list them in: Light, Medium, Dark.
inline constexpr Theme kThemeMenuOrder[kThemeCount] = {Theme::Light, Theme::Medium, Theme::Dark};

} // namespace noxsdl::editor
