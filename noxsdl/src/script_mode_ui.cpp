// The script modes' UI in noxcore.exe (mode_ui.hpp's ModeUi for modes/script's ScriptUiClient): the
// windows a server's UI script declares, each framed and labelled as the server's, its draws
// clipped inside it. The UI's 640 x 480 units are scaled to fit the game view, centred (an
// anchored window kept to its edge of the whole view: ScriptUiClient::setViewMargins). A click
// inside a window goes to the UI (and never to the game); the rest of the screen is the game's.
// A UI of API 2 also draws the mod's pictures (textures made as they're first drawn, let go when
// the set changes or the game goes) and thing types' pictures from our own art, plays sounds, is
// told of right clicks, and of a fixed set of keys while the mouse is over one of its windows and
// no chat or console line is open.
// A UI of API 3 also draws lines, a tooltip over everything (drawOver), labels over things in the
// world (drawWorld: only over what the scene drew, nudged apart, under the HUD), and windows with
// no frame, click-through ones (the game keeps the mouse there) and HUD pieces (drawUnderWindows:
// under every window, hidden while the game hides its HUD); it's told of drags, buttons let go
// and the wheel over its windows. Its input follows the draw order: framed windows over HUD
// pieces, and never a point the inventory, the summon cage or (over HUD pieces) the book holds; a
// press it took ends at its release whoever has the pointer then (release), or at a focus loss
// (mode_ui_pointer.hpp).
// A UI of API 4 also draws the game's own spell and ability icons (Hud::icon), and a frameless
// window that declares hit areas takes the mouse only inside them. With any script mode, a client
// of level 5 draws the things the server gave a draw scale (App::modeDrawScales, the scene) and
// their bursts (Effects::setBurstScale) at that scale.
#include "app.hpp"
#include "audio.hpp"
#include "chat_entry.hpp"
#include "cinematics.hpp"
#include "console.hpp"
#include "draw_box.hpp"
#include "effects.hpp"
#include "hud.hpp"
#include "inventory.hpp"
#include "minimap.hpp"
#include "mode_ui.hpp"
#include "mode_ui_pointer.hpp"
#include "spellbook.hpp"
#include "sprite_cache.hpp"
#include "summon_window.hpp"
#include "thing_art.hpp"
#include "trade_screens.hpp"

#include "noxcore/modes/script/script_ui.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace noxsdl {

namespace {

using nox::modes::script::ModAsset;
using nox::modes::script::ModAssets;
using nox::modes::script::ScriptUiClient;
using nox::modes::script::UiDraw;
using nox::modes::script::UiLabel;
using nox::modes::script::UiWindow;

using modeui::kTitleH;
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

/// Whether the game hides its HUD now: the GUI off (F11, a cutscene), a shop or a dialog up, the
/// full map, the console, a briefing.
bool gameHudHidden(const App& app)
{
    return !app.renderGui || (app.tradeScreens && app.tradeScreens->hidesHud()) || (app.minimap && app.minimap->shown()) ||
           (app.console && app.console->isOpen()) || (app.cinematics && app.cinematics->screenUp());
}

/// (API 3) Whether a game window drawn over a mod window of that kind (`hud`: a HUD piece) holds
/// the screen point (x, y): the open inventory and the summon cage are drawn over every mod window,
/// the book over the HUD pieces. (The shop and dialog windows take the mouse before the mod's.)
bool gameWindowOver(const App& app, float x, float y, bool hud)
{
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    return (app.inventory && app.inventory->isOpen() && app.inventory->contains(x, y, w, h)) ||
           (app.summon && app.summon->contains(x, y, w, h)) || (hud && app.book && app.book->contains(x, y, w, h));
}

/// A window, the game's own or one it opens, that the mod's tooltip and a release's mouseUp give
/// way to: the in-game menu and what it opens, a briefing.
bool menuUp(const App& app)
{
    return menuWindowUp(app) || (app.cinematics && app.cinematics->screenUp());
}

/// A line from (x1, y1) to (x2, y2), `width` pixels wide, in screen pixels.
void drawLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2, float width, SDL_Color c)
{
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (width < 1.5f || len < 0.01f) {
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        SDL_RenderLine(renderer, x1, y1, x2, y2);
        return;
    }
    // (A quad: the line's two ends pushed half its width either side.)
    const float nx = -dy / len * width / 2;
    const float ny = dx / len * width / 2;
    const SDL_FColor fc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
    const SDL_Vertex v[4] = {{{x1 + nx, y1 + ny}, fc, {}}, {{x2 + nx, y2 + ny}, fc, {}}, {{x2 - nx, y2 - ny}, fc, {}}, {{x1 - nx, y1 - ny}, fc, {}}};
    static constexpr int kQuad[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, v, 4, kQuad, 6);
}

