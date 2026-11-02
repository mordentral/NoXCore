// The centred messages (centred_messages.hpp): three slots, the newest drawn lowest.
#include "centred_messages.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl {

void CentredMessages::print(std::string text, std::uint32_t frame, std::optional<SDL_Color> colour)
{
    if (text.empty()) {
        return;
    }
    // The next slot round, overwriting the oldest.
    index_ = (index_ + 1) % 3;
    slots_[static_cast<std::size_t>(index_)] = {std::move(text), frame + kShowFrames, colour};
}

SDL_Color CentredMessages::slotColour(const Slot& s, bool newest)
{
    if (!s.colour) {
        return newest ? SDL_Color{255, 255, 255, 255} : SDL_Color{0xD4, 0xD4, 0xD4, 255};
    }
    if (newest) {
        return {s.colour->r, s.colour->g, s.colour->b, 255};
    }
    const auto dim = [](Uint8 v) { return static_cast<Uint8>(v * 0xD4 / 255); };
    return {dim(s.colour->r), dim(s.colour->g), dim(s.colour->b), 255};
}

void CentredMessages::clear()
{
    slots_ = {};
    index_ = 0;
}

void CentredMessages::draw(SDL_Renderer* renderer, Font* font, int screenWidth, const SDL_FRect& view, float scale,
                           std::uint32_t frame) const
{
    if (!renderer || !font) {
        return;
    }
    // The game's default font, a line 10 of its pixels tall, scaled as the HUD.
    const int size = font->lineSize(10, scale);
    const float lineHeight = std::round(font->lineHeight(size));
    const float shadow = std::max(1.0f, std::round(scale));
    // (viewport height * 3) / 4 (signed, CDQ) - 15 + the viewport's top.
    float y = view.y + static_cast<float>(static_cast<int>(view.h) * 3 / 4) - std::round(15 * scale);
    // From the newest upward, stopping at the first that has run out.
    int at = index_;
    for (int n = 0; n < 3; ++n) {
        const Slot& s = slots_[static_cast<std::size_t>(at)];
        if (s.until < frame) {
            return;
        }
        const float w = font->measure(s.text, size);
        const float x = static_cast<float>(static_cast<int>(static_cast<float>(screenWidth) - w) / 2);
        static constexpr int kShadow[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (const auto& d : kShadow) {
            font->draw(s.text, x + static_cast<float>(d[0]) * shadow, y + static_cast<float>(d[1]) * shadow, size,
                       SDL_Color{0, 0, 0, 255});
        }
        font->draw(s.text, x, y, size, slotColour(s, n == 0));
        y -= lineHeight + std::round(4 * scale);
        at = at == 0 ? 2 : at - 1;
    }
}

} // namespace noxsdl
