// A NoX Core client's script mode UI (script_ui.hpp).
#include "noxcore/modes/script/script_ui.hpp"

#include "noxcore/client/headless_client.hpp"
#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/modes/script/ui_proto.hpp"
#include "noxcore/modes/script/weather.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/video/nxz.hpp"

#include "value_codec.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <map>
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
    std::vector<UiWindow> windows;
    UiWindow* drawing = nullptr; ///< (the window whose draw is running: draws go to it)
    std::deque<std::pair<std::uint8_t, std::vector<std::uint8_t>>> inbox; ///< data and views, oldest first
    std::uint64_t nowMs = 0;
    std::string error;
    // API 2's.
    std::optional<std::pair<float, float>> mouse;
    std::optional<UiPlayer> player;
    const things::ThingDb* thingsOverride = nullptr;
    std::vector<UiSound> sounds;
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
        inbox.clear();
        sounds.clear();
        drawing = nullptr;
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
    // (On the screen whole, and no smaller than its frame's title needs.)
    w.w = std::clamp(field("w", 160), 32, 600);
    w.h = std::clamp(field("h", 100), 16, 440);
    w.x = std::clamp(field("x", 20), 4, 636 - w.w);
    w.y = std::clamp(field("y", 40), 20, 476 - w.h);
    const auto it = std::ranges::find(impl.windows, w.id, &UiWindow::id);
    if (it != impl.windows.end()) {
        it->title = w.title;
        it->x = w.x;
        it->y = w.y;
        it->w = w.w;
        it->h = w.h;
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
    w.draws.push_back(std::move(d));
    return 0;
}

int uiTextWidth(lua_State* L)
{
    const Impl& impl = implOf(L);
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    const std::string_view text(s, std::min(n, kMaxText));
    lua_pushinteger(L, impl.textWidth ? impl.textWidth(text) : static_cast<int>(text.size()) * 6);
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

int uiMouse(lua_State* L)
{
    const Impl& impl = implOf(L);
    if (!impl.mouse) {
        return 0;
    }
    const auto [x, y] = *impl.mouse;
    // (The top window under it: the last one opened.)
    for (auto it = impl.windows.rbegin(); it != impl.windows.rend(); ++it) {
        const UiWindow& w = *it;
        if (x >= static_cast<float>(w.x) && x < static_cast<float>(w.x + w.w) && y >= static_cast<float>(w.y) &&
            y < static_cast<float>(w.y + w.h)) {
            lua_pushlstring(L, w.id.data(), w.id.size());
            lua_pushnumber(L, x - static_cast<float>(w.x));
            lua_pushnumber(L, y - static_cast<float>(w.y));
            return 3;
        }
    }
    return 0;
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
    lua_createtable(L, 0, static_cast<int>(std::size(kToolbox) + std::size(kToolbox2)) + 1);
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
        if (name.size() > 32) {
            name.resize(32);
            while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) {
                name.pop_back();
            }
            if (!name.empty() && (static_cast<unsigned char>(name.back()) & 0x80) != 0) {
                name.pop_back(); // (a lead byte whose letter was cut)
            }
        }
        modeId = std::move(id).substr(0, 32);
        modeName = std::move(name);
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
    if (!impl.vm) {
        return;
    }
    // A frame's share of the UI: 32 of what the server sent at most, and no new call once ~30 ms
    // have gone (the rest next frame; a window not drawn keeps last frame's draws).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    const auto inTime = [&] { return std::chrono::steady_clock::now() < deadline; };
    try {
        for (int n = 0; impl.vm && !impl.inbox.empty() && n < 32 && inTime(); ++n) {
            auto [sub, bytes] = std::move(impl.inbox.front());
            impl.inbox.pop_front();
            const char* hook = sub == uimsg::kView ? "onView" : "onMessage";
            if (!(sub == uimsg::kView ? impl.hasView : impl.hasMessage) || !impl.begin(hook)) {
                continue;
            }
            if (!decodeValue(impl.vm->thread(), bytes)) {
                lua_pop(impl.vm->thread(), 1);
                continue; // (not a value: let go)
            }
            impl.finish(hook, 1, 0);
        }
        for (std::size_t i = 0; impl.vm && i < impl.windows.size() && inTime(); ++i) {
            UiWindow& w = impl.windows[i];
            if (!impl.hasDraw || !impl.begin("draw")) {
                w.draws.clear();
                continue;
            }
            w.draws.clear();
            lua_pushlstring(impl.vm->thread(), w.id.data(), w.id.size());
            impl.drawing = &w; // (windows open and shut only outside draw: it stays put)
            impl.finish("draw", 1, 0);
            impl.drawing = nullptr;
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
