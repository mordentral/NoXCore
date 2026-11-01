// A NoX Core client's script mode UI (script_ui.hpp).
#include "noxcore/modes/script/script_ui.hpp"

#include "noxcore/client/headless_client.hpp"
#include "noxcore/client/spell_bars.hpp"
#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/modes/script/ui_proto.hpp"
#include "noxcore/modes/script/weather.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/video/nxz.hpp"

#include "value_codec.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>

namespace nox::modes::script {

namespace wx = ::nox::modes::script::weather; // (inside ScriptUiClient, `weather` is its getter)

namespace {

constexpr int kMaxWindows = 8;
constexpr std::size_t kMaxDraws = 2000; ///< a window's in a frame
constexpr std::size_t kMaxText = 256;
constexpr std::size_t kMaxSounds = 16; ///< asked for between two takeSounds (more: let go)

/// The keys a UI is told of (API 2): never typed text.
constexpr std::array<std::string_view, 30> kKeys = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "up", "down", "left", "right", "enter",
    "escape", "tab", "space", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
};

/// A missile type's spark burst size, as MSG 0x93 carries it: its COLLIDE argument when its
/// COLLIDE is SparkExplosionCollide ("SparkExplosionCollide 255"), as the server reads it (the
/// message's one byte); 0: it makes no spark burst.
int sparkBurstSize(const ::nox::things::ThingDef& def)
{
    const auto c = def.attribute("COLLIDE");
    constexpr std::string_view kSpark = "SparkExplosionCollide";
    if (!c || !c->starts_with(kSpark) || c->size() <= kSpark.size() || (*c)[kSpark.size()] != ' ') {
        return 0;
    }
    const std::string arg(c->substr(kSpark.size() + 1));
    return std::atoi(arg.c_str()) & 0xFF;
}

} // namespace

struct ScriptUiClient::Impl {
    client::HeadlessClient* client = nullptr; ///< (none: a preview)
    LuauVm::Limits limits;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    std::function<int(std::string_view)> textWidth;
    std::function<void(std::vector<std::uint8_t>)> sender;

    // The source as it comes.
    std::string modeId;
    std::string modeName;
    std::uint32_t expected = 0;
    std::string source;
    bool receiving = false;
    int uiApi = 1; ///< (the one coming or running)

    // The mod's assets: the set in use, and one coming (ui_proto.hpp's asset subs), every
    // number in it checked again here (a server's are untrusted).
    std::shared_ptr<const ModAssets> assets;
    struct Incoming {
        bool active = false;
        std::size_t count = 0;
        std::size_t imageBytes = 0, soundBytes = 0; ///< (what its header says, at most the caps)
        std::vector<ModAsset> list;
        std::size_t images = 0, sounds = 0;         ///< (so far)
        ModAsset current;                           ///< (the one whose chunks come)
        bool inAsset = false;
        std::uint8_t coding = 0;
        std::size_t raw = 0;
        std::size_t coded = 0;
        std::vector<std::uint8_t> bytes;
    } incoming;
    std::string assetsError; ///< (the last set's refusal: an API 2 UI after it isn't run)

    // The running UI.
    std::unique_ptr<LuauVm> vm;
    int uiRef = 0;
    int runningApi = 0;
    bool hasDraw = false, hasClick = false, hasMessage = false, hasView = false, hasKey = false;
    bool hasDrag = false, hasMouseUp = false, hasWheel = false; ///< (API 3's hooks)
    bool hasTick = false;                                        ///< (API 4's)
    bool hasLabelClick = false;                                  ///< (API 5's)
    std::vector<UiWindow> windows;
    UiWindow* drawing = nullptr; ///< (the window whose draw is running: draws go to it)
    std::deque<std::pair<std::uint8_t, std::vector<std::uint8_t>>> inbox; ///< data and views, oldest first
    /// (Client level 6) A big value on its way (kDataBegin, then its kDataChunks); whole, it goes
    /// into the inbox under kDataBegin, its value's bytes unpacked. `bigIn` how many such wait there.
    struct BigData {
        bool active = false;
        std::uint32_t raw = 0;
        std::uint8_t coding = 0;
        std::uint32_t coded = 0;
        std::vector<std::uint8_t> bytes;
    };
    BigData big;
    int bigIn = 0;
    static constexpr int kMaxBigIn = 8;
    /// (Client level 7) The server's ground decals (kDecal), at most uimsg::kMaxDecals.
    std::vector<uimsg::Decal> decals;
    std::uint64_t nowMs = 0;
    std::string error;
    // API 2's.
    std::optional<std::pair<float, float>> mouse;
    std::optional<UiPlayer> player;
    const things::ThingDb* thingsOverride = nullptr;
    std::vector<UiSound> sounds;
    // API 3's.
    bool hudShown = true;
    /// The view's margins past the 640 x 480 (setViewMargins), in whole units: what an anchored
    /// window is moved by.
    int marginX = 0, marginY = 0;
    /// `w` at its place: nox.window's, moved to its anchor's edges.
    void anchor(UiWindow& w) const
    {
        w.x = w.specX + w.anchorX * marginX;
        w.y = w.specY + w.anchorY * marginY;
    }
    /// (API 5) nox.hudReserve's box, as given (its anchor moves it as a window's).
    struct Reserve {
        UiRect box;
        std::int8_t anchorX = 0, anchorY = 0;
    };
    std::optional<Reserve> reserve;
    bool overMap = false; ///< (API 5) nox.overMap's: the HUD pieces and labels kept up with the full map
    std::vector<UiTooltipLine> tooltip; ///< (this frame's, or the last one's when a frame ran out of time first)
    bool tooltipSet = false;            ///< (nox.tooltip called in this frame)
    std::vector<UiLabel> labels;
    std::uint32_t labelsLoad = 0; ///< (the client's map load the labels were set in)
    std::uint32_t runs = 0;       ///< (UIs started)
    std::function<std::optional<std::uint16_t>(std::uint16_t)> objectLookup;
    std::function<std::optional<UiObjectAt>(std::uint16_t)> objectAt;
    // The draw scales the server told (kDrawScale: with no UI too), and the client's map load
    // they came in (another load since: none).
    std::vector<UiDrawScale> drawScales;
    std::uint32_t drawScalesLoad = 0;
    // The weather the server last told (kWeather: with no UI too), and the client's map load it
    // came in (another load since: none, till the server tells it again).
    wx::Told told;
    std::uint32_t toldAtLoad = 0;

    Impl(client::HeadlessClient* c, LuauVm::Limits l) : client(c), limits(l) {}

    /// The weather now: none once a map has loaded since it was told.
    const wx::Told& weatherNow()
    {
        if (client && client->mapLoads() != toldAtLoad) {
            toldAtLoad = client->mapLoads();
            if (told.state != wx::State{}) {
                told.state = {};
                told.fade = 0;
                ++told.serial;
            }
        }
        return told;
    }

    void send(uimsg::Sub sub, std::span<const std::uint8_t> payload = {})
    {
        if (sender) {
            sender(uimsg::message(sub, payload));
        } else if (client) {
            client->sendNoxcpp(uimsg::message(sub, payload));
        }
    }

    void stop(std::string why)
    {
        if (vm && !why.empty()) {
            uimsg::Writer w;
            w.str(why.substr(0, 512));
            send(uimsg::kFailed, w.out);
        }
        error = std::move(why);
        vm.reset();
        uiRef = 0;
        runningApi = 0;
        windows.clear();
        reserve.reset();
        overMap = false;
        inbox.clear();
        big = BigData{};
        bigIn = 0;
        sounds.clear();
        tooltip.clear();
        labels.clear();
        drawing = nullptr;
    }

    /// (API 3) A map loaded since the labels were set: they all go (the new map's objects are
    /// numbered afresh: a code may come back on another object of the same type).
    void labelsOfThisMap()
    {
        if (client && client->mapLoads() != labelsLoad) {
            labelsLoad = client->mapLoads();
            labels.clear();
        }
    }
    /// (API 3) Labels whose object left the client's world (or never came) let go.
    void pruneLabels()
    {
        labelsOfThisMap();
        if (labels.empty() || (!objectLookup && !objectAt && !client)) {
            return; // (a preview: labels stay as set)
        }
        std::erase_if(labels, [&](UiLabel& l) {
            const std::optional<std::uint16_t> type = objectType(l.object);
            if (!type) {
                // (API 4's keep: away, or not come yet, it waits for its object.)
                return !l.keep && (l.seen || nowMs > l.setMs + kUiLabelWaitMs);
            }
            if (!l.seen) {
                l.seen = true;
                l.typeId = *type;
                return false;
            }
            return *type != l.typeId; // (another object at its net code)
        });
    }
    std::optional<std::uint16_t> objectType(std::uint16_t object) const
    {
        if (objectLookup) {
            return objectLookup(object);
        }
        const auto at = objectPlace(object);
        return at ? std::optional<std::uint16_t>(at->type) : std::nullopt;
    }
    /// The object at net code `object` in the client's world (objectAt's, or the client's).
    std::optional<UiObjectAt> objectPlace(std::uint16_t object) const
    {
        if (objectAt) {
            return objectAt(object);
        }
        if (!client) {
            return std::nullopt;
        }
        const auto& drawables = client->world().drawables();
        const auto it = drawables.find(object);
        if (it == drawables.end()) {
            return std::nullopt;
        }
        return UiObjectAt{it->second.typeId, static_cast<float>(it->second.x), static_cast<float>(it->second.y)};
    }

