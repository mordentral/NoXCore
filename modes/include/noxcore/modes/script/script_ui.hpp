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
// UI API 3 (mode.json's "uiApi": 3; sent only to a client at client level 3 or above that names
// ModeUi and ModeUi2: ui_proto.hpp's kUiApi3Level) adds, beside nox.api being 3:
//   nox.line(x1, y1, x2, y2, colour [, width])   a line in the window being drawn (width 1-8)
//   nox.tooltip(lines)               (inside draw) { { text, colour? }, ... } drawn at the mouse
//                                    over everything this frame, kept on the screen; the last call
//                                    of a frame wins (at most kUiMaxTooltipLines lines)
//   nox.label(object, text, colour [, opts])     a label over an object in the world: `object` its
//                                    net code (the server script's nox.object(id).netCode); opts
//                                    { show = "always" | "alt" (while Alt is held) | "near",
//                                    near = world units from our player (150), offset = pixels
//                                    above its top (4) }; false: over kUiMaxLabels. It goes when
//                                    the object leaves the client's world or a map loads (one never
//                                    there within kUiLabelWaitMs of the call, too); drawn only
//                                    where the object itself is
//   nox.clearLabel(object), nox.clearLabels()
//   NoxWindowSpec's frame (false: no frame, title or background, and any y on the screen),
//   clickThrough (true: clicks, the wheel and the mouse over it are the game's) and hud (true:
//   hidden while the game hides its HUD, and drawn under the framed windows)
//   NoxWindowSpec's anchor ("topleft", "top", "topright", "left", "right", "bottomleft",
//                                    "bottom", "bottomright"; none or "center": as before): x, y
//                                    stay its place in the 640 x 480, and on a view wider or taller
//                                    than that the window keeps its distance to that edge or corner
//                                    of the whole view instead (moved by the view's margin:
//                                    setViewMargins). A client before it ignores the field and
//                                    puts the window at x, y, as it was
//   ui.drag(window, phase, x, y, button)  a press in a window that moves 3 units or more: phase
//                                    "start" (at the press), "move", "end" (let go); x, y in the
//                                    pressed window's units (outside it too)
//   ui.mouseUp(window, x, y, button) a button let go over a window, pressed in one of them
//   ui.wheel(window, x, y, steps)    the wheel over a window: steps > 0 away from the player
//                                    (up), < 0 towards; over a window it's the UI's, taken or not
//
// UI API 4 (mode.json's "uiApi": 4; sent only to a client at client level 5 or above that names
// ModeUi and ModeUi2: ui_proto.hpp's kUiApi4Level) adds, beside nox.api being 4:
//   nox.spellIcon(name, x, y [, w, h])  (inside draw) the client's own icon for a spell
//                                    ("SPELL_FIREBALL") or a warrior's ability ("ABILITY_WARCRY"),
//                                    the book's and the bar's picture (the HD one with the HD
//                                    pack), its size or scaled to w x h; no image on the wire; an
//                                    unknown name draws nothing
//   nox.hitArea(x, y, w, h)          (inside draw) a part of a frameless window that takes the
//                                    mouse: once a window has any in a frame, input there goes to
//                                    it only inside them and is the game's elsewhere (at most
//                                    kUiMaxHitAreas a window a frame); a framed window ignores it
//   nox.label's opts.keep            true: the label stays while its object leaves the client's
//                                    world and comes back (the same net code, of the type it was
//                                    first seen as; another type there lets it go); it lasts till
//                                    nox.clearLabel or a map's load: the client can't tell away
//                                    from gone for good, so a UI clears it when its server says
//                                    the object is gone (picked up, killed, deleted)
//   ui.tick(nowMs)                   once a frame before the draws, the HUD shown or hidden, on
//                                    the call's budget like any hook; windows open, move and close
//                                    from it
//
// UI API 5 (mode.json's "uiApi": 5; sent only to a client at client level 6 or above that names
// ModeUi and ModeUi2: ui_proto.hpp's kUiApi5Level) adds, beside nox.api being 5:
//   nox.hudReserve({x, y, w, h, anchor}) a box (the 640 x 480's units, its anchor as a window's)
//                                    the game's own top-left icons (the enchantments' row) keep
//                                    out of: they start below it; nil: none (hudReserve())
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

#include <cstddef>
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

/// (API 3) The most labels a UI keeps, a label's text, a tooltip's lines and a line's text (bytes),
/// and how long a label waits for an object the client hasn't been sent yet.
inline constexpr std::size_t kUiMaxLabels = 128;
inline constexpr std::size_t kUiMaxLabelText = 64;
inline constexpr std::size_t kUiMaxTooltipLines = 16;
inline constexpr std::size_t kUiMaxTooltipText = 128;
inline constexpr std::uint64_t kUiLabelWaitMs = 3000;
/// (API 4) The most hit areas a window keeps a frame (nox.hitArea; more: let go).
inline constexpr std::size_t kUiMaxHitAreas = 32;

