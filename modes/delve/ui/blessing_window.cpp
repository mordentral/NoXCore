// Delving Mode's altar window (blessing_window.hpp): the offered blessings as cards to click.
#include "blessing_window.hpp"

#include "draw_box.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace noxsdl {

namespace {

// In the HUD's game pixels (its 800 x 600, times its scale).
constexpr float kPad = 12;
constexpr float kCardW = 170;
constexpr float kCardH = 86;
constexpr float kGap = 10;

using box::inside;
using box::outline;

/// `text` broken into lines no wider than `width`, at word ends.
std::vector<std::string> wrap(Font& font, const std::string& text, int size, float width)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find(' ', at), text.size());
        const std::string word = text.substr(at, end - at);
        const std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && font.measure(tried, size) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = tried;
        }
        at = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

// The panel centred a little above the middle, its title, and the cards side by side under it.
BlessingWindow::Layout BlessingWindow::layout(Offer offer, int w, int h, float scale) const
{
    Layout l;
    const float s = scale;
    // The text sizes: 11 and 14 game pixels high at the HUD's scale.
    l.text = font_->lineSize(11, s, 8);
    l.title = font_->lineSize(14, s, 9);
    l.line = font_->lineHeight(l.text);
    // The cards side by side; the title over them.
    const auto n = static_cast<float>(offer.size());
    const float header = l.line * 2.6f;
    const float pw = std::max(n * kCardW * s + (n - 1) * kGap * s + 2 * kPad * s,
                              font_->measure("Take one blessing for the rest of the delve", l.text) + 2 * kPad * s);
    const float ph = header + kCardH * s + 2 * kPad * s;
    l.panel = {std::floor((static_cast<float>(w) - pw) / 2), std::floor((static_cast<float>(h) - ph) / 2 - 40 * s), pw, ph};
    const float cardsW = n * kCardW * s + (n - 1) * kGap * s;
    for (std::size_t i = 0; i < offer.size(); ++i) {
        l.cards.push_back({l.panel.x + (pw - cardsW) / 2 + static_cast<float>(i) * (kCardW + kGap) * s, l.panel.y + kPad * s + header,
                           kCardW * s, kCardH * s});
    }
    return l;
}

void BlessingWindow::draw(SDL_Renderer* renderer, Offer offer, int w, int h, float scale, SDL_FPoint mouse) const
{
    if (offer.empty() || !font_) {
        return;
    }
    namespace dm = nox::server::delvemap;
    const Layout l = layout(offer, w, h, scale);
    const float s = scale;
    const SDL_Color white{235, 235, 235, 255};
    const SDL_Color grey{170, 170, 170, 255};
    const SDL_Color gold{255, 215, 90, 255};
    const SDL_Color pale{230, 215, 160, 255};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 10, 10, 20, 225);
    SDL_RenderFillRect(renderer, &l.panel);
    outline(renderer, l.panel, {170, 150, 90, 255});
    const float tx = l.panel.x + kPad * s;
    font_->draw("The altar's blessing", tx, l.panel.y + kPad * s, l.text, pale);
    font_->draw("Take one blessing for the rest of the delve", tx, l.panel.y + kPad * s + l.line * 1.1f, l.text, grey);
    for (std::size_t i = 0; i < offer.size() && i < l.cards.size(); ++i) {
        const SDL_FRect& r = l.cards[i];
        const bool hover = inside(r, mouse.x, mouse.y);
        SDL_SetRenderDrawColor(renderer, hover ? 60 : 28, hover ? 52 : 26, hover ? 30 : 40, 240);
        SDL_RenderFillRect(renderer, &r);
        outline(renderer, r, hover ? gold : SDL_Color{110, 100, 70, 255});
        const dm::Blessing b = offer[i].blessing;
        const std::string name = dm::blessingName(b);
        const float titleH = font_->lineHeight(l.title);
        font_->draw(name, r.x + (r.w - font_->measure(name, l.title)) / 2, r.y + 8 * s, l.title, hover ? gold : pale);
        float y = r.y + 8 * s + titleH + 6 * s;
        for (const std::string& line : wrap(*font_, dm::blessingText(b), l.text, r.w - 14 * s)) {
            font_->draw(line, r.x + (r.w - font_->measure(line, l.text)) / 2, y, l.text, white);
            y += l.line;
        }
        if (offer[i].held != 0) {
            const std::string held = "You have it: " + std::to_string(offer[i].held);
            font_->draw(held, r.x + (r.w - font_->measure(held, l.text)) / 2, r.y + r.h - l.line - 6 * s, l.text, grey);
        }
    }
}

bool BlessingWindow::handleEvent(const SDL_Event& event, Offer offer, int w, int h, float scale,
                                 std::optional<nox::server::delvemap::Blessing>& pick) const
{
    if (offer.empty() || !font_ || event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    const Layout l = layout(offer, w, h, scale);
    if (!inside(l.panel, event.button.x, event.button.y)) {
        return false; // (walking about by the altar goes on)
    }
    for (std::size_t i = 0; i < offer.size() && i < l.cards.size(); ++i) {
        if (inside(l.cards[i], event.button.x, event.button.y)) {
            pick = offer[i].blessing;
        }
    }
    return true;
}

} // namespace noxsdl