class ScriptModeUi final : public ModeUi {
public:
    explicit ScriptModeUi(App& app) : app_(app) {}
    // (Its textures go when the game does, unbound; at the program's end, with the renderer.)

    void bind(nox::modes::ClientMode* side) override
    {
        ui_ = dynamic_cast<ScriptUiClient*>(side);
        freeTextures(); // (the last game's pictures: gone with it)
        pointer_.reset();
        app_.modeDrawScales.clear();
        if (app_.effects) {
            // (Client level 5) A burst near a missile the server drew at a scale: drawn so too.
            if (ui_) {
                app_.effects->setBurstScale([this](int x, int y, std::optional<int> size) -> std::optional<float> {
                    return ui_ ? ui_->burstScaleAt(static_cast<float>(x), static_cast<float>(y), size) : std::nullopt;
                });
            } else {
                app_.effects->setBurstScale({});
            }
        }
        if (ui_) {
            ui_->setTextWidth([this](std::string_view text) {
                if (!app_.font || scale_ <= 0) {
                    return static_cast<int>(text.size()) * 6;
                }
                return static_cast<int>(app_.font->measure(text, textSize()) / scale_);
            });
        }
    }

    bool release(App& app, const SDL_Event& event) override
    {
        // (API 3) The release of a press the windows took, whoever has the pointer now: the drag's
        // end, then mouseUp over the window there unless a menu is up; it goes no further.
        if (!ui_) {
            pointer_.reset();
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_RIGHT ? 2 : event.button.button == SDL_BUTTON_LEFT ? 1 : 0;
        const SDL_FPoint at = toUi(event.button.x, event.button.y);
        const std::optional<modeui::Hit> over = menuUp(app) ? std::nullopt : hit(app, event.button.x, event.button.y, false);
        return pointer_.release(*ui_, button, at.x, at.y, over);
    }

    void inputLost(App&) override
    {
        pointer_.lost(ui_); // (a drag's end where the mouse last was)
    }

    void update(App& app) override
    {
        // (API 3) A drag's latest move, once a frame; then the labels' objects for the scene
        // (liveSprites), by net code: after the drag hook, which may change the labels.
        app.modeLabels.clear();
        // (Client level 5) The draw scales the server told, by net code for the scene: whether a
        // UI runs or not (any script mode's things).
        app.modeDrawScales.clear();
        if (ui_) {
            for (const nox::modes::script::UiDrawScale& s : ui_->drawScales()) {
                if (!s.gone) {
                    app.modeDrawScales.push_back({s.object, s.type, s.scale});
                }
            }
            std::ranges::sort(app.modeDrawScales, {}, &App::ModeDrawScale::object);
        }
        if (!ui_ || ui_->api() < 3) {
            pointer_.reset(); // (a UI stopped or replaced mid-press)
            return;
        }
        pointer_.frame(*ui_);
        if (!ui_ || ui_->api() < 3) {
            return;
        }
        const auto& labels = ui_->labels();
        for (std::size_t i = 0; i < labels.size(); ++i) {
            app.modeLabels.emplace_back(labels[i].object, static_cast<std::uint16_t>(i));
        }
        std::ranges::sort(app.modeLabels);
    }

    void drawWorld(App& app, SDL_Renderer* renderer) override
    {
        placed_.clear(); // (none drawn this frame: none to click)
        placedFrame_ = app.frames;
        if (!ui_ || ui_->api() < 3 || app.modeLabelSpots.empty() || !app.font || gameHudHidden(app)) {
            return;
        }
        drawLabels(app, renderer);
    }

    bool clickableAt(const App& app, float x, float y) const override { return labelClickAt(app, x, y).has_value(); }

    /// (Client level 7) The server's ground decals: each its mod image flat on the floor, centred
    /// on its point (or on the thing it follows while we have it), `size` world units across at the
    /// camera's zoom, turned by its spin and swung by its pulse as time goes (nothing per frame from
    /// the server); tinted, additive when it glows. Off screen: not drawn.
    void drawFloor(App& app, SDL_Renderer* renderer) override
    {
        if (!ui_ || ui_->decals().empty() || !app.live) {
            return;
        }
        const double t = static_cast<double>(SDL_GetTicks()) / 1000.0;
        const auto& world = app.live->client().world();
        const float zoom = app.camera.zoom;
        for (const nox::modes::script::uimsg::Decal& d : ui_->decals()) {
            SDL_Texture* tex = textureOf(renderer, d.asset);
            if (!tex) {
                continue;
            }
            float x = d.x, y = d.y;
            if (d.follow != 0) {
                if (const auto* o = world.drawable(d.follow)) {
                    x = static_cast<float>(o->x);
                    y = static_cast<float>(o->y);
                }
            }
            float size = d.size;
            float alpha = d.a / 255.0f;
            if (d.pulsePeriodMs != 0 && d.pulseAmount != 0) {
                const double wave = std::sin(2 * 3.14159265358979 * t * 1000.0 / d.pulsePeriodMs); // (-1 to 1)
                const float amount = d.pulseAmount / 255.0f;
                if (d.flags & nox::modes::script::uimsg::kDecalPulseScale) {
                    size *= 1.0f + 0.5f * amount * static_cast<float>(wave); // (half its amount either way)
                } else {
                    alpha *= 1.0f - amount * static_cast<float>(0.5 - 0.5 * wave); // (down to 1 - amount)
                }
            }
            const float px = size * zoom;
            const float cx = (x - app.camera.left()) * zoom;
            const float cy = (y - app.camera.top()) * zoom;
            if (cx + px < 0 || cy + px < 0 || cx - px > app.camera.viewWidth || cy - px > app.camera.viewHeight) {
                continue;
            }
            const double angle = std::fmod(static_cast<double>(d.spin) * t, 360.0);
            const SDL_FRect dst{cx - px / 2, cy - px / 2, px, px};
            SDL_SetTextureColorMod(tex, d.r, d.g, d.b);
            SDL_SetTextureAlphaModFloat(tex, std::clamp(alpha, 0.0f, 1.0f));
            SDL_SetTextureBlendMode(tex, (d.flags & nox::modes::script::uimsg::kDecalGlow) ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
            SDL_RenderTextureRotated(renderer, tex, nullptr, &dst, angle, nullptr, SDL_FLIP_NONE);
            // (The texture as the UI's images draw it again.)
            SDL_SetTextureColorMod(tex, 255, 255, 255);
            SDL_SetTextureAlphaModFloat(tex, 1.0f);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        }
    }

    void drawUnderWindows(App& app, SDL_Renderer* renderer, const HudFrame& hud) override
    {
        if (!ui_) {
            return;
        }
        step(app, hud);
        if (!hud.renderGui || !app.font || !ui_->hudShown()) {
            return;
        }
        // The HUD pieces, under every window (and the game's own windows drawn after them).
        drawWindows(app, renderer, true);
    }

    void drawHud(App& app, SDL_Renderer* renderer, const HudFrame& hud) override
    {
        if (!ui_) {
            return;
        }
        if (!stepped_) {
            step(app, hud); // (drawUnderWindows not called this frame)
        }
        stepped_ = false;
        if (!hud.renderGui || !app.font || ui_->windows().empty()) {
            return;
        }
        drawWindows(app, renderer, false);
    }

    void drawOver(App& app, SDL_Renderer* renderer, const HudFrame& hud) override
    {
        // (Only in a frame the UI's frame ran, at Stage::InGame: not over a map's load. For API 3,
        // not where a game window drawn over the mod's has the pointer.)
        if (!ui_ || stepFrame_ != app.frames || ui_->tooltip().empty() || !app.font || menuUp(app) ||
            (ui_->api() >= 3 && gameWindowOver(app, hud.mouse.x, hud.mouse.y, false))) {
            return;
        }
        place(hud);
        drawTooltip(app, renderer);
    }

    bool handleEvent(App& app, const SDL_Event& event, int, int, float) override
    {
        if (!ui_ || ui_->api() < 3) {
            pointer_.reset(); // (none of API 3 runs: no press of its)
        }
        if (!ui_) {
            return false;
        }
        if (ui_->api() >= 3) {
            // (API 3) Moves and the wheel (releases come through release, first).
            switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION: {
                const SDL_FPoint at = toUi(event.motion.x, event.motion.y);
                pointer_.moved(at.x, at.y);
                return false; // (the mouse is the game's too: told to a drag once a frame, update)
            }
            case SDL_EVENT_MOUSE_WHEEL: return wheel(app, event);
            default: break;
            }
        }
        // (API 5) A left click on a clickable label (none of the windows there): the UI's alone.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            if (const std::optional<std::uint16_t> object = labelClickAt(app, event.button.x, event.button.y)) {
                ui_->labelClick(*object);
                return true;
            }
        }
        if (ui_->windows().empty()) {
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
        // The window there, framed ones and their frames over the HUD pieces (an API 1 or 2 UI's
        // windows: newest first, frame and title included, as always).
        const std::optional<modeui::Hit> h = hit(app, event.button.x, event.button.y, true);
        if (!h) {
            return false;
        }
        if (h->inside) {
            const SDL_FPoint at = toUi(event.button.x, event.button.y);
            const int button = event.button.button == SDL_BUTTON_RIGHT ? 2 : 1;
            if (ui_->api() >= 3) {
                pointer_.press(*ui_, h->window, button, at.x, at.y, h->x, h->y);
            }
            ui_->click(h->window, at.x - h->x, at.y - h->y, button);
        }
        return true; // (a click on a window is the UI's, taken or not)
    }

private:
    /// The window the screen point (sx, sy) goes to (mode_ui_pointer.hpp's hitWindow): for API 3
    /// never where a game window drawn over it has the point. `frame`: frames and titles count.
    std::optional<modeui::Hit> hit(const App& app, float sx, float sy, bool frame) const
    {
        const SDL_FPoint at = toUi(sx, sy);
        return modeui::hitWindow(*ui_, at.x, at.y, frame, [&](bool hud) { return gameWindowOver(app, sx, sy, hud); });
    }

