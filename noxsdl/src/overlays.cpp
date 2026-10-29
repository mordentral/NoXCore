// What's drawn over a game's world (app.hpp): the frame-rate counter, the hover ring, the kill
// feed, names and flag labels, the game-over banner, health bars, friend dots, damage numbers,
// chat bubbles and tooltips.
#include "app.hpp"
#include "player_art.hpp"
#include "team_colors.hpp"

#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>

namespace noxsdl {

namespace {

/// Text at screen (x, y) over its dark shadow, `offset` pixels down and right.
void drawShadowed(Font& font, std::string_view text, float x, float y, int size, SDL_Color color, float offset)
{
    font.draw(text, x + offset, y + offset, size, {8, 8, 8, 255});
    font.draw(text, x, y, size, color);
}

/// The size of the text drawn in the world (names, flag labels, bubbles): 11 at the camera's
/// zoom, 9 to 64 pixels.
int worldFontSize(const Camera& cam)
{
    return std::clamp(static_cast<int>(std::lround(11 * cam.zoom)), 9, 64);
}

} // namespace

/// The frame-rate counter: frames a second and the average frame time over the last second,
/// in the top-right corner, over everything but the cursor.
void drawFps(App& app)
{
    const std::uint64_t now = SDL_GetTicks();
    ++app.fpsFrames;
    if (app.fpsSince == 0) {
        app.fpsSince = now;
    } else if (now - app.fpsSince >= 1000) {
        const auto elapsed = static_cast<float>(now - app.fpsSince);
        app.fps = static_cast<float>(app.fpsFrames) * 1000.0f / elapsed;
        app.frameMs = elapsed / static_cast<float>(app.fpsFrames);
        app.fpsFrames = 0;
        app.fpsSince = now;
    }
    if (!app.showFps || !app.font || app.fps <= 0) {
        return;
    }
    char text[112];
    if (app.live && (app.interpolator.smoothing() || app.interpolator.scrolling())) {
        // Smooth Motion's delay behind the server (it adapts to the connection).
        // With --predict-movement: how many frames ahead of the server we're drawn.
        const int lag = app.interpolator.predicting() ? app.interpolator.predictor().lagFrames() : 0;
        // And with Other Units' Ahead, how many frames others are moved on.
        const double others = app.interpolator.aheadFrames();
        const int n = std::snprintf(text, sizeof text, lag ? "%.0f fps  %.1f ms  delay %.0f ms  ahead %d" : "%.0f fps  %.1f ms  delay %.0f ms",
                                    app.fps, app.frameMs, app.interpolator.delayMs(), lag);
        std::size_t used = n > 0 ? static_cast<std::size_t>(n) : 0;
        if (others > 0 && used < sizeof text) {
            const int m = std::snprintf(text + used, sizeof text - used, "  others +%.1f", others);
            used += m > 0 ? static_cast<std::size_t>(m) : 0;
        }
        // Updates that came out of order (kept from putting anything back): the connection's.
        if (const std::uint32_t late = app.live->client().lateUpdateCount(); late > 0 && used < sizeof text) {
            std::snprintf(text + used, sizeof text - used, "  late %u", late);
        }
    } else {
        std::snprintf(text, sizeof text, "%.0f fps  %.1f ms", app.fps, app.frameMs);
    }
    const float scale = Hud::scale(app.camera.viewWidth, app.camera.viewHeight);
    const int size = static_cast<int>(std::lround(12 * scale));
    const float x = static_cast<float>(app.camera.viewWidth) - app.font->measure(text, size) - 6 * scale;
    const float y = 4 * scale;
    drawShadowed(*app.font, text, x, y, size, {255, 255, 255, 255}, scale);
}

/// Whether the drawable under the cursor is an enemy (video_cursorDrawImpl): a monster or player
/// not on our friends list. Its hover ring is red and the arrow cursor turns red; anything else
/// under the cursor gets blue.
bool hoveredEnemy(const App& app)
{
    namespace oc = nox::things::ObjectClass;
    if (!app.live || !app.hovered) {
        return false;
    }
    const auto& world = app.live->client().world();
    const auto* d = world.drawable(*app.hovered);
    const TypeArt* art = d ? app.art->get(d->typeId) : nullptr;
    return art && (art->classes & (oc::Monster | oc::Player)) && !world.isFriend(*app.hovered);
}

/// The ring at the feet of what's under the cursor (select.c), drawn on the
/// floor under the objects: three circles (one thick line once scaled up), the middle one solid
/// and the others half see-through, of the thing's EXTENT radius + 6 (a box's (width + height) / 2 + 6). An enemy creature's
/// flickers dark red and red, anything else's white, light blue and blue: a new pick each game
/// frame. Not for a trigger's. The ring's world centre, radius and colour (the 3D view lays it on
/// its floor), or nothing.
std::optional<View3D::Ring> hoverRing(App& app)
{
    if (!app.live || !app.renderGui) {
        return std::nullopt;
    }
    namespace oc = nox::things::ObjectClass;
    float wx = 0;
    float wy = 0;
    const TypeArt* art = nullptr;
    bool enemy = false;
    std::uint32_t seed = 0;
    if (app.hovered) {
        const auto* d = app.live->client().world().drawable(*app.hovered);
        art = d ? app.art->get(d->typeId) : nullptr;
        if (!art) {
            return std::nullopt;
        }
        const auto p = app.interpolator.position(*app.hovered, *d);
        wx = p.x;
        wy = p.y;
        enemy = hoveredEnemy(app);
        seed = *app.hovered;
    } else if (app.hoveredUseable) {
        art = app.hoveredUseable->art;
        if (!art) {
            return std::nullopt;
        }
        wx = app.hoveredUseable->x;
        wy = app.hoveredUseable->y;
        seed = app.hoveredUseable->extent;
    } else {
        return std::nullopt;
    }
    if (art->classes & oc::Trigger) {
        return std::nullopt;
    }
    const float r = art->extentBox ? std::floor((art->extentWidth + art->extentHeight) / 2) + 6
                    : art->extentRadius > 0 ? std::floor(art->extentRadius) + 6
                                            : 6;
    // Math_RandIntVisual each frame the game draws (30 a second): picked from the game frame.
    const auto frame = gameFrame(app);
    const std::uint32_t pick = ((frame * 2654435761u) ^ (seed * 40503u)) >> 16;
    const std::uint8_t color = enemy ? (pick % 2 == 0 ? DarkRed : Red)
                                     : (pick % 3 == 0 ? White : pick % 3 == 1 ? LightBlue : Blue);
    return View3D::Ring{wx, wy, r, paletteColor(color)};
}

/// Draws hoverRing's ring on the 2D view's floor, in screen pixels.
void drawHoverRing(App& app)
{
    const auto ring = hoverRing(app);
    if (!ring) {
        return;
    }
    const float r = ring->radius;
    const SDL_Color c = ring->colour;
    const float cx = (ring->x - app.camera.left()) * app.camera.zoom;
    const float cy = (ring->y - app.camera.top()) * app.camera.zoom;
    // The game's three one-pixel circles (video_drawCircle at r - 1, r, r + 1), drawn
    // at 640 x 480 and scaled up, run together into one thick line; drawn here as that line, a
    // band 3 game pixels wide: the solid middle pixel between the half see-through outer ones.
    const auto band = [&](float inner, float outer, std::uint8_t alpha) {
        const float ri = inner * app.camera.zoom;
        const float ro = outer * app.camera.zoom;
        const int n = std::clamp(static_cast<int>(ro * 0.75f), 32, 256);
        const SDL_FColor fc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha / 255.0f};
        std::vector<SDL_Vertex> v;
        std::vector<int> idx;
        v.reserve(static_cast<std::size_t>(2 * (n + 1)));
        idx.reserve(static_cast<std::size_t>(6 * n));
        // A strip of quads round the circle: an inner and an outer vertex each step, two triangles to the next.
        for (int i = 0; i <= n; ++i) {
            const float a = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n);
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            v.push_back({{cx + ri * ca, cy + ri * sa}, fc, {}});
            v.push_back({{cx + ro * ca, cy + ro * sa}, fc, {}});
            if (i < n) {
                const int k = 2 * i;
                idx.insert(idx.end(), {k, k + 1, k + 2, k + 1, k + 3, k + 2});
            }
        }
        SDL_RenderGeometry(app.renderer, nullptr, v.data(), static_cast<int>(v.size()), idx.data(),
                           static_cast<int>(idx.size()));
    };
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    band(r - 1.5f, r - 0.5f, 0x80);
    band(r - 0.5f, r + 0.5f, 255);
    band(r + 0.5f, r + 1.5f, 0x80);
}

