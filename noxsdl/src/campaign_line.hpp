// (noxcpp) A mod's campaign named on the screens that list or start its saves (the class and
// character screens, the in-game save menu): one line in the small font, the menus' gold, centred
// in a strip of the screen that's empty in the game's layout. The game's own campaign, and every
// other game: no line, the screen loaded as it always was.
#pragma once

#include "campaign_text.hpp"
#include "gui.hpp"

#include "noxcore/gui/window_file.hpp"

#include <cstdint>
#include <string>

namespace noxsdl {

/// The line's window's id (none of the game's window files use it).
inline constexpr std::int32_t kCampaignLineId = 4290;

/// `def` (a screen's window file, loaded) with the line's window added: at `x`, `y` in the screen,
/// `width` x `height`, taking no input. Nothing when `title` is empty.
void addCampaignLine(nox::gui::WindowDef& def, const std::string& title, int x, int y, int width, int height);
/// The line's window in `screen` (addCampaignLine's) drawing `title` ("Campaign: <title>").
void drawCampaignLine(gui::Window* screen, const std::string& title);
// (Its words, campaignSaveLine and campaignSavePlace: campaign_text.hpp.)

} // namespace noxsdl