    /// Once a frame: the UI's place, the mouse, the HUD's state; the UI's frame run; its sounds.
    void step(App& app, const HudFrame& hud)
    {
        stepFrame_ = app.frames;
        mouseScreen_ = hud.mouse;
        place(hud);
        ui_->setViewMargins(originX_ / scale_, originY_ / scale_); // (the anchored windows' edges)
        mouse_ = {(hud.mouse.x - originX_) / scale_, (hud.mouse.y - originY_) / scale_};
        ui_->setMouse(std::pair{mouse_.x, mouse_.y});
        ui_->setHudShown(!gameHudHidden(app));
        ui_->frame(hud.nowMs);
        if (ui_->assets() != texturesOf_) {
            freeTextures(); // (a new set: its pictures made as they're drawn)
            texturesOf_ = ui_->assets();
        }
        // (API 5) The buffs' row kept out of the UI's reserved box (nox.hudReserve): its bottom on the screen.
        if (app.inventory) {
            const auto box = ui_->hudReserve();
            const SDL_FRect r = box ? toScreen(box->x, box->y, box->w, box->h) : SDL_FRect{};
            app.inventory->setBuffsBelow(box && box->w > 0 && box->h > 0 ? std::max(0.0f, r.y + r.h) : 0.0f);
        }
        playSounds(app);
        stepped_ = true;
    }

