// Delving Mode's campfire window (campfire_window.hpp): the spells to strengthen, as rows to click.
#include "campfire_window.hpp"

#include "draw_box.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl {

namespace {

// In the HUD's game pixels (its 800 x 600, times its scale).
constexpr float kPad = 12;
constexpr float kRowH = 34;
constexpr float kIcon = 30;
constexpr float kColumnW = 230;
constexpr int kRowsPerColumn = 9;

using box::inside;
using box::outline;

} // namespace

// The panel centred a little above the middle, its title, and a row per spell in columns.
CampfireWindow::Layout CampfireWindow::layout(Offer offer, int w, int h, float scale) const
{
    Layout l;
    const float s = scale;
    l.text = font_->lineSize(11, s, 8);
    l.line = font_->lineHeight(l.text);
    // The spells in columns of nine, side by side; the title over them.
    const int n = static_cast<int>(offer.size());
    const int columns = std::max(1, (n + kRowsPerColumn - 1) / kRowsPerColumn);
    const int rows = std::min(n, kRowsPerColumn);
    const float header = l.line * 2.6f;
    const float pw = std::max(columns * kColumnW * s + 2 * kPad * s, font_->measure("Strengthen one spell, free: once at this fire", l.text) + 2 * kPad * s);
    const float ph = header + static_cast<float>(rows) * kRowH * s + 2 * kPad * s;
    l.panel = {std::floor((static_cast<float>(w) - pw) / 2), std::floor((static_cast<float>(h) - ph) / 2 - 40 * s), pw, ph};
    for (int i = 0; i < n; ++i) {
        const int c = i / kRowsPerColumn;
        const int r = i % kRowsPerColumn;
        l.rows.push_back({l.panel.x + kPad * s + static_cast<float>(c) * kColumnW * s, l.panel.y + kPad * s + header + static_cast<float>(r) * kRowH * s,
                          kColumnW * s - 6 * s, kRowH * s - 3 * s});
    }
    return l;
}

void CampfireWindow::draw(SDL_Renderer* renderer, Offer offer, int w, int h, float scale, SDL_FPoint mouse) const
{
    if (offer.empty() || !font_) {
        return;
    }
    const Layout l = layout(offer, w, h, scale);
    const float s = scale;
    const SDL_Color white{235, 235, 235, 255};
    const SDL_Color grey{160, 160, 160, 255};
    const SDL_Color gold{255, 200, 60, 255};
    const SDL_Color ember{255, 140, 50, 255};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 18, 10, 4, 225);
    SDL_RenderFillRect(renderer, &l.panel);
    outline(renderer, l.panel, {150, 90, 40, 255});
    const float tx = l.panel.x + kPad * s;
    font_->draw("The campfire's warmth", tx, l.panel.y + kPad * s, l.text, ember);
    font_->draw("Strengthen one spell, free: once at this fire", tx, l.panel.y + kPad * s + l.line * 1.1f, l.text, grey);
    for (std::size_t i = 0; i < offer.size() && i < l.rows.size(); ++i) {
        const SDL_FRect& r = l.rows[i];
        const bool hover = inside(r, mouse.x, mouse.y);
        SDL_SetRenderDrawColor(renderer, hover ? 70 : 34, hover ? 40 : 22, hover ? 16 : 10, 235);
        SDL_RenderFillRect(renderer, &r);
        outline(renderer, r, hover ? gold : SDL_Color{80, 55, 30, 255});
        float x = r.x + 3 * s;
        if (const Sprite* icon = icon_ ? icon_(offer[i].spell) : nullptr; icon && icon->texture) {
            const float size = kIcon * s * 0.9f;
            const SDL_FRect dst{x, r.y + (r.h - size) / 2, size, size};
            drawSprite(renderer, *icon, nullptr, dst);
            x += size + 6 * s;
        }
        std::string name = name_ ? name_(offer[i].spell) : std::string();
        if (name.empty()) {
            name = "Spell " + std::to_string(offer[i].spell);
        }
        const std::string levels = std::to_string(offer[i].level) + " > " + std::to_string(offer[i].level + 1);
        const float lw = font_->measure(levels, l.text);
        while (name.size() > 1 && x + font_->measure(name, l.text) > r.x + r.w - lw - 10 * s) {
            name.pop_back();
        }
        const float ty = r.y + (r.h - l.line) / 2;
        font_->draw(name, x, ty, l.text, hover ? gold : white);
        font_->draw(levels, r.x + r.w - lw - 5 * s, ty, l.text, hover ? gold : ember);
    }
}

bool CampfireWindow::handleEvent(const SDL_Event& event, Offer offer, int w, int h, float scale, std::optional<std::uint8_t>& pick) const
{
    if (offer.empty() || !font_ || event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    const Layout l = layout(offer, w, h, scale);
    if (!inside(l.panel, event.button.x, event.button.y)) {
        return false; // (walking about by the fire goes on)
    }
    for (std::size_t i = 0; i < offer.size() && i < l.rows.size(); ++i) {
        if (inside(l.rows[i], event.button.x, event.button.y)) {
            pick = offer[i].spell;
        }
    }
    return true;
}

} // namespace noxsdl
