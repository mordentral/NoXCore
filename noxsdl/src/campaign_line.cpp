// A mod's campaign named on the save screens (campaign_line.hpp).
#include "campaign_line.hpp"

#include "noxcore/strings/l10n.hpp"

#include <utility>

namespace noxsdl {

void addUserWindow(nox::gui::WindowDef& def, std::int32_t id, int x, int y, int width, int height, bool clickable)
{
    namespace st = nox::gui::status;
    nox::gui::WindowDef w;
    w.id = id;
    w.x = x;
    w.y = y;
    w.width = width;
    w.height = height;
    w.typeName = "USER";
    w.type = nox::gui::WindowType::User;
    w.status = clickable ? (st::Enabled | st::NoFocus) : st::NoFocus; // (not enabled: no click reaches it)
    def.children.push_back(std::move(w));
}

void drawGoldLine(gui::Window& self, gui::Painter& p, const std::string& text)
{
    const SDL_Point pos = self.screenPos();
    const int tw = p.textWidth(text, false);
    p.text(text, pos.x + (self.rect.w - tw) / 2, pos.y + (self.rect.h - p.fontHeight(false)) / 2, false, nox::gui::Color{240, 180, 42});
}

void addCampaignLine(nox::gui::WindowDef& def, const std::string& title, int x, int y, int width, int height)
{
    if (title.empty()) {
        return;
    }
    addUserWindow(def, kCampaignLineId, x, y, width, height, false);
}

void drawCampaignLine(gui::Window* screen, const std::string& title)
{
    gui::Window* w = screen && !title.empty() ? screen->find(kCampaignLineId) : nullptr;
    if (!w) {
        return;
    }
    const std::string text = nox::l10n::format(NOX_TR("menus.solo.campaign_line", "Campaign: {campaign}"), {{"campaign", title}});
    w->draw = [text](gui::Window& self, gui::Painter& p) { drawGoldLine(self, p, text); };
}

} // namespace noxsdl
