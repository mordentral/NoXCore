// A NoX Core client's side of a script mode's UI: the UI
// script the server sends (MSG_NOXCPP_MODEUI: ui_proto.hpp), run in a sandboxed Luau VM of its
// own (luau_vm.hpp: memory and time capped), in memory only, gone when the game is left.
//
// What the UI may do is narrow on purpose: declare windows (the client frames each and labels it
// as the server's: they never draw over the menus), draw boxes, outlines and text in them (in the
// game's 640 x 480 units), be told of clicks in them, send values to its server, answer an open
// choice, and read what the server sends it (data and views). No files, no OS, no network, no
// typed text, no account.
//
// The script returns a table of hooks; the toolbox is the global `nox`:
//
//   local ui = {}
//   function ui.start() nox.window("board", { title = "Board", x = 20, y = 60, w = 200, h = 120 }) end
//   function ui.draw(window) nox.box(0, 0, 200, 14, "dim") nox.text("Hello", 4, 2, "bright") end
//   function ui.click(window, x, y) nox.send({ clicked = window }) return true end
//   function ui.onMessage(value) end       -- the server's nox.send
//   function ui.onView(view) return true end -- a view (say / choice / table) as data
//   return ui
//
//   nox.window(id, {title, x, y, w, h}), nox.closeWindow(id), nox.box(x, y, w, h, colour),
//   nox.outline(x, y, w, h, colour), nox.text(text, x, y, colour), nox.textWidth(text),
//   nox.send(value), nox.answer(index), nox.time(), nox.mode()
//   (a colour: "gold", "bright", "dim", "grey", "white", "black", "red", "green", "blue", or
//   { r, g, b [, a] })
//
// UI API 2 (mode.json's "uiApi": 2; sent only to a client that names ClientFeature::ModeUi2) adds,
// beside nox.api being 2:
//   nox.image(name, x, y [, w, h])   a mod's own picture (its assets\: mod_assets.hpp), its size
//                                    or scaled to w x h
//   nox.thing(typeName, x, y)        a thing type's picture from the client's own art (thing.bin's
//                                    names), centred on (x, y)
//   nox.sound(name [, volume])       a mod's own sound; nox.gameSound(name [, volume]) the game's
//                                    (volume 0-100)
//   nox.mouse()                      window, x, y: the mouse over one of the windows; nil: not
//   nox.player()                     { name, class, level, health, maxHealth, mana, maxMana } of
//                                    our player (read-only); nil out of a game
//   ui.click(window, x, y, button)   button 1 left, 2 right
//   ui.key(window, key)              a key while the mouse is over a window and no chat or console
//                                    line is open: "0"-"9", "up", "down", "left", "right",
//                                    "enter", "escape", "tab", "space", "f1"-"f12" (true: taken)
// Its draws count against the same caps as the others.
//
// The program calls frame() once a frame and draws windows(); a click in one goes to click().
// Without a server (the map editor's UI preview), runLocal starts a UI from its source, useAssets
// hands it a mod's assets, receive() feeds it messages as a server would, setSender takes what it
// sends.
#pragma once

#include "noxcore/modes/mode.hpp"
#include "noxcore/modes/script/luau_vm.hpp"
#include "noxcore/modes/script/mod_assets.hpp"
#include "noxcore/modes/script/weather.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::client {
class HeadlessClient;
}

namespace nox::things {
struct ThingDb;
}

