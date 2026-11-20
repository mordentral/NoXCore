// A mod's campaign named on the save screens (campaign_line.hpp).
#include "campaign_line.hpp"

#include "noxcore/strings/l10n.hpp"

#include <utility>

namespace noxsdl {

void addCampaignLine(nox::gui::WindowDef& def, const std::string& title, int x, int y, int width, int height)
{
    if (title.empty()) {
        return;
    }
    nox::gui::WindowDef line;
    line.id = kCampaignLineId;
    line.x = x;
    line.y = y;
    line.width = width;
    line.height = height;
    line.typeName = "USER";
    line.type = nox::gui::WindowType::User;
    line.status = nox::gui::status::NoFocus; // (not enabled: no click reaches it)
    def.children.push_back(std::move(line));
}

void drawCampaignLine(gui::Window* screen, const std::string& title)
{
    gui::Window* w = screen && !title.empty() ? screen->find(kCampaignLineId) : nullptr;
    if (!w) {
        return;
    }
    const std::string text = nox::l10n::format(NOX_TR("menus.solo.campaign_line", "Campaign: {campaign}"), {{"campaign", title}});
    w->draw = [text](gui::Window& self, gui::Painter& p) {
        const SDL_Point pos = self.screenPos();
        const int tw = p.textWidth(text, false);
        p.text(text, pos.x + (self.rect.w - tw) / 2, pos.y + (self.rect.h - p.fontHeight(false)) / 2, false, nox::gui::Color{240, 180, 42});
    };
}

} // namespace noxsdl
