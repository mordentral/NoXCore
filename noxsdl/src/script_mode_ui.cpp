// The script modes' UI in noxcore.exe (mode_ui.hpp's ModeUi for modes/script's ScriptUiClient): the
// windows a server's UI script declares, each framed and labelled as the server's, its draws
// clipped inside it. The UI's 640 x 480 units are scaled to fit the game view, centred. A click
// inside a window goes to the UI (and never to the game); the rest of the screen is the game's.
// A UI of API 2 also draws the mod's pictures (textures made as they're first drawn, let go when
// the set changes or the game goes) and thing types' pictures from our own art, plays sounds, is
// told of right clicks, and of a fixed set of keys while the mouse is over one of its windows and
// no chat or console line is open.
#include "app.hpp"
#include "audio.hpp"
#include "chat_entry.hpp"
#include "console.hpp"
#include "draw_box.hpp"
#include "mode_ui.hpp"
#include "sprite_cache.hpp"
#include "thing_art.hpp"

#include "noxcore/modes/script/script_ui.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

namespace noxsdl {

namespace {

using nox::modes::script::ModAsset;
using nox::modes::script::ModAssets;
using nox::modes::script::ScriptUiClient;
using nox::modes::script::UiDraw;
using nox::modes::script::UiWindow;

constexpr float kTitleH = 13; ///< a window's title bar (UI units)
constexpr float kTextLine = 10;

/// The keys an API 2 UI is told of (script_ui.hpp's names); none: not one of them.
const char* keyName(SDL_Keycode k)
{
    static constexpr const char* kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    static constexpr const char* kF[] = {"f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12"};
    if (k >= SDLK_0 && k <= SDLK_9) {
        return kDigits[k - SDLK_0];
    }
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) {
        return kDigits[k - SDLK_KP_1 + 1];
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) {
        return kF[k - SDLK_F1];
    }
    switch (k) {
    case SDLK_KP_0: return "0";
    case SDLK_UP: return "up";
    case SDLK_DOWN: return "down";
    case SDLK_LEFT: return "left";
    case SDLK_RIGHT: return "right";
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return "enter";
    case SDLK_ESCAPE: return "escape";
    case SDLK_TAB: return "tab";
    case SDLK_SPACE: return "space";
    default: return nullptr;
    }
}

class ScriptModeUi final : public ModeUi {
public:
    explicit ScriptModeUi(App& app) : app_(app) {}
    // (Its textures go when the game does, unbound; at the program's end, with the renderer.)

    void bind(nox::modes::ClientMode* side) override
    {
        ui_ = dynamic_cast<ScriptUiClient*>(side);
        freeTextures(); // (the last game's pictures: gone with it)
        if (ui_) {
            ui_->setTextWidth([this](std::string_view text) {
                if (!app_.font || scale_ <= 0) {
                    return static_cast<int>(text.size()) * 6;
                }
                return static_cast<int>(app_.font->measure(text, textSize()) / scale_);
            });
        }
    }