    /// The draw scales of another map let go (the new map numbers its objects afresh).
    void drawScalesOfThisMap()
    {
        if (client && client->mapLoads() != drawScalesLoad) {
            drawScalesLoad = client->mapLoads();
            drawScales.clear();
        }
    }
    /// kDrawScale: a thing's scale kept (1: let go).
    void tellDrawScale(const uimsg::DrawScale& d)
    {
        drawScalesOfThisMap();
        const auto it = std::ranges::find(drawScales, d.object, &UiDrawScale::object);
        if (d.scale == 1.0f) {
            if (it != drawScales.end()) {
                drawScales.erase(it);
            }
            return;
        }
        UiDrawScale s;
        s.object = d.object;
        s.type = d.type;
        s.scale = d.scale;
        s.setMs = nowMs;
        if (const ::nox::things::ThingDb* db = things(); db) {
            // (Its type a missile's, by the client's own thing.bin; another type: no burst of its.)
            s.missile = d.type >= 1 && d.type <= db->things.size() &&
                        (::nox::things::thingClasses(db->things[d.type - 1u]) & ::nox::things::ObjectClass::Missile) != 0;
            if (s.missile) {
                s.burstSize = sparkBurstSize(db->things[d.type - 1u]); // (read once, as it comes)
            }
        }
        if (it != drawScales.end()) {
            if (it->type == d.type) {
                s.seen = it->seen; // (the same thing: where it was kept)
                s.gone = it->gone;
                s.x = it->x;
                s.y = it->y;
                s.goneMs = it->goneMs;
            }
            *it = s;
        } else if (drawScales.size() < kUiMaxDrawScales) {
            drawScales.push_back(s);
        }
    }
    /// Once a frame: each draw scale's thing looked for (where it is; gone a while, or never come,
    /// or another type at its code: let go).
    void pruneDrawScales()
    {
        drawScalesOfThisMap();
        if (drawScales.empty() || (!objectAt && !objectLookup && !client)) {
            return; // (a preview: kept as told)
        }
        std::erase_if(drawScales, [&](UiDrawScale& d) {
            std::optional<UiObjectAt> at = objectPlace(d.object);
            if (!at && objectLookup && !objectAt) {
                if (const auto type = objectLookup(d.object)) {
                    at = UiObjectAt{*type, d.x, d.y}; // (a test's world of types alone)
                }
            }
            if (!at || at->type != d.type) {
                if (!d.seen) {
                    return nowMs > d.setMs + kUiDrawScaleWaitMs;
                }
                if (!d.gone) {
                    d.gone = true;
                    d.goneMs = nowMs;
                }
                return nowMs > d.goneMs + kUiBurstGraceMs;
            }
            d.seen = true;
            d.gone = false;
            d.x = at->x;
            d.y = at->y;
            return false;
        });
    }
    /// A UI that can't run, the server told why (its views stay text).
    void refuse(const std::string& why, std::string mine)
    {
        uimsg::Writer w;
        w.str(why);
        send(uimsg::kFailed, w.out);
        error = std::move(mine);
    }

    const things::ThingDb* things() const { return thingsOverride ? thingsOverride : client ? client->things() : nullptr; }
    std::optional<UiPlayer> currentPlayer() const;

    void onMessage(std::span<const std::uint8_t> m);
    void onAssetMessage(std::uint8_t sub, uimsg::Reader& r);
    void dropIncoming(std::string why)
    {
        incoming = Incoming{};
        assets.reset();
        assetsError = std::move(why);
    }
    void start();
    bool begin(const char* hook);
    bool finish(const char* hook, int nargs, int nresults);
};

namespace {

using Impl = ScriptUiClient::Impl;

Impl& implOf(lua_State* L)
{
    auto* impl = static_cast<Impl*>(LuauVm::owner(L));
    if (!impl) {
        luaL_error(L, "the UI has no client");
    }
    return *impl;
}

/// A script's number as an int in [lo, hi]: clamped as a double first (a huge one or NaN cast to
/// int is undefined).
int clampedInt(double v, int lo, int hi)
{
    if (!(v >= lo)) {
        return lo; // (NaN too)
    }
    return v > hi ? hi : static_cast<int>(v);
}

UiColor colourArg(lua_State* L, int idx)
{
    if (lua_isstring(L, idx) && !lua_isnumber(L, idx)) {
        static const std::map<std::string, UiColor, std::less<>> kNamed = {
            {"gold", {240, 180, 42, 255}},  {"bright", {255, 236, 170, 255}}, {"dim", {150, 128, 92, 255}},
            {"grey", {118, 110, 98, 255}},  {"white", {255, 255, 255, 255}},  {"black", {0, 0, 0, 255}},
            {"red", {220, 60, 50, 255}},    {"green", {80, 200, 90, 255}},    {"blue", {80, 140, 230, 255}},
        };
        const auto it = kNamed.find(std::string_view(lua_tostring(L, idx)));
        luaL_argcheck(L, it != kNamed.end(), idx, "a colour's name: gold, bright, dim, grey, white, black, red, green or blue");
        return it->second;
    }
    if (lua_istable(L, idx)) {
        const auto part = [&](int i, int fallback) {
            lua_rawgeti(L, idx, i);
            const int v = lua_isnumber(L, -1) ? clampedInt(lua_tonumber(L, -1), 0, 255) : fallback;
            lua_pop(L, 1);
            return static_cast<std::uint8_t>(v);
        };
        return {part(1, 255), part(2, 255), part(3, 255), part(4, 255)};
    }
    if (lua_isnoneornil(L, idx)) {
        return {240, 180, 42, 255};
    }
    luaL_argerror(L, idx, "a colour: a name or { r, g, b [, a] }");
    return {};
}

UiWindow& drawingWindow(lua_State* L)
{
    Impl& impl = implOf(L);
    if (!impl.drawing) {
        luaL_error(L, "drawing only in ui.draw");
    }
    if (impl.drawing->draws.size() >= kMaxDraws) {
        luaL_error(L, "over %d draws in a window in one frame", static_cast<int>(kMaxDraws));
    }
    return *impl.drawing;
}

float clampedFloat(double v, float lo, float hi, float fallback);

/// The table at `idx`'s anchor (NoxWindowSpec's: none or "center" 0, 0) into `ax`, `ay`: -1 the left
/// or top, 1 the right or bottom; another name stops the UI with the names.
void readAnchor(lua_State* L, int idx, std::int8_t& ax, std::int8_t& ay)
{
    lua_getfield(L, idx, "anchor");
    if (!lua_isnil(L, -1)) {
        const char* a = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        struct Anchor {
            std::string_view name;
            std::int8_t x, y;
        };
        static constexpr Anchor kAnchors[] = {{"center", 0, 0},  {"topleft", -1, -1}, {"top", 0, -1},       {"topright", 1, -1}, {"left", -1, 0},
                                              {"right", 1, 0},   {"bottomleft", -1, 1}, {"bottom", 0, 1}, {"bottomright", 1, 1}};
        const auto found = std::ranges::find(kAnchors, std::string_view(a), &Anchor::name);
        luaL_argcheck(L, found != std::end(kAnchors), idx,
                      "anchor is \"center\", \"topleft\", \"top\", \"topright\", \"left\", \"right\", \"bottomleft\", \"bottom\" or \"bottomright\"");
        ax = found->x;
        ay = found->y;
    }
    lua_pop(L, 1);
}

int uiHudReserve(lua_State* L)
{
    // (API 5) nox.hudReserve({x, y, w, h, anchor}) or nil: a box (the 640 x 480's units, its anchor
    // as a window's) the game's own top-left icons (the enchantments' row) keep out of: they start
    // below it. nil: none (as the game draws them).
    Impl& impl = implOf(L);
    if (lua_isnoneornil(L, 1)) {
        impl.reserve.reset();
        return 0;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    const auto num = [&](const char* name, float lo, float hi) {
        lua_getfield(L, 1, name);
        luaL_argcheck(L, lua_isnumber(L, -1), 1, "x, y, w and h are numbers");
        const float v = clampedFloat(lua_tonumber(L, -1), lo, hi, 0);
        lua_pop(L, 1);
        return v;
    };
    Impl::Reserve r;
    r.box.x = num("x", 0, 640);
    r.box.y = num("y", 0, 480);
    r.box.w = num("w", 0, 640);
    r.box.h = num("h", 0, 480);
    readAnchor(L, 1, r.anchorX, r.anchorY);
    impl.reserve = r;
    return 0;
}

int uiOverMap(lua_State* L)
{
    // (API 5) nox.overMap(on): true keeps the UI's HUD pieces (drawn and taking the mouse) and its
    // labels up while the full map is open; false or nil: hidden with it, as the game's HUD hid them.
    luaL_argcheck(L, lua_isnoneornil(L, 1) || lua_isboolean(L, 1), 1, "on is a boolean");
    implOf(L).overMap = lua_toboolean(L, 1) != 0;
    return 0;
}

int uiWindow(lua_State* L)
{
    Impl& impl = implOf(L);
    if (impl.drawing) {
        luaL_error(L, "windows open and close outside ui.draw");
    }
    const std::string id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const auto field = [&](const char* name, int fallback) {
        lua_getfield(L, 2, name);
        const int v = lua_isnumber(L, -1) ? clampedInt(lua_tonumber(L, -1), -100000, 100000) : fallback;
        lua_pop(L, 1);
        return v;
    };
    lua_getfield(L, 2, "title");
    const std::string title = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)).substr(0, 48) : id;
    lua_pop(L, 1);
    UiWindow w;
    w.id = id.substr(0, 32);
    w.title = title;
    if (impl.runningApi >= 3) {
        // (API 3's fields: an older UI's windows are framed, take clicks, and aren't HUD pieces.)
        const auto flag = [&](const char* name, bool fallback) {
            lua_getfield(L, 2, name);
            luaL_argcheck(L, lua_isnil(L, -1) || lua_isboolean(L, -1), 2, "frame, clickThrough and hud are booleans");
            const bool v = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return v;
        };
        w.frame = flag("frame", true);
        w.clickThrough = flag("clickThrough", false);
        w.hud = flag("hud", false);
        readAnchor(L, 2, w.anchorX, w.anchorY);
    }
    if (w.frame) {
        // (On the screen whole, and no smaller than its frame's title needs.)
        w.w = std::clamp(field("w", 160), 32, 600);
        w.h = std::clamp(field("h", 100), 16, 440);
        w.x = std::clamp(field("x", 20), 4, 636 - w.w);
        w.y = std::clamp(field("y", 40), 20, 476 - w.h);
    } else {
        // (No frame: anywhere on the screen, whole.)
        w.w = std::clamp(field("w", 160), 1, 640);
        w.h = std::clamp(field("h", 100), 1, 480);
        w.x = std::clamp(field("x", 20), 0, 640 - w.w);
        w.y = std::clamp(field("y", 40), 0, 480 - w.h);
    }
    w.specX = w.x;
    w.specY = w.y;
    impl.anchor(w);
    const auto it = std::ranges::find(impl.windows, w.id, &UiWindow::id);
    if (it != impl.windows.end()) {
        it->title = w.title;
        it->x = w.x;
        it->y = w.y;
        it->w = w.w;
        it->h = w.h;
        it->frame = w.frame;
        it->clickThrough = w.clickThrough;
        it->hud = w.hud;
        it->anchorX = w.anchorX;
        it->anchorY = w.anchorY;
        it->specX = w.specX;
        it->specY = w.specY;
    } else {
        luaL_argcheck(L, static_cast<int>(impl.windows.size()) < kMaxWindows, 1, "at most 8 windows");
        impl.windows.push_back(std::move(w));
    }
    return 0;
}