namespace nox::modes::script {

struct UiColor {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
};

struct UiDraw {
    enum class Kind : std::uint8_t {
        Box,
        Outline,
        Text,
        Image, ///< (API 2) assets()->list[index], at (x, y), w x h
        Thing, ///< (API 2) thing type `index` (its THNG index + 1: a net type id), centred on (x, y)
    };
    Kind kind = Kind::Box;
    float x = 0, y = 0, w = 0, h = 0; ///< (Text: its top left)
    UiColor color;
    std::string text;
    int index = -1; ///< (Image, Thing)
};

/// A sound a UI asked for this frame (API 2): the program plays it and lets it go (takeSounds).
struct UiSound {
    bool game = false;    ///< the game's own sound `name`; else the mod's, assets()->list[index]
    int index = -1;
    std::string name;
    int volume = 100;     ///< 0-100
};

/// What nox.player() gives (API 2).
struct UiPlayer {
    std::string name;
    std::uint8_t playerClass = 0; ///< 0 warrior, 1 wizard, 2 conjurer
    int level = 0;
    int health = 0, maxHealth = 0;
    int mana = 0, maxMana = 0;
};

struct UiWindow {
    std::string id;
    std::string title;
    int x = 0, y = 0, w = 0, h = 0; ///< its inside, in the game's 640 x 480 units
    std::vector<UiDraw> draws;      ///< this frame's, in its own coordinates
};

class ScriptUiClient final : public ClientMode {
public:
    /// The UI client of `client` (its MSG_NOXCPP_MODEUI handler set); `client` must outlive it.
    explicit ScriptUiClient(client::HeadlessClient& client, LuauVm::Limits limits = defaultLimits());
    /// One with no server (a preview): runLocal starts its UI; what it sends goes to setSender's.
    explicit ScriptUiClient(LuauVm::Limits limits = defaultLimits());
    ~ScriptUiClient() override;
    ScriptUiClient(const ScriptUiClient&) = delete;
    ScriptUiClient& operator=(const ScriptUiClient&) = delete;

    /// A client's UI VM: 16 MB, and a frame's calls kept short.
    static LuauVm::Limits defaultLimits();

    /// How wide the program draws `text` (in the 640 x 480 units); unset: 6 a letter.
    void setTextWidth(std::function<int(std::string_view)> measure);
    /// Once a frame: what the server sent handed to the UI, then each window drawn (windows()).
    void frame(std::uint64_t nowMs);
    const std::vector<UiWindow>& windows() const noexcept;
    /// A click at (x, y) inside window `id`: whether the UI took it. `button` 1 left, 2 right: an
    /// API 1 UI is told of left clicks alone (and never of the button).
    bool click(std::string_view windowId, float x, float y, int button = 1);
    /// (API 2) A key (one of the names script_ui.hpp lists; any other: false) while the mouse is
    /// over window `id`: whether the UI took it.
    bool key(std::string_view windowId, std::string_view keyName);
    /// (API 2) Where the mouse is, in the 640 x 480 units (none: off the game); nox.mouse's.
    void setMouse(std::optional<std::pair<float, float>> at);
    /// (API 2) What nox.player gives; unset: our player's from the client (none without one).
    void setPlayer(std::optional<UiPlayer> player);
    /// (API 2) The thing types nox.thing names (unset: the client's).
    void setThings(const things::ThingDb* things);
    /// (API 2) The sounds the UI asked for since the last call.
    std::vector<UiSound> takeSounds();

    /// Whether a UI is running, the mode's name (its windows' label), and why the last one stopped.
    bool running() const noexcept;
    const std::string& modeName() const noexcept;
    const std::string& error() const noexcept;
    /// The running UI's API (1 or 2; 0: none running).
    int api() const noexcept;
    /// The mod's assets as they came (or useAssets gave them); null: none.
    const std::shared_ptr<const ModAssets>& assets() const noexcept;
    /// The weather the server last told (ui_proto.hpp's kWeather: nox.weather), for the program
    /// to draw: kind, strength, wind and fade, and a serial that moves with each one told. Kept
    /// whether or not a UI runs; none again once a map loads, till the server tells it again.
    const ::nox::modes::script::weather::Told& weather();

    // ---- without a server (the map editor's UI preview)
    /// `assets` the UI's from now (in place of any a server sent).
    void useAssets(std::shared_ptr<const ModAssets> assets);
    /// A UI started from `source` as one sent by a server would be (a running one stopped):
    /// false, with error(), when it fails.
    bool runLocal(std::string modeId, std::string modeName, int uiApi, std::string source);
    /// A MSG_NOXCPP_MODEUI message as from a server (data, views, a drop...).
    void receive(std::span<const std::uint8_t> message);
    /// Where the UI's own messages go (kReady, kSend, kAnswer, kFailed) in place of the client.
    void setSender(std::function<void(std::vector<std::uint8_t>)> send);

    struct Impl; ///< (script_ui.cpp's)

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nox::modes::script