/// The kill feed, as the game draws it: the kills of the last 91 frames,
/// a row each from the top of the screen, 36 game pixels tall, as many as a quarter of the screen
/// holds. A row, centred on a dark bar: the killer's name (and "+" a helper's), the icon of what
/// killed (a weapon's or missile's PRETTYIMAGE, or a damage type's spell or ability icon), then
/// the victim's name. Names light grey, ours green. Options' Kill Feed switches it off.
void drawKillFeed(App& app)
{
    constexpr std::uint32_t kShowFrames = 91; // (0x5B)
    constexpr float kRow = 36;
    const auto frame = gameFrame(app);
    // Drop the kills that have had their time (one is kept while the clock reads earlier than its frame).
    while (!app.kills.empty() && frame - app.kills.front().frame >= kShowFrames && frame >= app.kills.front().frame) {
        app.kills.pop_front();
    }
    if (!app.killFeed || !app.font || !app.live || !app.renderGui || app.kills.empty()) {
        return;
    }
    const auto& client = app.live->client();
    const std::uint16_t me = client.netCode();
    const auto nameOf = [&](std::uint16_t nc) -> std::string {
        const auto it = client.world().players().find(nc);
        return it != client.world().players().end() ? it->second.name : std::string();
    };
    // What killed: the feed's draw's icons (kind 1 a thing type, kind 2 a damage type; else spell 15's).
    const auto iconOf = [&](const App::KillLine& k) -> std::optional<nox::things::ImageRef> {
        const auto spellIcon = [&](std::uint32_t id) -> std::optional<nox::things::ImageRef> {
            const auto* s = app.things.findSpell(id);
            return s ? std::optional<nox::things::ImageRef>(s->icon) : std::nullopt;
        };
        // Kind 1: `weapon` is a thing type id (1-based).
        if (k.kind == 1 && k.weapon >= 1 && k.weapon <= app.things.things.size()) {
            if (const auto& img = app.things.things[k.weapon - 1u].prettyImage) {
                return img;
            }
        } else if (k.kind == 2) {
            switch (k.weapon) {
            case 1: case 0xC: return spellIcon(5);
            case 2:
                if (!app.things.abilities.empty()) {
                    return app.things.abilities[0].icons[0];
                }
                break;
            case 4: return spellIcon(0x82);
            case 5: return spellIcon(0x3C);
            case 9: case 0x11: return spellIcon(0x2B);
            case 0xF: return spellIcon(0x38);
            case 0x10: return spellIcon(0x10);
            default: break;
            }
        }
        return spellIcon(0xF);
    };
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    const float s = Hud::scale(w, h);
    const int size = static_cast<int>(std::lround(15 * s)); // (the "large" font)
    const int rows = static_cast<int>(static_cast<float>(h) / s / 4 / kRow);  // as many rows as fill a quarter of the screen
    const SDL_Color grey{212, 212, 212, 255};
    const SDL_Color green{0, 255, 0, 255};
    int row = 0;
    for (const auto& k : app.kills) {
        if (row >= rows) {
            break;
        }
        if (frame - k.frame >= kShowFrames) {
            continue;
        }
        const std::string killer = nameOf(k.attacker);
        std::string helper = nameOf(k.assist);
        if (!helper.empty() && !killer.empty()) {
            helper = "+" + helper;
        }
        const std::string victim = nameOf(k.victim);
        const auto icon = iconOf(k);
        // A weapon's (or wand's) icon in its type's own colours, as it shows in the inventory
        // (the feed's draw sets them for WEAPON and WAND types); else as it is.
        std::optional<SpriteCache::Materials> colours;
        if (k.kind == 1 && icon) {
            if (const TypeArt* a = app.art->get(k.weapon)) {
                colours = itemMaterials(*a, nullptr, app.modifiers);
            }
        }
        const Sprite* sp = !icon ? nullptr : colours ? app.sprites->tinted(*icon, *colours) : app.sprites->get(*icon);
        const float iconW = sp ? sp->width * s : 0;
        const float iconH = sp ? sp->height * s : 0;
        const float total = app.font->measure(killer, size) + app.font->measure(helper, size) + 10 * s + iconW +
                            app.font->measure(victim, size);
        // The row centred on the screen, on a half-dark bar 5 game pixels wider each side.
        float x = (static_cast<float>(w) - total) / 2;
        const float top = static_cast<float>(row) * kRow * s;
        SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 128);
        const SDL_FRect bar{x - 5 * s, top, total + 10 * s, kRow * s};
        SDL_RenderFillRect(app.renderer, &bar);
        const float ty = top + (kRow * s - app.font->lineHeight(size)) / 2;  // the text centred in the row's height
        if (!killer.empty()) {
            app.font->draw(killer, x, ty, size, k.attacker == me ? green : grey);
            x += app.font->measure(killer, size);
        }
        if (!helper.empty()) {
            app.font->draw(helper, x, ty, size, k.assist == me ? green : grey);
            x += app.font->measure(helper, size);
        }
        if (sp) {
            const SDL_FRect dst{x + 5 * s, top + (kRow * s - iconH) / 2, iconW, iconH};
            app.sprites->render(*sp, nullptr, dst); // (a tinted one is shaded as it's drawn)
        }
        x += 10 * s + iconW;
        if (!victim.empty()) {
            app.font->draw(victim, x, ty, size, k.victim == me ? green : grey);
        }
        ++row;
    }
}