    /// The windows that are HUD pieces (`hud`) or the others, bottom to top.
    void drawWindows(App& app, SDL_Renderer* renderer, bool hud)
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_Rect oldClip{};
        const bool clipped = SDL_RenderClipEnabled(renderer);
        if (clipped) {
            SDL_GetRenderClipRect(renderer, &oldClip);
        }
        const int px = textSize();
        for (const UiWindow& w : ui_->windows()) {
            if (w.hud != hud) {
                continue;
            }
            const SDL_FRect inner = toScreen(static_cast<float>(w.x), static_cast<float>(w.y), static_cast<float>(w.w), static_cast<float>(w.h));
            if (w.frame) {
                // The frame and its label: the server's mode, then the window's own title.
                const SDL_FRect outer = toScreen(static_cast<float>(w.x) - 2, static_cast<float>(w.y) - kTitleH - 2, static_cast<float>(w.w) + 4,
                                                 static_cast<float>(w.h) + kTitleH + 4);
                box::fill(renderer, outer, {10, 8, 6, 225});
                box::outline(renderer, outer, {160, 120, 40, 255});
                const std::string label = ui_->modeName() + ": " + w.title;
                app.font->draw(fitted(label, inner.w - 6 * scale_, px), outer.x + 4 * scale_, outer.y + 2 * scale_, px, SDL_Color{240, 180, 42, 255});
            }
            // Its draws, inside it alone.
            const SDL_Rect clip{static_cast<int>(inner.x), static_cast<int>(inner.y), static_cast<int>(inner.w), static_cast<int>(inner.h)};
            SDL_SetRenderClipRect(renderer, &clip);
            for (const UiDraw& d : w.draws) {
                const SDL_Color c{d.color.r, d.color.g, d.color.b, d.color.a};
                const SDL_FRect r = toScreen(static_cast<float>(w.x) + d.x, static_cast<float>(w.y) + d.y, d.w, d.h);
                switch (d.kind) {
                case UiDraw::Kind::Box: box::fill(renderer, r, c); break;
                case UiDraw::Kind::Outline: box::outline(renderer, r, c); break;
                case UiDraw::Kind::Text:
                    app.font->draw(d.text, r.x, r.y, d.size != 1 ? app.font->lineSize(kTextLine * d.size, scale_) : px, c);
                    break;
                case UiDraw::Kind::Image: drawImage(renderer, d, r); break;
                case UiDraw::Kind::Thing: drawThing(app, renderer, d, static_cast<float>(w.x), static_cast<float>(w.y)); break;
                case UiDraw::Kind::SpellIcon: drawSpellIcon(app, renderer, d, static_cast<float>(w.x), static_cast<float>(w.y)); break;
                case UiDraw::Kind::Line: {
                    const SDL_FRect to = toScreen(static_cast<float>(w.x) + d.w, static_cast<float>(w.y) + d.h, 0, 0);
                    drawLine(renderer, r.x, r.y, to.x, to.y, d.width * scale_, c);
                    break;
                }
                }
            }
            SDL_SetRenderClipRect(renderer, clipped ? &oldClip : nullptr);
        }
    }

    /// (API 3) The tooltip at the mouse, whole on the screen.
    void drawTooltip(App& app, SDL_Renderer* renderer)
    {
        const auto& lines = ui_->tooltip();
        const int px = textSize();
        const float lineH = app.font->lineHeight(px) / scale_; // (UI units)
        float widest = 0;
        for (const auto& l : lines) {
            widest = std::max(widest, app.font->measure(l.text, px) / scale_);
        }
        const float w = widest + 8;
        const float h = lineH * static_cast<float>(lines.size()) + 6;
        const auto [x, y] = nox::modes::script::placeTooltip(mouse_.x, mouse_.y, w, h, originX_ / scale_, originY_ / scale_);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        const SDL_FRect r = toScreen(x, y, w, h);
        box::fill(renderer, r, {10, 8, 6, 235});
        box::outline(renderer, r, {160, 120, 40, 255});
        float ty = y + 3;
        for (const auto& l : lines) {
            const SDL_FRect at = toScreen(x + 4, ty, 0, 0);
            app.font->draw(l.text, at.x, at.y, px, SDL_Color{l.color.r, l.color.g, l.color.b, l.color.a});
            ty += lineH;
        }
    }

    /// (API 3) The labels over what the scene drew this frame (App::modeLabelSpots): those shown
    /// now (Alt held, our player near), on the screen, stacked upwards where they'd overlap.
    void drawLabels(App& app, SDL_Renderer* renderer)
    {
        const auto& labels = ui_->labels();
        const Camera& cam = app.camera;
        const auto& client = app.live->client();
        const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
        std::optional<SDL_FPoint> self;
        if (const auto it = client.world().drawables().find(client.netCode()); it != client.world().drawables().end()) {
            const auto p = app.interpolator.position(it->first, it->second);
            self = SDL_FPoint{p.x, p.y};
        }
        const int px = std::clamp(static_cast<int>(std::lround(10 * cam.zoom)), 9, 48);
        const float lineH = app.font->lineHeight(px);
        placed_.clear();
        for (const App::ModeLabelSpot& spot : app.modeLabelSpots) {
            if (spot.index >= labels.size()) {
                continue;
            }
            const UiLabel& l = labels[spot.index];
            if ((l.show == UiLabel::Show::Alt && !alt) ||
                (l.show == UiLabel::Show::Near && (!self || std::hypot(self->x - spot.x, self->y - spot.y) > l.near))) {
                continue;
            }
            const float w = app.font->measure(l.text, px) + 6;
            const float h = lineH + 2;
            const float cx = (spot.x - cam.left()) * cam.zoom;
            const float bottom = (spot.top - l.offset - cam.top()) * cam.zoom;
            const SDL_FRect r{std::round(cx - w / 2), std::round(bottom - h), w, h};
            if (r.x + r.w < 0 || r.x > static_cast<float>(cam.viewWidth) || r.y + r.h < 0 || r.y > static_cast<float>(cam.viewHeight)) {
                continue; // (off the screen: not laid out)
            }
            placed_.push_back({r, spot.index, l.object});
        }
        // The nearest the bottom first; each later one moved up above any it overlaps (a few
        // passes at most: n is the labels on the screen, at most kUiMaxLabels).
        std::ranges::sort(placed_, [](const LabelBox& a, const LabelBox& b) { return a.r.y + a.r.h > b.r.y + b.r.h; });
        for (std::size_t i = 1; i < placed_.size(); ++i) {
            SDL_FRect& r = placed_[i].r;
            for (int pass = 0; pass < 8; ++pass) {
                bool moved = false;
                for (std::size_t j = 0; j < i; ++j) {
                    const SDL_FRect& o = placed_[j].r;
                    if (r.x < o.x + o.w && o.x < r.x + r.w && r.y < o.y + o.h && o.y < r.y + r.h) {
                        r.y = o.y - r.h - 1;
                        moved = true;
                    }
                }
                if (!moved) {
                    break;
                }
            }
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        // (API 5) The clickable one under the pointer brightened.
        const SDL_FPoint m = mousePos(app);
        const std::optional<std::uint16_t> hovered = labelClickAt(app, m.x, m.y);
        for (const LabelBox& b : placed_) {
            const UiLabel& l = labels[b.index];
            if (hovered && b.object == *hovered) {
                box::fill(renderer, b.r, {60, 46, 16, 210});
                box::outline(renderer, b.r, {240, 180, 42, 255});
            } else {
                box::fill(renderer, b.r, {0, 0, 0, 160});
            }
            app.font->draw(l.text, b.r.x + 3, b.r.y + 1, px, SDL_Color{l.color.r, l.color.g, l.color.b, l.color.a});
        }
    }

    /// (API 5) The object of the clickable label at screen point (x, y) as last drawn: the topmost
    /// label there (drawn last) when it was set with click = true; none where a window (the mod's
    /// or the game's), the HUD or a menu has the point, or while a spell waits for its target.
    std::optional<std::uint16_t> labelClickAt(const App& app, float x, float y) const
    {
        if (!ui_ || ui_->api() < 5 || placed_.empty() || placedFrame_ + 1 < app.frames || app.armed || menuUp(app)) {
            return std::nullopt;
        }
        const LabelBox* top = nullptr;
        for (auto it = placed_.rbegin(); it != placed_.rend(); ++it) {
            const SDL_FRect& r = it->r;
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                top = &*it;
                break;
            }
        }
        const auto& labels = ui_->labels();
        if (!top || top->index >= labels.size() || labels[top->index].object != top->object || !labels[top->index].click) {
            return std::nullopt; // (none there, or the topmost isn't clickable: the game's)
        }
        if (gameWindowOver(app, x, y, true) || hit(app, x, y, true) ||
            (app.tradeScreens && app.tradeScreens->contains(x, y, app.camera.viewWidth, app.camera.viewHeight))) {
            return std::nullopt;
        }
        if (app.hud && app.live) {
            const auto& client = app.live->client();
            if (app.hud->hit(client.world(), client.netCode(), quickbarState(app), x, y, app.camera.viewWidth, app.camera.viewHeight)) {
                return std::nullopt;
            }
        }
        return top->object;
    }

    /// (API 3) The wheel over one of the windows (its frame and title bar too, as a click there):
    /// the UI's, in whole steps.
    bool wheel(const App& app, const SDL_Event& event)
    {
        const std::optional<modeui::Hit> h = hit(app, event.wheel.mouse_x, event.wheel.mouse_y, true);
        if (!h) {
            wheelLeft_ = 0;
            return false;
        }
        wheelLeft_ += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        const int steps = static_cast<int>(std::trunc(wheelLeft_)); // (a fine wheel's parts add up)
        wheelLeft_ -= static_cast<float>(steps);
        if (steps != 0) {
            const SDL_FPoint at = toUi(event.wheel.mouse_x, event.wheel.mouse_y);
            ui_->wheel(h->window, at.x - h->x, at.y - h->y, steps);
        }
        return true;
    }

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
        if (const std::optional<modeui::Hit> h = hit(app, mouseScreen_.x, mouseScreen_.y, false)) {
            return ui_->key(h->window, name);
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

    /// The mod's image `index` as a texture (made at its first use, let go with the set); null: no
    /// such image in the set now (a draw from before a new set came, or a sound).
    SDL_Texture* textureOf(SDL_Renderer* renderer, int index)
    {
        const ModAssets* set = ui_->assets().get();
        if (!set || index < 0 || static_cast<std::size_t>(index) >= set->list.size()) {
            return nullptr;
        }
        const ModAsset& a = set->list[static_cast<std::size_t>(index)];
        if (a.kind != ModAsset::Kind::Image) {
            return nullptr;
        }
        auto it = textures_.find(index);
        if (it == textures_.end()) {
            SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, a.width, a.height);
            if (t) {
                SDL_UpdateTexture(t, nullptr, a.rgba.data(), a.width * 4);
                SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            }
            it = textures_.emplace(index, t).first;
        }
        return it->second;
    }

    void drawImage(SDL_Renderer* renderer, const UiDraw& d, const SDL_FRect& r)
    {
        if (!(d.w > 0) || !(d.h > 0)) {
            return;
        }
        if (SDL_Texture* t = textureOf(renderer, d.index)) {
            SDL_RenderTexture(renderer, t, nullptr, &r);
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

    /// (API 4) A spell's or an ability's icon as the book and the bars draw it (Hud::icon: the HD
    /// one when the HD pack has it), its top left at (x, y), its size or w x h.
    void drawSpellIcon(App& app, SDL_Renderer* renderer, const UiDraw& d, float wx, float wy)
    {
        if (!app.hud || !app.sprites || d.index <= 0) {
            return;
        }
        const auto icon = app.hud->icon(static_cast<std::uint32_t>(d.index), false);
        const Sprite* sp = icon ? app.sprites->get(*icon) : nullptr;
        if (!sp) {
            return;
        }
        const float w = d.w > 0 ? d.w : sp->width;
        const float h = d.h > 0 ? d.h : sp->height;
        sp->scale = SDL_SCALEMODE_LINEAR;
        drawSprite(renderer, *sp, nullptr, toScreen(wx + d.x, wy + d.y, w, h));
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
    SDL_FPoint toUi(float x, float y) const { return {(x - originX_) / scale_, (y - originY_) / scale_}; }
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

    /// (A label laid out this frame: its box on the screen, its index among the labels and its object.)
    struct LabelBox {
        SDL_FRect r;
        std::size_t index;
        std::uint16_t object;
    };

    App& app_;
    ScriptUiClient* ui_ = nullptr;
    float scale_ = 1;
    float originX_ = 0;
    float originY_ = 0;
    SDL_FPoint mouse_{-1, -1}; ///< (in the UI's units)
    SDL_FPoint mouseScreen_{-100000, -100000}; ///< (the same, on the screen)
    bool stepped_ = false;     ///< (the UI's frame run this frame, in drawUnderWindows)
    long stepFrame_ = -1;      ///< (App::frames when it last ran)
    modeui::Pointer pointer_;  ///< (API 3: a press in a window and its drag)
    float wheelLeft_ = 0;      ///< (a fine wheel's part of a step)
    std::vector<LabelBox> placed_; ///< (kept: no allocation a frame)
    long placedFrame_ = -1;        ///< (App::frames when placed_ was laid out: stale ones take no click)
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