int uiCloseWindow(lua_State* L)
{
    Impl& impl = implOf(L);
    if (impl.drawing) {
        luaL_error(L, "windows open and close outside ui.draw");
    }
    const std::string id = luaL_checkstring(L, 1);
    std::erase_if(impl.windows, [&](const UiWindow& w) { return w.id == id; });
    return 0;
}

int uiBox(lua_State* L, UiDraw::Kind kind)
{
    UiWindow& w = drawingWindow(L);
    UiDraw d;
    d.kind = kind;
    d.x = static_cast<float>(luaL_checknumber(L, 1));
    d.y = static_cast<float>(luaL_checknumber(L, 2));
    d.w = static_cast<float>(luaL_checknumber(L, 3));
    d.h = static_cast<float>(luaL_checknumber(L, 4));
    d.color = colourArg(L, 5);
    w.draws.push_back(std::move(d));
    return 0;
}
int uiFill(lua_State* L) { return uiBox(L, UiDraw::Kind::Box); }
int uiOutline(lua_State* L) { return uiBox(L, UiDraw::Kind::Outline); }

/// (API 5) nox.text's and nox.textWidth's size at `idx`: 1-4 times the UI's text (none: 1; an
/// API 4 UI's: 1 whatever it passes, as before).
float textSizeArg(lua_State* L, int idx)
{
    if (implOf(L).runningApi < 5 || lua_isnoneornil(L, idx)) {
        return 1;
    }
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= 1 && v <= 4, idx, "a text size is 1-4");
    return static_cast<float>(v);
}

int uiText(lua_State* L)
{
    UiWindow& w = drawingWindow(L);
    UiDraw d;
    d.kind = UiDraw::Kind::Text;
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    d.text.assign(s, std::min(n, kMaxText));
    d.x = static_cast<float>(luaL_checknumber(L, 2));
    d.y = static_cast<float>(luaL_checknumber(L, 3));
    d.color = colourArg(L, 4);
    d.size = textSizeArg(L, 5);
    w.draws.push_back(std::move(d));
    return 0;
}

int uiTextWidth(lua_State* L)
{
    const Impl& impl = implOf(L);
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    const std::string_view text(s, std::min(n, kMaxText));
    const int width = impl.textWidth ? impl.textWidth(text) : static_cast<int>(text.size()) * 6;
    lua_pushinteger(L, static_cast<int>(std::lround(static_cast<float>(width) * textSizeArg(L, 2))));
    return 1;
}

int uiSend(lua_State* L)
{
    Impl& impl = implOf(L);
    std::vector<std::uint8_t> bytes;
    std::string why;
    if (!encodeValue(L, 1, bytes, why)) {
        luaL_error(L, "send: %s", why.c_str());
    }
    impl.send(uimsg::kSend, bytes);
    return 0;
}

int uiAnswer(lua_State* L)
{
    Impl& impl = implOf(L);
    const int index = luaL_checkinteger(L, 1);
    luaL_argcheck(L, index >= 1 && index <= 9, 1, "an answer 1-9");
    const std::uint8_t b = static_cast<std::uint8_t>(index);
    impl.send(uimsg::kAnswer, std::span(&b, 1));
    return 0;
}

int uiTime(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(implOf(L).nowMs));
    return 1;
}

int uiMode(lua_State* L)
{
    const Impl& impl = implOf(L);
    lua_pushlstring(L, impl.modeName.data(), impl.modeName.size());
    return 1;
}

// ---- UI API 2

int uiImage(lua_State* L)
{
    UiWindow& w = drawingWindow(L);
    const Impl& impl = implOf(L);
    size_t n = 0;
    const char* name = luaL_checklstring(L, 1, &n);
    const int index = impl.assets ? impl.assets->find(std::string_view(name, n), ModAsset::Kind::Image) : -1;
    luaL_argcheck(L, index >= 0, 1, "no such image among the mod's assets");
    const ModAsset& a = impl.assets->list[static_cast<std::size_t>(index)];
    UiDraw d;
    d.kind = UiDraw::Kind::Image;
    d.index = index;
    d.x = static_cast<float>(luaL_checknumber(L, 2));
    d.y = static_cast<float>(luaL_checknumber(L, 3));
    d.w = static_cast<float>(luaL_optnumber(L, 4, a.width));
    d.h = static_cast<float>(luaL_optnumber(L, 5, a.height));
    w.draws.push_back(std::move(d));
    return 0;
}

int uiThing(lua_State* L)
{
    UiWindow& w = drawingWindow(L);
    const Impl& impl = implOf(L);
    size_t n = 0;
    const char* name = luaL_checklstring(L, 1, &n);
    const things::ThingDb* db = impl.things();
    const things::ThingDef* def = db ? db->findThing(std::string_view(name, n)) : nullptr;
    luaL_argcheck(L, def != nullptr, 1, "no thing type of that name");
    UiDraw d;
    d.kind = UiDraw::Kind::Thing;
    d.index = static_cast<int>(def - db->things.data()) + 1; // (its net type id)
    d.x = static_cast<float>(luaL_checknumber(L, 2));
    d.y = static_cast<float>(luaL_checknumber(L, 3));
    w.draws.push_back(std::move(d));
    return 0;
}

int soundVolume(lua_State* L, int idx)
{
    return clampedInt(luaL_optnumber(L, idx, 100), 0, 100);
}