/// A player's name over its head, drawn in the scene with the player (out of sight, the scene
/// leaves it out with the player).
void drawNamePlate(App& app, const NamePlate& plate)
{
    const bool name = app.font && app.namePlates && !plate.name.empty();
    if (!name && !plate.pencil) {
        return;
    }
    // The game draws the name's top 64 units above the player's feet, whatever their height, in
    // default.fnt (about 9 units to the baseline), with a dark shadow a pixel down and right.
    constexpr float kBaselineAbove = 55;
    const Camera& cam = app.camera;
    const int size = worldFontSize(cam);
    const float width = name ? app.font->measure(plate.name, size) : 0;
    const float sx = (plate.x - cam.left()) * cam.zoom - width / 2;
    if (name) {
        const float shadow = std::max(1.0f, std::round(cam.zoom));
        const float sy = (plate.y - kBaselineAbove - cam.top()) * cam.zoom - app.font->ascent(size);
        drawShadowed(*app.font, plate.name, sx, sy, size, plate.color, shadow);
    }
    if (plate.pencil) {
        // (Who's editing) The pencil before the name, on its line; alone where the name would be. A
        // NoX Core mark only: stock clients never get the session's messages.
        const float s = std::round(static_cast<float>(size) * 1.25f);
        const float cy = (plate.y - kBaselineAbove - cam.top()) * cam.zoom - static_cast<float>(size) * 0.35f;
        const float cx = name ? sx - s * 0.5f - 3 : (plate.x - cam.left()) * cam.zoom;
        const auto& p = *plate.pencil;
        drawPencil(app.renderer, cx, cy, s, p.idle ? idleRgb(p.colour) : p.colour, p.idle ? 150 : 255);
    }
}

/// FlagDraw's team name over a flag: in games with flag 0x80, an enabled flag's
/// material modifier gives a team colour (MaterialTeamRed... table), and the
/// team of that colour is named in green, centred on the flag with the text's top its
/// ZSIZE's top above its feet. A carried flag has none: PlayerDraw draws its own copy, which is
/// never enabled. Like the name plates, at screen resolution; only over the flags the scene's sort
/// drew this frame (FlagDraw draws it: lingering too), as liveSprites noted them (App::flagLabels).
void drawFlagLabels(App& app)
{
    constexpr std::uint32_t kFlagLabels = 0x80;
    if (!app.font || !app.live || !app.namePlates) {
        return;
    }
    const auto& world = app.live->client().world();
    if (!(world.game().gameFlags & kFlagLabels)) {
        return;
    }
    const Camera& cam = app.camera;
    const int size = worldFontSize(cam);
    for (const std::uint16_t netCode : app.flagLabels) {
        const auto found = world.drawables().find(netCode);
        if (found == world.drawables().end()) {
            continue;
        }
        const auto& d = found->second;
        if (!d.enabled || !d.modifiers) {
            continue;
        }
        const TypeArt* a = app.art->get(d.typeId);
        const auto* material = app.modifiers.modifier((*d.modifiers)[1]);
        if (!a || a->drawFn != DrawFn::Flag || !material) {
            continue;
        }
        const std::uint8_t colour = flagMaterialColor(material->name);
        if (colour == 0) {
            continue;
        }
        // (The first team, by id, of that colour: FlagDraw's team lookup, the team's ColorInd. The
        // minimap, cliDrawMinimap, takes it as a team id instead: its own.)
        const auto team = std::ranges::find_if(world.teams(), [&](const auto& t) { return t.second.color == colour; });
        if (team == world.teams().end() || team->second.name.empty()) {
            continue;
        }
        const auto p = app.interpolator.position(netCode, d);
        const std::string& name = team->second.name;
        const float sx = (p.x - cam.left()) * cam.zoom - app.font->measure(name, size) / 2;
        const float sy = (p.y - p.z - std::trunc(a->zSizeMax) - cam.top()) * cam.zoom;
        app.font->draw(name, sx, sy, size, {0, 255, 0, 255});
    }
}

