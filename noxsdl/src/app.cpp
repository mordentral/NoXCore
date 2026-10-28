// The small helpers the front end's files share (app.hpp): the mouse in the game's area, the
// string table's texts, the centred messages, the game's flags as the client reads them.
#include "app.hpp"

#include "noxcore/client/inform_text.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <functional>
#include <utility>

#if NOX_PRIVATE_TOOLS
#include "dev_hooks.hpp"
#endif

namespace fs = std::filesystem;

namespace noxsdl {

/// A window point in the game's area (letterboxed: from its corner, the bars aside), kept on it as
/// the game kept the mouse on its screen.
void intoGameArea(const App& app, float& x, float& y)
{
    x = std::clamp(x - app.gameArea.x, 0.0f, std::max(app.gameArea.w - 1, 0.0f));
    y = std::clamp(y - app.gameArea.y, 0.0f, std::max(app.gameArea.h - 1, 0.0f));
}

/// The mouse in window pixels (or --mouse's point).
SDL_FPoint mousePos(const App& app)
{
    if (const auto v = app.view3d.cursorMouse(app.camera, app.running)) {
        return *v; // the 3D view's crosshair
    }
    SDL_FPoint m{};
#if NOX_PRIVATE_TOOLS
    if (devhooks::pointer(m.x, m.y)) { // (a recorded walkthrough's pointer: the desktop's mouse left alone)
        if (app.letterboxed) {
            intoGameArea(app, m.x, m.y);
        }
        return m;
    }
#endif
    // (mouse_handover.hpp) A held place only while a game is in the editor's Map panel without the
    // input: anywhere else a stale one froze the drawn pointer (the menus' buttons still lit under
    // the real mouse).
    const bool heldShown = app.editor && app.live && (app.playTestEmbedded || app.sessionEmbedded) && !app.playTestCaptured;
    switch (pointerSource(app.testMouse.has_value(), app.playTestMouse.has_value(), heldShown)) {
    case PointerSource::Test:
        m = *app.testMouse;
        break;
    case PointerSource::Held:
        m = *app.playTestMouse; // (a play-test in the editor's Map panel, the input the editor's: where the game had it)
        break;
    case PointerSource::Mouse:
        SDL_GetMouseState(&m.x, &m.y);
        break;
    }
    if (app.letterboxed) {
        intoGameArea(app, m.x, m.y);
    }
    return m;
}

/// The string table's text for `id` as UTF-8; nullopt without the table or the string.
std::optional<std::string> tableText(const App& app, std::string_view id)
{
    const auto t = app.strings ? app.strings->text(id) : std::nullopt;
    return t ? std::optional<std::string>(nox::strings::toUtf8(*t)) : std::nullopt;
}

/// A thing type's network id (its THNG index + 1); 0 for a name thing.bin doesn't have.
std::uint16_t typeIdOf(const nox::things::ThingDb& things, std::string_view name)
{
    const auto* t = things.findThing(name);
    return t ? static_cast<std::uint16_t>(t - things.things.data() + 1) : 0;
}

/// A line the server sent as a string-table id (chat flag 8, MSG_INFORM's ids), which noxcore
/// passes on as "[file:id]": its text, as the exe looks it up (Str_GetString); else as it came.
std::string resolveStringId(const App& app, const std::string& text)
{
    if (app.strings && text.size() > 2 && text.front() == '[' && text.back() == ']') {
        if (const auto t = app.strings->text(text.substr(1, text.size() - 2))) {
            return nox::strings::toUtf8(*t);
        }
    }
    return text;
}

/// A chat line as the game shows it in the console and its bubble: a string id's
/// text, and team chat as "<Guirank.c:team>: text".
std::string chatLine(const App& app, const std::string& text, bool team)
{
    std::string line = resolveStringId(app, text);
    if (team) {
        line = tableText(app, "Guirank.c:team").value_or("Team") + ": " + line;
    }
    return line;
}

/// A --map argument as a file: a path when it has an extension, else maps\NAME\NAME.map in the game folder.
fs::path resolveMap(const fs::path& gameDir, const std::string& arg)
{
    const fs::path p(arg);
    if (p.has_extension()) {
        return p;
    }
    return gameDir / "maps" / arg / (arg + ".map");
}

/// Str_GetString's pick of a string's variants (at least one): one at random, no draw for a
/// string that has only one.
std::size_t pickVariant(App& app, const nox::strings::StringEntry& e)
{
    return e.variants.size() < 2 ? 0 : nox::util::uniform_int_distribution<std::size_t>(0, e.variants.size() - 1)(app.rng);
}

/// Str_GetString: a label's text, one of its variants at random (the label itself when missing).
std::string gameString(App& app, std::string_view label)
{
    if (app.strings) {
        if (const auto* e = app.strings->find(label); e && !e->variants.empty()) {
            return nox::strings::toUtf8(e->variants[pickVariant(app, *e)].text);
        }
    }
    return std::string(label);
}

/// Gui_PrintCenteredMessage: the line in the middle of the screen for 5 s of game
/// frames (CentredMessages), and in the console in red as "GuiMsg.c:SystemMsg" ("system> %s").
void centredMessage(App& app, const std::string& text)
{
    if (text.empty()) {
        return;
    }
    app.centred.print(text, gameFrame(app));
    if (app.console) {
        app.console->print(nox::client::inform::format(gameString(app, "GuiMsg.c:SystemMsg"), {text}), Red);
    }
}

/// Game_HasFlags(0x800), a solo game. The exe's client and server share the game flags; our
/// client's copy lacks 0x800, so a game we host answers from the host's.
bool soloGame(App& app)
{
    return (app.live->client().world().game().gameFlags & 0x800) ||
           (app.host && (app.host->gameFlags() & nox::server::GameFlag::Solo));
}

/// The game flags as the exe's client reads them (the client's, with soloGame's 0x800).
std::uint32_t gameFlags(App& app)
{
    return app.live->client().world().game().gameFlags | (soloGame(app) ? 0x800u : 0u);
}

/// Our player's drawable's animation state, -1 without one.
int ownAnimState(App& app)
{
    if (!app.live) {
        return -1;
    }
    auto& client = app.live->client();
    const auto ours = client.world().drawables().find(client.netCode());
    return ours == client.world().drawables().end() ? -1 : static_cast<int>(ours->second.animState);
}

/// Our drawable's flags (MSG_REPORT_PLAYER_STATUS) have DEAD (0x8000).
bool ownDeadFlag(App& app)
{
    return app.live && (app.live->client().world().self().statusFlags & 0x8000u) != 0;
}

/// The Escape menu or a window it opens over the game (Options, the key bindings) is up.
bool menuWindowUp(const App& app)
{
    return (app.quitMenu && app.quitMenu->isOpen()) || (app.options && app.options->isOpen()) ||
           (app.inputConfig && app.inputConfig->isOpen());
}

/// Snd_PlaySound for the HUD's and the controls' own sounds.
void uiSound(App& app, std::uint32_t id, int volume)
{
    if (app.audio) {
        app.audio->play(id, volume);
    }
}

/// What the .wnd windows draw and sound with: thing.bin, the sprites, the font, the string table,
/// and the interface's sounds.
gui::Resources guiResources(App& app)
{
    gui::Resources res{app.things, *app.sprites, app.font.get(), app.strings.get()};
    res.playSound = [&app](std::uint32_t id) { uiSound(app, id); };
    return res;
}

/// The player for `name` when game_setMovieFile finds it, else `next` at once.
void playMovie(App& app, const std::string& name, movies::Skip skip, std::function<void()> next)
{
    if (app.movie && app.movie->available(name)) {
        app.movie->play(name, skip, std::move(next));
    } else if (next) {
        next();
    }
}

/// The game frame by the wall clock (30 a second), for things timed in the client alone.
std::uint32_t clockFrame()
{
    return static_cast<std::uint32_t>(SDL_GetTicks() * 30 / 1000);
}

} // namespace noxsdl