int uiSound(lua_State* L)
{
    Impl& impl = implOf(L);
    size_t n = 0;
    const char* name = luaL_checklstring(L, 1, &n);
    const int index = impl.assets ? impl.assets->find(std::string_view(name, n), ModAsset::Kind::Sound) : -1;
    luaL_argcheck(L, index >= 0, 1, "no such sound among the mod's assets");
    // (The mod's sounds share one stream, a new one cutting the last at once: of a batch, only
    // the last is kept.)
    std::erase_if(impl.sounds, [](const UiSound& s) { return !s.game; });
    if (impl.sounds.size() < kMaxSounds) {
        impl.sounds.push_back({false, index, {}, soundVolume(L, 2)});
    }
    return 0;
}

int uiGameSound(lua_State* L)
{
    Impl& impl = implOf(L);
    size_t n = 0;
    const char* name = luaL_checklstring(L, 1, &n);
    luaL_argcheck(L, n > 0 && n <= 64, 1, "a sound's name");
    if (impl.sounds.size() < kMaxSounds) {
        impl.sounds.push_back({true, -1, std::string(name, n), soundVolume(L, 2)});
    }
    return 0;
}

/// The topmost window at (x, y) the mouse may be over (ScriptUiClient::windowAt's), as they're
/// drawn: the others newest first, then the HUD pieces (drawn under them) newest first. (An API 1
/// or 2 UI has no HUD pieces: newest first, as it always was.)
const UiWindow* windowUnder(const Impl& impl, float x, float y)
{
    for (const bool hud : {false, true}) {
        for (auto it = impl.windows.rbegin(); it != impl.windows.rend(); ++it) {
            const UiWindow& w = *it;
            if (w.hud != hud || w.clickThrough || (w.hud && !impl.hudShown)) {
                continue;
            }
            if (windowTakes(w, x, y)) {
                return &w;
            }
        }
    }
    return nullptr;
}

int uiMouse(lua_State* L)
{
    const Impl& impl = implOf(L);
    if (!impl.mouse) {
        return 0;
    }
    const auto [x, y] = *impl.mouse;
    // (The top window under it: the last one opened; never a clickThrough one.)
    const UiWindow* w = windowUnder(impl, x, y);
    if (!w) {
        return 0;
    }
    lua_pushlstring(L, w->id.data(), w->id.size());
    lua_pushnumber(L, x - static_cast<float>(w->x));
    lua_pushnumber(L, y - static_cast<float>(w->y));
    return 3;
}

int uiPlayer(lua_State* L)
{
    const auto p = implOf(L).currentPlayer();
    if (!p) {
        return 0;
    }
    static constexpr const char* kClasses[] = {"warrior", "wizard", "conjurer"};
    lua_createtable(L, 0, 7);
    lua_pushlstring(L, p->name.data(), std::min<std::size_t>(p->name.size(), 64));
    lua_setfield(L, -2, "name");
    lua_pushstring(L, kClasses[std::min<int>(p->playerClass, 2)]);
    lua_setfield(L, -2, "class");
    const std::pair<const char*, int> numbers[] = {{"level", p->level}, {"health", p->health}, {"maxHealth", p->maxHealth},
                                                   {"mana", p->mana},   {"maxMana", p->maxMana}};
    for (const auto& [key, v] : numbers) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, key);
    }
    return 1;
}

// ---- UI API 3

/// `s` (n bytes) cut to `cap` bytes, at a whole letter.
std::string cutText(const char* s, std::size_t n, std::size_t cap)
{
    if (n <= cap) {
        return std::string(s, n);
    }
    // (The first byte left out inside a letter: the cut goes back to that letter's start.)
    std::size_t end = cap;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
        --end;
    }
    return std::string(s, end);
}

/// A script's number as a float in [lo, hi] (NaN: `fallback`).
float clampedFloat(double v, float lo, float hi, float fallback)
{
    if (v != v) {
        return fallback;
    }
    return static_cast<float>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
}

int uiLine(lua_State* L)
{
    UiWindow& w = drawingWindow(L);
    constexpr float kFar = 100000;
    UiDraw d;
    d.kind = UiDraw::Kind::Line;
    d.x = clampedFloat(luaL_checknumber(L, 1), -kFar, kFar, 0);
    d.y = clampedFloat(luaL_checknumber(L, 2), -kFar, kFar, 0);
    d.w = clampedFloat(luaL_checknumber(L, 3), -kFar, kFar, 0);
    d.h = clampedFloat(luaL_checknumber(L, 4), -kFar, kFar, 0);
    d.color = colourArg(L, 5);
    d.width = clampedFloat(luaL_optnumber(L, 6, 1), 1, 8, 1);
    w.draws.push_back(std::move(d));
    return 0;
}

int uiTooltip(lua_State* L)
{
    Impl& impl = implOf(L);
    if (!impl.drawing) {
        luaL_error(L, "a tooltip only in ui.draw");
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    // (Each call this frame replaces the last: one tooltip a frame.)
    impl.tooltip.clear();
    impl.tooltipSet = true;
    const int n = std::min(lua_objlen(L, 1), static_cast<int>(kUiMaxTooltipLines));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        const int line = lua_gettop(L);
        UiTooltipLine t;
        t.color = {255, 255, 255, 255};
        if (lua_istable(L, line)) {
            lua_rawgeti(L, line, 1);
            size_t len = 0;
            const char* s = lua_isstring(L, -1) ? lua_tolstring(L, -1, &len) : nullptr;
            luaL_argcheck(L, s != nullptr, 1, "a tooltip's line is { text, colour? }");
            t.text = cutText(s, len, kUiMaxTooltipText);
            lua_rawgeti(L, line, 2);
            if (!lua_isnil(L, -1)) {
                t.color = colourArg(L, lua_gettop(L));
            }
            lua_pop(L, 2);
        } else if (lua_isstring(L, line)) {
            size_t len = 0;
            const char* s = lua_tolstring(L, line, &len);
            t.text = cutText(s, len, kUiMaxTooltipText);
        } else {
            luaL_argerror(L, 1, "a tooltip's line is { text, colour? }");
        }
        lua_pop(L, 1);
        impl.tooltip.push_back(std::move(t));
    }
    return 0;
}

std::uint16_t objectArg(lua_State* L, int idx)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= 1 && v <= 0xFFFF && v == static_cast<double>(static_cast<int>(v)), idx, "an object's net code, 1-65535");
    return static_cast<std::uint16_t>(v);
}