/// The game-over banner's words and picture for the last winner message (CliNet_OnPacket's
/// 0x56-0x59, CliNet_OnPacket), from the string table's cdecode.c texts:
///   flag ball   our team: TeamWon; else FB_Victory with "Team %s" (defeat)
///   flag        CTF_Tie; else CTF_Victory with the team's name (defeat unless ours)
///   deathmatch  DM_Tie; us: DM_MaleVictory / DM_FemaleVictory; else DM_Loss with the winner's
///               name (defeat). Elimination games (flag 0x400): HL_Tie, or HL_Header and
///               HL_Victory with HL_You or the name
///   team DM     DM_Tie; ours: DM_TeamVictory; else DM_Loss with "Team %s" (defeat). Elimination:
///               HL_Tie, or HL_Header and HL_Victory with HL_YourTeam or "Team %s"
/// The time limit puts TimeLimitReached first (not for flag ball).
std::optional<App::GameOverBanner> gameOverBanner(const App& app)
{
    if (!app.live) {
        return std::nullopt;
    }
    const auto& client = app.live->client();
    const auto& world = client.world();
    const auto& game = world.game();
    // text: a cdecode.c string in UTF-8 (empty if missing); with: the same with its %s (or %S) filled.
    const auto text = [&](const char* id) -> std::string {
        return tableText(app, std::string("cdecode.c:") + id).value_or(std::string());
    };
    const auto with = [&](const char* id, const std::string& arg) {
        std::string f = text(id);
        if (const auto at = f.find("%s"); at != std::string::npos) {
            f.replace(at, 2, arg);
        } else if (const auto atS = f.find("%S"); atS != std::string::npos) {
            f.replace(atS, 2, arg);
        }
        return f;
    };
    const auto* me = ownPlayer(client);
    const std::uint32_t ourTeam = me ? me->team : 0;
    // overWinner: a team id in the team games, a player's net code in deathmatch.
    const auto team = world.teams().find(game.overWinner);
    const bool teamKnown = game.overWinner != 0 && team != world.teams().end();
    const std::string teamName = teamKnown ? team->second.name : std::string();
    const bool elimination = (game.gameFlags & 0x400) != 0;  // game flag 0x400: elimination
    App::GameOverBanner b;
    b.shownMs = SDL_GetTicks();
    std::string words;
    switch (game.overKind) {
    case 0x56: // flag ball
        if (teamKnown && ourTeam == game.overWinner) {
            words = text("TeamWon");
        } else {
            words = with("FB_Victory", with("Team", teamName));
            b.defeat = true;
        }
        b.text = words;
        return b;  // (flag ball: no time-limit line)
    case 0x57: // flag
        if (!teamKnown) {
            words = text("CTF_Tie");
        } else {
            words = with("CTF_Victory", teamName);
            b.defeat = ourTeam != game.overWinner;
        }
        break;
    case 0x58: { // deathmatch
        const auto winner = world.players().find(game.overWinner);
        const std::string name = winner != world.players().end() ? winner->second.name : std::string();
        const bool us = game.overWinner == client.netCode();
        if (game.overWinner == 0) {
            words = text(elimination ? "HL_Tie" : "DM_Tie");
        } else if (!elimination) {
            if (us) {
                words = text(me && me->female ? "DM_FemaleVictory" : "DM_MaleVictory");
            } else {
                words = with("DM_Loss", name);
                b.defeat = true;
            }
        } else {
            words = text("HL_Header") + with("HL_Victory", us ? text("HL_You") : name);
            b.defeat = !us;
        }
        break;
    }
    case 0x59: { // team deathmatch
        const bool ours = teamKnown && ourTeam == game.overWinner;
        if (!teamKnown) {
            words = text(elimination ? "HL_Tie" : "DM_Tie");
        } else if (!elimination) {
            words = ours ? text("DM_TeamVictory") : with("DM_Loss", with("Team", teamName));
            b.defeat = !ours;
        } else {
            words = text("HL_Header") + with("HL_Victory", ours ? text("HL_YourTeam") : with("Team", teamName));
            b.defeat = !ours;
        }
        break;
    }
    default:
        return std::nullopt;
    }
    b.text = (game.overTimeLimit ? text("TimeLimitReached") : std::string()) + words;
    return b;
}

/// The game-over banner, 10 seconds from the winner message: GameVictory or
/// GameDefeat (310 x 200) in the middle of the screen, the words in white below its middle, each
/// line centred in a 220-wide column 45 in, the block centred in the 49 lines from 143 down.
void drawGameOverBanner(App& app)
{
    if (!app.banner || !app.hud || !app.font) {
        return;
    }
    if (SDL_GetTicks() - app.banner->shownMs > 10000) {
        app.banner.reset();
        return;
    }
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    const float s = Hud::scale(w, h);
    const float x = std::floor((static_cast<float>(w) / s - 310) / 2);
    const float y = std::floor((static_cast<float>(h) / s - 200) / 2);
    if (const auto* def = app.hud->image(app.banner->defeat ? "GameDefeat" : "GameVictory"); def && def->image) {
        if (const Sprite* sp = app.sprites->get(*def->image)) {
            const SDL_FRect dst{(x + sp->offsetX) * s, (y + sp->offsetY) * s, sp->width * s, sp->height * s};
            app.sprites->render(*sp, nullptr, dst);
        }
    }
    const int size = app.font->lineSize(10, s);
    std::vector<std::string> lines;
    std::size_t from = 0;
    // Split at line breaks, then wrap each part to the 220-pixel column.
    while (from <= app.banner->text.size()) {
        const auto nl = app.banner->text.find_first_of("\n\r", from);
        const std::string part = app.banner->text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        for (auto& l : app.font->wrap(part, size, 220 * s)) {
            lines.push_back(l);
        }
        if (nl == std::string::npos) {
            break;
        }
        from = nl + 1;
    }
    // Trailing empty lines dropped, so the block centres on the text.
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    const float lineH = app.font->lineHeight(size);
    float ty = (y + 143) * s + (49 * s - lineH * static_cast<float>(lines.size())) / 2;
    for (const auto& l : lines) {
        const float tw = app.font->measure(l, size);
        app.font->draw(l, (x + 45) * s + (220 * s - tw) / 2, ty, size, {255, 255, 255, 255});
        ty += lineH;
    }
}

namespace {

/// The world box (x, y, w, h) on screen in whole pixels, so a health column stays crisp at any zoom.
SDL_FRect snappedRect(const Camera& cam, float x, float y, float w, float h)
{
    const float k = cam.zoom;
    const float x0 = std::round((x - cam.left()) * k);
    const float y0 = std::round((y - cam.top()) * k);
    return {x0, y0, std::round((x + w - cam.left()) * k) - x0, std::round((y + h - cam.top()) * k) - y0};
}

} // namespace