/// (Client level 5: ui_proto.hpp's kDrawScale) The most draw scales a client keeps (more: let go,
/// those things drawn as their types), how long one waits for a thing the client hasn't been sent
/// yet, how long one stays after its thing has left (its burst may come after it: a missile
/// bursts as it goes), and how near its thing's last place a burst must be to take its scale.
inline constexpr std::size_t kUiMaxDrawScales = 256;
inline constexpr std::uint64_t kUiDrawScaleWaitMs = 3000;
inline constexpr std::uint64_t kUiBurstGraceMs = 1000;
inline constexpr float kUiBurstReach = 96;

struct UiDraw {
    enum class Kind : std::uint8_t {
        Box,
        Outline,
        Text,
        Image, ///< (API 2) assets()->list[index], at (x, y), w x h
        Thing, ///< (API 2) thing type `index` (its THNG index + 1: a net type id), centred on (x, y)
        Line,  ///< (API 3) from (x, y) to (w, h) (w, h: its end), `width` wide
        /// (API 4) a spell's icon (`index` its spell id) or a warrior ability's (`index` the
        /// ability's id with client::kAbilityTag), its top left at (x, y), w x h (0: its own size)
        SpellIcon,
    };
    Kind kind = Kind::Box;
    float x = 0, y = 0, w = 0, h = 0; ///< (Text: its top left)
    UiColor color;
    std::string text;
    int index = -1;    ///< (Image, Thing)
    float width = 1;   ///< (Line) 1-8 UI units
};

/// (API 3) A line of a tooltip (nox.tooltip).
struct UiTooltipLine {
    std::string text;
    UiColor color;
};

/// (API 3) Where a tooltip of w x h goes for the mouse at (mouseX, mouseY), all in the UI's 640 x
/// 480 units: below and right of the pointer, flipped to its other side where it would run off,
/// and always whole on the screen (a tooltip bigger than it: its top left at the screen's). The
/// screen: the 640 x 480 and `marginX`, `marginY` past it on each side (the view's margins).
std::pair<float, float> placeTooltip(float mouseX, float mouseY, float w, float h, float marginX = 0, float marginY = 0);

/// (API 3) A label over an object in the world (nox.label).
struct UiLabel {
    enum class Show : std::uint8_t {
        Always,
        Alt,  ///< while the player holds Alt
        Near, ///< while our player is within `near` world units of it
    };
    std::uint16_t object = 0; ///< its net code
    std::string text;         ///< (at most kUiMaxLabelText bytes, whole letters)
    UiColor color;
    Show show = Show::Always;
    float near = 150;  ///< (Near) world units
    float offset = 4;  ///< game pixels above the object's top
    bool keep = false; ///< (API 4) kept while its object is away (back with the same type: drawn again)
    // (The client's: whether its object has been in the client's world, its type then, and when
    // it was set.)
    bool seen = false;
    std::uint16_t typeId = 0;
    std::uint64_t setMs = 0;
};

/// (API 3) A drag's phase (ui.drag).
enum class UiDrag : std::uint8_t { Start, Move, End };

/// A box in a window's units (API 4's hit areas).
struct UiRect {
    float x = 0, y = 0, w = 0, h = 0;
};

/// (Client level 5) A thing's draw scale the server told (ui_proto.hpp's kDrawScale).
struct UiDrawScale {
    std::uint16_t object = 0; ///< its net code
    std::uint16_t type = 0;   ///< its type as the server sent it (another type at the code: not it)
    float scale = 1;          ///< 0.5-3
    // (The client's: whether its thing has been in the client's world, where it was last, when
    // the scale came and when its thing left.)
    bool seen = false;
    bool gone = false;
    bool missile = true; ///< its type a missile's (only those lend their scale to a burst)
    /// Its type's spark burst's size (its COLLIDE argument in the client's thing.bin, as MSG 0x93
    /// carries it; 0: none): only a spark burst of that size takes its scale.
    int burstSize = 0;
    float x = 0, y = 0;
    std::uint64_t setMs = 0;
    std::uint64_t goneMs = 0;
};

/// Where an object of the client's world is, and its type (ScriptUiClient::setObjectAt).
struct UiObjectAt {
    std::uint16_t type = 0;
    float x = 0, y = 0;
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
    // (API 3's; an API 1 or 2 UI's windows keep these defaults.)
    bool frame = true;         ///< framed and labelled as the server's (false: bare, any y)
    bool clickThrough = false; ///< clicks, the wheel and the mouse over it are the game's
    bool hud = false;          ///< hidden while the game hides its HUD; under the framed windows
    /// (API 3: NoxWindowSpec's anchor) Which edge it keeps its distance to on each axis: -1 the
    /// left or top, 0 none (centred with the 640 x 480, as before), 1 the right or bottom; and its
    /// place as nox.window gave it. x, y are that place moved by the view's margins (whole units).
    std::int8_t anchorX = 0, anchorY = 0;
    int specX = 0, specY = 0;
    /// (API 4) This frame's hit areas (nox.hitArea, in its own units): a frameless window with any
    /// takes the mouse only inside them (windowTakes).
    std::vector<UiRect> hitAreas;
};