int uiLabel(lua_State* L)
{
    Impl& impl = implOf(L);
    UiLabel l;
    l.object = objectArg(L, 1);
    size_t n = 0;
    const char* s = luaL_checklstring(L, 2, &n);
    l.text = cutText(s, n, kUiMaxLabelText);
    l.color = colourArg(L, 3);
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        lua_getfield(L, 4, "show");
        if (!lua_isnil(L, -1)) {
            const char* show = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            const std::string_view v(show);
            luaL_argcheck(L, v == "always" || v == "alt" || v == "near", 4, "show is \"always\", \"alt\" or \"near\"");
            l.show = v == "alt" ? UiLabel::Show::Alt : v == "near" ? UiLabel::Show::Near : UiLabel::Show::Always;
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "near");
        if (lua_isnumber(L, -1)) {
            l.near = clampedFloat(lua_tonumber(L, -1), 0, 2000, 150);
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "offset");
        if (lua_isnumber(L, -1)) {
            l.offset = clampedFloat(lua_tonumber(L, -1), -64, 256, 4);
        }
        lua_pop(L, 1);
        if (impl.runningApi >= 4) {
            // (API 4's keep; an API 3 UI's opts are read as they always were.)
            lua_getfield(L, 4, "keep");
            luaL_argcheck(L, lua_isnil(L, -1) || lua_isboolean(L, -1), 4, "keep is a boolean");
            l.keep = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        if (impl.runningApi >= 5) {
            // (API 5's click: a left click on the label is the UI's.)
            lua_getfield(L, 4, "click");
            luaL_argcheck(L, lua_isnil(L, -1) || lua_isboolean(L, -1), 4, "click is a boolean");
            l.click = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
    }
    impl.labelsOfThisMap(); // (a map loaded since the last frame: the old map's labels gone first)
    const auto it = std::ranges::find(impl.labels, l.object, &UiLabel::object);
    if (it != impl.labels.end()) {
        // (Its object as it was: a label waiting for one waits afresh.)
        l.seen = it->seen;
        l.typeId = it->typeId;
        l.setMs = it->seen ? it->setMs : impl.nowMs;
        *it = std::move(l);
    } else if (impl.labels.size() >= kUiMaxLabels) {
        lua_pushboolean(L, 0);
        return 1;
    } else {
        l.setMs = impl.nowMs;
        impl.labels.push_back(std::move(l));
    }
    lua_pushboolean(L, 1);
    return 1;
}

int uiClearLabel(lua_State* L)
{
    const std::uint16_t object = objectArg(L, 1);
    std::erase_if(implOf(L).labels, [&](const UiLabel& l) { return l.object == object; });
    return 0;
}

int uiClearLabels(lua_State* L)
{
    implOf(L).labels.clear();
    return 0;
}

// ---- UI API 4

int uiSpellIcon(lua_State* L)
{
    UiWindow& w = drawingWindow(L);
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    const std::string_view name(s, n);
    constexpr float kFar = 100000; // (as a line's ends: NaN 0, the infinities kept to it)
    const float x = clampedFloat(luaL_checknumber(L, 2), -kFar, kFar, 0);
    const float y = clampedFloat(luaL_checknumber(L, 3), -kFar, kFar, 0);
    const float dw = clampedFloat(luaL_optnumber(L, 4, 0), 0, 640, 0);
    const float dh = clampedFloat(luaL_optnumber(L, 5, 0), 0, 640, 0);
    // A spell by the game's name, or a warrior's ability by its (tagged, as the bars keep them in
    // a multiclass book); any other name draws nothing.
    int index = -1;
    if (name.starts_with("SPELL_")) {
        if (const auto id = ::nox::things::spellId(name); id && *id != 0) {
            index = static_cast<int>(*id);
        }
    } else if (name.starts_with("ABILITY_")) {
        for (std::size_t i = 1; i < ::nox::client::kAbilityNames.size(); ++i) {
            if (name == ::nox::client::kAbilityNames[i]) {
                index = static_cast<int>(::nox::client::kAbilityTag | i);
            }
        }
    }
    if (index < 0) {
        return 0;
    }
    UiDraw d;
    d.kind = UiDraw::Kind::SpellIcon;
    d.index = index;
    d.x = x;
    d.y = y;
    d.w = dw;
    d.h = dh;
    w.draws.push_back(std::move(d));
    return 0;
}

int uiHitArea(lua_State* L)
{
    Impl& impl = implOf(L);
    if (!impl.drawing) {
        luaL_error(L, "hit areas only in ui.draw");
    }
    UiWindow& w = *impl.drawing;
    constexpr float kFar = 100000;
    UiRect r;
    r.x = clampedFloat(luaL_checknumber(L, 1), -kFar, kFar, 0);
    r.y = clampedFloat(luaL_checknumber(L, 2), -kFar, kFar, 0);
    r.w = clampedFloat(luaL_checknumber(L, 3), 0, kFar, 0);
    r.h = clampedFloat(luaL_checknumber(L, 4), 0, kFar, 0);
    // (Past the cap, or empty: let go. A framed window's are kept but never read.)
    if (r.w > 0 && r.h > 0 && w.hitAreas.size() < kUiMaxHitAreas) {
        w.hitAreas.push_back(r);
    }
    return 0;
}

/// An API 3 hook that may take what it's told: `push` its arguments (`nargs` of them).
template <class Push>
bool callTaking(Impl& impl, const char* hook, int nargs, Push&& push)
{
    try {
        if (!impl.vm || !impl.begin(hook)) {
            return false;
        }
        lua_State* L = impl.vm->thread();
        push(L);
        if (!impl.finish(hook, nargs, 1)) {
            return false;
        }
        const bool taken = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return taken;
    } catch (const std::exception& e) {
        impl.stop(std::string("the UI failed: ") + e.what());
        return false;
    }
}

} // namespace

std::optional<UiPlayer> Impl::currentPlayer() const
{
    if (player) {
        return player;
    }
    if (!client || client->stage() != client::HeadlessClient::Stage::InGame) {
        return std::nullopt;
    }
    const auto& world = client->world();
    const auto& self = world.self();
    UiPlayer p;
    p.name = client->profile().name;
    if (const auto it = world.players().find(client->netCode()); it != world.players().end()) {
        p.playerClass = it->second.playerClass;
    } else {
        p.playerClass = client->profile().playerClass;
    }
    p.level = self.level;
    p.health = self.health;
    p.maxHealth = self.maxHealth;
    p.mana = self.mana;
    p.maxMana = self.maxMana;
    return p;
}

bool Impl::begin(const char* hook)
{
    lua_State* L = vm->thread();
    lua_getref(L, uiRef);
    lua_rawgetfield(L, -1, hook); // (raw: no metamethod runs outside a budgeted call)
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool Impl::finish(const char* hook, int nargs, int nresults)
{
    std::string why;
    if (!vm->call(nargs, nresults, why)) {
        stop(std::string("ui.") + hook + ": " + why);
        return false;
    }
    return true;
}

void Impl::start()
{
    vm.reset();
    windows.clear();
    error.clear();
    sounds.clear();
    tooltip.clear();
    labels.clear();
    labelsLoad = client ? client->mapLoads() : 0;
    ++runs;
    vm = std::make_unique<LuauVm>(limits);
    vm->setOwner(this);
    lua_State* L = vm->thread();
    static constexpr std::pair<const char*, lua_CFunction> kToolbox[] = {
        {"window", uiWindow}, {"closeWindow", uiCloseWindow}, {"box", uiFill},     {"outline", uiOutline}, {"text", uiText},
        {"textWidth", uiTextWidth}, {"send", uiSend},      {"answer", uiAnswer}, {"time", uiTime},       {"mode", uiMode},
    };
    // (API 2's, only for a UI written for it: an API 1 UI sees the toolbox it always did.)
    static constexpr std::pair<const char*, lua_CFunction> kToolbox2[] = {
        {"image", uiImage}, {"thing", uiThing}, {"sound", uiSound}, {"gameSound", uiGameSound}, {"mouse", uiMouse}, {"player", uiPlayer},
    };
    static constexpr std::pair<const char*, lua_CFunction> kToolbox3[] = {
        {"line", uiLine}, {"tooltip", uiTooltip}, {"label", uiLabel}, {"clearLabel", uiClearLabel}, {"clearLabels", uiClearLabels},
    };
    static constexpr std::pair<const char*, lua_CFunction> kToolbox4[] = {
        {"spellIcon", uiSpellIcon}, {"hitArea", uiHitArea},
    };
    static constexpr std::pair<const char*, lua_CFunction> kToolbox5[] = {
        {"hudReserve", uiHudReserve}, {"overMap", uiOverMap},
    };
    lua_createtable(L, 0,
                    static_cast<int>(std::size(kToolbox) + std::size(kToolbox2) + std::size(kToolbox3) + std::size(kToolbox4) + std::size(kToolbox5)) + 1);
    for (const auto& [name, fn] : kToolbox) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    if (uiApi >= 2) {
        for (const auto& [name, fn] : kToolbox2) {
            lua_pushcfunction(L, fn, name);
            lua_setfield(L, -2, name);
        }
    }
    if (uiApi >= 3) {
        for (const auto& [name, fn] : kToolbox3) {
            lua_pushcfunction(L, fn, name);
            lua_setfield(L, -2, name);
        }
    }
    if (uiApi >= 4) {
        for (const auto& [name, fn] : kToolbox4) {
            lua_pushcfunction(L, fn, name);
            lua_setfield(L, -2, name);
        }
    }
    if (uiApi >= 5) {
        for (const auto& [name, fn] : kToolbox5) {
            lua_pushcfunction(L, fn, name);
            lua_setfield(L, -2, name);
        }
    }
    lua_pushinteger(L, uiApi >= 2 ? uiApi : kScriptApi);
    lua_setfield(L, -2, "api");
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "nox");
    runningApi = uiApi;
    std::string why;
    if (!vm->run("ui (" + modeId + ")", source, 1, why)) {
        stop(why);
        return;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        stop("the UI script must return its table");
        return;
    }
    uiRef = lua_ref(L, -1);
    const auto has = [&](const char* name) {
        lua_rawgetfield(L, -1, name);
        const bool f = lua_isfunction(L, -1);
        lua_pop(L, 1);
        return f;
    };
    hasDraw = has("draw");
    hasClick = has("click");
    hasMessage = has("onMessage");
    hasView = has("onView");
    hasKey = uiApi >= 2 && has("key");
    hasDrag = uiApi >= 3 && has("drag");
    hasMouseUp = uiApi >= 3 && has("mouseUp");
    hasWheel = uiApi >= 3 && has("wheel");
    hasTick = uiApi >= 4 && has("tick");
    hasLabelClick = uiApi >= 5 && has("labelClick");
    lua_pop(L, 1);
    if (begin("start") && !finish("start", 0, 0)) {
        return;
    }
    const std::uint8_t views = hasView ? 1 : 0;
    send(uimsg::kReady, std::span(&views, 1));
}