/// The health bars beside other players (PlayerDraw's), in world units: a 2 x 48 column standing on
/// the player's feet less their height, black, filled from the bottom to percent x 48 / 100 in
/// red (green while poisoned), in a 4 x 50 frame of dark red (dark green). `bars` are those of the
/// players the scene's sort drew this frame (PlayerDraw draws them), as liveSprites' overlays noted them.
void drawHealthBars(App& app, const std::vector<HealthBar>& bars)
{
    const Camera& cam = app.camera;
    const float k = cam.zoom;
    const auto rect = [&](float x, float y, float w, float h, SDL_Color c, bool fill) {
        const SDL_FRect r = snappedRect(cam, x, y, w, h);
        SDL_SetRenderDrawColor(app.renderer, c.r, c.g, c.b, 255);
        if (fill) {
            SDL_RenderFillRect(app.renderer, &r);
        } else {
            // (the frame a world unit wide, as the game's one pixel)
            const float t = std::max(1.0f, std::round(k));
            const SDL_FRect sides[4] = {{r.x, r.y, r.w, t}, {r.x, r.y + r.h - t, r.w, t}, {r.x, r.y, t, r.h},
                                        {r.x + r.w - t, r.y, t, r.h}};
            SDL_RenderFillRects(app.renderer, sides, 4);
        }
    };
    for (const auto& bar : bars) {
        const float x = bar.x + bar.side;
        const float bottom = bar.y - bar.z;
        const float filled = static_cast<float>(std::min(48, bar.percent * 48 / 100));
        rect(x, bottom - 48, 2, 48, {0, 0, 0, 255}, true);
        rect(x, bottom - filled, 2, filled, bar.poisoned ? SDL_Color{0, 255, 0, 255} : SDL_Color{255, 0, 0, 255}, true);
        rect(x - 1, bottom - 49, 4, 50, bar.poisoned ? SDL_Color{0, 100, 0, 255} : SDL_Color{100, 0, 0, 255}, false);
    }
}

/// The health bar's column beside a creature we watch (creature_bar.hpp), from its feet (fx, fy
/// less its z), in world units; drawn in the scene's order, right after the creature.
void drawCreatureBar(SDL_Renderer* r, const Camera& cam, const CreatureBar& bar, float fx, float fy, bool poisoned)
{
    const auto rect = [&](float x, float y, float w, float h, SDL_Color c) {
        const SDL_FRect rc = snappedRect(cam, x, y, w, h);
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        SDL_RenderFillRect(r, &rc);
    };
    const float x = fx + static_cast<float>(bar.x);
    const float top = fy + static_cast<float>(bar.top);
    rect(x, top, 2, static_cast<float>(bar.height), {0, 0, 0, 255});
    rect(x, top + static_cast<float>(bar.height - bar.filled), 2, static_cast<float>(bar.filled),
         poisoned ? SDL_Color{0, 255, 0, 255} : SDL_Color{255, 0, 0, 255});
}

/// The dots over our summons (MonsterDraw, client_drawPoint): a filled disc of
/// radius 3 game pixels; over the creatures the scene's sort drew this frame (App::minionDots).
void drawMinionDots(App& app)
{
    const Camera& cam = app.camera;
    for (const auto& dot : app.minionDots) {
        const float cx = (dot.x - cam.left()) * cam.zoom;
        const float cy = (dot.y - cam.top()) * cam.zoom;
        SDL_SetRenderDrawColor(app.renderer, dot.color.r, dot.color.g, dot.color.b, 255);
        for (int row = -3; row <= 3; ++row) {
            // The game's rows of a radius-3 disc, scaled: half-widths 1, 2, 3, 3, 3, 2, 1.
            const int half = std::abs(row) == 3 ? 1 : std::abs(row) == 2 ? 2 : 3;
            const SDL_FRect span{cx - (half + 0.5f) * cam.zoom, cy + (row - 0.5f) * cam.zoom, (2 * half + 1) * cam.zoom,
                                 cam.zoom};
            SDL_RenderFillRect(app.renderer, &span);
        }
    }
}

/// Floating damage numbers (MSG_REPORT_HEALTH_DELTA; with each drawable drawn): the
/// size of the hit, centred over whoever took it from the top of their height (ZSIZE's top)
/// up, rising two game pixels a frame for 31 frames, outlined in black (drawn one pixel off
/// each diagonal), red over us, yellow over anyone or anything else (green for a gain, which
/// the server doesn't send), in the numbers font (here the game font, small). Only over the
/// creatures, players and generators the scene's sort drew this frame (the scene draw draws them
/// after their DRAW; App::damageShown), lingering ones too.
void drawDamageNumbers(App& app)
{
    if (!app.font || !app.live || !app.art) {
        return;
    }
    const auto& client = app.live->client();
    const auto& world = client.world();
    const Camera& cam = app.camera;
    const float k = cam.zoom;
    const double now = app.interpolator.gameTime();
    const int size = std::clamp(static_cast<int>(std::lround(10 * k)), 8, 48);
    const float edge = std::max(1.0f, std::round(k));
    for (const auto& f : world.healthFloaters()) {
        const double age = now - static_cast<double>(f.frame);
        if (age < 0 || age >= 31) {
            continue;
        }
        const auto d = world.drawables().find(f.netCode);
        if (d == world.drawables().end()) {
            continue;
        }
        if (std::ranges::find(app.damageShown, f.netCode) == app.damageShown.end()) {
            continue;
        }
        const auto p = app.interpolator.position(f.netCode, d->second);
        const TypeArt* art = app.art->get(d->second.typeId);
        const float top = art ? art->zSizeMax : 0;
        const std::string text = std::to_string(std::abs(static_cast<int>(f.delta)));
        const float w = app.font->measure(text, size);
        const float x = std::round((p.x - cam.left()) * k - w / 2);
        const float y = std::round((p.y - p.z - top - cam.top() - 2 * static_cast<float>(age)) * k);
        for (const auto& [dx, dy] : {std::pair{-1, -1}, std::pair{-1, 1}, std::pair{1, -1}, std::pair{1, 1}}) {
            app.font->draw(text, x + static_cast<float>(dx) * edge, y + static_cast<float>(dy) * edge, size, {0, 0, 0, 255});
        }
        const SDL_Color color = f.delta > 0                     ? SDL_Color{0, 255, 0, 255}
                                : f.netCode == client.netCode() ? SDL_Color{255, 0, 0, 255}
                                                                : SDL_Color{255, 255, 0, 255};
        app.font->draw(text, x, y, size, color);
    }
}