/// Whether `w` takes the mouse at (x, y) (the 640 x 480 units): inside its rectangle, and for a
/// frameless window with hit areas (API 4) inside one of them. (Its click-through and HUD state
/// aside: the callers ask those.)
bool windowTakes(const UiWindow& w, float x, float y);

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
    /// Once a frame: the draw scales looked after (drawScales; a UI running or not), then (API 4)
    /// ui.tick, what the server sent handed to the UI, and each window drawn (windows()).
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

    /// (API 3) A drag of `button`, pressed in window `id`, at (x, y) in its units: whether the UI
    /// took it (no ui.drag: false).
    bool drag(std::string_view windowId, UiDrag phase, float x, float y, int button);
    /// (API 3) `button` let go over window `id`: whether the UI took it.
    bool mouseUp(std::string_view windowId, float x, float y, int button);
    /// (API 3) The wheel over window `id` (steps > 0 up): whether the UI took it.
    bool wheel(std::string_view windowId, float x, float y, int steps);
    /// The window the mouse at (x, y) (the 640 x 480 units) is over, the topmost as drawn (the
    /// others, then the hud ones): none over a clickThrough window or a hud one while the HUD is
    /// hidden.
    const UiWindow* windowAt(float x, float y) const;
    /// (API 3) Whether the game shows its HUD (false: hud windows aren't drawn, nor told of the
    /// mouse); true until told.
    void setHudShown(bool shown);
    bool hudShown() const noexcept;
    /// How far the view reaches past the 640 x 480 on each side, in its units (the program's fit:
    /// 0, 0 until told, as in a 4:3 view); anchored windows (UiWindow::anchorX/Y) moved by them.
    void setViewMargins(float x, float y);
    /// (API 5) nox.hudReserve's box, in the 640 x 480's units moved by its anchor (as a window's, by
    /// the view's margins): the game's top-left icons (the enchantments' row) start below it. None:
    /// no box, or no UI running.
    std::optional<UiRect> hudReserve() const;
    /// (API 3) This frame's tooltip (empty: none).
    const std::vector<UiTooltipLine>& tooltip() const noexcept;
    /// (API 3) The labels, in the order set.
    const std::vector<UiLabel>& labels() const noexcept;
    /// (API 3) What a label asks of the client's world: the type id of the object with net code
    /// `object` there (none: not there). Unset: the client's own world; without a client (a
    /// preview) labels stay as they're set.
    void setObjectLookup(std::function<std::optional<std::uint16_t>(std::uint16_t object)> lookup);

    /// (Client level 5) The draw scales the server told (ui_proto.hpp's kDrawScale), kept whether
    /// or not a UI runs, looked after in frame(): one goes when its thing has been gone
    /// kUiBurstGraceMs, when it never came within kUiDrawScaleWaitMs, when another type holds its
    /// code, when a map loads, or when the server tells 1.
    const std::vector<UiDrawScale>& drawScales() const noexcept;
    /// (Client level 5) The scale of a burst at (x, y) (world units): that of the scaled missile
    /// nearest it within kUiBurstReach of where it last was, in the client's world or gone within
    /// kUiBurstGraceMs. A spark burst (MSG 0x93) gives its `size`: only a missile whose type's
    /// burst is that size matches, and the match uses it up (a fireball bursts once). A lesser
    /// explosion (0x86, no size: a magic missile's, which also bursts off walls and flies on)
    /// matches any and uses none up. None: none there.
    std::optional<float> burstScaleAt(float x, float y, std::optional<int> size = std::nullopt);
    /// What the draw scales (and, without setObjectLookup, the labels) ask of the client's world:
    /// the object with net code `object`, its type and place. Unset: the client's own world;
    /// without a client either (a preview) they stay as they're told.
    void setObjectAt(std::function<std::optional<UiObjectAt>(std::uint16_t object)> lookup);

    /// Whether a UI is running, the mode's name (its windows' label), and why the last one stopped.
    bool running() const noexcept;
    const std::string& modeName() const noexcept;
    const std::string& error() const noexcept;
    /// The running UI's API (1 to 4; 0: none running).
    int api() const noexcept;
    /// How many UIs have started (each start one more): a UI running now that isn't the one that
    /// ran before (a stop and a start between two looks) has another number.
    std::uint32_t runs() const noexcept;
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