void Impl::onAssetMessage(std::uint8_t sub, uimsg::Reader& r)
{
    Incoming& in = incoming;
    switch (sub) {
    case uimsg::kAssets: {
        const std::uint16_t count = r.u16();
        const std::uint32_t imageBytes = r.u32();
        const std::uint32_t soundBytes = r.u32();
        if (!r.ok) {
            return dropIncoming("the server's assets: a short header");
        }
        if (count > kMaxAssets || imageBytes > kMaxImageBytes || soundBytes > kMaxSoundBytes) {
            return dropIncoming("the server's assets are over the caps");
        }
        dropIncoming({});
        in.active = true;
        in.count = count;
        in.imageBytes = imageBytes;
        in.soundBytes = soundBytes;
        break;
    }
    case uimsg::kAsset: {
        if (!in.active) {
            return;
        }
        const std::uint16_t index = r.u16();
        const std::uint8_t kind = r.u8();
        std::string name = r.str();
        ModAsset a;
        std::size_t raw = 0;
        if (kind == static_cast<std::uint8_t>(ModAsset::Kind::Image)) {
            a.kind = ModAsset::Kind::Image;
            a.width = r.u16();
            a.height = r.u16();
            if (a.width < 1 || a.height < 1 || a.width > kMaxAssetSide || a.height > kMaxAssetSide) {
                return dropIncoming("the server's assets: an image over " + std::to_string(kMaxAssetSide) + " a side");
            }
            raw = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height) * 4;
            if (in.images + raw > in.imageBytes) {
                return dropIncoming("the server's assets: images over what it said");
            }
        } else if (kind == static_cast<std::uint8_t>(ModAsset::Kind::Sound)) {
            a.kind = ModAsset::Kind::Sound;
            a.rate = r.u32();
            a.channels = r.u8();
            const std::uint32_t frames = r.u32();
            if (a.channels < 1 || a.channels > 2 || a.rate < 1 || a.rate > kMaxSoundRate || frames < 1 ||
                frames > kMaxSoundBytes / 2) {
                return dropIncoming("the server's assets: a sound out of bounds");
            }
            raw = static_cast<std::size_t>(frames) * a.channels * 2;
            if (in.sounds + raw > in.soundBytes) {
                return dropIncoming("the server's assets: sounds over what it said");
            }
        } else {
            return dropIncoming("the server's assets: an unknown kind");
        }
        const std::uint8_t coding = r.u8();
        const std::uint32_t coded = r.u32();
        if (!r.ok) {
            return dropIncoming("the server's assets: a short asset");
        }
        if (in.inAsset || index != in.list.size() || index >= in.count) {
            return dropIncoming("the server's assets: out of order");
        }
        // (Names in byte order, each once: as the host lists them.)
        if (!validAssetName(name) || (!in.list.empty() && !(in.list.back().name < name))) {
            return dropIncoming("the server's assets: a bad name");
        }
        if (coding > 1 || (coding == 0 && coded != raw) || coded > raw || coded == 0) {
            return dropIncoming("the server's assets: a bad coding");
        }
        a.name = std::move(name);
        in.current = std::move(a);
        in.inAsset = true;
        in.coding = coding;
        in.raw = raw;
        in.coded = coded;
        in.bytes.clear();
        in.bytes.reserve(std::min<std::size_t>(coded, 1u << 20));
        break;
    }
    case uimsg::kAssetChunk: {
        if (!in.active) {
            return;
        }
        const std::uint16_t index = r.u16();
        const std::uint32_t at = r.u32();
        const auto bytes = r.rest();
        if (!r.ok || !in.inAsset || index != in.list.size() || at != in.bytes.size() || bytes.empty() ||
            bytes.size() > in.coded - in.bytes.size()) {
            return dropIncoming("the server's assets: a bad chunk");
        }
        in.bytes.insert(in.bytes.end(), bytes.begin(), bytes.end());
        if (in.bytes.size() < in.coded) {
            return;
        }
        std::vector<std::uint8_t> plain;
        if (in.coding == 1) {
            try {
                plain = video::nxzDecompress(in.bytes, in.raw);
            } catch (const std::exception&) {
                return dropIncoming("the server's assets: bad NXZ");
            }
            if (plain.size() != in.raw) {
                return dropIncoming("the server's assets: bad NXZ");
            }
        } else {
            plain = std::move(in.bytes);
        }
        in.bytes = {};
        ModAsset a = std::move(in.current);
        in.inAsset = false;
        if (a.kind == ModAsset::Kind::Image) {
            a.rgba = std::move(plain);
            in.images += in.raw;
        } else {
            a.samples.resize(in.raw / 2);
            for (std::size_t i = 0; i < a.samples.size(); ++i) {
                a.samples[i] = static_cast<std::int16_t>(plain[2 * i] | plain[2 * i + 1] << 8);
            }
            in.sounds += in.raw;
        }
        if (const std::string bad = checkAsset(a); !bad.empty()) {
            return dropIncoming("the server's assets: " + bad);
        }
        in.list.push_back(std::move(a));
        break;
    }
    default: return;
    }
    if (in.active && !in.inAsset && in.list.size() == in.count) {
        auto set = std::make_shared<ModAssets>();
        set->list = std::move(in.list);
        set->tally();
        incoming = Incoming{};
        assets = std::move(set);
    }
}

void Impl::onMessage(std::span<const std::uint8_t> m)
{
    const auto p = uimsg::parse(m);
    if (!p) {
        return;
    }
    const auto& [sub, rest] = *p;
    uimsg::Reader r{rest};
    switch (sub) {
    case uimsg::kBegin: {
        const std::uint8_t api = r.u8();
        const std::uint32_t size = r.u32();
        std::string id = r.str();
        std::string name = r.str();
        if (!r.ok) {
            return;
        }
        stop({});
        // (The label on its windows: 32 bytes at most, whole letters.)
        modeId = std::move(id).substr(0, 32);
        modeName = cutText(name.data(), name.size(), 32);
        source.clear();
        expected = size;
        receiving = true;
        uiApi = api;
        if (api == 1) {
            assets.reset(); // (an API 1 UI has no use for a set: none kept)
            incoming = Incoming{};
        }
        if (api < 1 || api > kUiApi || size > uimsg::kMaxSource) {
            receiving = false;
            refuse(api < 1 || api > kUiApi ? "written for another UI API" : "the UI is over the 256 KB cap",
                   api < 1 || api > kUiApi ? "the server's UI is for another API" : "the server's UI is over the 256 KB cap");
        } else if (api >= 2 && (incoming.active || !assetsError.empty())) {
            receiving = false;
            const std::string why = assetsError.empty() ? "its assets came short" : assetsError;
            refuse("its assets were refused: " + why, why);
        } else if (size == 0) {
            receiving = false;
            start();
        }
        break;
    }
    case uimsg::kChunk: {
        const std::uint32_t at = r.u32();
        if (!receiving || !r.ok || at != source.size()) {
            return; // (out of order: the server sends them in order on a reliable stream)
        }
        const auto bytes = r.rest();
        if (source.size() + bytes.size() > expected) {
            receiving = false;
            return;
        }
        source.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (source.size() == expected) {
            receiving = false;
            start();
        }
        break;
    }
    case uimsg::kData:
    case uimsg::kView:
        if (vm && inbox.size() < 256) {
            inbox.emplace_back(sub, std::vector<std::uint8_t>(rest.begin(), rest.end()));
        }
        break;
    case uimsg::kDataBegin: {
        // (Client level 6) A big value: its sizes checked against the caps (a packed one no bigger
        // than its value and a little more), then its pieces in order.
        const std::uint32_t raw = r.u32();
        const std::uint8_t coding = r.u8();
        const std::uint32_t coded = r.u32();
        big = BigData{};
        if (!r.ok || raw == 0 || raw > uimsg::kMaxBigValue || coding > 1 || coded == 0 || coded > raw + raw / 8 + 64 ||
            (coding == 0 && coded != raw)) {
            break; // (not one we take: its pieces let go)
        }
        big.active = true;
        big.raw = raw;
        big.coding = coding;
        big.coded = coded;
        big.bytes.reserve(coded);
        break;
    }
    case uimsg::kDataChunk: {
        const std::uint32_t at = r.u32();
        if (!big.active || !r.ok || at != big.bytes.size()) {
            big = BigData{}; // (out of order or not begun: the server sends them in order)
            break;
        }
        const auto bytes = r.rest();
        if (big.bytes.size() + bytes.size() > big.coded) {
            big = BigData{};
            break;
        }
        big.bytes.insert(big.bytes.end(), bytes.begin(), bytes.end());
        if (big.bytes.size() < big.coded) {
            break;
        }
        std::vector<std::uint8_t> value;
        if (big.coding == 1) {
            try {
                value = video::nxzDecompress(big.bytes, big.raw);
            } catch (const std::exception&) {
                value.clear();
            }
        } else {
            value = std::move(big.bytes);
        }
        if (value.size() == big.raw && vm && inbox.size() < 256 && bigIn < kMaxBigIn) {
            inbox.emplace_back(uimsg::kDataBegin, std::move(value));
            ++bigIn;
        }
        big = BigData{};
        break;
    }
    case uimsg::kDrop:
        receiving = false;
        stop({});
        source.clear();
        break;
    case uimsg::kAssets:
    case uimsg::kAsset:
    case uimsg::kAssetChunk: onAssetMessage(sub, r); break;
    case uimsg::kWeather:
        // (Whatever the UI's state: a mode with no UI sends it too.)
        if (const auto w = wx::decode(rest)) {
            weatherNow(); // (a map loaded before it came: that clear first)
            told.state = w->state;
            told.fade = w->fade;
            ++told.serial;
        }
        break;
    case uimsg::kBook:
        // (Whatever the UI's state: the client's book lists only what was granted.)
        if (const std::uint8_t on = r.u8(); r.ok && client) {
            client->setModeOwnsBook(on != 0);
        }
        break;
    case uimsg::kDrawScale:
        // (Whatever the UI's state: any script mode's things.)
        if (const auto d = uimsg::decodeDrawScale(rest)) {
            tellDrawScale(*d);
        }
        break;
    case uimsg::kDecal:
        // (Client level 7) A ground decal: the same id again replaces it; past kMaxDecals let go.
        // (Whatever the UI's state: they're the server's, kept through a UI's drop and sent again
        // with it; a map's load clears them by the server's kDecalRemove 0.)
        if (const auto d = uimsg::decodeDecal(rest)) {
            if (const auto it = std::ranges::find(decals, d->id, &uimsg::Decal::id); it != decals.end()) {
                *it = *d;
            } else if (decals.size() < uimsg::kMaxDecals) {
                decals.push_back(*d);
            }
        }
        break;
    case uimsg::kDecalRemove:
        if (const std::uint16_t id = r.u16(); r.ok) {
            if (id == 0) {
                decals.clear();
            } else {
                std::erase_if(decals, [&](const uimsg::Decal& d) { return d.id == id; });
            }
        }
        break;
    default: break; // (a sub from a newer server: let go)
    }
}