namespace {

/// A chat bubble's text box in screen pixels, as the game lays it out.
struct BubbleBox {
    std::uint16_t speaker = 0;
    std::uint32_t order = 0; ///< ChatBubble::order: the game's list
    std::vector<std::string> lines;
    float x = 0; ///< the text's top left
    float y = 0;
    float w = 0;
    float h = 0;
    bool tail = true;
    bool shown = true; ///< solo games hide bubbles that are off screen
};

/// The HUD's panels a bubble keeps off, in screen pixels from the game's pixels at the
/// HUD's scale: the inventory's strip along the top (55 high, 279 with the inventory open), whose
/// bubbles go below it; with the HUD on, the bottom-left panel (111 x 127), the quickbar (320 x 74,
/// centred) and the meters (91 x 201, bottom right), whose bubbles go up above them; and the summon
/// cage (87 x 145, top right) while we have creatures, whose bubbles go below it. As the game, a
/// bubble is moved when its top-left or bottom-right corner is in one.
struct BubbleHud {
    float s = 1;     ///< screen pixels a game pixel (Hud::scale)
    float W = 640;   ///< the screen in game pixels
    float H = 480;
    bool inventoryOpen = false;
    bool gui = true;
    bool cage = false;

    /// Moves `b` off the panels; false if it had to move.
    bool keepOff(BubbleBox& b) const
    {
        bool ok = true;
        const auto corner = [&](float x0, float y0, float x1, float y1) {
            const auto in = [&](float px, float py) { return px >= x0 * s && px < x1 * s && py >= y0 * s && py < y1 * s; };
            return in(b.x, b.y) || in(b.x + b.w, b.y + b.h);
        };
        const float strip = inventoryOpen ? 279.0f : 55.0f;
        if (corner(0.0f, 0.0f, 563.0f, strip)) {
            b.y = strip * s;
            ok = false;
        }
        if (gui) {
            for (const auto& [x0, y0, x1] : {std::array{0.0f, H - 127, 111.0f}, std::array{W / 2 - 160, H - 74, W / 2 + 160},
                                             std::array{W - 91, H - 201, W}}) {
                if (corner(x0, y0, x1, H)) {
                    b.y = y0 * s - b.h; // up above it
                    ok = false;
                }
            }
            if (cage && corner(W - 87, 0.0f, W, 145.0f)) {
                b.y = 145 * s;
                ok = false;
            }
        }
        return ok;
    }
};

/// A bubble that overlaps an earlier one on screen moves beside the first such one,
/// two lines clear of it, to the first free spot of the sides the screen third its
/// centre is in prefers, else of a second set. The sides are bits, tried from 1 up:
/// 1 up-left, 2 up-right, 4 down-left, 8 down-right, 0x10 up, 0x20 down, 0x40 left, 0x80 right.
/// A spot is free if the text stays on screen and a line clear of every earlier bubble.
/// With none free it stays. The list's first bubble never moves.
void shiftBubble(std::vector<BubbleBox>& boxes, std::size_t i, float fh, float screenW, float screenH,
                 const BubbleHud& hud)
{
    BubbleBox& b = boxes[i];
    // Whether `a` at (ax, ay) comes within a line's height of `o`.
    const auto overlaps = [fh](const BubbleBox& a, float ax, float ay, const BubbleBox& o) {
        return ax - fh < o.x + o.w + fh && o.x - fh < ax + a.w + fh && ay - fh < o.y + o.h + fh &&
               o.y - fh < ay + a.h + fh;
    };
    const BubbleBox* other = nullptr;
    for (std::size_t j = 0; j < i && !other; ++j) {
        if (boxes[j].shown && overlaps(b, b.x, b.y, boxes[j])) {
            other = &boxes[j];
        }
    }
    if (!other) {
        return;
    }
    // [left, middle, right third][top, middle, bottom third]: the sides first, then the others.
    static constexpr std::uint8_t kSides[3][3][2] = {
        {{0xA0, 0x08}, {0x30, 0x8A}, {0x90, 0x02}},
        {{0xC0, 0x2C}, {0xF0, 0x0F}, {0xC0, 0x13}},
        {{0x60, 0x04}, {0x30, 0x45}, {0x50, 0x01}},
    };
    const auto third = [](float v, float size) {
        return v < std::floor(size / 3) ? 0 : v < std::floor(size * 2 / 3) ? 1 : 2;
    };
    const auto& sides =
        kSides[third(b.x + std::floor(b.w / 2), screenW)][third(b.y + std::floor(b.h / 2), screenH)];
    const BubbleBox& o = *other;
    const float gap = 2 * fh;
    const float left = o.x - b.w - gap;
    const float right = o.x + o.w + gap;
    const float up = o.y - b.h - gap;
    const float down = o.y + o.h + gap;
    for (const std::uint8_t mask : sides) {
        for (int bit = 0; bit < 8; ++bit) {
            const unsigned side = 1u << bit;
            if (!(mask & side)) {
                continue;
            }
            // Left-hand sides 0x45 (1, 4, 0x40), right-hand 0x8A; upper 0x13 (1, 2, 0x10), lower 0x2C; the rest keep b's x or y.
            const float x = side & 0x45 ? left : side & 0x8A ? right : b.x;
            const float y = side & 0x13 ? up : side & 0x2C ? down : b.y;
            if (x < 0 || y < 0 || x + b.w > screenW || y + b.h > screenH) {
                continue;
            }
            BubbleBox there = b;
            there.x = x;
            there.y = y;
            if (!hud.keepOff(there)) {
                continue; // (on a HUD panel: the game counts that as off screen)
            }
            bool clear = true;
            for (std::size_t j = 0; j < i && clear; ++j) {
                clear = !overlaps(b, x, y, boxes[j]);
            }
            if (clear) {
                b.x = x;
                b.y = y;
                return;
            }
        }
    }
}

} // namespace