    void drawHud(App& app, SDL_Renderer* renderer, const HudFrame& hud) override
    {
        if (!ui_) {
            return;
        }
        place(hud);
        mouse_ = {(hud.mouse.x - originX_) / scale_, (hud.mouse.y - originY_) / scale_};
        ui_->setMouse(std::pair{mouse_.x, mouse_.y});
        ui_->frame(hud.nowMs);
        if (ui_->assets() != texturesOf_) {
            freeTextures(); // (a new set: its pictures made as they're drawn)
            texturesOf_ = ui_->assets();
        }
        playSounds(app);
        if (!hud.renderGui || !app.font || ui_->windows().empty()) {
            return;
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_Rect oldClip{};
        const bool clipped = SDL_RenderClipEnabled(renderer);
        if (clipped) {
            SDL_GetRenderClipRect(renderer, &oldClip);
        }
        const int px = textSize();
        for (const UiWindow& w : ui_->windows()) {
            const SDL_FRect outer = toScreen(static_cast<float>(w.x) - 2, static_cast<float>(w.y) - kTitleH - 2, static_cast<float>(w.w) + 4,
                                             static_cast<float>(w.h) + kTitleH + 4);
            const SDL_FRect inner = toScreen(static_cast<float>(w.x), static_cast<float>(w.y), static_cast<float>(w.w), static_cast<float>(w.h));
            // The frame and its label: the server's mode, then the window's own title.
            box::fill(renderer, outer, {10, 8, 6, 225});
            box::outline(renderer, outer, {160, 120, 40, 255});
            const std::string label = ui_->modeName() + ": " + w.title;
            app.font->draw(fitted(label, inner.w - 6 * scale_, px), outer.x + 4 * scale_, outer.y + 2 * scale_, px, SDL_Color{240, 180, 42, 255});
            // Its draws, inside it alone.
            const SDL_Rect clip{static_cast<int>(inner.x), static_cast<int>(inner.y), static_cast<int>(inner.w), static_cast<int>(inner.h)};
            SDL_SetRenderClipRect(renderer, &clip);
            for (const UiDraw& d : w.draws) {
                const SDL_Color c{d.color.r, d.color.g, d.color.b, d.color.a};
                const SDL_FRect r = toScreen(static_cast<float>(w.x) + d.x, static_cast<float>(w.y) + d.y, d.w, d.h);
                switch (d.kind) {
                case UiDraw::Kind::Box: box::fill(renderer, r, c); break;
                case UiDraw::Kind::Outline: box::outline(renderer, r, c); break;
                case UiDraw::Kind::Text: app.font->draw(d.text, r.x, r.y, px, c); break;
                case UiDraw::Kind::Image: drawImage(renderer, d, r); break;
                case UiDraw::Kind::Thing: drawThing(app, renderer, d, static_cast<float>(w.x), static_cast<float>(w.y)); break;
                }
            }
            SDL_SetRenderClipRect(renderer, clipped ? &oldClip : nullptr);
        }
    }

    bool handleEvent(App& app, const SDL_Event& event, int, int, float) override
    {
        if (!ui_ || ui_->windows().empty()) {
            return false;
        }
        if (event.type == SDL_EVENT_KEY_DOWN) {
            return keyDown(app, event);
        }
        // Left clicks; and right ones for a UI of API 2.
        if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN ||
            !(event.button.button == SDL_BUTTON_LEFT || (event.button.button == SDL_BUTTON_RIGHT && ui_->api() >= 2))) {
            return false;
        }
        const float ux = (event.button.x - originX_) / scale_;
        const float uy = (event.button.y - originY_) / scale_;
        // (Copied: the click may open or shut windows.)
        const std::vector<UiWindow> windows = ui_->windows();
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            const UiWindow& w = *it;
            const bool inFrame = ux >= w.x - 2 && ux < w.x + w.w + 2 && uy >= w.y - kTitleH - 2 && uy < w.y + w.h + 2;
            if (!inFrame) {
                continue;
            }
            if (ux >= w.x && ux < w.x + w.w && uy >= w.y && uy < w.y + w.h) {
                ui_->click(w.id, ux - static_cast<float>(w.x), uy - static_cast<float>(w.y), event.button.button == SDL_BUTTON_RIGHT ? 2 : 1);
            }
            return true; // (a click on a window is the UI's, taken or not)
        }
        return false;
    }

private:
    /// (API 2) A key of the fixed set, the mouse over one of its windows, no line being typed:
    /// the UI's when it takes it, else the game's.
    bool keyDown(App& app, const SDL_Event& event)
    {
        if (ui_->api() < 2 || event.key.repeat || (app.chat && app.chat->isOpen()) || (app.console && app.console->isOpen())) {
            return false;
        }
        const char* name = keyName(event.key.key);
        if (!name) {
            return false;
        }
        const std::vector<UiWindow>& windows = ui_->windows();
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            if (mouse_.x >= static_cast<float>(it->x) && mouse_.x < static_cast<float>(it->x + it->w) && mouse_.y >= static_cast<float>(it->y) &&
                mouse_.y < static_cast<float>(it->y + it->h)) {
                const std::string id = it->id; // (the key may open or shut windows)
                return ui_->key(id, name);
            }
        }
        return false;
    }

    void playSounds(App& app)
    {
        const auto sounds = ui_->takeSounds();
        if (!app.audio) {
            return;
        }
        const std::shared_ptr<const ModAssets>& set = ui_->assets();
        const nox::modes::script::UiSound* mod = nullptr; // (of a batch, the last: each cuts the one before)
        for (const auto& s : sounds) {
            if (s.game) {
                app.audio->play(s.name, s.volume);
            } else {
                mod = &s;
            }
        }
        if (mod && set && mod->index >= 0 && static_cast<std::size_t>(mod->index) < set->list.size()) {
            const ModAsset& a = set->list[static_cast<std::size_t>(mod->index)];
            if (a.kind == ModAsset::Kind::Sound) {
                app.audio->playClip(set, a.samples, a.rate, a.channels, mod->volume); // (the set held while it plays)
            }
        }
    }

    void drawImage(SDL_Renderer* renderer, const UiDraw& d, const SDL_FRect& r)
    {
        const ModAssets* set = ui_->assets().get();
        if (!set || d.index < 0 || static_cast<std::size_t>(d.index) >= set->list.size() || !(d.w > 0) || !(d.h > 0)) {
            return; // (a draw from before a new set came)
        }
        const ModAsset& a = set->list[static_cast<std::size_t>(d.index)];
        if (a.kind != ModAsset::Kind::Image) {
            return;
        }
        auto it = textures_.find(d.index);
        if (it == textures_.end()) {
            SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, a.width, a.height);
            if (t) {
                SDL_UpdateTexture(t, nullptr, a.rgba.data(), a.width * 4);
                SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            }
            it = textures_.emplace(d.index, t).first;
        }
        if (it->second) {
            SDL_RenderTexture(renderer, it->second, nullptr, &r);
        }
    }

    void drawThing(App& app, SDL_Renderer* renderer, const UiDraw& d, float wx, float wy)
    {
        if (!app.art || !app.sprites || d.index <= 0 || d.index > 0xFFFF) {
            return;
        }
        const TypeArt* art = app.art->get(static_cast<std::uint16_t>(d.index));
        if (!art || !art->still) {
            return;
        }
        const Sprite* sp = app.sprites->get(*art->still);
        if (!sp) {
            return;
        }
        // (Its picture centred on the point.)
        const SDL_FRect dst = toScreen(wx + d.x - sp->width / 2, wy + d.y - sp->height / 2, sp->width, sp->height);
        sp->scale = SDL_SCALEMODE_LINEAR;
        drawSprite(renderer, *sp, nullptr, dst);
    }

    void freeTextures()
    {
        for (auto& [index, t] : textures_) {
            if (t) {
                SDL_DestroyTexture(t);
            }
        }
        textures_.clear();
        texturesOf_.reset();
    }

    /// The UI's 640 x 480 fitted to the view, centred.
    void place(const HudFrame& hud)
    {
        scale_ = std::max(0.25f, std::min(static_cast<float>(hud.width) / 640.0f, static_cast<float>(hud.height) / 480.0f));
        originX_ = (static_cast<float>(hud.width) - 640.0f * scale_) / 2;
        originY_ = (static_cast<float>(hud.height) - 480.0f * scale_) / 2;
    }
    SDL_FRect toScreen(float x, float y, float w, float h) const { return {originX_ + x * scale_, originY_ + y * scale_, w * scale_, h * scale_}; }
    int textSize() const { return app_.font ? app_.font->lineSize(kTextLine, scale_) : 12; }
    std::string fitted(std::string s, float width, int px) const
    {
        while (s.size() > 1 && app_.font->measure(s, px) > width) {
            while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
                s.pop_back(); // (a letter's trailing bytes, then its lead)
            }
            if (!s.empty()) {
                s.pop_back();
            }
        }
        return s;
    }

    App& app_;
    ScriptUiClient* ui_ = nullptr;
    float scale_ = 1;
    float originX_ = 0;
    float originY_ = 0;
    SDL_FPoint mouse_{-1, -1}; ///< (in the UI's units)
    /// The mod's pictures as textures (by asset index), of the set `texturesOf_` (held: never
    /// another set at its address).
    std::map<int, SDL_Texture*> textures_;
    std::shared_ptr<const ModAssets> texturesOf_;
};

} // namespace

void registerScriptModeUi(std::vector<ModeUiEntry>& entries)
{
    ModeUiEntry e;
    e.modeId = "@script-ui"; // (not a registered mode: every script mode's, by its server's UI)
    e.make = [](App& app) { return std::make_unique<ScriptModeUi>(app); };
    e.makeSide = [](nox::client::HeadlessClient& c) { return std::make_unique<ScriptUiClient>(c); };
    entries.push_back(std::move(e));
}

} // namespace noxsdl