ScriptUiClient::ScriptUiClient(client::HeadlessClient& client, LuauVm::Limits limits)
    : impl_(std::make_unique<Impl>(&client, limits))
{
    // (The handler outlives us on the client: it calls in only while we're here.)
    client.setNoxcppHandler(uimsg::kOp, [impl = impl_.get(), alive = std::weak_ptr<int>(impl_->alive)](std::span<const std::uint8_t> m) {
        if (alive.expired()) {
            return;
        }
        try {
            impl->onMessage(m); // (a UI's start runs from here)
        } catch (const std::exception& e) {
            impl->stop(std::string("the UI failed: ") + e.what());
        }
    });
}

ScriptUiClient::ScriptUiClient(LuauVm::Limits limits) : impl_(std::make_unique<Impl>(nullptr, limits)) {}

ScriptUiClient::~ScriptUiClient() = default;

LuauVm::Limits ScriptUiClient::defaultLimits()
{
    LuauVm::Limits l;
    l.memoryBytes = 16u << 20;
    l.safepointsPerCall = 1'000'000;
    l.timePerCall = std::chrono::milliseconds(20);
    return l;
}

void ScriptUiClient::setTextWidth(std::function<int(std::string_view)> measure)
{
    impl_->textWidth = std::move(measure);
}

void ScriptUiClient::frame(std::uint64_t nowMs)
{
    Impl& impl = *impl_;
    impl.nowMs = nowMs;
    impl.pruneDrawScales(); // (a UI running or not: the server's draw scales are any script mode's)
    if (!impl.vm) {
        return;
    }
    // A frame's share of the UI: 32 of what the server sent at most, and no new call once ~30 ms
    // have gone (the rest next frame; a window not drawn keeps last frame's draws).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    const auto inTime = [&] { return std::chrono::steady_clock::now() < deadline; };
    // (A tooltip lasts the frame that drew it; through a frame whose time ran out before every
    // window was drawn, the last one's is kept, as those windows keep their draws.)
    impl.tooltipSet = false;
    impl.pruneLabels();
    try {
        // (API 4) The UI's beat, first: once a frame, the HUD shown or not; outside draw, so its
        // windows may open, move and close. Its call has the budget any hook has.
        if (impl.hasTick && inTime() && impl.begin("tick")) {
            lua_pushnumber(impl.vm->thread(), static_cast<double>(nowMs));
            impl.finish("tick", 1, 0);
        }
        for (int n = 0; impl.vm && !impl.inbox.empty() && n < 32 && inTime(); ++n) {
            auto [sub, bytes] = std::move(impl.inbox.front());
            impl.inbox.pop_front();
            if (sub == uimsg::kDataBegin) {
                --impl.bigIn; // (a big value: its own limits)
            }
            const char* hook = sub == uimsg::kView ? "onView" : "onMessage";
            if (!(sub == uimsg::kView ? impl.hasView : impl.hasMessage) || !impl.begin(hook)) {
                continue;
            }
            const CodecLimits limits = sub == uimsg::kDataBegin
                                           ? CodecLimits{.depth = uimsg::kBigValueDepth, .pairs = uimsg::kBigValuePairs, .bytes = uimsg::kMaxBigValue}
                                           : CodecLimits{};
            if (!decodeValue(impl.vm->thread(), bytes, limits)) {
                lua_pop(impl.vm->thread(), 1);
                continue; // (not a value: let go)
            }
            impl.finish(hook, 1, 0);
        }
        std::size_t i = 0;
        for (; impl.vm && i < impl.windows.size() && inTime(); ++i) {
            UiWindow& w = impl.windows[i];
            if (!impl.hasDraw || (w.hud && !impl.hudShown) || !impl.begin("draw")) {
                w.draws.clear();
                w.hitAreas.clear();
                continue;
            }
            w.draws.clear();
            w.hitAreas.clear();
            lua_pushlstring(impl.vm->thread(), w.id.data(), w.id.size());
            impl.drawing = &w; // (windows open and shut only outside draw: it stays put)
            impl.finish("draw", 1, 0);
            impl.drawing = nullptr;
        }
        if (!impl.tooltipSet && (!impl.vm || i >= impl.windows.size())) {
            impl.tooltip.clear();
        }
    } catch (const std::exception& e) {
        // (A Lua error outside a protected call: the host's own pushes, out of memory.)
        impl.drawing = nullptr;
        impl.stop(std::string("the UI failed: ") + e.what());
    }
}

const std::vector<UiWindow>& ScriptUiClient::windows() const noexcept
{
    return impl_->windows;
}

bool ScriptUiClient::click(std::string_view windowId, float x, float y, int button)
{
    Impl& impl = *impl_;
    try {
        if (!impl.vm || !impl.hasClick || (impl.runningApi < 2 && button != 1) || !impl.begin("click")) {
            return false;
        }
        lua_State* L = impl.vm->thread();
        lua_pushlstring(L, windowId.data(), windowId.size());
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        int nargs = 3;
        if (impl.runningApi >= 2) {
            lua_pushinteger(L, button == 2 ? 2 : 1);
            nargs = 4;
        }
        if (!impl.finish("click", nargs, 1)) {
            return false;
        }
        const bool taken = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return taken;
    } catch (const std::exception& e) {
        impl.stop(std::string("the UI failed: ") + e.what());
        return false;
    }
}

bool ScriptUiClient::key(std::string_view windowId, std::string_view keyName)
{
    Impl& impl = *impl_;
    if (!impl.vm || !impl.hasKey || std::ranges::find(kKeys, keyName) == kKeys.end()) {
        return false;
    }
    try {
        if (!impl.begin("key")) {
            return false;
        }
        lua_State* L = impl.vm->thread();
        lua_pushlstring(L, windowId.data(), windowId.size());
        lua_pushlstring(L, keyName.data(), keyName.size());
        if (!impl.finish("key", 2, 1)) {
            return false;
        }
        const bool taken = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return taken;
    } catch (const std::exception& e) {
        impl.stop(std::string("the UI failed: ") + e.what());
        return false;
    }
}

void ScriptUiClient::setMouse(std::optional<std::pair<float, float>> at)
{
    impl_->mouse = at;
}

void ScriptUiClient::setPlayer(std::optional<UiPlayer> player)
{
    impl_->player = std::move(player);
}

void ScriptUiClient::setThings(const things::ThingDb* things)
{
    impl_->thingsOverride = things;
}

std::vector<UiSound> ScriptUiClient::takeSounds()
{
    return std::exchange(impl_->sounds, {});
}

namespace {

const char* dragPhase(UiDrag phase)
{
    switch (phase) {
    case UiDrag::Start: return "start";
    case UiDrag::Move: return "move";
    case UiDrag::End: return "end";
    }
    return "move";
}

/// A hook's window, place and button pushed (the click hook's first arguments).
void pushAt(lua_State* L, std::string_view windowId, float x, float y)
{
    lua_pushlstring(L, windowId.data(), windowId.size());
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
}

} // namespace