/// Chat bubbles (laid out as the game does), over everything at screen resolution.
///
/// The text wraps at 128 game pixels and sits 64 above the speaker (their drawable while the
/// client has it, else where they spoke), kept a line's height inside the screen; a bubble pushed
/// in from an edge loses its tail. Then, in the order they were made, bubbles that overlap an
/// earlier one move off it (shiftBubble), keeping their tail. The outline is an octagon with
/// corners cut by half a line, drawn in near-black and again one pixel up and left in grey, with
/// no fill. A player's bubble has their name over it in their team's colour (white without a
/// team). Solo games hide bubbles that are off screen. Muted players get none (playerCanTalkMB). Like the
/// game, bubbles show whether or not the speaker is in sight. They keep off the HUD's panels too
/// (BubbleHud), so one pushed to the bottom or a corner isn't hidden under them.
void drawChatBubbles(App& app, std::uint32_t frame)
{
    if (!app.font || !app.live) {
        return;
    }
    const auto& client = app.live->client();
    const auto& world = client.world();
    const Camera& cam = app.camera;
    const float k = cam.zoom;
    const int size = worldFontSize(cam);
    const float fh = app.font->lineHeight(size);
    const float h = std::floor(fh / 2);  // half a line: the octagon's corner cut
    int screenW = 0;
    int screenH = 0;
    SDL_GetCurrentRenderOutputSize(app.renderer, &screenW, &screenH);
    BubbleHud hud;
    // The HUD's panels are laid out in game pixels, for keepOff.
    hud.s = Hud::scale(screenW, screenH);
    hud.W = static_cast<float>(screenW) / hud.s;
    hud.H = static_cast<float>(screenH) / hud.s;
    hud.inventoryOpen = app.inventory && app.inventory->isOpen();
    hud.gui = app.renderGui;
    hud.cage = world.self().summonCount() > 0;
    std::vector<BubbleBox> boxes;
    for (const auto& [speaker, bubble] : world.chatBubbles()) {
        if (bubble.expires <= frame || bubble.text.empty() || app.muted.contains(speaker)) {
            continue;
        }
        // Anchored on the speaker's drawable while the client has it, else where they spoke.
        float wx = static_cast<float>(bubble.x);
        float wy = static_cast<float>(bubble.y);
        if (const auto d = world.drawables().find(speaker); d != world.drawables().end()) {
            const auto p = app.interpolator.position(speaker, d->second);
            wx = p.x;
            wy = p.y;
        }
        BubbleBox b;
        b.speaker = speaker;
        b.order = bubble.order;
        b.lines = app.font->wrap(chatLine(app, bubble.text, bubble.team), size, 128 * k);
        for (const auto& line : b.lines) {
            b.w = std::max(b.w, app.font->measure(line, size));
        }
        b.w = std::ceil(b.w);
        b.h = fh * static_cast<float>(b.lines.size());
        b.x = std::round((wx - cam.left()) * k - b.w / 2);
        b.y = std::round((wy - cam.top()) * k - 64 * k - b.h);  // 64 game pixels above the anchor
        // Solo: an off-screen bubble is hidden. Else it's pulled onto the screen (and off the HUD), losing its tail.
        if (soloGame(app) && (b.x < 0 || b.x > static_cast<float>(screenW) || b.y - 64 < 0 ||
                                                     b.y - 64 > static_cast<float>(screenH))) {
            b.shown = false;
            b.tail = false;
        } else {
            if (b.x < fh) {
                b.x = fh;
                b.tail = false;
            } else if (b.x + b.w + fh > static_cast<float>(screenW)) {
                b.x = static_cast<float>(screenW) - b.w - fh;
                b.tail = false;
            }
            if (b.y < 2 + 2 * fh) {
                b.y = 2 + 2 * fh;
                b.tail = false;
            } else if (b.y + b.h + fh > static_cast<float>(screenH)) {
                b.y = static_cast<float>(screenH) - b.h - fh;
                b.tail = false;
            }
            if (!hud.keepOff(b)) {
                b.tail = false;
            }
        }
        boxes.push_back(std::move(b));
    }
    // In the order they were made: each one moves off the earlier ones.
    std::ranges::sort(boxes, {}, &BubbleBox::order);
    for (std::size_t i = 1; i < boxes.size(); ++i) {  // the first never moves
        if (boxes[i].shown) {
            shiftBubble(boxes, i, fh, static_cast<float>(screenW), static_cast<float>(screenH), hud);
        }
    }

    for (const BubbleBox& b : boxes) {
        if (!b.shown) {
            continue;
        }
        float x = b.x;
        float y = b.y;
        const float w = b.w;
        const float textH = b.h;
        const bool tail = b.tail;
        const auto& lines = b.lines;
        const std::uint16_t speaker = b.speaker;
        const auto d = world.drawables().find(speaker);
        const auto line = [&](float x0, float y0, float x1, float y1) { SDL_RenderLine(app.renderer, x0, y0, x1, y1); };
        // Pass 0 the outline in near-black, pass 1 again in grey one pixel up and left.
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                x -= 1;
                y -= 1;
                SDL_SetRenderDrawColor(app.renderer, 0x73, 0x73, 0x73, 255);
            } else {
                SDL_SetRenderDrawColor(app.renderer, 8, 8, 8, 255);
            }
            // The octagon: edges with corners cut by h (half a line); with a tail, the bottom edge breaks for a V under the middle.
            const float right = x + w + h;
            const float bottom = y + textH + h;
            line(x, y - h, x - h, y);
            line(x, y - h, x + w, y - h);
            line(right, y, x + w, y - h);
            line(right, y, right, y + textH);
            line(x + w, bottom, right, y + textH);
            if (tail) {
                const float mid = x + std::floor(w / 2);
                line(x + w, bottom, mid + h, bottom);
                line(mid, bottom + fh, mid + h, bottom);
                line(mid, bottom + fh, mid - h, bottom);
                line(x, bottom, mid - h, bottom);
            } else {
                line(x, bottom, x + w, bottom);
            }
            line(x, bottom, x - h, y + textH);
            line(x - h, y, x - h, y + textH);
        }
        const float shadow = std::max(1.0f, std::round(k));
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const float ly = y + fh * static_cast<float>(i);
            drawShadowed(*app.font, lines[i], x, ly, size, {255, 255, 255, 255}, shadow);
        }
        // The speaker's name over a player's bubble, while their drawable is known.
        if (const auto pl = world.players().find(speaker); pl != world.players().end() && d != world.drawables().end()) {
            const SDL_Color color = teamColor(world, pl->second.team).value_or(SDL_Color{255, 255, 255, 255});
            const float ny = y - fh - 1;  // a line above the text
            drawShadowed(*app.font, pl->second.name, x, ny, size, color, shadow);
        }
    }
}

