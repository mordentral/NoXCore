// The game's mouse cursor (cursor.hpp): the hover shape test, the cursor images and their words.
#include "cursor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

/// cursorLoadAll's table, by CursorType.
constexpr std::array<const char*, 17> kCursorImages{
    "CursorSelect", "CursorGrab",       "CursorPickup",       "CursorTrade",  "CursorTalk", "CursorTarget",
    "CursorIdentify", "CursorIdentifyNo", "CursorRepair",     "CursorCreateGame", "CursorBusy", "CursorBuy",
    "CursorSell",   "CursorUse",        "CursorMove",         "CursorPickupFar", "CursorCaution"};

constexpr SDL_Color kYellow{255, 255, 0, 255};

} // namespace

pick::Shape pickShape(const TypeArt& art)
{
    return pick::Shape{art.extentBox, art.extentRadius, art.extentWidth, art.extentHeight, art.zSizeMin, art.zSizeMax};
}

bool underCursor(const TypeArt& art, float x, float y, float z, float cx, float cy)
{
    const auto ix = static_cast<int>(std::lround(x));
    const auto iy = static_cast<int>(std::lround(y));
    const auto icx = static_cast<int>(std::floor(cx));
    const auto icy = static_cast<int>(std::floor(cy));
    return pick::nearCursor(ix, iy, icx, icy) &&
           pick::under(pickShape(art), ix, iy, static_cast<int>(std::lround(z)), icx, icy);
}

float hoverDepth(const TypeArt& art, float y, float z)
{
    return static_cast<float>(pick::depth(pickShape(art), static_cast<int>(std::lround(y)), static_cast<int>(std::lround(z))));
}

Cursor::Cursor(const nox::things::ThingDb& things, SpriteCache& sprites, Font* font,
               const nox::strings::StringTable* strings)
    : things_(things), sprites_(sprites), font_(font), strings_(strings)
{
    for (const auto& i : things_.images) {
        images_.emplace(i.name, &i);
    }
}

const nox::things::ImageDef* Cursor::image(CursorType type) const
{
    const auto index = static_cast<std::size_t>(type);
    if (index >= kCursorImages.size()) {
        return nullptr;
    }
    const auto it = images_.find(kCursorImages[index]);
    return it == images_.end() ? nullptr : it->second;
}

std::string Cursor::label(const char* id) const
{
    if (!strings_) {
        return id;
    }
    // Str_GetString with guicursor.c as the source.
    for (const std::string& key : {std::string("guicursor.c:") + id, std::string(id)}) {
        if (const auto t = strings_->text(key)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return id;
}

int Cursor::moveFrame(float mx, float my, float width, float height, bool running)
{
    // The direction: (atan2 + 2 pi) * 128 / pi, rounded, mod 256; then eighths.
    const float dx = mx - width / 2;
    const float dy = my - height / 2;
    int angle = static_cast<int>(std::floor((std::atan2(dy, dx) + 2 * std::numbers::pi_v<float>) * 128.0f /
                                                std::numbers::pi_v<float> +
                                            0.5f)) &
                0xFF;
    // 32 directions (256 / 8); the running arrows are frames 32-63, past 100 units (10000 squared).
    int frame = angle / 8;
    if (dx * dx + dy * dy > 10000.0f || running) {
        frame += 32;
    }
    return frame;
}

void Cursor::draw(SDL_Renderer* renderer, CursorType type, float x, float y, float scale, int moveFrame,
                  std::optional<SDL_Color> tint)
{
    // Pick the image: the type's own, else the plain Select arrow.
    const nox::things::ImageDef* def = image(type);
    if (!def) {
        def = image(CursorType::Select);
    }
    std::optional<nox::things::ImageRef> ref;
    if (def) {
        ref = def->image;
        if (def->animation && !def->animation->frames.empty()) {
            const auto& a = *def->animation;
            if (type == CursorType::Move) {
                ref = a.frames[static_cast<std::size_t>(std::clamp<int>(moveFrame, 0, static_cast<int>(a.frames.size()) - 1))];
            } else {
                // a looping animation, a frame every frameDelay + 1 game frames.
                const auto frame = SDL_GetTicks() * 30 / 1000 / (a.frameDelay + 1u);
                ref = a.frames[frame % a.frames.size()];
            }
        }
    }
    // Draw the picked frame, tinted for the call only.
    if (ref) {
        if (const Sprite* s = sprites_.get(*ref)) {
            // Drawn at the tip less (64, 64), plus the image's own offset.
            const SDL_FRect dst{x + (s->offsetX - 64) * scale, y + (s->offsetY - 64) * scale, s->width * scale,
                                s->height * scale};
            s->scale = SDL_SCALEMODE_LINEAR;
            if (tint) {
                SDL_SetTextureColorMod(s->texture, tint->r, tint->g, tint->b);
            }
            drawSprite(renderer, *s, nullptr, dst);
            if (tint) {
                SDL_SetTextureColorMod(s->texture, 255, 255, 255);
            }
        }
    }

    // The words some cursors carry, in yellow, in the default font (10 high).
    if (!font_) {
        return;
    }
    constexpr float kFontHeight = 10;
    // Font pixel size that makes a line 10 game pixels high at this scale (lineHeight is per 100 px).
    const int size = font_->lineSize(kFontHeight, scale);
    const auto text = [&](const char* id, float dx, float dy) {
        font_->draw(label(id), x + dx * scale, y + dy * scale, size, kYellow);
    };
    switch (type) {
    case CursorType::Grab:
        text("GRAB", -10, -kFontHeight);
        break;
    case CursorType::Pickup:
        text("PICKUP", -15, kFontHeight);
        break;
    case CursorType::Trade:
        text("SHOPKEEPER", -25, -kFontHeight);
        break;
    case CursorType::Talk:
        text("TALK", -15, -kFontHeight);
        break;
    case CursorType::Identify:
    case CursorType::IdentifyNo:
        text("IDENTIFY", -15, 24);
        break;
    case CursorType::Repair:
        text("REPAIR", -15, -kFontHeight);
        break;
    case CursorType::Use:
        text("USE", -10, kFontHeight);
        break;
    default:
        break;
    }
}

} // namespace noxsdl