bool ScriptUiClient::drag(std::string_view windowId, UiDrag phase, float x, float y, int button)
{
    Impl& impl = *impl_;
    if (!impl.hasDrag) {
        return false;
    }
    return callTaking(impl, "drag", 5, [&](lua_State* L) {
        lua_pushlstring(L, windowId.data(), windowId.size());
        lua_pushstring(L, dragPhase(phase));
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        lua_pushinteger(L, button == 2 ? 2 : 1);
    });
}

bool ScriptUiClient::mouseUp(std::string_view windowId, float x, float y, int button)
{
    Impl& impl = *impl_;
    if (!impl.hasMouseUp) {
        return false;
    }
    return callTaking(impl, "mouseUp", 4, [&](lua_State* L) {
        pushAt(L, windowId, x, y);
        lua_pushinteger(L, button == 2 ? 2 : 1);
    });
}

bool ScriptUiClient::wheel(std::string_view windowId, float x, float y, int steps)
{
    Impl& impl = *impl_;
    steps = std::clamp(steps, -10, 10);
    if (!impl.hasWheel || steps == 0) {
        return false;
    }
    return callTaking(impl, "wheel", 4, [&](lua_State* L) {
        pushAt(L, windowId, x, y);
        lua_pushinteger(L, steps);
    });
}

void ScriptUiClient::labelClick(std::uint16_t object)
{
    Impl& impl = *impl_;
    if (!impl.hasLabelClick) {
        return;
    }
    callTaking(impl, "labelClick", 1, [&](lua_State* L) { lua_pushinteger(L, object); });
}

const UiWindow* ScriptUiClient::windowAt(float x, float y) const
{
    return windowUnder(*impl_, x, y);
}

void ScriptUiClient::setHudShown(bool shown)
{
    impl_->hudShown = shown;
}

bool ScriptUiClient::hudShown() const noexcept
{
    return impl_->hudShown;
}

bool ScriptUiClient::overMap() const noexcept
{
    return impl_->overMap && impl_->vm;
}

const std::vector<uimsg::Decal>& ScriptUiClient::decals() const noexcept
{
    return impl_->decals;
}

std::optional<UiRect> ScriptUiClient::hudReserve() const
{
    const Impl& impl = *impl_;
    if (!impl.reserve || !impl.vm) {
        return std::nullopt;
    }
    UiRect r = impl.reserve->box;
    r.x += static_cast<float>(impl.reserve->anchorX * impl.marginX);
    r.y += static_cast<float>(impl.reserve->anchorY * impl.marginY);
    return r;
}

void ScriptUiClient::setViewMargins(float x, float y)
{
    // (Whole units: a window's place is; at most half a unit off at the view's edge.)
    const auto whole = [](float v) { return v > 0 && v < 100000 ? static_cast<int>(std::lround(v)) : 0; };
    const int mx = whole(x), my = whole(y);
    if (mx == impl_->marginX && my == impl_->marginY) {
        return;
    }
    impl_->marginX = mx;
    impl_->marginY = my;
    for (UiWindow& w : impl_->windows) {
        impl_->anchor(w);
    }
}

const std::vector<UiTooltipLine>& ScriptUiClient::tooltip() const noexcept
{
    return impl_->tooltip;
}

const std::vector<UiLabel>& ScriptUiClient::labels() const noexcept
{
    return impl_->labels;
}

void ScriptUiClient::setObjectLookup(std::function<std::optional<std::uint16_t>(std::uint16_t)> lookup)
{
    impl_->objectLookup = std::move(lookup);
}

void ScriptUiClient::setObjectAt(std::function<std::optional<UiObjectAt>(std::uint16_t)> lookup)
{
    impl_->objectAt = std::move(lookup);
}

const std::vector<UiDrawScale>& ScriptUiClient::drawScales() const noexcept
{
    return impl_->drawScales;
}

std::optional<float> ScriptUiClient::burstScaleAt(float x, float y, std::optional<int> size)
{
    Impl& impl = *impl_;
    // The nearest scaled missile's last place (at most kUiMaxDrawScales of them; none: nothing
    // looked at). A spark burst only from a missile whose own burst is its size, and that missile
    // lends its scale once (it bursts once, in the world still or gone); a lesser explosion (a
    // magic missile's, which bursts off walls too) from any, a gone one once.
    auto best = impl.drawScales.end();
    float bestDist = kUiBurstReach;
    for (auto it = impl.drawScales.begin(); it != impl.drawScales.end(); ++it) {
        if (!it->seen || !it->missile || (it->gone && impl.nowMs > it->goneMs + kUiBurstGraceMs) || (size && it->burstSize != *size)) {
            continue;
        }
        const float d = std::hypot(it->x - x, it->y - y);
        if (d <= bestDist) {
            bestDist = d;
            best = it;
        }
    }
    if (best == impl.drawScales.end()) {
        return std::nullopt;
    }
    const float scale = best->scale;
    if (best->gone || size) {
        impl.drawScales.erase(best);
    }
    return scale;
}

bool windowTakes(const UiWindow& w, float x, float y)
{
    const auto inside = [&](float left, float top, float width, float height) {
        return x >= left && x < left + width && y >= top && y < top + height;
    };
    const auto wx = static_cast<float>(w.x), wy = static_cast<float>(w.y);
    if (!inside(wx, wy, static_cast<float>(w.w), static_cast<float>(w.h))) {
        return false;
    }
    if (w.frame || w.hitAreas.empty()) {
        return true; // (as API 3: its whole rectangle)
    }
    return std::ranges::any_of(w.hitAreas, [&](const UiRect& r) { return inside(wx + r.x, wy + r.y, r.w, r.h); });
}

std::pair<float, float> placeTooltip(float mouseX, float mouseY, float w, float h, float marginX, float marginY)
{
    if (!(mouseX == mouseX) || !(mouseY == mouseY)) {
        mouseX = mouseY = 0; // (NaN)
    }
    if (!(w > 0)) {
        w = 0;
    }
    if (!(h > 0)) {
        h = 0;
    }
    if (!(marginX > 0)) {
        marginX = 0;
    }
    if (!(marginY > 0)) {
        marginY = 0;
    }
    const float left = -marginX, top = -marginY, right = 640 + marginX, bottom = 480 + marginY;
    float x = mouseX + 12; // (clear of the pointer's arrow)
    float y = mouseY + 16;
    if (x + w > right) {
        x = mouseX - 4 - w;
    }
    if (y + h > bottom) {
        y = mouseY - 4 - h;
    }
    return {std::clamp(x, left, std::max(left, right - w)), std::clamp(y, top, std::max(top, bottom - h))};
}

bool ScriptUiClient::running() const noexcept
{
    return impl_->vm != nullptr;
}

const std::string& ScriptUiClient::modeName() const noexcept
{
    return impl_->modeName;
}

const std::string& ScriptUiClient::error() const noexcept
{
    return impl_->error;
}

int ScriptUiClient::api() const noexcept
{
    return impl_->runningApi;
}

std::uint32_t ScriptUiClient::runs() const noexcept
{
    return impl_->runs;
}

const std::shared_ptr<const ModAssets>& ScriptUiClient::assets() const noexcept
{
    return impl_->assets;
}

void ScriptUiClient::useAssets(std::shared_ptr<const ModAssets> assets)
{
    impl_->incoming = Impl::Incoming{};
    impl_->assetsError.clear();
    impl_->assets = std::move(assets);
}

bool ScriptUiClient::runLocal(std::string modeId, std::string modeName, int uiApi, std::string source)
{
    Impl& impl = *impl_;
    impl.stop({});
    impl.receiving = false;
    impl.modeId = std::move(modeId);
    impl.modeName = std::move(modeName);
    if (uiApi < 1 || uiApi > kUiApi || source.size() > uimsg::kMaxSource) {
        impl.error = uiApi < 1 || uiApi > kUiApi ? "the UI is for another API" : "the UI is over the 256 KB cap";
        return false;
    }
    impl.uiApi = uiApi;
    impl.source = std::move(source);
    impl.expected = static_cast<std::uint32_t>(impl.source.size());
    try {
        impl.start();
    } catch (const std::exception& e) {
        impl.stop(std::string("the UI failed: ") + e.what());
    }
    return impl.vm != nullptr;
}

void ScriptUiClient::receive(std::span<const std::uint8_t> message)
{
    try {
        impl_->onMessage(message);
    } catch (const std::exception& e) {
        impl_->stop(std::string("the UI failed: ") + e.what());
    }
}

const wx::Told& ScriptUiClient::weather()
{
    return impl_->weatherNow();
}

void ScriptUiClient::setSender(std::function<void(std::vector<std::uint8_t>)> send)
{
    impl_->sender = std::move(send);
}

} // namespace nox::modes::script