/// This frame's tooltip in a game: the book's tabs, the inventory's items and weapon panel, the
/// bars and meters, else an item on the ground under the mouse (cursorUpdate: windows first).
std::string liveTooltip(App& app)
{
    // None while a menu is up or a spell is being dragged.
    if (!app.live || !app.hud || menuWindowUp(app) || (app.book && app.book->dragging()) || app.barDrag) {
        return {};
    }
    auto& client = app.live->client();
    const auto& world = client.world();
    const SDL_FPoint m = mousePos(app);
    const int w = app.camera.viewWidth;
    const int h = app.camera.viewHeight;
    // ask: MSG_INFO_BOOK_DATA, for a book or scroll whose name tells what it holds.
    const auto ask = [&client](std::uint16_t nc, std::uint8_t kind) { client.askBook(nc, kind); };
    // itemName: the full name of an item we carry or one on the ground.
    const auto itemName = [&](std::uint16_t nc) -> std::string {
        if (!app.itemNames) {
            return {};
        }
        if (const auto it = world.self().inventory.find(nc); it != world.self().inventory.end()) {
            return app.itemNames->name(nc, it->second.typeId, it->second.modifiers, world, ask);
        }
        if (const auto* d = world.drawable(nc)) {
            return app.itemNames->name(nc, d->typeId, d->modifiers.value_or(std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0xFF}),
                                       world, ask);
        }
        return {};
    };
    // The windows first, in turn: the first one under the mouse answers.
    if (app.tradeScreens && app.tradeScreens->contains(m.x, m.y, w, h)) {
        return app.tradeScreens->tooltip(m.x, m.y, w, h, world);
    }
    if (app.summon && app.summon->contains(m.x, m.y, w, h)) {
        return app.summon->tooltip(m.x, m.y, w, h, world);
    }
    if (app.book) {
        if (app.book->contains(m.x, m.y, w, h)) {
            return app.book->tooltip(m.x, m.y, w, h, bookContext(app));
        }
    }
    if (app.inventory) {
        const auto hover = app.inventory->hoverAt(m.x, m.y, w, h, world);
        if (!hover.text.empty()) {
            return hover.text;
        }
        if (hover.item) {
            return itemName(*hover.item);
        }
        const auto guiText = [&](const char* id) { return tableText(app, id).value_or(std::string()); };
        if (hover.inventoryButton) {
            return guiText(hover.inventoryOpen ? "guiinv.c:CloseInventoryTT" : "guiinv.c:OpenInventoryTT");
        }
        if (hover.weaponPanel) {
            // The weapon in hand's name (a bow's or crossbow's with its quiver under),
            // else ToolTipCurWeapon.
            if (!hover.weapon) {
                return guiText("guimeter.c:ToolTipCurWeapon");
            }
            std::string text = itemName(*hover.weapon);
            const auto& inv = world.self().inventory;
            const TypeArt* a = app.art->get(inv.at(*hover.weapon).typeId);
            if (a && (a->subclasses & 0xC) && hover.quiver) {  // 0xC: a bow or crossbow
                text += "\n" + itemName(*hover.quiver);
            }
            return text;
        }
        if (app.inventory->contains(m.x, m.y, w, h)) {
            return {};
        }
    }
    if (auto t = app.hud->tooltip(world, client.netCode(), quickbarState(app), m.x, m.y, w, h); !t.empty()) {
        return t;
    }
    // (Not under a cinematic's widescreen: cursorUpdate skips the world there.)
    if (app.cursorHovered && !(app.inventory && app.inventory->dragging()) &&
        !(app.cinematics && app.cinematics->wideScreen())) {
        if (const auto* d = world.drawable(*app.cursorHovered)) {
            const TypeArt* a = app.art->get(d->typeId);
            if (a && (a->classes & nox::things::ObjectClass::Pickup)) {
                return itemName(*app.cursorHovered); // (cursorUpdate for an item the cursor's on)
            }
        }
    }
    return {};
}

/// The tooltip once the mouse has kept still (client_drawCursorAndTooltips): its lines in yellow on a darkened box
/// at the cursor, a line and 4 above it, kept on the screen. `below` (Options' switches, whose
/// tooltips run to several lines): under the pointer instead, at the menus' scale.
void drawTooltip(App& app, bool below)
{
    const SDL_FPoint m = mousePos(app);
    const float s = below ? std::min(static_cast<float>(app.camera.viewWidth) / 640.0f,
                                     static_cast<float>(app.camera.viewHeight) / 480.0f)
                          : Hud::scale(app.camera.viewWidth, app.camera.viewHeight);
    const std::uint64_t now = SDL_GetTicks();
    if (app.tooltipStepMs == 0 || now - app.tooltipStepMs > 1000) {
        app.tooltipStepMs = now;
    }
    // Counted at 30 a second however fast we draw: a step under 2 game pixels counts as still.
    for (; now - app.tooltipStepMs >= 33; app.tooltipStepMs += 1000 / 30) {
        const float dx = (m.x - app.tooltipMouse.x) / s;
        const float dy = (m.y - app.tooltipMouse.y) / s;
        app.tooltipStill = dx * dx + dy * dy < 4 ? app.tooltipStill + 1 : 0;
        app.tooltipMouse = m;
    }
    if (app.tooltip.empty() || app.tooltipStill <= 9 || !app.font) {
        return;
    }
    constexpr float kFontHeight = 10;
    // kFontHeight game pixels a line, whatever the font's own line height.
    const int size = app.font->lineSize(kFontHeight, s);
    std::vector<std::string> lines;
    for (std::size_t from = 0;;) {
        const auto nl = app.tooltip.find('\n', from);
        lines.push_back(app.tooltip.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
        if (nl == std::string::npos) {
            break;
        }
        from = nl + 1;
    }
    float tw = 0;
    for (const auto& l : lines) {
        tw = std::max(tw, app.font->measure(l, size) / s);
    }
    const float th = static_cast<float>(lines.size()) * kFontHeight + 4;
    const float W = static_cast<float>(app.camera.viewWidth) / s;
    const float H = static_cast<float>(app.camera.viewHeight) / s;
    float x = below ? m.x / s + 12 : m.x / s;
    float y = below ? m.y / s + 20 : m.y / s - (kFontHeight + 4);
    // Kept on the screen: moved left of the pointer at the right edge, clamped at the others.
    if (x + 4 + tw >= W) {
        x -= tw;
    }
    x = std::max(x, 0.0f);
    y = y + th < H ? std::max(y, 0.0f) : H - th;
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, below ? 216 : 128); // (client_drawRectFilledAlpha; darker over the menus' text)
    const SDL_FRect box{x * s, y * s, (tw + 4) * s, th * s};
    SDL_RenderFillRect(app.renderer, &box);
    float ly = y + 2;
    for (const auto& l : lines) {
        app.font->draw(l, (x + 2) * s, ly * s, size, {255, 255, 0, 255});
        ly += kFontHeight;
    }
}

} // namespace noxsdl
