// A script mode's server side (scripted_mode.hpp): the VM, the script's hooks, and the toolbox.
#include "noxcore/modes/script/scripted_mode.hpp"

#include "noxcore/lobby/extended.hpp"
#include "noxcore/modes/script/ui_proto.hpp"
#include "noxcore/net/messages.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/rewards.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/video/nxz.hpp"

#include "foes.hpp"
#include "mod_items.hpp"
#include "mod_parts.hpp"
#include "mode_shared.hpp"
#include "toolbox.hpp"
#include "value_codec.hpp"

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nox::modes::script {

namespace fs = std::filesystem;

// ------------------------------------------------------------------------- the package's files

namespace {

bool insideFolder(std::string_view rel)
{
    return !rel.empty() && rel.find("..") == std::string_view::npos && rel.front() != '/' && rel.find(':') == std::string_view::npos &&
           rel.find('\\') == std::string_view::npos;
}

} // namespace

std::optional<std::string> ModPackage::read(std::string_view relPath) const
{
    if (!insideFolder(relPath)) {
        return std::nullopt;
    }
    if (source) {
        return source(relPath);
    }
    const fs::path p = folder / fs::path(std::string(relPath));
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec || size > (1u << 20)) {
        return std::nullopt;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::string s(static_cast<std::size_t>(size), '\0');
    f.read(s.data(), static_cast<std::streamsize>(size));
    s.resize(static_cast<std::size_t>(f.gcount()));
    if (s.starts_with("\xEF\xBB\xBF")) {
        s.erase(0, 3); // (a UTF-8 mark, as Notepad and PowerShell write one: Luau refuses it)
    }
    return s;
}

fs::file_time_type ModPackage::newest() const
{
    if (source) {
        return {}; // (built in: what it was when loaded)
    }
    // (From the clock's least, not its epoch: libstdc++'s file clock counts from 2174, so a real
    // file's time is below file_time_type{} there and a max from it would never move.)
    fs::file_time_type t = fs::file_time_type::min();
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 4) {
            it.disable_recursion_pending();
            continue;
        }
        const fs::path& p = it->path();
        if (p.extension() == ".luau" || p.filename() == "mode.json") {
            t = std::max(t, it->last_write_time(ec));
        }
    }
    if (manifest.uiApi >= 2) {
        t = std::max(t, newestAsset(folder / "assets")); // (its UI's pictures and sounds)
    }
    return t;
}

// ------------------------------------------------------------------------- the mode's state

struct ScriptedMode::Impl {
    struct Hooks {
        bool configure = false, start = false, onJoin = false, onLeave = false, onChat = false, onChoice = false, tick = false,
             console = false, save = false, restore = false, stop = false, onUiMessage = false, onKill = false, respawnPoint = false,
             collide = false, mapLoaded = false, playerReady = false;
    };
    /// The engine's hooks inside the frame (mode_hooks.hpp), each the script's when it has it.
    class EngineHooks;
    /// A NoX Core player's copy of the UI (one whose query named ClientFeature::ModeUi).
    struct UiPeer {
        std::size_t sent = 0; ///< the source's bytes sent so far
        bool begun = false;   ///< kBegin sent
        bool ready = false;   ///< its kReady came: it runs
        bool views = false;   ///< it draws the views itself (data, not text)
        std::uint32_t sendsFrom = 0; ///< (the frame its count of sends began)
        int sends = 0;               ///< its kSends since: 20 a second at most reach the script
        bool failedLogged = false;   ///< (its UI's failure logged: once a game)
        // (A UI of API 2's) The mod's assets on their way to it, before the UI's kBegin.
        bool assetsBegun = false;    ///< kAssets sent
        bool assetsDone = false;     ///< every asset's chunks sent
        std::size_t asset = 0;       ///< the one on its way
        bool assetHeaderSent = false; ///< (its kAsset)
        std::size_t assetSent = 0;   ///< (its coded bytes sent so far)
        /// The set it has (the hash of the last one sent it whole): kept through a reload, so a
        /// set that didn't change isn't sent again.
        std::optional<std::uint64_t> hasSet;
        /// Views held while it had no reliable room (GameHost::reliableRoom), each its bytes and
        /// its text: pumpUi sends them first as room comes; past kHeldViews the oldest goes as
        /// text. (nox.send says false while any is held: a value never overtakes a view.)
        std::deque<std::pair<std::vector<std::uint8_t>, std::string>> heldViews;
    };
    static constexpr std::size_t kHeldViews = 16;
    /// A mod asset as the wire carries it (ui_proto.hpp's kAsset and kAssetChunk): its header's
    /// payload and its bytes, NXZ-packed when that's smaller (each peer's copy is this one).
    struct WireAsset {
        std::vector<std::uint8_t> header;
        std::vector<std::uint8_t> coded;
    };
    /// Assets' chunks a frame to a player at most (the UI's are 4), each while the player has bulk
    /// room for it (GameHost::reliableBulkRoom, as pumpUi): the game's own messages, and the mode's
    /// values, views and weather, keep their place.
    static constexpr int kAssetChunksPerFrame = 16;
    // (A player with no reliable room, GameHost::reliableRoom: no more data for its UI (nox.send
    // says false, views go as text) till its acknowledgements catch up.)

    std::shared_ptr<const ModPackage> package;
    ServerContext ctx;
    LuauVm::Limits limits;
    std::unique_ptr<LuauVm> vm;
    int modeRef = 0;     ///< the script's mode table (0: none yet)
    int requiredRef = 0; ///< require's cache (by name)
    Hooks has;
    server::GameHost* host = nullptr;
    std::uint32_t lastFrame = ~0u;
    int sinceCheck = 0;
    fs::file_time_type loadedAt{};
    std::minstd_rand rng;
    std::map<std::uint8_t, int> choices; ///< a player's open choice (by peer): how many answers
    std::string uiSource;                ///< the UI script (empty: none)
    std::map<std::uint8_t, UiPeer> ui;   ///< (by peer)
    /// The mod's assets (a UI of API 2's: mod_assets.hpp) while it's hosted: their contents' hash
    /// (none: no set), their newest write time when read, and as the wire carries them (kAssets'
    /// payload, then each asset's); decoded only to be packed, gone when the game goes (detach).
    std::optional<std::uint64_t> assetsHash;
    fs::file_time_type assetsAt{};
    std::vector<std::uint8_t> wireSet;
    std::vector<WireAsset> wireAssets;
    std::vector<std::string> errors;
    std::map<std::string, std::chrono::steady_clock::time_point> lastLogged;
    std::unique_ptr<server::ModeHooks> engineHooks; ///< (installed on the server while attached)
    bool installEngineHooks = true;                  ///< (false: an owning native mode forwards)
    std::optional<bool> f32Chop;                     ///< (nox.f32's rounding set by the owner; none: the host's)
    /// The parts beyond the first toolbox (mod_parts.hpp: timers, events, the store, generation).
    Parts parts;
    /// The owner's functions (the script's `owner` table), by name.
    std::map<std::string, std::function<int(lua_State*)>, std::less<>> ownerFns;
    /// Rules looked up once (ScriptedMode::rule): their functions' refs, again at each load.
    struct RuleRef {
        std::string name;
        int ref = 0; ///< (0: the script has none)
    };
    std::vector<RuleRef> rules;
    /// Revokes owed (nox.revoke: MSG_GAUNTLET 0x11 / 0x12 / 0x13, by player name), sent
    /// kRevokesPerFrame a frame by nox.sendRevokes (a client keeps 64 book events between its
    /// frames).
    static constexpr std::size_t kRevokesPerFrame = 30;
    std::map<std::string, std::vector<std::array<std::uint8_t, 4>>> revokes;
    std::uint32_t revokedAt = ~0u;
    /// nox.lock's: the host's own Closed and allowed list, put back by nox.unlock.
    bool locked = false;
    bool savedClosed = false;
    std::vector<std::string> savedAllowed;
    /// A frame's text messages (tell, textToAll): at most kTextPerFrame (chargeText).
    static constexpr int kTextPerFrame = 256;
    std::uint32_t textFrame = ~0u;
    int textSent = 0;
    bool chargeText(int messages);

    /// A rule's ref (0: none) from the mode table.
    int resolve(const std::string& name)
    {
        lua_State* L = vm->thread();
        lua_getref(L, modeRef);
        lua_rawgetfield(L, -1, name.c_str());
        const int ref = lua_isfunction(L, -1) ? lua_ref(L, -1) : 0;
        lua_pop(L, 2);
        return ref;
    }
    /// The function on top of the stack called: `pushArgs` its arguments, `readResults` its
    /// results; the stack as it was after.
    bool callTop(const char* name, int nresults, const std::function<int(lua_State*)>& pushArgs,
                 const std::function<void(lua_State*)>& readResults)
    {
        lua_State* L = vm->thread();
        const int nargs = pushArgs ? pushArgs(L) : 0;
        if (!finish(name, nargs, nresults)) {
            return false;
        }
        const int top = lua_gettop(L) - nresults;
        if (readResults) {
            readResults(L);
        }
        lua_settop(L, top); // (whatever the reader left)
        return true;
    }
    void log(std::string_view line) const
    {
        if (ctx.log) {
            ctx.log(line);
        }
    }

    void report(std::string e)
    {
        const auto now = std::chrono::steady_clock::now();
        if (lastLogged.size() > 64) {
            lastLogged.clear(); // (many different lines: start over rather than grow)
        }
        auto& at = lastLogged[e];
        if (at == std::chrono::steady_clock::time_point{} || now - at > std::chrono::seconds(5)) {
            at = now;
            log("script error: " + e);
        }
        if (errors.size() >= 50) {
            errors.erase(errors.begin());
        }
        errors.push_back(std::move(e));
    }

    /// A host entry point's work, a Lua error outside a protected call (the host's own pushes and
    /// reads, out of memory) caught: reported, the stack reset, the default result.
    template <class F>
    auto guarded(const char* what, F&& f)
    {
        using R = std::invoke_result_t<F>;
        // (The stack as it was: a hook called while a script's call is running, set off by a
        // toolbox function, must leave that call's own values alone.)
        lua_State* const thread = vm ? vm->thread() : nullptr;
        const int top = thread ? lua_gettop(thread) : 0;
        try {
            return f();
        } catch (const std::exception& e) {
            std::string why = e.what(); // (before the stack goes: Luau's reads its message off it)
            if (vm && vm->thread() == thread) {
                lua_settop(thread, top);
            } else if (vm) {
                lua_settop(vm->thread(), 0); // (a new thread since: a reload's)
            }
            report(std::string(what) + ": " + why);
            if constexpr (!std::is_void_v<R>) {
                return R{};
            }
        }
    }

    // ---- calling the script's hooks: begin pushes the hook (false: it has none), the caller
    // pushes the arguments, finish calls it.
    bool begin(const char* name)
    {
        lua_State* L = vm->thread();
        lua_getref(L, modeRef);
        lua_rawgetfield(L, -1, name);
        lua_remove(L, -2);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        return true;
    }
    bool finish(const char* name, int nargs, int nresults)
    {
        std::string error;
        if (!vm->call(nargs, nresults, error)) {
            report(std::string(name) + ": " + error);
            return false;
        }
        return true;
    }

    /// Whether the script sees `peer`: a player in the game, not a stock one held out to be put
    /// out (GameHost::Options::stockRefusal: the mode never hears of those).
    bool sees(std::uint8_t peer) const { return host && host->players().contains(peer) && !host->refused(peer); }
    /// Whether `o` is such a held-out player's unit (never listed, counted or told anything).
    bool hidden(const server::ServerObject& o) const { return host && o.player && host->anyRefused() && host->refusedUnit(o.extent); }

    void pushPlayer(lua_State* L, std::uint8_t peer) const
    {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, peer);
        lua_setfield(L, -2, "peer");
        if (host) {
            if (const auto it = host->players().find(peer); it != host->players().end()) {
                lua_pushlstring(L, it->second.name.data(), it->second.name.size());
                lua_setfield(L, -2, "name");
                lua_pushnumber(L, it->second.extent); // (its unit: nox.object's id)
                lua_setfield(L, -2, "unit");
            }
            lua_pushboolean(L, host->identified(peer));
            lua_setfield(L, -2, "identified");
            if (const auto it = host->players().find(peer); it != host->players().end() && sees(peer)) {
                addPlayerFacts(L, it->second, host->server().world().byExtent(it->second.extent)); // (account, class, level...)
            }
        }
    }

    bool load(std::string& error);
    void installToolbox();
    std::optional<std::string> option(std::string_view key) const;
    /// A text line (MSG_TEXT_MESSAGE lines) to one player.
    void tell(std::uint8_t peer, std::string_view text);
    /// A view (the table at `viewIdx`) to `peer` (none: everyone): as data to a player whose UI
    /// draws views, as `text` to any other.
    void deliver(lua_State* L, int viewIdx, std::optional<std::uint8_t> peer, std::string_view text);
    void onJoin(std::uint8_t peer);
    /// playerReady(unit): its client has the map loaded and is in play (its join's, or a map
    /// change's, CLIENT_READY).
    void playerReady(std::uint8_t peer);
    void onLeave(std::uint8_t peer);
    bool onChat(std::uint8_t peer, std::u16string_view text);
    void answer(std::uint8_t peer, int index);
    // ---- the UI (ui_proto.hpp)
    void clientReady(std::uint8_t peer);
    void onUiMessage(std::uint8_t peer, std::span<const std::uint8_t> m);
    /// The UI's source to each player due it, a few blocks a frame.
    void pumpUi();
    /// Every player's UI dropped and sent again (a reload's new UI).
    void resendUi();
    /// The mod's assets to one player due them, a few chunks a frame: true once they're all
    /// queued (or it has the set already).
    bool pumpAssets(std::uint8_t peer, UiPeer& u);
    /// The assets read for a load (none: the UI isn't API 2's, or a built-in mode's), their wire
    /// form made when they changed; kept by the load once its scripts run.
    struct LoadedAssets {
        std::uint64_t hash = 0;
        fs::file_time_type at{};
        bool changed = false; ///< (else only `at` moved)
        std::vector<std::uint8_t> wireSet;
        std::vector<WireAsset> wireAssets;
    };
    /// False: `error` why (over a cap...). None: as they are.
    bool loadAssets(std::string& error, std::optional<LoadedAssets>& out);
    /// `a` the set from now: whether it changed.
    bool keepAssets(LoadedAssets&& a);
    /// The set let go (the game gone: its memory freed).
    void dropAssets()
    {
        assetsHash.reset();
        assetsAt = {};
        std::vector<std::uint8_t>().swap(wireSet);
        std::vector<WireAsset>().swap(wireAssets);
    }
    /// The client features the UI needs: ModeUi, and ModeUi2 for a UI of API 2 (a client without
    /// them gets the views as text, the UI never sent).
    std::uint32_t uiNeeds() const
    {
        return lobby::ClientFeature::ModeUi | (package->manifest.uiApi >= 2 ? lobby::ClientFeature::ModeUi2 : 0u);
    }
    /// Whether `peer` takes this UI: it named what the UI needs, and for a UI of API 3 its client
    /// is at uimsg::kUiApi3Level or above, for API 4 at kUiApi4Level (asked each time: a slot may
    /// change hands).
    bool uiReaches(std::uint8_t peer) const
    {
        const int api = package->manifest.uiApi;
        return host && (host->clientFeatures(peer) & uiNeeds()) == uiNeeds() &&
               (api < 3 || host->clientLevel(peer) >= (api >= 4 ? uimsg::kUiApi4Level : uimsg::kUiApi3Level));
    }
    void sendUi(std::uint8_t peer, uimsg::Sub sub, std::span<const std::uint8_t> payload = {})
    {
        // (Only ever to a client whose query named what the UI needs: a peer slot taken since by
        // another client never gets one.)
        if (uiReaches(peer)) {
            host->sendToIdentified(peer, uimsg::message(sub, payload));
        }
    }
};

namespace {

using Impl = ScriptedMode::Impl;

Impl& implOf(lua_State* L)
{
    auto* impl = static_cast<Impl*>(LuauVm::owner(L));
    if (!impl) {
        luaL_error(L, "the toolbox has no mode");
    }
    return *impl;
}

/// MSG_TEXT_MESSAGE lines of `text`: its own lines, each cut at 250 characters (the message's
/// one-byte length) at a space where there's one.
std::vector<std::u16string> textLines(std::string_view utf8)
{
    std::vector<std::u16string> out;
    const std::u16string all = strings::fromUtf8(utf8);
    std::size_t start = 0;
    while (start <= all.size()) {
        std::size_t end = all.find(u'\n', start);
        if (end == std::u16string::npos) {
            end = all.size();
        }
        std::u16string_view line(all.data() + start, end - start);
        while (line.size() > 250) {
            std::size_t cut = line.rfind(u' ', 250);
            if (cut == std::u16string_view::npos || cut == 0) {
                cut = 250;
                if (line[cut - 1] >= 0xD800 && line[cut - 1] <= 0xDBFF) {
                    --cut; // (never between the halves of a surrogate pair)
                }
            }
            out.emplace_back(line.substr(0, cut));
            line.remove_prefix(cut);
            while (!line.empty() && line.front() == u' ') {
                line.remove_prefix(1);
            }
        }
        if (!line.empty()) {
            out.emplace_back(line);
        }
        start = end + 1;
    }
    return out;
}

// ---- the toolbox (nox.*)

int noxLog(lua_State* L)
{
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    implOf(L).log(std::string_view(s, n));
    return 0;
}

int noxPlayers(lua_State* L)
{
    const Impl& impl = implOf(L);
    lua_createtable(L, impl.host ? static_cast<int>(impl.host->players().size()) : 0, 0);
    if (impl.host) {
        int i = 0;
        for (const auto& [peer, p] : impl.host->players()) {
            if (impl.host->refused(peer)) {
                continue; // (a stock player held out: not the mode's)
            }
            impl.pushPlayer(L, peer);
            lua_rawseti(L, -2, ++i);
        }
    }
    return 1;
}

int noxPlayer(lua_State* L)
{
    const Impl& impl = implOf(L);
    const int peer = luaL_checkinteger(L, 1);
    if (peer < 0 || peer > 255 || !impl.sees(static_cast<std::uint8_t>(peer))) {
        lua_pushnil(L);
        return 1;
    }
    impl.pushPlayer(L, static_cast<std::uint8_t>(peer));
    return 1;
}

std::optional<std::uint8_t> peerArg(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx)) {
        return std::nullopt; // (everyone)
    }
    const int peer = luaL_checkinteger(L, idx);
    luaL_argcheck(L, peer >= 0 && peer <= 255, idx, "not a player");
    return static_cast<std::uint8_t>(peer);
}

/// A say view ({kind = "say", text}) pushed.
void pushSayView(lua_State* L, const char* s, size_t n)
{
    lua_createtable(L, 0, 2);
    lua_pushstring(L, "say");
    lua_setfield(L, -2, "kind");
    lua_pushlstring(L, s, n);
    lua_setfield(L, -2, "text");
}

int noxSay(lua_State* L)
{
    Impl& impl = implOf(L);
    const auto peer = peerArg(L, 1);
    luaL_argcheck(L, peer.has_value(), 1, "say: a player's peer (sayAll: everyone)");
    size_t n = 0;
    const char* s = luaL_checklstring(L, 2, &n);
    pushSayView(L, s, n);
    impl.deliver(L, lua_gettop(L), peer, std::string_view(s, n));
    return 0;
}

int noxSayAll(lua_State* L)
{
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    pushSayView(L, s, n);
    implOf(L).deliver(L, lua_gettop(L), std::nullopt, std::string_view(s, n));
    return 0;
}

std::string cellText(lua_State* L, int idx)
{
    size_t n = 0;
    const char* s = lua_tolstring(L, idx, &n); // (numbers too)
    return s ? std::string(s, n) : std::string();
}

int noxChoice(lua_State* L)
{
    Impl& impl = implOf(L);
    const auto peer = peerArg(L, 1);
    luaL_argcheck(L, peer.has_value(), 1, "choice: a player's peer");
    const std::string title = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    const int count = lua_objlen(L, 3);
    luaL_argcheck(L, count >= 1 && count <= 9, 3, "choice: 1-9 answers");
    // Its text form: the title, the answers numbered, what to say; its data: {kind = "choice",
    // title, options}.
    std::string text = title;
    lua_createtable(L, 0, 3);
    lua_pushstring(L, "choice");
    lua_setfield(L, -2, "kind");
    lua_pushlstring(L, title.data(), title.size());
    lua_setfield(L, -2, "title");
    lua_createtable(L, count, 0);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 3, i);
        const std::string option = cellText(L, -1);
        lua_pop(L, 1);
        text += "\n" + std::to_string(i) + ") " + option;
        lua_pushlstring(L, option.data(), option.size());
        lua_rawseti(L, -2, i);
    }
    lua_setfield(L, -2, "options");
    text += count == 1 ? "\nSay 1." : "\nSay 1-" + std::to_string(count) + ".";
    impl.choices[*peer] = count;
    impl.deliver(L, lua_gettop(L), peer, text);
    return 0;
}

int noxTable(lua_State* L)
{
    Impl& impl = implOf(L);
    const auto peer = peerArg(L, 1);
    const std::string title = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    std::string text = title;
    const int rows = std::min(lua_objlen(L, 3), 32);
    // Its data: {kind = "table", title, rows = {{cell, ...}, ...}} (cells as text).
    lua_createtable(L, 0, 3);
    lua_pushstring(L, "table");
    lua_setfield(L, -2, "kind");
    lua_pushlstring(L, title.data(), title.size());
    lua_setfield(L, -2, "title");
    lua_createtable(L, rows, 0);
    for (int r = 1; r <= rows; ++r) {
        lua_rawgeti(L, 3, r);
        std::string line;
        lua_newtable(L);
        if (lua_istable(L, -2)) {
            const int cells = std::min(lua_objlen(L, -2), 16);
            for (int c = 1; c <= cells; ++c) {
                lua_rawgeti(L, -2, c);
                const std::string cell = cellText(L, -1);
                lua_pop(L, 1);
                line += (c > 1 ? "  " : "") + cell;
                lua_pushlstring(L, cell.data(), cell.size());
                lua_rawseti(L, -2, c);
            }
        } else {
            line = cellText(L, -2);
            lua_pushlstring(L, line.data(), line.size());
            lua_rawseti(L, -2, 1);
        }
        lua_remove(L, -2); // (the script's row)
        lua_rawseti(L, -2, r);
        text += "\n" + line;
    }
    lua_setfield(L, -2, "rows");
    impl.deliver(L, lua_gettop(L), peer, text);
    return 0;
}

int noxSend(lua_State* L)
{
    // A value to a player's UI (its onMessage); whether it went (no UI there, or its link behind:
    // no reliable room, or views held for it: false).
    Impl& impl = implOf(L);
    const auto peer = peerArg(L, 1);
    luaL_argcheck(L, peer.has_value(), 1, "send: a player's peer");
    const auto u = impl.ui.find(*peer);
    if (u == impl.ui.end() || !u->second.ready || !impl.host || impl.host->reliableRoom(*peer) == 0 || !u->second.heldViews.empty()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    std::vector<std::uint8_t> bytes;
    std::string why;
    if (!encodeValue(L, 2, bytes, why)) {
        luaL_error(L, "send: %s", why.c_str());
    }
    impl.sendUi(*peer, uimsg::kData, bytes);
    lua_pushboolean(L, 1);
    return 1;
}

int noxHasUi(lua_State* L)
{
    // Whether a player's client runs the mode's UI.
    const Impl& impl = implOf(L);
    const auto peer = peerArg(L, 1);
    const auto u = peer ? impl.ui.find(*peer) : impl.ui.end();
    lua_pushboolean(L, u != impl.ui.end() && u->second.ready);
    return 1;
}

// ---- the world: things by their id (their extent), players' units among them. Engine
// primitives only (the stock game's own functions and messages): what a mode does with them is
// its script's.

server::World& worldOf(lua_State* L)
{
    Impl& impl = implOf(L);
    if (!impl.host) {
        luaL_error(L, "no game yet (the world is there from start on)");
    }
    return impl.host->server().world();
}

server::Fight fightOf(lua_State* L)
{
    worldOf(L);
    return implOf(L).host->server().fight();
}

/// The thing whose id is at `idx`; an error when there's none (gone).
server::ServerObject& objectArg(lua_State* L, int idx)
{
    const double id = luaL_checknumber(L, idx);
    server::ServerObject* o = id >= 1 && id <= 4294967295.0 ? worldOf(L).byExtent(static_cast<std::uint32_t>(id)) : nullptr;
    if (!o || o->gone()) {
        luaL_argerror(L, idx, "no such thing (gone?)");
    }
    return *o;
}

server::ServerObject& unitArg(lua_State* L, int idx)
{
    server::ServerObject& o = objectArg(L, idx);
    if (!o.player) {
        luaL_argerror(L, idx, "not a player's unit");
    }
    return o;
}

/// The map's extent: 256 wall cells of 23.
constexpr double kMapExtent = 256.0 * 23.0;

/// A coordinate (or a distance) on the map: a number from 0 to the map's extent, never NaN or
/// infinite (they would reach the engine's casts and every client).
float coordArg(lua_State* L, int idx)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= 0 && v <= kMapExtent, idx, "a point on the map (0 to 5888)");
    return static_cast<float>(v);
}

/// A unit's peer (its player's), none for a unit without one (a test's, a bot's own).
std::optional<std::uint8_t> peerOfUnit(const Impl& impl, std::uint32_t extent)
{
    if (impl.host) {
        for (const auto& [peer, p] : impl.host->players()) {
            if (p.extent == extent) {
                return impl.host->refused(peer) ? std::nullopt : std::optional<std::uint8_t>(peer);
            }
        }
    }
    return std::nullopt;
}

void setField(lua_State* L, const char* name, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, name);
}

} // namespace

// ---- what the parts in their own files use (toolbox.hpp)

namespace toolbox {

Parts& partsOf(lua_State* L)
{
    return implOf(L).parts;
}

const ServerContext& contextOf(lua_State* L)
{
    return implOf(L).ctx;
}

const Manifest& manifestOf(lua_State* L)
{
    return implOf(L).package->manifest;
}

server::GameHost* hostOf(lua_State* L)
{
    return implOf(L).host;
}

server::World& worldOf(lua_State* L)
{
    return script::worldOf(L);
}

server::Fight fightOf(lua_State* L)
{
    return script::fightOf(L);
}

const things::ThingDb& thingsOf(lua_State* L)
{
    const Impl& impl = implOf(L);
    const things::ThingDb* db = impl.host ? impl.host->server().fight().things : impl.ctx.things;
    if (!db) {
        luaL_error(L, "no thing.bin");
    }
    return *db;
}

const things::ModifierDb* modifiersOf(lua_State* L)
{
    const Impl& impl = implOf(L);
    return impl.host ? impl.host->server().modifiers() : nullptr;
}

server::ServerObject& objectArg(lua_State* L, int idx)
{
    return script::objectArg(L, idx);
}

server::ServerObject& playerUnitArg(lua_State* L, int idx)
{
    server::ServerObject& o = unitArg(L, idx);
    if (implOf(L).hidden(o)) {
        luaL_argerror(L, idx, "no such thing (gone?)"); // (a held-out player's: the script never sees it)
    }
    return o;
}

bool hidden(lua_State* L, const server::ServerObject& o)
{
    return implOf(L).hidden(o);
}

void refuseInsideAsked(lua_State* L, const char* what)
{
    if (const char* hook = implOf(L).parts.asked) {
        luaL_error(L, "%s changes the world: not inside %s (nox.after(1, ...) does it the next frame)", what, hook);
    }
}

void report(lua_State* L, std::string line)
{
    implOf(L).report(std::move(line));
}

bool beginHook(lua_State* L, const char* name)
{
    return implOf(L).begin(name);
}

bool callHook(lua_State* L, const char* what, int nargs, int nresults)
{
    return implOf(L).finish(what, nargs, nresults);
}

} // namespace toolbox

namespace {

int noxObject(lua_State* L)
{
    // {id, type, x, y, team, holder, health, maxHealth, radius, mass, direction, owner, player,
    // alive, observing, peer, netCode, lessons, deaths, colour, home = {x, y}, dropped}: what it is
    // now (nil: gone). (direction: 0-255; owner: its owner's id, 0 none.)
    Impl& impl = implOf(L);
    const double id = luaL_checknumber(L, 1);
    server::World& world = worldOf(L);
    const server::ServerObject* o = id >= 1 && id <= 4294967295.0 ? world.byExtent(static_cast<std::uint32_t>(id)) : nullptr;
    if (!o || o->gone()) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 20);
    setField(L, "id", o->extent);
    lua_pushlstring(L, o->typeName.data(), o->typeName.size());
    lua_setfield(L, -2, "type");
    setField(L, "x", o->x);
    setField(L, "y", o->y);
    setField(L, "team", o->team);
    setField(L, "radius", o->radius);
    setField(L, "mass", o->mass);
    setField(L, "direction", o->direction);
    setField(L, "owner", o->owner);
    lua_pushboolean(L, o->pendingDelete ? 1 : 0);
    lua_setfield(L, -2, "pendingDelete");
    setField(L, "holder", o->holder);
    setField(L, "health", o->health);
    setField(L, "maxHealth", o->maxHealth);
    setField(L, "netCode", server::netCodeOf(*o));
    lua_pushboolean(L, (o->flags & server::ObjFlag::Dead) == 0);
    lua_setfield(L, -2, "alive");
    lua_pushboolean(L, o->player ? 1 : 0);
    lua_setfield(L, -2, "player");
    if (o->player) {
        lua_pushboolean(L, (o->player->statusFlags & 1u) != 0);
        lua_setfield(L, -2, "observing");
        setField(L, "lessons", o->player->lessons);
        setField(L, "deaths", o->player->deaths);
        if (const auto peer = peerOfUnit(impl, o->extent)) {
            setField(L, "peer", *peer);
        }
    }
    if (o->classes & things::ObjectClass::Flag) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "flag");
        setField(L, "colour", server::flagColor(*o, impl.host->server().modifiers()));
    }
    if (o->flag) {
        lua_createtable(L, 0, 2);
        setField(L, "x", o->flag->homeX);
        setField(L, "y", o->flag->homeY);
        lua_setfield(L, -2, "home");
        setField(L, "dropped", o->flag->dropped);
    }
    return 1;
}

int noxObjects(lua_State* L)
{
    // The ids of the things of a type (its name, as thing.bin has it), in the world now: a set-up's
    // call (a whole-world walk), not a frame's.
    const std::string type = luaL_checkstring(L, 1);
    server::World& world = worldOf(L);
    const Impl& impl = implOf(L);
    lua_newtable(L);
    int n = 0;
    for (const auto& [extent, o] : world.objects()) {
        if (!o.gone() && o.typeName == type && !impl.hidden(o)) {
            lua_pushnumber(L, extent);
            lua_rawseti(L, -2, ++n);
        }
    }
    return 1;
}

int noxUnits(lua_State* L)
{
    // The players' units in the world (observing ones too), in the world's own order (the game's
    // loops over its players: who wins a tie).
    // (World::playerUnits: those a walk of the objects finds, in its order, without the walk.)
    server::World& world = worldOf(L);
    const Impl& impl = implOf(L);
    lua_newtable(L);
    int n = 0;
    for (const server::ServerObject* o : world.playerUnits()) {
        if (o->player && !o->gone() && !impl.hidden(*o)) {
            lua_pushnumber(L, o->extent);
            lua_rawseti(L, -2, ++n);
        }
    }
    return 1;
}

int noxInventory(lua_State* L)
{
    // What a unit carries: {id, type, flag} each.
    server::ServerObject& u = objectArg(L, 1);
    server::World& world = worldOf(L);
    lua_createtable(L, static_cast<int>(u.inventory.size()), 0);
    int n = 0;
    for (const std::uint32_t e : u.inventory) {
        const server::ServerObject* i = world.byExtent(e);
        if (!i) {
            continue;
        }
        lua_createtable(L, 0, 3);
        setField(L, "id", e);
        lua_pushlstring(L, i->typeName.data(), i->typeName.size());
        lua_setfield(L, -2, "type");
        lua_pushboolean(L, (i->classes & things::ObjectClass::Flag) != 0);
        lua_setfield(L, -2, "flag");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int noxWatchCollide(lua_State* L)
{
    // The thing's touches the script's from now on (its collide hook), its own COLLIDE no more.
    objectArg(L, 1).collideFn = server::Name("ModeCollide");
    return 0;
}

int noxTake(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.take"); // (not inside onDamage or onExit)
    // A thing on the ground into a unit's inventory, as a pickup puts it there (a flag shown
    // carried: its holder's weapon bit 1, its class told to everyone).
    server::ServerObject& u = unitArg(L, 1);
    server::ServerObject& item = objectArg(L, 2);
    luaL_argcheck(L, item.holder == 0, 2, "held already");
    // (Things a pickup could take: never a unit, a player's or a creature, the taker least of all.)
    luaL_argcheck(L, &item != &u && !item.player && !item.monster &&
                         !(item.classes & (things::ObjectClass::Player | things::ObjectClass::Monster)),
                  2, "not a thing a unit can carry");
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 2, "no things");
    server::World& world = f.world;
    server::Items items{world, *f.things, f.modifiers, f.rng, f.frame};
    world.removed().push_back(item.extent);
    item.flags &= ~server::ObjFlag::Active;
    item.decayAt = 0;
    items.receive(u, item);
    if (item.classes & things::ObjectClass::Flag) {
        u.player->weaponEquip |= 1u;
        items.reportEquip(u, item);
        if (item.flag) {
            item.flag->dropped = 0;
        }
    }
    return 0;
}

int noxDrop(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.drop"); // (not inside onDamage or onExit)
    // Out of a unit's inventory onto the ground at (x, y), as the game drops it (a flag's drop
    // told as the game tells it).
    server::ServerObject& u = unitArg(L, 1);
    server::ServerObject& item = objectArg(L, 2);
    luaL_argcheck(L, item.holder == u.extent, 2, "not its");
    const float x = coordArg(L, 3);
    const float y = coordArg(L, 4);
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 2, "no things");
    server::Items items{f.world, *f.things, f.modifiers, f.rng, f.frame};
    lua_pushboolean(L, items.drop(u, item, x, y));
    return 1;
}

int noxPlace(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.place"); // (not inside onDamage or onExit)
    // A thing put at (x, y): moved there on the ground, or out of its holder's inventory and into
    // the world there as a capture puts a flag home (a flag back at rest: dropped 0).
    server::ServerObject& item = objectArg(L, 1);
    const float x = coordArg(L, 2);
    const float y = coordArg(L, 3);
    server::Fight f = fightOf(L);
    server::World& world = f.world;
    if (item.holder != 0) {
        server::ServerObject* holder = world.byExtent(item.holder);
        luaL_argcheck(L, holder && f.things, 1, "its holder is gone");
        server::Items items{world, *f.things, f.modifiers, f.rng, f.frame};
        items.removeFromInventory(*holder, item);
        server::placeAt(world, item, x, y, f.frame);
    } else {
        server::setPosition(world, item, x, y, f.frame);
    }
    if (item.flag) {
        item.flag->dropped = 0;
    }
    return 0;
}

int noxClearPointNear(lua_State* L)
{
    // A clear spot within r of (x, y), as the game finds one.
    server::Fight f = fightOf(L);
    const auto [x, y] = server::clearPointNear(f.world, f.rng, coordArg(L, 1), coordArg(L, 2), coordArg(L, 3));
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

int noxEndSpellEffects(lua_State* L)
{
    // The unit's enchantments from spells whose flags have any of `mask` (thing.bin's spell flags)
    // ended.
    server::ServerObject& u = objectArg(L, 1);
    const auto mask = static_cast<std::uint32_t>(luaL_checknumber(L, 2));
    server::Fight f = fightOf(L);
    for (int e = 0; e < 32; ++e) {
        if (!server::enchanted(u, e)) {
            continue;
        }
        const std::uint32_t spell = server::enchantmentSpell(e);
        const things::SpellDef* def = spell && f.things ? f.things->findSpell(spell) : nullptr;
        if (def && (def->flags & mask)) {
            server::disenchant(f.world, u, e);
        }
    }
    return 0;
}

int noxMinimap(lua_State* L)
{
    // The thing marked on (or off) every player's minimap.
    server::ServerObject& o = objectArg(L, 1);
    if (lua_toboolean(L, 2)) {
        server::markMinimapForAll(worldOf(L), o, 1);
    } else {
        server::unmarkMinimapForAll(worldOf(L), o, 1);
    }
    return 0;
}

int noxSound(lua_State* L)
{
    // A sound at the thing (its number, or its name).
    server::ServerObject& o = objectArg(L, 1);
    if (lua_isnumber(L, 2)) {
        server::playSound(worldOf(L), static_cast<std::uint32_t>(lua_tonumber(L, 2)), o);
    } else {
        server::playSound(worldOf(L), std::string_view(luaL_checkstring(L, 2)), o);
    }
    return 0;
}

/// One of the owner's functions (its std::function the closure's upvalue).
int ownerCall(lua_State* L)
{
    auto* fn = static_cast<std::function<int(lua_State*)>*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
    return (*fn)(L);
}

// ---- the engine's paths: health, spells, enchantments, damage, the grid, effects

/// An enchantment by its name ("ENCHANT_HASTED") or bit (0-31).
int enchantArg(lua_State* L, int idx)
{
    if (lua_isnumber(L, idx)) {
        const int e = luaL_checkinteger(L, idx);
        luaL_argcheck(L, e >= 0 && e < 32, idx, "an enchantment's bit 0-31");
        return e;
    }
    // (By name: the game's own table indexed once, its first of a name as the look along it found.)
    static const std::unordered_map<std::string_view, int> byName = [] {
        std::unordered_map<std::string_view, int> m;
        for (int e = 0; e < 32; ++e) {
            m.emplace(server::kEnchantNames[e], e);
        }
        return m;
    }();
    if (const auto it = byName.find(luaL_checkstring(L, idx)); it != byName.end()) {
        return it->second;
    }
    luaL_argerror(L, idx, "an enchantment's name (ENCHANT_...) or bit");
    return 0;
}

/// A spell by its name ("SPELL_SHIELD") or number.
std::uint32_t spellArg(lua_State* L, int idx)
{
    if (lua_isnumber(L, idx)) {
        const double n = lua_tonumber(L, idx);
        luaL_argcheck(L, n >= 0 && n < 65536, idx, "a spell's number");
        return static_cast<std::uint32_t>(n);
    }
    const auto id = things::spellId(luaL_checkstring(L, idx));
    luaL_argcheck(L, id.has_value(), idx, "a spell's name (SPELL_...) or number");
    return *id;
}

int noxHeal(lua_State* L)
{
    // Health given as the game heals (adjustHealth: up to its most, nothing at full), its report
    // with it. (Taking health is nox.damage's: the engine's way.)
    server::ServerObject& o = objectArg(L, 1);
    const double amount = luaL_checknumber(L, 2);
    luaL_argcheck(L, amount >= 0 && amount <= 65535 && amount == std::floor(amount), 2, "a whole amount 0-65535");
    server::adjustHealth(worldOf(L), o, static_cast<int>(amount));
    return 0;
}

int noxCast(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.cast"); // (not inside onDamage or onExit)
    // nox.cast(spell, caster, target, x, y, power): the spell going off as an item's effect casts
    // it (the spell's own function, no "can't" sound); target nil: none. Whether it worked.
    const std::uint32_t spell = spellArg(L, 1);
    server::ServerObject& caster = objectArg(L, 2);
    server::ServerObject* target = lua_isnoneornil(L, 3) ? nullptr : &objectArg(L, 3);
    const float x = coordArg(L, 4);
    const float y = coordArg(L, 5);
    const int power = luaL_optinteger(L, 6, 0);
    luaL_argcheck(L, power >= 0 && power <= 5, 6, "a power 0-5");
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    server::Spells spells{f.world, *f.things, f.modifiers, f.rng, f.frame};
    spells.direct = true;
    lua_pushboolean(L, spells.cast(spell, caster, target, x, y, power));
    return 1;
}

int noxEnchant(lua_State* L)
{
    // nox.enchant(thing, enchantment, frames, power): on for that long (0: for good), with its
    // spell's sound; frames < 0: taken off.
    server::ServerObject& o = objectArg(L, 1);
    const int e = enchantArg(L, 2);
    const double frames = luaL_checknumber(L, 3);
    luaL_argcheck(L, frames == std::floor(frames) && frames <= 65535, 3, "whole frames, at most 65535 (0: for good; < 0: off)");
    if (frames < 0) {
        server::disenchant(worldOf(L), o, e);
    } else {
        const int power = luaL_optinteger(L, 4, 1);
        luaL_argcheck(L, power >= 0 && power <= 255, 4, "a power 0-255");
        server::enchant(worldOf(L), o, e, static_cast<int>(frames), power);
    }
    return 0;
}

int noxEnchanted(lua_State* L)
{
    lua_pushboolean(L, server::enchanted(objectArg(L, 1), enchantArg(L, 2)));
    return 1;
}

int noxDamage(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.damage"); // (not inside onDamage or onExit)
    // nox.damage(target, source, amount, type, weapon): the engine's damage (armour, the
    // enchantments, the death that follows) from source (nil: none), weapon its source's id
    // unless given. What the target's Damage function answers (0: it took none at all).
    server::ServerObject& target = objectArg(L, 1);
    server::ServerObject* source = lua_isnoneornil(L, 2) ? nullptr : &objectArg(L, 2);
    const int amount = luaL_checkinteger(L, 3);
    const int type = luaL_checkinteger(L, 4);
    const auto weapon = static_cast<std::uint32_t>(luaL_optnumber(L, 5, source ? source->extent : 0));
    server::Fight f = fightOf(L);
    lua_pushinteger(L, f.damage(target, source, weapon, amount, type));
    return 1;
}

/// The map's grid free for a walk: inside one the engine is running (a hook set off by a spell's
/// area, an earthquake's), the game's own rule finds nothing, so it's an error, not an empty list.
void gridFree(lua_State* L, const server::World& world)
{
    if (world.grid().walking()) {
        luaL_error(L, "the engine is walking the map's grid now (a hook inside an area's effect): ask after it");
    }
}

/// unitsNear's and countNear's filter (a table at `idx`, optional): monsters = true (creatures
/// only), players = true, alive = true (not dead, not going, some health), notDead = true (not
/// flagged DEAD), notGone = true, enemiesOf = id.
struct UnitFilter {
    bool monsters = false, players = false, alive = false, notDead = false, notGone = false;
    const server::ServerObject* enemiesOf = nullptr;
    const Impl* impl = nullptr; ///< (a held-out stock player's unit: never one)

    UnitFilter(lua_State* L, int idx) : impl(&implOf(L))
    {
        if (!lua_istable(L, idx)) {
            return;
        }
        const auto flag = [&](const char* k) {
            lua_rawgetfield(L, idx, k);
            const bool v = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return v;
        };
        monsters = flag("monsters");
        players = flag("players");
        alive = flag("alive");
        notDead = flag("notDead");
        notGone = flag("notGone");
        lua_rawgetfield(L, idx, "enemiesOf");
        if (!lua_isnil(L, -1)) {
            enemiesOf = &objectArg(L, lua_gettop(L));
        }
        lua_pop(L, 1);
    }
    bool operator()(const server::World& world, const server::ServerObject& o) const
    {
        return !(monsters && !o.monster) && !(players && !o.player) &&
               !(alive && (o.gone() || o.pendingDelete || (o.flags & server::ObjFlag::Dead) || o.health == 0)) &&
               !(notDead && (o.flags & server::ObjFlag::Dead)) && !(notGone && o.gone()) &&
               !(enemiesOf && !server::isEnemy(world, *enemiesOf, o)) && !impl->hidden(o);
    }
};

int noxUnitsNear(lua_State* L)
{
    // nox.unitsNear(x, y, radius, filter): the ids within the radius, from the map's grid in its
    // own order (the game's unitsGetInCircle), filtered (UnitFilter).
    const float x = coordArg(L, 1);
    const float y = coordArg(L, 2);
    const float r = coordArg(L, 3);
    const UnitFilter keep(L, 4);
    server::World& world = worldOf(L);
    gridFree(L, world);
    lua_newtable(L);
    int n = 0;
    world.unitsInCircle(x, y, r, [&](server::ServerObject& o) {
        if (keep(world, o)) {
            lua_pushnumber(L, o.extent);
            lua_rawseti(L, -2, ++n);
        }
    });
    return 1;
}

int noxCountNear(lua_State* L)
{
    // nox.countNear(x, y, radius, filter): how many unitsNear would list (no list made).
    const float x = coordArg(L, 1);
    const float y = coordArg(L, 2);
    const float r = coordArg(L, 3);
    const UnitFilter keep(L, 4);
    server::World& world = worldOf(L);
    gridFree(L, world);
    int n = 0;
    world.unitsInCircle(x, y, r, [&](server::ServerObject& o) { n += keep(world, o) ? 1 : 0; });
    lua_pushinteger(L, n);
    return 1;
}

/// A thing type at `idx`: its name (thing.bin's), or its number (thing.bin's, 1-based: nox.thingType's)
/// as the name it stands for, so a call by number is the call by that name. A number not a whole one
/// within the loaded thing.bin's is refused.
std::string_view typeArg(lua_State* L, int idx, const things::ThingDb& things)
{
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const double n = lua_tonumber(L, idx);
        luaL_argcheck(L, n >= 1 && n <= static_cast<double>(things.things.size()) && n == std::floor(n), idx, "a type's number (nox.thingType's)");
        return things.things[static_cast<std::size_t>(n) - 1].name;
    }
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    return {s, len};
}

int noxCreate(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.create"); // (not inside onDamage or onExit)
    // nox.create(type, x, y, maker): a thing of that type (thing.bin's name, or its number:
    // nox.thingType's) made at the frame now (the world's createAt); its id, nil when thing.bin has
    // no such type. With `maker` (an id), its owner and team the maker's as the engine's casts set
    // those of the things they make (a Pixie's, a missile's: the two fields; nox.setOwner is
    // unitSetOwner's way, with the owner's list).
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    const std::string_view type = typeArg(L, 1, *f.things);
    const float x = coordArg(L, 2);
    const float y = coordArg(L, 3);
    const server::ServerObject* maker = lua_isnoneornil(L, 4) ? nullptr : &objectArg(L, 4);
    const ItemSpec spec = itemSpecArg(L, 5);
    server::ServerObject* o = f.world.createAt(*f.things, type, x, y, f.frame);
    if (o && spec.any()) {
        // (Its modifiers, durability and charges in the frame it's made: told to no one yet.)
        server::Items::setModifiers(*o, spec.mods);
        std::string refused;
        if (!finishItem(L, *o, spec, refused)) {
            server::deleteObject(f.world, *o);
            luaL_error(L, "nox.create: %s", refused.c_str());
        }
    }
    if (o) {
        if (maker) {
            o->owner = maker->extent;
            o->team = maker->team;
        }
        lua_pushnumber(L, o->extent);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int noxFloorAt(lua_State* L)
{
    // The floor tile (a FLOR index) at a point; -1 none.
    lua_pushinteger(L, worldOf(L).floorAt(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2))));
    return 1;
}

int noxWallAt(lua_State* L)
{
    // Whether the wall cell (x, y: 23-unit cells) holds a wall.
    lua_pushboolean(L, worldOf(L).wall(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2)) != nullptr);
    return 1;
}

int noxCosf(lua_State* L)
{
    // cos of the value as a float, the float result (the C++'s std::cos(float), in the thread's
    // rounding: the server's).
    lua_pushnumber(L, std::cos(static_cast<float>(luaL_checknumber(L, 1))));
    return 1;
}

int noxSinf(lua_State* L)
{
    lua_pushnumber(L, std::sin(static_cast<float>(luaL_checknumber(L, 1))));
    return 1;
}

int noxMonsterInfo(lua_State* L)
{
    // monster.bin's numbers for a creature type: {experience, health, questHealth, speed}; nil
    // when it has none.
    const Impl& impl = implOf(L);
    const std::string_view type = luaL_checkstring(L, 1);
    const things::MonsterDef* def = impl.host && impl.host->options().monsterDb ? impl.host->options().monsterDb->find(type) : nullptr;
    if (!def) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    setField(L, "experience", def->experience);
    setField(L, "health", def->health);
    setField(L, "questHealth", def->questHealth);
    setField(L, "speed", def->speed);
    return 1;
}

int noxSetHealth(lua_State* L)
{
    // Its health and most health set, as the fields are (no report, no touch: nox.touch).
    server::ServerObject& o = objectArg(L, 1);
    const int health = luaL_checkinteger(L, 2);
    const int most = luaL_checkinteger(L, 3);
    luaL_argcheck(L, health >= 0 && health <= 65535, 2, "0-65535");
    luaL_argcheck(L, most >= 0 && most <= 65535, 3, "0-65535");
    o.health = static_cast<std::uint16_t>(health);
    o.maxHealth = static_cast<std::uint16_t>(most);
    return 0;
}

int noxTouch(lua_State* L)
{
    // Every part of it owed to the clients again (its update's fields all sent).
    objectArg(L, 1).touch();
    return 0;
}

int noxSightRange(lua_State* L)
{
    // A creature's sight range: nox.sightRange(id) it; nox.sightRange(id, r) set.
    server::ServerObject& o = objectArg(L, 1);
    luaL_argcheck(L, o.monster ? 1 : 0, 1, "not a creature");
    if (!lua_isnoneornil(L, 2)) {
        o.monster->sightRange = coordArg(L, 2);
        return 0;
    }
    lua_pushnumber(L, o.monster->sightRange);
    return 1;
}

// ---- cheap checks for every frame (no table made), and a quest's way back into play

int noxThingType(lua_State* L)
{
    // A type's number (thing.bin's, 1-based: what nox.present compares, no name compare per
    // call); nil when there's no such type. Ask once, keep it.
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    const things::ThingDef* t = f.things->findThing(luaL_checkstring(L, 1));
    if (!t) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, static_cast<int>(t - f.things->things.data() + 1));
    }
    return 1;
}

/// The thing whose id is at `idx` if it's in the world and not going (nil id or none: null).
const server::ServerObject* presentAt(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx)) {
        return nullptr;
    }
    const double id = luaL_checknumber(L, idx);
    const server::ServerObject* o = id >= 1 && id <= 4294967295.0 ? worldOf(L).byExtent(static_cast<std::uint32_t>(id)) : nullptr;
    return o && !o->gone() && !o->pendingDelete ? o : nullptr;
}

int noxPresent(lua_State* L)
{
    // nox.present(id [, type]): it's in the world, not going (destroyed or to be deleted), and,
    // given a type's number (nox.thingType), of that type.
    const server::ServerObject* o = presentAt(L, 1);
    bool yes = o != nullptr;
    if (yes && !lua_isnoneornil(L, 2)) {
        yes = o->type == luaL_checkinteger(L, 2);
    }
    lua_pushboolean(L, yes);
    return 1;
}

int noxLiving(lua_State* L)
{
    // nox.living(id): present, not flagged DEAD, some health.
    const server::ServerObject* o = presentAt(L, 1);
    lua_pushboolean(L, o && !(o->flags & server::ObjFlag::Dead) && o->health != 0);
    return 1;
}

int noxSetCollide(lua_State* L)
{
    // A thing's COLLIDE set by name: "DefaultCollide" (its touches do nothing: a soul gate's look
    // alone, no checkpoint) or "ModeCollide" (the mode's: nox.watchCollide). No other: the
    // game's own need the data their things are made with.
    server::ServerObject& o = objectArg(L, 1);
    const std::string_view name = luaL_checkstring(L, 2);
    luaL_argcheck(L, name == "DefaultCollide" || name == "ModeCollide", 2, "DefaultCollide or ModeCollide");
    o.collideFn = server::Name(name);
    return 0;
}

int noxBackIntoPlay(lua_State* L)
{
    // A player watching (observing, not dead) brought back into play as the quest brings its
    // watchers back: its team, out of observing, its camera its own.
    server::ServerObject& u = unitArg(L, 1);
    luaL_argcheck(L, (u.player->statusFlags & 1u) && !(u.flags & server::ObjFlag::Dead), 1, "not a player watching");
    luaL_argcheck(L, !implOf(L).hidden(u), 1, "not a player watching"); // (a stock player held out: never the mode's)
    server::Fight f = fightOf(L);
    server::joinTeam(f, u);
    server::leaveObserver(f, u);
    u.player->cameraFollow = 0;
    return 0;
}

int noxNearSoulGate(lua_State* L)
{
    // The spot beside a soul gate (any thing) where its fallen come back: within 60, one a
    // teleport may reach (the quest's own search).
    const server::ServerObject& gate = objectArg(L, 1);
    server::Fight f = fightOf(L);
    const auto [x, y] = server::nearSoulGate(f, gate);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

// ---- seeded streams: the engines the C++ draws from, for a mode whose draws must match it

constexpr const char* kMtType = "nox.mt19937";

std::mt19937& mtArg(lua_State* L, int idx)
{
    return *static_cast<std::mt19937*>(luaL_checkudata(L, idx, kMtType));
}

/// A whole number 0..2^32-1 (a seed, a u32), checked.
std::uint32_t u32Arg(lua_State* L, int idx)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= 0 && v <= 4294967295.0 && v == std::floor(v), idx, "a whole number 0 to 2^32 - 1");
    return static_cast<std::uint32_t>(v);
}

int noxMt19937(lua_State* L)
{
    // nox.mt19937(seed): a std::mt19937 seeded so, as a stream: stream:next() its next number.
    const std::uint32_t seed = u32Arg(L, 1);
    void* p = lua_newuserdatadtor(L, sizeof(std::mt19937), [](lua_State*, void* u) { static_cast<std::mt19937*>(u)->~mersenne_twister_engine(); });
    new (p) std::mt19937(seed);
    luaL_getmetatable(L, kMtType);
    lua_setmetatable(L, -2);
    return 1;
}

int mtNext(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(mtArg(L, 1)()));
    return 1;
}

int noxShuffle(lua_State* L)
{
    // nox.shuffle(stream, list): the list (an array from 1) shuffled in place as the C++'s
    // util::shuffle draws it from that engine (Microsoft's algorithm on every system); the list.
    std::mt19937& engine = mtArg(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const int n = lua_objlen(L, 2);
    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        order[static_cast<std::size_t>(i)] = i + 1;
    }
    util::shuffle(order, engine); // (the same swaps whatever the values: the permutation)
    lua_createtable(L, n, 0); // (the old order)
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        lua_rawseti(L, -2, i);
    }
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, -1, order[static_cast<std::size_t>(i)]);
        lua_rawseti(L, 2, i + 1);
    }
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    return 1;
}

int noxMul32(lua_State* L)
{
    // a * b as a u32's product wraps (a seed's mixing: past a number's exact range).
    lua_pushnumber(L, static_cast<double>(u32Arg(L, 1) * u32Arg(L, 2)));
    return 1;
}

int noxTextToAll(lua_State* L)
{
    // A line to everyone as the engine broadcasts one (one message, every client): its bytes as
    // they are, 250 at most.
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    server::World& world = worldOf(L);
    Impl& impl = implOf(L);
    if (!impl.host->anyRefused()) {
        if (!impl.chargeText(std::max(1, static_cast<int>(impl.host->players().size())))) {
            return 0; // (over the frame's text: dropped, reported)
        }
        world.textToAll(std::u16string(s, s + std::min<size_t>(len, 250)));
        return 0;
    }
    // (A stock player held out to be put out: to each of the others in the game alone, the same
    // message; one not in yet never gets the broadcast either.)
    std::vector<std::uint32_t> units;
    for (const auto& [peer, p] : impl.host->players()) {
        if (!impl.host->refused(peer) && p.joined) {
            units.push_back(p.extent);
        }
    }
    if (!impl.chargeText(std::max(1, static_cast<int>(units.size())))) {
        return 0;
    }
    const auto m = server::World::textMessage(std::u16string(s, s + std::min<size_t>(len, 250)), 0);
    for (const std::uint32_t e : units) {
        world.broadcasts().push_back({m, true, e});
    }
    return 0;
}

int noxTell(lua_State* L)
{
    // nox.tell(unit, text): a line to that player alone, as the native modes tell one (one
    // MSG_TEXT_MESSAGE from no one, its bytes each a character as they are, 250 at most). Counted
    // against the frame's text (over it: dropped, reported).
    const server::ServerObject& u = unitArg(L, 1);
    size_t len = 0;
    const char* s = luaL_checklstring(L, 2, &len);
    server::World& world = worldOf(L);
    if (implOf(L).hidden(u) || !implOf(L).chargeText(1)) {
        return 0; // (a stock player held out, or over the frame's text)
    }
    const std::u16string text(s, s + std::min<size_t>(len, 250));
    world.broadcasts().push_back({server::World::textMessage(text, 0), true, u.extent});
    return 0;
}

int noxEffect(lua_State* L)
{
    // A point effect every client draws: one of the game's MSG_FX_* that is just a point
    // ({op, u16 x, u16 y}), never another message (a client would frame what follows wrongly).
    const int op = luaL_checkinteger(L, 1);
    luaL_argcheck(L, op >= 0 && op < 256, 1, "a message 0-255");
    const net::OpInfo& info = net::opInfo(static_cast<std::uint8_t>(op));
    luaL_argcheck(L, info.name.starts_with("MSG_FX_") && info.serverToClient == 5, 1, "a point effect's message (MSG_FX_*, {x, y})");
    server::pointFx(worldOf(L), static_cast<std::uint8_t>(op), coordArg(L, 2), coordArg(L, 3));
    return 0;
}

int noxSpellSound(lua_State* L)
{
    // A spell's sound's name (which: 0 its cast, 1 on, 2 off), for nox.sound; nil: none.
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    const std::string_view s = server::spellSound(*f.things, spellArg(L, 1), luaL_optinteger(L, 2, 0));
    if (s.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, s.data(), s.size());
    }
    return 1;
}

// ---- more of the engine's paths: foes near, motion and facing, missiles, areas, ownership. Each
// calls the very function the engine's own callers call, with the server's Fight (its RNG, its
// frame), and checks its input first.

/// A whole number from lo to hi (checked before it's made an int).
int wholeArg(lua_State* L, int idx, double lo, double hi, const char* what)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= lo && v <= hi && v == std::floor(v), idx, what);
    return static_cast<int>(v);
}

/// A number from lo to hi as the float the engine takes: never NaN or infinite.
float floatArg(lua_State* L, int idx, double lo, double hi, const char* what)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= lo && v <= hi, idx, what);
    return static_cast<float>(v);
}

/// A thing on the map (not in an inventory: moving one there would leave it in two places).
server::ServerObject& onMapArg(lua_State* L, int idx)
{
    server::ServerObject& o = objectArg(L, idx);
    luaL_argcheck(L, o.holder == 0, idx, "held (nox.drop or nox.place puts it on the map)");
    return o;
}

/// The damage types (DAMAGE_BLADE 0 to DAMAGE_AIRBORNE_ELECTRIC 17).
constexpr double kLastDamageType = 17;

int noxNearestFoes(lua_State* L)
{
    // nox.nearestFoes(unit, range, most, losTested): the ids of the live enemy creatures (not dead,
    // going or without health; Obj_IsEnemyTo's enemies of `unit`) within range of it that it sees,
    // nearest first: of the 24 nearest (one past that replacing the farthest kept), the first
    // losTested traced for walls (Doors | ThroughWindows), at most `most` of those seen.
    server::ServerObject& unit = objectArg(L, 1);
    const float range = coordArg(L, 2);
    const int most = wholeArg(L, 3, 0, 65535, "at most how many: 0-65535");
    const int tested = wholeArg(L, 4, 0, 65535, "how many traced: 0-65535");
    server::World& world = worldOf(L);
    gridFree(L, world);
    lua_newtable(L);
    if (most == 0) {
        return 1;
    }
    // (The blocks' own search, foes.hpp: the same order, ties too.)
    int out = 0;
    const Impl& impl = implOf(L);
    server::detail::visibleFoes(world, unit, range, static_cast<std::size_t>(tested), [&](server::ServerObject* o) {
        if (impl.hidden(*o)) {
            return true; // (a held-out stock player's unit: not the mode's)
        }
        lua_pushnumber(L, o->extent);
        lua_rawseti(L, -2, ++out);
        return out < most;
    });
    return 1;
}

int noxApplyForce(lua_State* L)
{
    // nox.applyForce(id, fromX, fromY, force): the engine's push away from (fromX, fromY)
    // (objectApplyForce: force x 10 / its mass; nothing on what can't move); a negative force pulls.
    server::ServerObject& o = onMapArg(L, 1);
    const float x = coordArg(L, 2);
    const float y = coordArg(L, 3);
    const float force = floatArg(L, 4, -10000, 10000, "a force -10000 to 10000");
    server::applyForce(worldOf(L), o, x, y, force);
    return 0;
}

int noxAttack(lua_State* L)
{
    // nox.attack(attacker, {damage, type, range, area, sides, x, y}): a blow as the engine swings
    // one (Fight::attack: playerTraceAttack's hit, the walls in its reach after), from the
    // attacker's facing (nox.direction): the nearest in range before it, or with `area` everything
    // within range of (x, y). Fields left out are the attack record's own (sides 1, the front; area
    // false; the rest 0). Whether it hit anything.
    server::ServerObject& a = onMapArg(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    server::Attack atk;
    const auto given = [&](const char* k) {
        lua_rawgetfield(L, 2, k);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return 0;
        }
        return lua_gettop(L);
    };
    if (const int i = given("damage")) {
        atk.damage = floatArg(L, i, 0, 65535, "attack.damage: 0-65535");
        lua_pop(L, 1);
    }
    if (const int i = given("type")) {
        atk.type = static_cast<std::uint8_t>(wholeArg(L, i, 0, kLastDamageType, "attack.type: a damage type 0-17"));
        lua_pop(L, 1);
    }
    if (const int i = given("range")) {
        atk.range = coordArg(L, i);
        lua_pop(L, 1);
    }
    if (const int i = given("area")) {
        atk.area = lua_toboolean(L, i) != 0;
        lua_pop(L, 1);
    }
    if (const int i = given("sides")) {
        atk.sides = static_cast<std::uint8_t>(wholeArg(L, i, 0, 255, "attack.sides: 0-255"));
        lua_pop(L, 1);
    }
    if (const int i = given("x")) {
        atk.x = coordArg(L, i);
        lua_pop(L, 1);
    }
    if (const int i = given("y")) {
        atk.y = coordArg(L, i);
        lua_pop(L, 1);
    }
    server::Fight f = fightOf(L);
    gridFree(L, f.world);
    lua_pushboolean(L, f.attack(a, atk));
    return 1;
}

int noxDirection(lua_State* L)
{
    // nox.direction(id): its facing, 0-255 (east, then round through south); nox.direction(id, d)
    // set (the field, as the engine's own turns set it: nox.touch to have it sent at once).
    // Its use: a facing is always one of the 256 steps (nox.directionOf gives one from a vector;
    // a step's arithmetic wraps with % 256). The object's field is 16 bits wide, but the engine
    // and every client read only 0-255 (a client's turn comes as a byte), so the setter takes
    // nothing past 255: wrap a sum before setting it (an error, not a silent wrap).
    server::ServerObject& o = objectArg(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushnumber(L, o.direction);
        return 1;
    }
    o.direction = static_cast<std::uint16_t>(wholeArg(L, 2, 0, 255, "a direction 0-255"));
    return 0;
}

int noxDirectionOf(lua_State* L)
{
    // nox.directionOf(dx, dy): the vector's 256-step direction, as the engine works it out.
    const float x = floatArg(L, 1, -1e7, 1e7, "a finite number");
    const float y = floatArg(L, 2, -1e7, 1e7, "a finite number");
    lua_pushinteger(L, server::directionOf(x, y));
    return 1;
}

int noxDirectionVector(lua_State* L)
{
    // nox.directionVector(d): the unit vector of a 256-step direction (the engine's own table).
    const auto [x, y] = server::directionVector(wholeArg(L, 1, 0, 255, "a direction 0-255"));
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

int noxNewMark(lua_State* L)
{
    // nox.newMark(): where the world's new list stands now (the things made since the last new
    // things' pass), for nox.madeSince.
    const server::World& world = worldOf(L);
    const auto& list = world.newList();
    lua_createtable(L, 0, 3);
    setField(L, "frame", world.frameNow);
    setField(L, "n", static_cast<double>(list.size()));
    setField(L, "last", list.empty() ? 0.0 : static_cast<double>(list.back()));
    return 1;
}

int noxMadeSince(lua_State* L)
{
    // nox.madeSince(mark): the ids put on the new list since the mark, oldest first (what a cast
    // just made). Once a new things' pass has taken the list since, those it took are gone from it:
    // then all those on it now (each made after the mark).
    luaL_checktype(L, 1, LUA_TTABLE);
    const auto whole = [&](const char* k) {
        lua_rawgetfield(L, 1, k);
        const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1.0;
        lua_pop(L, 1);
        return v >= 0 && v <= 4294967295.0 && v == std::floor(v) ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(v)) : std::nullopt;
    };
    const auto frame = whole("frame");
    const auto n = whole("n");
    const auto last = whole("last");
    luaL_argcheck(L, frame && n && last, 1, "a mark (nox.newMark's)");
    const server::World& world = worldOf(L);
    const auto& list = world.newList();
    std::size_t from = *n;
    // (Still the list it marked: the same frame, at least as long, its last entry then the same.)
    if (*frame != world.frameNow || from > list.size() || (from > 0 && list[from - 1] != *last)) {
        from = 0;
    }
    lua_createtable(L, static_cast<int>(std::min<std::size_t>(list.size() - from, 1u << 16)), 0);
    int k = 0;
    for (std::size_t i = from; i < list.size(); ++i) {
        lua_pushnumber(L, list[i]);
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

int noxSpellInfo(lua_State* L)
{
    // nox.spellInfo(spell): thing.bin's record of a spell (its name or number): {flags, onceOnly
    // (a quest's once-only spell: granted once, never past level 1), manaCost (what a player's
    // cast costs as the game has it: a summoning's its table's), trapCost (put into a glyph's
    // trap)}; nil when it has none.
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    const std::uint32_t spell = spellArg(L, 1);
    const things::SpellDef* def = f.things->findSpell(spell);
    if (!def) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    setField(L, "flags", def->flags);
    lua_pushboolean(L, server::questOnceOnlySpell(static_cast<int>(spell)) ? 1 : 0);
    lua_setfield(L, -2, "onceOnly");
    setField(L, "manaCost", server::Spells::priceOf(f.world, *f.things, spell, true, false));
    setField(L, "trapCost", server::Spells::priceOf(f.world, *f.things, spell, true, true));
    return 1;
}

int noxSetCharmable(lua_State* L)
{
    // nox.setCharmable(id, charmable): false: Charm refuses the creature as one not charmable (a
    // charm on it now ends uncontrolled); true or nil: as the game has it.
    toolbox::refuseInsideAsked(L, "nox.setCharmable");
    server::ServerObject& o = objectArg(L, 1);
    luaL_argcheck(L, static_cast<bool>(o.monster), 1, "a creature");
    luaL_argcheck(L, lua_isnoneornil(L, 2) || lua_isboolean(L, 2), 2, "true, false or nil");
    o.monster->notCharmable = !lua_isnoneornil(L, 2) && !lua_toboolean(L, 2);
    return 0;
}

int noxEnchantTime(lua_State* L)
{
    // nox.enchantTime(id, enchantment): its frames left (0: on for good, or not on).
    lua_pushnumber(L, server::enchantTime(objectArg(L, 1), enchantArg(L, 2)));
    return 1;
}

int noxGameNumber(lua_State* L)
{
    // nox.gameNumber(name, index): gamedata.bin's number as this game reads it (the SOLO / ARENA
    // lines by its flags; 0 past a table's end); nil for a name it hasn't.
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    const std::size_t index = lua_isnoneornil(L, 2) ? 0 : static_cast<std::size_t>(wholeArg(L, 2, 0, 254, "an index 0-254"));
    const server::World& world = worldOf(L);
    const std::string_view name(s, len);
    if (!world.gameData || !world.gameData->find(name, world.gameFlags)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, world.number(name, index));
    return 1;
}

/// The ray's flags a script may ask for (world.hpp's Trace: Doors, ThroughWindows, NearWindows,
/// SightOnly, PassSecret).
constexpr int kRayFlags = server::Trace::Doors | server::Trace::ThroughWindows | server::Trace::NearWindows | server::Trace::SightOnly |
                          server::Trace::PassSecret;

int noxTraceRay(lua_State* L)
{
    // nox.traceRay(x1, y1, x2, y2, flags): whether no wall stands between the points, as the
    // engine's ray walks the cells (flags: 1 shut doors block, 4 through windows, 8 windows near the
    // start don't block, 0x40 only walls that block sight, 0x80 past open secret walls). Blocked:
    // false and, when it met a wall, where (x, y) and that wall's cell (cx, cy).
    const float x1 = coordArg(L, 1);
    const float y1 = coordArg(L, 2);
    const float x2 = coordArg(L, 3);
    const float y2 = coordArg(L, 4);
    const int flags = wholeArg(L, 5, 0, 255, "the ray's flags (1, 4, 8, 0x40, 0x80)");
    luaL_argcheck(L, (flags & ~kRayFlags) == 0, 5, "the ray's flags (1, 4, 8, 0x40, 0x80)");
    server::RayHit hit;
    hit.cellX = -1;
    if (worldOf(L).traceRay(x1, y1, x2, y2, static_cast<std::uint8_t>(flags), &hit)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    if (hit.cellX < 0) {
        return 1;
    }
    lua_pushnumber(L, hit.x);
    lua_pushnumber(L, hit.y);
    lua_pushnumber(L, hit.cellX);
    lua_pushnumber(L, hit.cellY);
    return 5;
}

int noxShoot(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.shoot"); // (not inside onDamage or onExit)
    // nox.shoot(type, owner, direction): a missile of `type` (thing.bin's; ArcherArrow,
    // ArcherBolt) loosed as a bow looses one with no bow or quiver (shootBowCrossbow2's): from the
    // owner's edge (its radius + 4) in the 256-step direction, the owner's, made at the frame now,
    // ACTIVE and AIRBORNE, at its own speed that way. Its id; nil, nothing made, when a wall
    // (Doors | ThroughWindows) stands between the owner and that edge. (type: its name or number.)
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    const std::string_view type = typeArg(L, 1, *f.things);
    const server::ServerObject& unit = onMapArg(L, 2);
    const int d = wholeArg(L, 3, 0, 255, "a direction 0-255");
    const things::ThingDef* def = f.things->findThing(type);
    luaL_argcheck(L, def && (things::thingClasses(*def) & things::ObjectClass::Missile), 1, "a missile's type (thing.bin's)");
    server::World& world = f.world;
    const auto [fx, fy] = server::directionVector(d);
    const float r0 = unit.radius + 4.0f;
    const float sx = r0 * fx + unit.x;
    const float sy = r0 * fy + unit.y;
    if (!world.traceRay(unit.x, unit.y, sx, sy, server::Trace::Doors | server::Trace::ThroughWindows)) {
        lua_pushnil(L);
        return 1;
    }
    server::ServerObject& shaft = world.spawn(*f.things, type, sx, sy);
    shaft.owner = unit.extent;
    shaft.born = f.frame;
    shaft.flags |= server::ObjFlag::Active | server::ObjFlag::Airborne;
    shaft.vx = fx * shaft.speedCur;
    shaft.vy = fy * shaft.speedCur;
    shaft.newX = sx;
    shaft.newY = sy;
    shaft.direction = static_cast<std::uint16_t>(d);
    shaft.touch();
    lua_pushnumber(L, shaft.extent);
    return 1;
}

int noxSummon(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.summon"); // (not inside onDamage or onExit)
    // nox.summon(type, x, y, owner, direction): a creature summoned as a summoning spell ends
    // (unitDoSummonAt: at (x, y) facing `direction`, SUMMONED, its owner's: a player's follows its
    // orders, is told it has it and joins its team; another's escorts it); owner nil: nobody's.
    // Its id; nil for a type thing.bin hasn't. (type: its name or number.)
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    const std::string_view type = typeArg(L, 1, *f.things);
    const float x = coordArg(L, 2);
    const float y = coordArg(L, 3);
    server::ServerObject* owner = lua_isnoneornil(L, 4) ? nullptr : &objectArg(L, 4);
    const int direction = wholeArg(L, 5, 0, 65535, "a direction 0-255");
    server::ServerObject* c = server::summonAt(f, type, x, y, owner, direction);
    if (c) {
        lua_pushnumber(L, c->extent);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int noxAbilitySound(lua_State* L)
{
    // nox.abilitySound(ability, which): an ability's sound's name (thing.bin's ABIL: which 0 its
    // start, 1, 2 its end), for nox.sound; nil: none.
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    const int ability = wholeArg(L, 1, 0, 255, "an ability's number");
    const int which = lua_isnoneornil(L, 2) ? 0 : wholeArg(L, 2, 0, 2, "which: 0-2");
    const std::string_view s = server::abilitySound(*f.things, ability, which);
    if (s.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, s.data(), s.size());
    }
    return 1;
}

int noxSetPosition(lua_State* L)
{
    // nox.setPosition(id, x, y): put there as the engine places a thing (Obj_SetPosition: its
    // grid, its motion; a player's client told to forget what it saw).
    server::ServerObject& o = onMapArg(L, 1);
    const float x = coordArg(L, 2);
    const float y = coordArg(L, 3);
    server::Fight f = fightOf(L);
    server::setPosition(f.world, o, x, y, f.frame);
    return 0;
}

int noxSetVelocity(lua_State* L)
{
    // nox.setVelocity(id, vx, vy): its velocity (a frame's, the field), -1000 to 1000 each.
    server::ServerObject& o = onMapArg(L, 1);
    const float vx = floatArg(L, 2, -1000, 1000, "a velocity -1000 to 1000");
    const float vy = floatArg(L, 3, -1000, 1000, "a velocity -1000 to 1000");
    o.vx = vx;
    o.vy = vy;
    return 0;
}

int noxSetOwner(lua_State* L)
{
    // nox.setOwner(id, owner): the engine's unitSetOwner (its old owner let go, the head of the
    // new one's list, a creature's enemy forgotten, its friendships marked); owner nil:
    // unitClearOwner (a player's creature lost to it). Never a player's unit, never a ring.
    server::ServerObject& o = objectArg(L, 1);
    luaL_argcheck(L, !o.player && !(o.classes & things::ObjectClass::Player), 1, "a player's unit has no owner");
    server::World& world = worldOf(L);
    if (lua_isnoneornil(L, 2)) {
        server::clearOwner(world, o);
        return 0;
    }
    server::ServerObject& owner = objectArg(L, 2);
    // (A ring of owners would never end setOwner's walk up past those being deleted.)
    int steps = 0;
    for (const server::ServerObject* up = &owner; up; up = up->owner ? world.byExtent(up->owner) : nullptr) {
        luaL_argcheck(L, up != &o && ++steps < 256, 2, "its own owner (a ring of owners)");
    }
    server::setOwner(world, &owner, o);
    return 0;
}

int noxSetTeam(lua_State* L)
{
    // nox.setTeam(id, team): a thing's team id, 0-255, as the engine's casts set their things'
    // (the field). Not a player's unit: a player's team is its membership (the team's list).
    server::ServerObject& o = objectArg(L, 1);
    luaL_argcheck(L, !o.player && !(o.classes & things::ObjectClass::Player), 1, "a player's unit (its team is the team's list)");
    o.team = static_cast<std::uint8_t>(wholeArg(L, 2, 0, 255, "a team 0-255"));
    return 0;
}

int noxSetNoCollide(lua_State* L)
{
    // nox.setNoCollide(id, on): its NO_COLLIDE flag (what an off trap, light or fire has): nothing
    // touches it, it pushes nothing.
    server::ServerObject& o = objectArg(L, 1);
    if (lua_toboolean(L, 2)) {
        o.flags |= server::ObjFlag::NoCollide;
    } else {
        o.flags &= ~server::ObjFlag::NoCollide;
    }
    return 0;
}

/// A missile (class MISSILE): the only things nox.setHitRadius and nox.setBurstScale size.
server::ServerObject& missileArg(lua_State* L, int idx)
{
    server::ServerObject& o = objectArg(L, idx);
    luaL_argcheck(L, (o.classes & things::ObjectClass::Missile) != 0, idx, "not a missile");
    return o;
}

int noxSetHitRadius(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.setHitRadius"); // (not inside onDamage or onExit)
    // nox.setHitRadius(id, radius): a missile's hit shape, in world units: a point made a circle
    // of 1-32, a circle's radius 1 to 4 x its type's, a box's sides scaled alike (its larger
    // side's half 1 to 4 x its type's); what its flight meets is what that shape touches (several
    // in one piece: the grid walk's last, not the nearest), and nox.object(id).radius reads it.
    // Its type's own flight meets only what holds its centre, so only nil gives the stock hit
    // back: its type's radius set again is a hit circle of that size. It stays with the missile
    // when it's reflected or turned. Walls are still met by its centre's path. It may be set in the
    // frame the missile is made (onSpellCast's `made`): until it has first flown clear of its owner,
    // the owner is met only as the stock missile would meet it (a new owner, a new grace).
    server::ServerObject& m = missileArg(L, 1);
    if (lua_isnoneornil(L, 2)) {
        server::restoreHitShape(worldOf(L), m);
        return 0;
    }
    const auto [lo, hi] = server::hitRadiusRange(m);
    const float r = floatArg(L, 2, lo, hi, "a radius from 1 to 32 for a point, to 4 x its type's for a circle or a box");
    server::setHitRadius(worldOf(L), m, r);
    return 0;
}

int noxSetBurstScale(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.setBurstScale"); // (not inside onDamage or onExit)
    // nox.setBurstScale(id, scale): a missile's burst (a fireball's, a magic missile's) reaching
    // `scale` x its type's, 0.5-3: its harm's reach and its push's, read as it bursts. Its damage,
    // its hit and the burst the clients draw stay its type's. nil: 1. One that never bursts
    // ignores it; one reflected or turned keeps it.
    server::ServerObject& m = missileArg(L, 1);
    const float s = lua_isnoneornil(L, 2) ? 1.0f : floatArg(L, 2, 0.5, 3, "a scale from 0.5 to 3");
    server::setBurstScale(m, s);
    return 0;
}

int noxDamageAt(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.damageAt"); // (not inside onDamage or onExit)
    // nox.damageAt(x, y, reach, full, damage, type, source, except, spareSource): the engine's
    // area harm (mapDamageUnitsAround, a DeathBall's): everything within reach of (x, y) in a clear
    // ray (Doors), the whole damage within `full` and less towards `reach` (full below reach), from
    // source (nil: none), `except` spared (nil: none), and the source too with spareSource; then
    // the walls about it.
    const float x = coordArg(L, 1);
    const float y = coordArg(L, 2);
    const float reach = coordArg(L, 3);
    const float full = coordArg(L, 4);
    luaL_argcheck(L, full < reach, 4, "the full damage's radius below the reach");
    const int damage = wholeArg(L, 5, 0, 65535, "a whole damage 0-65535");
    const int type = wholeArg(L, 6, 0, kLastDamageType, "a damage type 0-17");
    server::ServerObject* source = lua_isnoneornil(L, 7) ? nullptr : &objectArg(L, 7);
    const server::ServerObject* except = lua_isnoneornil(L, 8) ? nullptr : &objectArg(L, 8);
    const bool spareSource = lua_toboolean(L, 9) != 0;
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    gridFree(L, f.world);
    server::Missiles harm{f.world, *f.things, f.modifiers, f.rng, f.frame};
    harm.damageAt(x, y, reach, full, damage, type, source, except, spareSource);
    return 0;
}

int noxDelete(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.delete"); // (not inside onDamage or onExit)
    // nox.delete(id): the engine's delete (delayedDeleteObject: out of its holder's inventory at
    // once, its lasting spells ended, gone at the frame's end). Never a player's unit.
    server::ServerObject& o = objectArg(L, 1);
    luaL_argcheck(L, !o.player && !(o.classes & things::ObjectClass::Player), 1, "a player's unit");
    server::deleteObject(worldOf(L), o);
    return 0;
}

// ---- batches: one call for what a frame would otherwise ask thing by thing (each Lua-to-C call
// costs more than the work it does)

int noxPosition(lua_State* L)
{
    // nox.position(id): x, y and radius (no table); an error when it isn't present (nox.present's
    // test).
    const server::ServerObject* o = presentAt(L, 1);
    luaL_argcheck(L, o != nullptr, 1, "not present (gone, or being deleted)");
    lua_pushnumber(L, o->x);
    lua_pushnumber(L, o->y);
    lua_pushnumber(L, o->radius);
    return 3;
}

/// The array at `idx` checked: a writable table whose entries 1..n are all numbers. Its n.
int idListArg(lua_State* L, int idx, bool writes)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    luaL_argcheck(L, !writes || !lua_getreadonly(L, idx), idx, "a list it may change (not frozen)");
    const int n = lua_objlen(L, idx);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        const bool number = lua_type(L, -1) == LUA_TNUMBER;
        lua_pop(L, 1);
        if (!number) {
            luaL_error(L, "the list's entry %d isn't an id", i);
        }
    }
    return n;
}

/// The thing an id stands for as World::byExtent finds it (none for a number that can't be one).
server::ServerObject* byIdNumber(server::World& world, double id)
{
    return id >= 1 && id <= 4294967295.0 ? world.byExtent(static_cast<std::uint32_t>(id)) : nullptr;
}

/// The list compacted in place, in order, keeping the ids `keep` passes; the tail made nil. Its
/// new length.
template <class Keep>
int compactIds(lua_State* L, int n, Keep&& keep)
{
    server::World& world = worldOf(L);
    int kept = 0;
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        const server::ServerObject* o = byIdNumber(world, lua_tonumber(L, -1));
        if (o && !o->pendingDelete && keep(*o)) { // (byExtent: never a destroyed one)
            if (++kept != i) {
                lua_rawseti(L, 1, kept);
                continue;
            }
        }
        lua_pop(L, 1);
    }
    for (int i = kept + 1; i <= n; ++i) {
        lua_pushnil(L);
        lua_rawseti(L, 1, i);
    }
    return kept;
}

int noxKeepPresent(lua_State* L)
{
    // nox.keepPresent(list [, type]): the list of ids compacted in place (its order kept) to those
    // nox.present(id [, type]) would answer true for, the rest of it nil; its new length.
    const int n = idListArg(L, 1, true);
    if (lua_isnoneornil(L, 2)) {
        lua_pushinteger(L, compactIds(L, n, [](const server::ServerObject&) { return true; }));
    } else {
        const int type = wholeArg(L, 2, 0, 65535, "a type's number (nox.thingType)");
        lua_pushinteger(L, compactIds(L, n, [type](const server::ServerObject& o) { return o.type == type; }));
    }
    return 1;
}

int noxKeepLiving(lua_State* L)
{
    // nox.keepLiving(list): the same, by nox.living's test.
    const int n = idListArg(L, 1, true);
    lua_pushinteger(L, compactIds(L, n, [](const server::ServerObject& o) { return !(o.flags & server::ObjFlag::Dead) && o.health != 0; }));
    return 1;
}

/// A list's ids read out first (a hook the engine sets off while the batch runs could change the
/// script's list).
struct IdSnapshot {
    std::uint32_t small[16];
    std::vector<std::uint32_t> big;
    int n = 0;

    IdSnapshot(lua_State* L, int idx)
    {
        n = idListArg(L, idx, false);
        if (n > static_cast<int>(std::size(small))) {
            big.resize(static_cast<std::size_t>(n));
        }
        std::uint32_t* out = big.empty() ? small : big.data();
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            const double v = lua_tonumber(L, -1);
            lua_pop(L, 1);
            out[i - 1] = v >= 1 && v <= 4294967295.0 ? static_cast<std::uint32_t>(v) : 0u; // (0: no thing's)
        }
    }
    std::uint32_t operator[](int i) const { return big.empty() ? small[i] : big[static_cast<std::size_t>(i)]; }
};

int noxCircle(lua_State* L)
{
    // nox.circle(ids, cx, cy, radius, angle, count): the things set evenly about (cx, cy) as a ring
    // turns them: the i-th (from 0, by its place in the list) at angle + 2 pi i / count, each
    // stopped (velocity 0) and placed there by the engine's setPosition at the frame now. The
    // figures are the float expression a native ring used (float cos and sin, in the thread's
    // rounding: the server's). An id not in the world is passed over, its place kept; one being
    // deleted is still placed (as that ring placed every thing its list held). How many placed.
    const IdSnapshot ids(L, 1);
    const float cx = coordArg(L, 2);
    const float cy = coordArg(L, 3);
    const float radius = coordArg(L, 4);
    const float angle = floatArg(L, 5, -1e6, 1e6, "an angle (radians), finite");
    const int count = wholeArg(L, 6, 1, 65535, "a count 1-65535");
    server::Fight f = fightOf(L);
    int placed = 0;
    for (int i = 0; i < ids.n; ++i) {
        server::ServerObject* b = ids[i] ? f.world.byExtent(ids[i]) : nullptr;
        if (!b || b->holder != 0) {
            continue;
        }
        const float a = angle + 2.0f * std::numbers::pi_v<float> * static_cast<float>(static_cast<std::size_t>(i)) / static_cast<float>(count);
        b->vx = b->vy = 0;
        server::setPosition(f.world, *b, cx + std::cos(a) * radius, cy + std::sin(a) * radius, f.frame);
        ++placed;
    }
    lua_pushinteger(L, placed);
    return 1;
}

int noxDamageAtEach(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.damageAtEach"); // (not inside onDamage or onExit)
    // nox.damageAtEach(ids, reach, full, damage, type, except, spareSource): nox.damageAt about each
    // thing in the list, in its order, at where it is when its turn comes, each its own source (an
    // id not in the world passed over; one being deleted still harms, as a native ring's did).
    const IdSnapshot ids(L, 1);
    const float reach = coordArg(L, 2);
    const float full = coordArg(L, 3);
    luaL_argcheck(L, full < reach, 3, "the full damage's radius below the reach");
    const int damage = wholeArg(L, 4, 0, 65535, "a whole damage 0-65535");
    const int type = wholeArg(L, 5, 0, kLastDamageType, "a damage type 0-17");
    const server::ServerObject* except = lua_isnoneornil(L, 6) ? nullptr : &objectArg(L, 6);
    const bool spareSource = lua_toboolean(L, 7) != 0;
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    gridFree(L, f.world);
    server::Missiles harm{f.world, *f.things, f.modifiers, f.rng, f.frame};
    for (int i = 0; i < ids.n; ++i) {
        if (server::ServerObject* b = ids[i] ? f.world.byExtent(ids[i]) : nullptr) {
            harm.damageAt(b->x, b->y, reach, full, damage, type, b, except, spareSource);
        }
    }
    return 0;
}

// ---- a player's own numbers and book, its quest fields, the quest's rewards, the game's access
// and teams: the fields and the engine's functions the quest-like modes use, each as they use it

int noxGold(lua_State* L)
{
    lua_pushnumber(L, unitArg(L, 1).player->gold);
    return 1;
}

int noxSetGold(lua_State* L)
{
    // nox.setGold(unit, n, report): its gold; with report, MSG_REPORT_GOLD (0x4A) to it now (else
    // its client is told with the frame's reports).
    server::ServerObject& u = unitArg(L, 1);
    const auto n = static_cast<std::uint32_t>(u32Arg(L, 2));
    u.player->gold = n;
    if (lua_toboolean(L, 3)) {
        worldOf(L).broadcasts().push_back({{0x4A, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n >> 16),
                                            static_cast<std::uint8_t>(n >> 24)},
                                           true,
                                           u.extent});
    }
    return 0;
}

int noxMana(lua_State* L)
{
    const server::PlayerControl& pc = *unitArg(L, 1).player;
    lua_pushnumber(L, pc.mana);
    lua_pushnumber(L, pc.maxMana);
    return 2;
}

int noxSetMana(lua_State* L)
{
    // nox.setMana(unit, mana, most, report): both. report "total" (or nil): its client owed all of
    // it (MSG_REPORT_TOTAL_MANA, 0xDE: a guardian's, a blessing's new most); "plain": the fields
    // alone, the frame's report telling a change as it tells any (MSG_REPORT_MANA, 0x45: a shrine's
    // or a renewal's mana back).
    server::PlayerControl& pc = *unitArg(L, 1).player;
    const int mana = wholeArg(L, 2, 0, 65535, "mana 0-65535");
    const int most = wholeArg(L, 3, 0, 65535, "most mana 0-65535");
    bool total = true;
    if (!lua_isnoneornil(L, 4)) {
        const std::string_view report = luaL_checkstring(L, 4);
        luaL_argcheck(L, report == "total" || report == "plain", 4, "\"total\" or \"plain\"");
        total = report == "total";
    }
    pc.mana = static_cast<std::uint16_t>(mana);
    pc.maxMana = static_cast<std::uint16_t>(most);
    if (total) {
        pc.manaSent = -1;
    }
    return 0;
}

int noxSoundAt(lua_State* L)
{
    // nox.soundAt(x, y, sound): a sound at a point, from no thing (audCreate): its number (0-1023)
    // or its name (a name that isn't one: nothing).
    const float x = coordArg(L, 1);
    const float y = coordArg(L, 2);
    server::World& world = worldOf(L);
    if (lua_type(L, 3) == LUA_TNUMBER) {
        server::playSoundAt(world, static_cast<std::uint32_t>(wholeArg(L, 3, 0, 1023, "a sound's number 0-1023")), x, y);
    } else {
        server::playSoundAt(world, std::string_view(luaL_checkstring(L, 3)), x, y);
    }
    return 0;
}

int noxGameRandom(lua_State* L)
{
    // nox.gameRandom(n): the server's own game stream (the Fight's rng, which the engine's draws
    // share) drawn once, modulo n: 0 to n - 1, as `rng() % n` gives it in C++. Its place among the
    // engine's own draws is the caller's order.
    const double n = luaL_checknumber(L, 1);
    luaL_argcheck(L, n >= 1 && n <= 4294967295.0 && n == std::floor(n), 1, "a whole n, 1 to 2^32 - 1");
    server::Fight f = fightOf(L);
    lua_pushnumber(L, static_cast<double>(static_cast<std::size_t>(f.rng()) % static_cast<std::size_t>(n)));
    return 1;
}

int noxSetPlayerHealth(lua_State* L)
{
    // nox.setPlayerHealth(unit, health, most): both, touched, its client owed both
    // (MSG_REPORT_TOTAL_HEALTH).
    server::ServerObject& u = unitArg(L, 1);
    const int health = wholeArg(L, 2, 0, 65535, "health 0-65535");
    const int most = wholeArg(L, 3, 0, 65535, "most health 0-65535");
    u.maxHealth = static_cast<std::uint16_t>(most);
    u.health = static_cast<std::uint16_t>(health);
    u.touch();
    u.player->healthSent = -1;
    return 0;
}

int noxStrength(lua_State* L)
{
    lua_pushnumber(L, unitArg(L, 1).player->strength);
    return 1;
}

int noxSetStrength(lua_State* L)
{
    // nox.setStrength(unit, n): its strength, its client told (MSG_REPORT_STATS).
    server::PlayerControl& pc = *unitArg(L, 1).player;
    pc.strength = u32Arg(L, 2);
    pc.reportStats = true;
    return 0;
}

/// A spell of a player's book: 1-0x88.
std::size_t bookSpellArg(lua_State* L, int idx)
{
    const std::uint32_t s = spellArg(L, idx);
    luaL_argcheck(L, s >= 1 && s < 0x89, idx, "a spell of the book (1-136)");
    return s;
}

int noxSpellLevel(lua_State* L)
{
    server::ServerObject& u = unitArg(L, 1);
    lua_pushnumber(L, u.player->spellLevels[bookSpellArg(L, 2)]);
    return 1;
}

int noxSpells(lua_State* L)
{
    // nox.spells(unit): its book, spell 1-136's level each (0: unknown).
    const server::PlayerControl& pc = *unitArg(L, 1).player;
    lua_createtable(L, 0x88, 0);
    for (int s = 1; s < 0x89; ++s) {
        lua_pushnumber(L, pc.spellLevels[static_cast<std::size_t>(s)]);
        lua_rawseti(L, -2, s);
    }
    return 1;
}

int noxRaiseSpell(lua_State* L)
{
    // nox.raiseSpell(unit, spell): a spell it knows below 5 up a level, its client told
    // (MSG_REPORT_SPELL_AWARD 0x6F: spell, level, 1); whether it rose. (Its group is the caller's:
    // nox.spellInfo's flags.)
    server::ServerObject& u = unitArg(L, 1);
    const std::size_t s = bookSpellArg(L, 2);
    server::PlayerControl& p = *u.player;
    if (p.spellLevels[s] == 0 || 5 <= p.spellLevels[s]) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ++p.spellLevels[s];
    worldOf(L).broadcasts().push_back({{0x6F, static_cast<std::uint8_t>(s), p.spellLevels[s], 1}, true, u.extent});
    lua_pushboolean(L, 1);
    return 1;
}

int noxGrantSpell(lua_State* L)
{
    // nox.grantSpell(unit, spell, level, notify, show): the engine's grant (Spell_GrantToPlayer:
    // up a level, or to `level` when not 0, its group with it; refused at the most); whether it
    // was granted.
    server::ServerObject& u = unitArg(L, 1);
    const std::size_t s = bookSpellArg(L, 2);
    const int level = wholeArg(L, 3, 0, 5, "a level 0-5 (0: up one)");
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    lua_pushboolean(L, server::grantSpell(f.world, *f.things, u, static_cast<int>(s), lua_toboolean(L, 4) != 0, lua_toboolean(L, 5) != 0, level));
    return 1;
}

int noxAbilityLevel(lua_State* L)
{
    server::ServerObject& u = unitArg(L, 1);
    const int a = wholeArg(L, 2, 1, 5, "an ability 1-5");
    lua_pushnumber(L, u.player->abilityLevel(static_cast<std::size_t>(a)));
    return 1;
}

int noxSetAbility(lua_State* L)
{
    // nox.setAbility(unit, ability, level, report, show): its level (its own book's for a multiclass
    // player); with report, MSG_REPORT_ABILITY_AWARD (0xCD) to it, its level with 0x80 when show (the
    // award's flight to the bar, as the engine's grant sends it).
    server::ServerObject& u = unitArg(L, 1);
    const int a = wholeArg(L, 2, 1, 5, "an ability 1-5");
    const int level = wholeArg(L, 3, 0, 5, "a level 0-5");
    u.player->abilityLevel(static_cast<std::size_t>(a)) = static_cast<std::uint8_t>(level);
    if (lua_toboolean(L, 4)) {
        const auto told = static_cast<std::uint8_t>(level | (lua_toboolean(L, 5) ? 0x80 : 0));
        worldOf(L).broadcasts().push_back({{0xCD, static_cast<std::uint8_t>(a), told}, true, u.extent});
    }
    return 0;
}

int noxGrantCreature(lua_State* L)
{
    // nox.grantCreature(unit, creature, notify): Plr_AwardFieldGuide, as a found field guide gives
    // it (its group with it; with notify, it and the others told); whether it was put in.
    server::ServerObject& u = unitArg(L, 1);
    int creature = 0;
    if (lua_isnumber(L, 2)) {
        creature = wholeArg(L, 2, 1, 40, "a guide creature 1-40");
    } else {
        creature = server::guideIndex(luaL_checkstring(L, 2));
        luaL_argcheck(L, creature != 0, 2, "a field guide creature's type (or its number 1-40)");
    }
    if (u.player->fieldGuide[static_cast<std::size_t>(creature)]) {
        lua_pushboolean(L, 0); // (known: Plr_AwardFieldGuide would tell nothing and give nothing)
        return 1;
    }
    lua_pushboolean(L, server::awardFieldGuide(worldOf(L), u, creature, lua_toboolean(L, 3) != 0) ? 1 : 0);
    return 1;
}

int noxSpellId(lua_State* L)
{
    // nox.spellId(name): a spell's number from its name; nil for none.
    if (const auto id = things::spellId(luaL_checkstring(L, 1))) {
        lua_pushnumber(L, *id);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int noxGuideNumber(lua_State* L)
{
    // nox.guideNumber(name): a creature's number in the field guide (1-40) from its type's name,
    // as nox.grantCreature reads one; nil for none.
    const int creature = server::guideIndex(luaL_checkstring(L, 1));
    if (creature >= 1 && creature <= 40) {
        lua_pushnumber(L, creature);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int noxFieldGuide(lua_State* L)
{
    // nox.fieldGuide(unit): its field guide, creature 1-40 each whether it has it.
    const server::PlayerControl& pc = *unitArg(L, 1).player;
    lua_createtable(L, static_cast<int>(pc.fieldGuide.size()) - 1, 0);
    for (std::size_t g = 1; g < pc.fieldGuide.size(); ++g) {
        lua_pushboolean(L, pc.fieldGuide[g] ? 1 : 0);
        lua_rawseti(L, -2, static_cast<int>(g));
    }
    return 1;
}

int noxClearBook(lua_State* L)
{
    // nox.clearBook(unit): its spells, abilities and field guide all none (no messages: nox.revoke
    // tells its client).
    server::PlayerControl& pc = *unitArg(L, 1).player;
    pc.spellLevels.fill(0);
    pc.abilityLevels.fill(0);
    pc.fieldGuide.fill(false);
    return 0;
}

int noxRevoke(lua_State* L)
{
    // nox.revoke(playerName, kind, id): a spell (0x11), ability (0x12) or field guide creature
    // (0x13) taken off that player's client (MSG_GAUNTLET), queued: nox.sendRevokes sends 30 a frame.
    Impl& impl = implOf(L);
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len <= 64, 1, "a player's name");
    const int kind = wholeArg(L, 2, 0x11, 0x13, "0x11 a spell, 0x12 an ability, 0x13 a field guide's creature");
    const int what = wholeArg(L, 3, 0, 65535, "its number 0-65535");
    auto& owed = impl.revokes[std::string(name, len)];
    luaL_argcheck(L, owed.size() < 4096, 1, "too many revokes owed");
    owed.push_back({0xF0, static_cast<std::uint8_t>(kind), static_cast<std::uint8_t>(what), static_cast<std::uint8_t>(what >> 8)});
    return 0;
}

int noxSendRevokes(lua_State* L)
{
    // nox.sendRevokes(): once a frame (a second call in the frame sends nothing), up to 30 of each
    // player's revokes to it, oldest first, by name; those of one gone dropped, those of one mid
    // map change (not in, or not ready) kept. How many are still owed after it (0: none).
    Impl& impl = implOf(L);
    server::World& world = worldOf(L);
    const auto owedNow = [&] {
        std::size_t n = 0;
        for (const auto& [name, owed] : impl.revokes) {
            n += owed.size();
        }
        lua_pushnumber(L, static_cast<double>(n));
        return 1;
    };
    if (world.frameNow == impl.revokedAt) {
        return owedNow();
    }
    impl.revokedAt = world.frameNow;
    for (auto it = impl.revokes.begin(); it != impl.revokes.end();) {
        const auto who = std::ranges::find_if(impl.host->players(), [&](const auto& e) { return e.second.name == it->first && !impl.host->refused(e.first); });
        if (who == impl.host->players().end()) {
            it = impl.revokes.erase(it);
            continue;
        }
        const server::ServerObject* unit = world.byExtent(who->second.extent);
        if (!unit || !who->second.ready) {
            ++it;
            continue;
        }
        auto& owed = it->second;
        const std::size_t n = std::min(Impl::kRevokesPerFrame, owed.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& m = owed[i];
            world.broadcasts().push_back({{m.begin(), m.end()}, true, unit->extent});
        }
        owed.erase(owed.begin(), owed.begin() + static_cast<std::ptrdiff_t>(n));
        it = owed.empty() ? impl.revokes.erase(it) : std::next(it);
    }
    return owedNow();
}

int noxStatus(lua_State* L)
{
    // nox.status(unit): its status flags (1 observing, 0x20 eliminated, 0x400 poisoned...).
    lua_pushnumber(L, unitArg(L, 1).player->statusFlags);
    return 1;
}

int noxGameOverAt(lua_State* L)
{
    // nox.gameOverAt(unit): the frame its quest's lives ran out (0: never).
    lua_pushnumber(L, unitArg(L, 1).player->questGameOverAt);
    return 1;
}

int noxSetGameOverAt(lua_State* L)
{
    server::ServerObject& u = unitArg(L, 1);
    u.player->questGameOverAt = u32Arg(L, 2);
    return 0;
}

int noxExitGate(lua_State* L)
{
    // nox.exitGate(unit): the quest exit it went out by (an id; 0 none).
    lua_pushnumber(L, unitArg(L, 1).player->exitGate);
    return 1;
}

int noxRewards(lua_State* L)
{
    // nox.rewards(holder, stage, count): a boss's loot (giveRewards: a fresh RewardMarker of every
    // kind activated `count` times at `stage`, each item into the holder).
    server::ServerObject& o = objectArg(L, 1);
    const int stage = wholeArg(L, 2, 0, 1000, "a stage 0-1000");
    const int count = wholeArg(L, 3, 0, 64, "a count 0-64");
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    server::giveRewards(f, o, static_cast<std::uint32_t>(stage), count);
    return 0;
}

int noxRewardAt(lua_State* L)
{
    // nox.rewardAt(x, y, stage, kinds): what a fresh RewardMarker (its kinds: thing.bin's 8 bits,
    // nil all) gives at `stage`, made at (x, y) (server_rewardgen_activateMarker); its id, nil for
    // nothing.
    const float x = coordArg(L, 1);
    const float y = coordArg(L, 2);
    const int stage = wholeArg(L, 3, 0, 1000, "a stage 0-1000");
    const int kinds = lua_isnoneornil(L, 4) ? -1 : wholeArg(L, 4, 0, 255, "its kinds (bits 0-0xFF)");
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    server::ServerObject marker;
    marker.typeName = "RewardMarker";
    marker.rewardMarker.emplace();
    if (kinds >= 0) {
        marker.rewardMarker->kinds = static_cast<std::uint32_t>(kinds);
    }
    if (const server::ServerObject* made = server::activateRewardMarker(f, marker, static_cast<std::uint32_t>(stage), x, y)) {
        lua_pushnumber(L, made->extent);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int noxSetOnOff(lua_State* L)
{
    // nox.setOnOff(id, on): switched on or off as the engine does (ENABLED, its clients told:
    // MSG_DISABLE_OBJECT / MSG_ENABLE_OBJECT).
    server::setOnOff(objectArg(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}

int noxUnitSay(lua_State* L)
{
    // nox.unitSay(id, text, frames): a bubble over it every client draws (World::unitSay: its
    // bytes each a character, 250 at most), shown that many frames.
    const server::ServerObject& o = objectArg(L, 1);
    size_t len = 0;
    const char* s = luaL_checklstring(L, 2, &len);
    const int frames = wholeArg(L, 3, 0, 65535, "frames 0-65535");
    const std::u16string text(s, s + std::min<size_t>(len, 250));
    worldOf(L).unitSay(o, text, static_cast<std::uint16_t>(frames));
    return 0;
}

int noxSetServerName(lua_State* L)
{
    // nox.setServerName(name): the game's listed name (15 letters at most, gameSetServername's).
    worldOf(L);
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len <= 255, 1, "a name");
    implOf(L).host->setServerName(std::string(s, len));
    return 0;
}

int noxQuestStage(lua_State* L)
{
    lua_pushnumber(L, worldOf(L).questStage);
    return 1;
}

int noxExitTo(lua_State* L)
{
    // nox.exitTo(map): the next map the game goes to ("Name.map": letters, digits, _ and -).
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    const std::string_view name(s, len);
    luaL_argcheck(L, name.size() > 4 && name.size() <= 36 && name.ends_with(".map") && std::ranges::all_of(name.substr(0, name.size() - 4), [](char c) {
                         return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
                     }),
                  1, "a map's name (Name.map: letters, digits, _ and -)");
    worldOf(L).exitTo = std::string(name);
    return 0;
}

int noxLock(lua_State* L)
{
    // nox.lock(): the game closed, its allowed list everyone in it now (the host's own Closed and
    // list kept for nox.unlock). Once: a second lock changes nothing.
    Impl& impl = implOf(L);
    worldOf(L);
    if (!impl.locked) {
        server::detail::lockGame(*impl.host, impl.savedClosed, impl.savedAllowed);
        impl.locked = true;
    }
    return 0;
}

int noxUnlock(lua_State* L)
{
    // nox.unlock(): the host's own Closed and allowed list back (nothing when not locked).
    Impl& impl = implOf(L);
    worldOf(L);
    if (impl.locked) {
        server::detail::unlockGame(*impl.host, impl.savedClosed, impl.savedAllowed);
        impl.locked = false;
    }
    return 0;
}

int noxAllow(lua_State* L)
{
    // nox.allow(name, on): a name added to the game's allowed list (on; a locked game lets it in),
    // or every entry of it taken off.
    Impl& impl = implOf(L);
    worldOf(L);
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len >= 1 && len <= 64, 1, "a player's name");
    server::GameHost::AccessSettings a = impl.host->accessSettings();
    const std::string name(s, len);
    if (lua_toboolean(L, 2)) {
        luaL_argcheck(L, a.allowed.size() < 256, 1, "the allowed list is full");
        a.allowed.push_back(name);
    } else {
        std::erase(a.allowed, name);
    }
    impl.host->setAccessSettings(a);
    return 0;
}

int noxIsEnemy(lua_State* L)
{
    // nox.isEnemy(a, b): whether a is hostile to b (Obj_IsEnemyTo).
    const server::ServerObject& a = objectArg(L, 1);
    const server::ServerObject& b = objectArg(L, 2);
    lua_pushboolean(L, server::isEnemy(worldOf(L), a, b));
    return 1;
}

int noxChests(lua_State* L)
{
    // nox.chests(): the empty chests on the map (things whose INIT is ChestInit, holding nothing,
    // not held), by id: a set-up's call (a whole-world walk).
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    lua_newtable(L);
    int n = 0;
    for (const auto& [extent, o] : f.world.objects()) {
        if (o.gone() || o.holder != 0) {
            continue;
        }
        const things::ThingDef* def = f.things->findThing(o.typeName);
        if (def && def->attribute("INIT").value_or("") == "ChestInit" && o.inventory.empty()) {
            lua_pushnumber(L, extent);
            lua_rawseti(L, -2, ++n);
        }
    }
    return 1;
}

int noxGiveItem(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.giveItem"); // (not inside onDamage or onExit)
    // nox.giveItem(unit, type, wear, spec): a new item of that type into its inventory, worn if it may
    // be, told to every client (Items::give); with wear, a weapon or armour not worn yet then put on
    // (Obj_PlayerEquipWeapon / ...Armor, told to everyone). Its id; nil for a type thing.bin hasn't.
    // spec (mod_items.hpp): its modifiers, durability and charges, set before anyone hears of it.
    server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    const char* type = luaL_checkstring(L, 2);
    const bool wear = lua_toboolean(L, 3) != 0;
    const ItemSpec spec = itemSpecArg(L, 4);
    server::Fight f = fightOf(L);
    luaL_argcheck(L, f.things != nullptr, 1, "no things");
    if (!f.things->findThing(type)) {
        lua_pushnil(L);
        return 1;
    }
    server::Items items = implOf(L).host->server().items();
    std::string refused;
    server::ServerObject* it = !spec.any() ? items.give(u, type, {0xFF, 0xFF, 0xFF, 0xFF}, true)
                                           : items.give(u, type, spec.mods, true, [&](server::ServerObject& item) {
                                                 return finishItem(L, item, spec, refused);
                                             });
    if (!refused.empty()) {
        luaL_error(L, "nox.giveItem: %s", refused.c_str());
    }
    if (it && wear && !(it->flags & server::ObjFlag::Equipped)) {
        if (it->classes & things::ObjectClass::Weapon) {
            items.equipWeapon(u, *it, true);
        } else if (it->classes & things::ObjectClass::Armor) {
            items.equipArmor(u, *it, true);
        }
    }
    if (it) {
        lua_pushnumber(L, it->extent);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int noxCreateTeam(lua_State* L)
{
    // nox.createTeam(name, colour): a new team (the lowest free id), its name and colour (1-16),
    // every client told (MSG_TEAM_MSG); its id, nil when there's no room.
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    const int colour = wholeArg(L, 2, 1, 16, "a colour 1-16");
    server::World& world = worldOf(L);
    server::ServerTeam* t = server::createTeam(world, 0);
    if (!t) {
        lua_pushnil(L);
        return 1;
    }
    t->color = static_cast<std::uint8_t>(colour);
    server::setTeamName(*t, std::u16string(s, s + std::min<size_t>(len, 250)), false);
    world.broadcasts().push_back({server::teamMessage(world, *t), true, 0});
    lua_pushnumber(L, t->id);
    return 1;
}

/// A team now in the game, by id.
server::ServerTeam& teamArg(lua_State* L, int idx)
{
    const int id = wholeArg(L, idx, 1, 255, "a team's id");
    server::ServerTeam* t = server::teamById(worldOf(L), static_cast<std::uint8_t>(id));
    luaL_argcheck(L, t != nullptr, idx, "no such team");
    return *t;
}

int noxJoinTeam(lua_State* L)
{
    // nox.joinTeam(id, team): onto that team (Team_AddMember, every client told; not moved to its
    // base).
    server::ServerObject& o = objectArg(L, 1);
    const server::ServerTeam& t = teamArg(L, 2);
    server::Fight f = fightOf(L);
    std::minstd_rand trng(f.frame);
    server::addTeamMember(f.world, t.id, o, true, false, trng, f.frame);
    return 0;
}

int noxRemoveTeam(lua_State* L)
{
    // nox.removeTeam(team): the team gone, its members with none, every client told.
    server::removeTeam(worldOf(L), teamArg(L, 1), true);
    return 0;
}

int noxGameplay(lua_State* L)
{
    // nox.gameplay() -> {teamDamage, autoAssign, teamPlay}: the gameplay flags 1, 2 and 4 as they
    // stand (the host's team options; mode.json's "gameplay" pins them).
    const std::uint32_t f = worldOf(L).gameplayFlags;
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, (f & 1u) != 0);
    lua_setfield(L, -2, "teamDamage");
    lua_pushboolean(L, (f & 2u) != 0);
    lua_setfield(L, -2, "autoAssign");
    lua_pushboolean(L, (f & 4u) != 0);
    lua_setfield(L, -2, "teamPlay");
    return 1;
}

// ---- teams and scores (the stock messages: MSG_REPORT_LESSON, MSG_TEAM_MSG's score)

/// A team's players, as teamPlayers counts them, from its members rather than the whole world:
/// player units (not gone) of its id among the members of the first active team of that id
/// (inTeam's), each unit once.
int teamPlayerCount(const Impl& impl, server::World& world, const server::ServerTeam& team)
{
    if (team.id == 0) {
        return 0;
    }
    const auto first = std::ranges::find_if(world.teams, [&](const server::ServerTeam& t) { return t.active && t.id == team.id; });
    if (first == world.teams.end()) {
        return 0;
    }
    const std::vector<std::uint32_t>& members = first->members;
    int n = 0;
    for (auto it = members.begin(); it != members.end(); ++it) {
        if (std::find(members.begin(), it, *it) != it) {
            continue; // (a unit listed twice counted once)
        }
        const server::ServerObject* o = world.byExtent(*it);
        n += o && o->player && o->team == team.id && !impl.hidden(*o) ? 1 : 0;
    }
    return n;
}

int noxTeams(lua_State* L)
{
    // The teams now: {id, name, score, colour, players} each.
    server::World& world = worldOf(L);
    lua_newtable(L);
    int n = 0;
    for (server::ServerTeam& t : world.teams) {
        if (!t.active) {
            continue;
        }
        lua_createtable(L, 0, 5);
        setField(L, "id", t.id);
        const std::string name = strings::toUtf8(t.name);
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "name");
        setField(L, "score", t.lessons);
        setField(L, "colour", t.color);
        setField(L, "players", teamPlayerCount(implOf(L), world, t));
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int noxTeamScore(lua_State* L)
{
    // A team's score set, told to everyone (as a capture's, a kill's).
    const int id = luaL_checkinteger(L, 1);
    server::World& world = worldOf(L);
    server::ServerTeam* t = id > 0 && id < 256 ? server::teamById(world, static_cast<std::uint8_t>(id)) : nullptr;
    luaL_argcheck(L, t != nullptr, 1, "no such team");
    server::setTeamScore(world, *t, luaL_checkinteger(L, 2));
    return 0;
}

int noxChangeLessons(lua_State* L)
{
    // A player's lessons up or down, told to everyone; its lessons now.
    server::ServerObject& u = unitArg(L, 1);
    server::changeLessons(u, luaL_checkinteger(L, 2));
    server::reportLesson(worldOf(L), u);
    lua_pushnumber(L, u.player->lessons);
    return 1;
}

int noxAddDeath(lua_State* L)
{
    // A player's deaths up (the game's own count, as a kill's victim), told to everyone.
    server::ServerObject& u = unitArg(L, 1);
    server::Fight f = fightOf(L);
    server::addDeath(f, u);
    server::reportLesson(f.world, u);
    return 0;
}

int noxAnyonePlaying(lua_State* L)
{
    // More than one player (in team play, team) in play: the game's test before anything scores.
    lua_pushboolean(L, server::anyonePlaying(worldOf(L)));
    return 1;
}

int noxLimits(lua_State* L)
{
    // The limits the host chose for the stock mode it plays as (mode.json's looksLike): lessons
    // (a score to win at; 0 none) and minutes (0 none), the ones clients are told (the console's
    // "set lessons" and "set time" change both). The game doesn't act on them: the script does
    // (World::modeOwnsLimits). A mode that plays as no stock one has none: 0, 0.
    const Impl& impl = implOf(L);
    const server::World& world = worldOf(L);
    const bool looks = impl.package->manifest.looksLike != 0;
    lua_pushnumber(L, looks ? server::lessonLimit(world) : 0);
    lua_pushnumber(L, looks ? server::timeLimit(world) : 0);
    return 2;
}

// ---- the stock protocol for what a mode tells (MSG_INFORM, the flags' status, the timer, the
// winner)

int noxInform(lua_State* L)
{
    // MSG_INFORM to everyone: its code and u32s (the flags' events 4-8 among the game's own). Only
    // a notice the game has, with exactly the u32s it carries: every client frames it by its code.
    const int code = luaL_checkinteger(L, 1);
    luaL_argcheck(L, code >= 0 && code <= 255, 1, "a code 0-255");
    const net::SubOpInfo* info = net::subOpInfo(net::Op::Inform, static_cast<std::uint8_t>(code));
    const int given = lua_gettop(L) - 1;
    luaL_argcheck(L, info && info->serverToClient != net::kVariableSize && info->serverToClient >= 2 && (info->serverToClient - 2) % 4 == 0, 1,
                  "a notice the game has, made of u32s (a string's: nox.informString)");
    if (given != (info->serverToClient - 2) / 4) {
        luaL_error(L, "inform %d carries %d numbers, not %d", code, (info->serverToClient - 2) / 4, given);
    }
    std::vector<std::uint8_t> m = {0xA9, static_cast<std::uint8_t>(code)};
    const int n = 1 + given;
    for (int i = 2; i <= n; ++i) {
        const double d = luaL_checknumber(L, i);
        luaL_argcheck(L, d >= 0 && d <= 4294967295.0 && d == std::floor(d), i, "a whole number 0 to 2^32 - 1");
        const auto v = static_cast<std::uint32_t>(d);
        for (int b = 0; b < 4; ++b) {
            m.push_back(static_cast<std::uint8_t>(v >> (8 * b)));
        }
    }
    server::World& world = worldOf(L);
    world.broadcasts().push_back({std::move(m), true, 0});
    return 0;
}

int noxInformString(lua_State* L)
{
    // One of the game's lines (a string id: "objcoll.c:FlagNoTeam") to one unit's player.
    server::ServerObject& u = unitArg(L, 1);
    worldOf(L).broadcasts().push_back({server::informStringBytes(luaL_checkstring(L, 2)), true, u.extent});
    return 0;
}

int noxFlagStatus(lua_State* L)
{
    // MSG_REPORT_FLAG_STATUS to everyone (and the world's record of it): a team's flag 0 home,
    // 1 carried, 2 dropped; its colour; its carrier's net code (0 none).
    const int team = luaL_checkinteger(L, 1);
    luaL_argcheck(L, team >= 0 && team < static_cast<int>(worldOf(L).flagStatus.size()), 1, "no such team");
    server::flagStatus(worldOf(L), static_cast<std::uint8_t>(team), static_cast<std::uint8_t>(luaL_checkinteger(L, 2)),
                       static_cast<std::uint8_t>(luaL_checkinteger(L, 3)), static_cast<std::uint16_t>(luaL_optinteger(L, 4, 0)));
    return 0;
}

int noxTimer(lua_State* L)
{
    // nox.timer(): running, ms left. nox.timer(seconds): the match timer started at that, told to
    // everyone (the stock clock on every HUD); nox.timer(false): stopped.
    server::Fight f = fightOf(L);
    server::World& world = f.world;
    if (lua_gettop(L) >= 1) {
        if (lua_isboolean(L, 1) && !lua_toboolean(L, 1)) {
            server::setTimerRunning(world, false, f.frame);
        } else {
            const double seconds = luaL_checknumber(L, 1);
            luaL_argcheck(L, seconds >= 0 && seconds <= 24 * 3600, 1, "0-24 hours");
            world.timerEnd = f.frame + static_cast<std::uint32_t>(seconds * 30.0);
            server::setTimerRunning(world, true, f.frame);
        }
        return 0;
    }
    lua_pushboolean(L, world.timerRunning);
    lua_pushnumber(L, world.timerRunning ? (static_cast<double>(world.timerEnd) - static_cast<double>(f.frame)) * 1000.0 / 30.0 : 0.0);
    return 2;
}

int noxEndMatch(lua_State* L)
{
    // The match over (the game's game over: nothing moves for a while, then the next map), its
    // winner told: nox.endMatch(kind, who, byTime). kind "team" (a team id), "player" (a unit),
    // "flagTeam" (a team: CTF's banner), "ballTeam" (flagball's); who 0: nobody (a tie).
    const std::string kind = luaL_checkstring(L, 1);
    server::Fight f = fightOf(L);
    server::World& world = f.world;
    std::uint16_t who = 0;
    std::uint8_t op = 0;
    if (kind == "player") {
        op = 0x58;
        if (!lua_isnoneornil(L, 2) && luaL_checknumber(L, 2) != 0) {
            who = server::netCodeOf(unitArg(L, 2));
        }
    } else {
        op = kind == "team" ? 0x59 : kind == "flagTeam" ? 0x57 : kind == "ballTeam" ? 0x56 : 0;
        luaL_argcheck(L, op != 0, 1, "team, player, flagTeam or ballTeam");
        who = static_cast<std::uint16_t>(luaL_optinteger(L, 2, 0));
    }
    world.gameFlags |= server::GameFlag::GameOver;
    server::winnerMessage(world, op, who, lua_toboolean(L, 3) != 0, f.frame);
    return 0;
}

int noxGameOver(lua_State* L)
{
    // (Still over once its wait ends, till the next map loads: the game flag goes before the load,
    // and a mode checking its limit then would end the match again, holding the load off for good.)
    const Impl& impl = implOf(L);
    bool over = (worldOf(L).gameFlags & server::GameFlag::GameOver) != 0;
    if (!over && impl.host) {
        const server::Server& s = impl.host->server();
        over = s.nextMapDue() || s.mapLoadRequested();
    }
    lua_pushboolean(L, over);
    return 1;
}

int noxOption(lua_State* L)
{
    const auto v = implOf(L).option(luaL_checkstring(L, 1));
    if (v) {
        lua_pushlstring(L, v->data(), v->size());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int noxFrame(lua_State* L)
{
    const Impl& impl = implOf(L);
    lua_pushnumber(L, impl.host ? static_cast<double>(impl.host->server().world().frameNow) : 0.0);
    return 1;
}

int noxRandom(lua_State* L)
{
    // (The mode's own stream, from the host's seed: never the game's draws.)
    Impl& impl = implOf(L);
    const double draw = static_cast<double>(impl.rng() - std::minstd_rand::min()) /
                        (static_cast<double>(std::minstd_rand::max() - std::minstd_rand::min()) + 1.0);
    switch (lua_gettop(L)) {
    case 0:
        lua_pushnumber(L, draw);
        return 1;
    case 1: {
        const int n = luaL_checkinteger(L, 1);
        luaL_argcheck(L, n >= 1, 1, "random: an empty range");
        lua_pushinteger(L, 1 + static_cast<int>(draw * n));
        return 1;
    }
    default: {
        const int lo = luaL_checkinteger(L, 1);
        const int hi = luaL_checkinteger(L, 2);
        luaL_argcheck(L, lo <= hi, 2, "random: an empty range");
        // (In 64 bits: the whole int range is more than an int holds.)
        const std::int64_t span = static_cast<std::int64_t>(hi) - lo + 1;
        lua_pushinteger(L, static_cast<int>(lo + static_cast<std::int64_t>(draw * static_cast<double>(span))));
        return 1;
    }
    }
}

int noxF32(lua_State* L)
{
    // The server's float as its maths makes it, so script maths matches the C++'s when each step
    // goes through f32: towards zero with the host's exact maths on (the exe's FPU chops), else to
    // nearest (VD-37). Worked out by hand from the two floats either side, never from the
    // conversion's own rounding: that's the thread's (a scope's switch around a cast isn't
    // enough, the compiler can convert before it).
    const Impl& impl = implOf(L);
    const bool chop = impl.f32Chop ? *impl.f32Chop : (!impl.host || impl.host->server().exactMaths());
    const double x = luaL_checknumber(L, 1);
    float f = static_cast<float>(x); // (one of the two either side, whatever the rounding)
    if (std::isfinite(x) && std::fabs(static_cast<double>(f)) > std::fabs(x)) {
        f = std::nextafter(f, 0.0f); // (towards zero)
    }
    if (!chop && std::isfinite(x) && static_cast<double>(f) != x) {
        const float away = std::nextafter(f, std::signbit(x) ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity());
        const double toward = std::fabs(x - static_cast<double>(f));
        const double beyond = std::fabs(static_cast<double>(away) - x);
        // (Nearest; a tie to the even one.)
        if (beyond < toward || (beyond == toward && (std::bit_cast<std::uint32_t>(away) & 1u) == 0)) {
            f = away;
        }
    }
    lua_pushnumber(L, f);
    return 1;
}

/// require("name"): the folder's script beside the server script (server/name.luau), run once,
/// its result kept (a module's table).
int noxRequire(lua_State* L)
{
    Impl& impl = implOf(L);
    const std::string name = luaL_checkstring(L, 1);
    luaL_argcheck(L, !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](char c) {
                         return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '/';
                     }) && name.front() != '/',
                  1, "require: a script's name (letters, digits, _ and /)");
    lua_getref(L, impl.requiredRef);
    const int cache = lua_gettop(L);
    lua_rawgetfield(L, cache, name.c_str());
    if (!lua_isnil(L, -1)) {
        if (lua_islightuserdata(L, -1)) {
            luaL_error(L, "require: %s requires itself (a cycle)", name.c_str());
        }
        return 1;
    }
    lua_pop(L, 1);
    const std::string dir = impl.package->manifest.serverScript.substr(0, impl.package->manifest.serverScript.rfind('/') + 1);
    const std::string path = dir + name + ".luau";
    const auto source = impl.package->read(path);
    if (!source) {
        luaL_error(L, "require: no %s", path.c_str());
    }
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    std::size_t size = 0;
    std::unique_ptr<char, decltype(&std::free)> bytecode(luau_compile(source->data(), source->size(), &options, &size), &std::free);
    if (!bytecode) {
        luaL_error(L, "require: %s couldn't be compiled", path.c_str());
    }
    const std::string chunk = "=" + path;
    // (A second chunk into the same globals: their "safe" mark off first, or Luau resolves the
    // module's globals once as it loads and keeps the values they had then.)
    lua_setsafeenv(L, LUA_GLOBALSINDEX, 0);
    if (luau_load(L, chunk.c_str(), bytecode.get(), size, 0) != 0) {
        lua_error(L); // (its message on the stack)
    }
    lua_pushlightuserdata(L, &impl); // (loading: a cycle's mark)
    lua_rawsetfield(L, cache, name.c_str());
    if (lua_pcall(L, 0, 1, 0) != 0) {
        // (Its error: the mark taken back, so a later require tries again, then passed on.)
        lua_pushnil(L);
        lua_rawsetfield(L, cache, name.c_str());
        lua_error(L);
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_rawsetfield(L, cache, name.c_str());
    return 1;
}

} // namespace

std::optional<std::string> Impl::option(std::string_view key) const
{
    if (const auto v = modes::option(ctx.options, key)) {
        return std::string(*v);
    }
    for (const OptionSpec& s : package->manifest.info.options) {
        if (s.key == key) {
            return s.kind == OptionSpec::Kind::Text ? std::nullopt : std::optional<std::string>(std::to_string(s.defaultValue));
        }
    }
    return std::nullopt;
}

void Impl::tell(std::uint8_t peer, std::string_view text)
{
    if (!host) {
        return;
    }
    const auto it = host->players().find(peer);
    if (it == host->players().end() || host->refused(peer)) {
        return; // (none, or a stock player held out: not the mode's)
    }
    server::World& world = host->server().world();
    for (const std::u16string& line : textLines(text)) {
        if (!chargeText(1)) {
            return;
        }
        world.broadcasts().push_back({server::World::textMessage(line, 0), true, it->second.extent});
    }
}

bool Impl::chargeText(int messages)
{
    // (A frame's text messages from the script, a player's each counted: past the budget they're
    // dropped, so a script can't fill the players' reliable queues and push the game's own out.)
    const std::uint32_t frame = host ? host->server().world().frameNow : 0;
    if (frame != textFrame) {
        textFrame = frame;
        textSent = 0;
    }
    if (textSent + messages > kTextPerFrame) {
        if (textSent <= kTextPerFrame) {
            report("over " + std::to_string(kTextPerFrame) + " text messages in a frame: the rest dropped");
            textSent = kTextPerFrame + 1;
        }
        return false;
    }
    textSent += messages;
    return true;
}

void Impl::deliver(lua_State* L, int viewIdx, std::optional<std::uint8_t> peer, std::string_view text)
{
    if (!host) {
        return;
    }
    std::vector<std::uint8_t> view;
    bool encoded = false;
    const auto one = [&](std::uint8_t p) {
        const auto u = ui.find(p);
        if (u != ui.end() && u->second.ready && u->second.views) {
            if (!encoded) {
                std::string why;
                encoded = true;
                if (!encodeValue(L, viewIdx, view, why)) {
                    view.clear();
                }
            }
            if (!view.empty()) {
                // (No reliable room, or views already held: held, in order, for pumpUi.)
                auto& held = u->second.heldViews;
                if (!held.empty() || host->reliableRoom(p) == 0) {
                    held.emplace_back(view, std::string(text));
                    if (held.size() > kHeldViews) {
                        tell(p, held.front().second);
                        held.pop_front();
                    }
                    return;
                }
                sendUi(p, uimsg::kView, view);
                return;
            }
        }
        tell(p, text); // (a client without the UI, or a view that can't be encoded)
    };
    if (peer) {
        one(*peer);
        return;
    }
    std::vector<std::uint8_t> peers;
    for (const auto& [p, hp] : host->players()) {
        if (!host->refused(p)) { // (a stock player held out: not the mode's)
            peers.push_back(p);
        }
    }
    for (const std::uint8_t p : peers) {
        one(p);
    }
}

void Impl::answer(std::uint8_t peer, int index)
{
    const auto it = choices.find(peer);
    if (it == choices.end() || index < 1 || index > it->second) {
        return;
    }
    choices.erase(it);
    if (has.onChoice && begin("onChoice")) {
        pushPlayer(vm->thread(), peer);
        lua_pushinteger(vm->thread(), index);
        finish("onChoice", 2, 0);
    }
}

void Impl::clientReady(std::uint8_t peer)
{
    // A NoX Core player that asked for mode UIs (of this UI's API): the UI sent (pumpUi), once a
    // game.
    if (uiSource.empty() || !uiReaches(peer) || ui.contains(peer)) {
        return;
    }
    ui[peer] = UiPeer{};
}

bool Impl::pumpAssets(std::uint8_t peer, UiPeer& u)
{
    if (u.assetsDone) {
        return true;
    }
    if (!u.assetsBegun) {
        if (u.hasSet && u.hasSet == assetsHash) {
            u.assetsDone = true; // (a reload's: it has these)
            return true;
        }
        sendUi(peer, uimsg::kAssets, wireSet);
        u.assetsBegun = true;
        u.hasSet.reset();
    }
    // (More a frame than the UI's chunks, assets being bigger, and none while it has no reliable
    // room.)
    for (int k = 0; k < kAssetChunksPerFrame && u.asset < wireAssets.size() && host->reliableBulkRoom(peer) > 0; ++k) {
        const WireAsset& a = wireAssets[u.asset];
        if (!u.assetHeaderSent) {
            sendUi(peer, uimsg::kAsset, a.header);
            u.assetHeaderSent = true;
        } else {
            const std::size_t n = std::min(uimsg::kChunkBytes, a.coded.size() - u.assetSent);
            uimsg::Writer w;
            w.u16(static_cast<std::uint16_t>(u.asset));
            w.u32(static_cast<std::uint32_t>(u.assetSent));
            w.out.insert(w.out.end(), a.coded.begin() + static_cast<std::ptrdiff_t>(u.assetSent),
                         a.coded.begin() + static_cast<std::ptrdiff_t>(u.assetSent + n));
            sendUi(peer, uimsg::kAssetChunk, w.out);
            u.assetSent += n;
        }
        if (u.assetSent == a.coded.size()) {
            ++u.asset;
            u.assetHeaderSent = false;
            u.assetSent = 0;
        }
    }
    if (u.asset < wireAssets.size()) {
        return false;
    }
    u.assetsDone = true;
    u.hasSet = assetsHash;
    return true;
}

void Impl::pumpUi()
{
    if (!host || ui.empty()) {
        return;
    }
    for (auto& [peer, u] : ui) {
        // Views held for want of room first, in order, as room comes.
        while (!u.heldViews.empty() && host->reliableRoom(peer) > 0) {
            sendUi(peer, uimsg::kView, u.heldViews.front().first);
            u.heldViews.pop_front();
        }
        if (!u.begun) {
            // (A UI of API 2: its mod's assets first, all of them queued before it.)
            if (assetsHash && !pumpAssets(peer, u)) {
                continue;
            }
            uimsg::Writer w;
            w.u8(static_cast<std::uint8_t>(package->manifest.uiApi)); // (1: kScriptApi, as before)
            w.u32(static_cast<std::uint32_t>(uiSource.size()));
            w.str(package->manifest.info.id);
            w.str(package->manifest.info.name);
            sendUi(peer, uimsg::kBegin, w.out);
            u.begun = true;
        }
        // (A few blocks a frame, and none while it has no reliable room: the game's own messages
        // keep their place.)
        for (int k = 0; k < 4 && u.sent < uiSource.size() && host->reliableBulkRoom(peer) > 0; ++k) {
            const std::size_t n = std::min(uimsg::kChunkBytes, uiSource.size() - u.sent);
            uimsg::Writer w;
            w.u32(static_cast<std::uint32_t>(u.sent));
            w.out.insert(w.out.end(), uiSource.begin() + static_cast<std::ptrdiff_t>(u.sent),
                         uiSource.begin() + static_cast<std::ptrdiff_t>(u.sent + n));
            sendUi(peer, uimsg::kChunk, w.out);
            u.sent += n;
        }
    }
}

void Impl::resendUi()
{
    for (auto& [peer, u] : ui) {
        sendUi(peer, uimsg::kDrop);
        const auto hasSet = u.hasSet; // (the client keeps its set through a drop)
        u = UiPeer{};
        u.hasSet = hasSet;
    }
    if (uiSource.empty()) {
        ui.clear();
    }
}

void Impl::onUiMessage(std::uint8_t peer, std::span<const std::uint8_t> m)
{
    const auto p = uimsg::parse(m);
    const auto u = ui.find(peer);
    if (!p || u == ui.end()) {
        return; // (only from a player we sent the UI)
    }
    const auto& [sub, rest] = *p;
    switch (sub) {
    case uimsg::kReady:
        u->second.ready = true;
        u->second.views = !rest.empty() && rest[0] != 0;
        break;
    case uimsg::kAnswer:
        if (!rest.empty()) {
            answer(peer, rest[0]);
        }
        break;
    case uimsg::kSend: {
        // (20 a second at most from a player reach the script: a client sending more is let go,
        // so a modified one can't hold the server's thread with them.)
        const std::uint32_t frame = host ? host->server().world().frameNow : 0;
        if (frame - u->second.sendsFrom >= 30) {
            u->second.sendsFrom = frame;
            u->second.sends = 0;
        }
        if (++u->second.sends > 20) {
            break;
        }
        if (u->second.ready && has.onUiMessage && begin("onUiMessage")) {
            lua_State* L = vm->thread();
            pushPlayer(L, peer);
            if (!decodeValue(L, rest)) {
                lua_pop(L, 2); // (not a value: let go)
                break;
            }
            finish("onUiMessage", 2, 0);
        }
        break;
    }
    case uimsg::kFailed: {
        uimsg::Reader r{rest};
        const std::string why = r.str();
        if (!std::exchange(u->second.failedLogged, true)) {
            log("a player's UI stopped: " + why.substr(0, 200)); // (once: a client can send it again and again)
        }
        u->second.ready = false;
        u->second.views = false;
        break;
    }
    default: break;
    }
}

void Impl::installToolbox()
{
    lua_State* L = vm->thread();
    static constexpr std::pair<const char*, lua_CFunction> kToolbox[] = {
        {"log", noxLog},       {"players", noxPlayers}, {"player", noxPlayer}, {"say", noxSay},       {"sayAll", noxSayAll},
        {"choice", noxChoice}, {"table", noxTable},     {"option", noxOption}, {"frame", noxFrame}, {"random", noxRandom},
        {"f32", noxF32},       {"send", noxSend},       {"hasUi", noxHasUi},
        // (the world)
        {"object", noxObject}, {"objects", noxObjects}, {"units", noxUnits}, {"inventory", noxInventory},
        {"watchCollide", noxWatchCollide},
        {"take", noxTake},     {"drop", noxDrop},       {"place", noxPlace},     {"clearPointNear", noxClearPointNear},
        {"endSpellEffects", noxEndSpellEffects}, {"minimap", noxMinimap}, {"sound", noxSound},
        // (the engine's paths)
        {"heal", noxHeal},     {"cast", noxCast},       {"enchant", noxEnchant}, {"enchanted", noxEnchanted},
        {"damage", noxDamage}, {"unitsNear", noxUnitsNear}, {"effect", noxEffect}, {"spellSound", noxSpellSound},
        {"countNear", noxCountNear}, {"create", noxCreate}, {"floorAt", noxFloorAt}, {"wallAt", noxWallAt},
        {"cosf", noxCosf},     {"sinf", noxSinf},       {"monsterInfo", noxMonsterInfo}, {"setHealth", noxSetHealth},
        {"touch", noxTouch},   {"sightRange", noxSightRange}, {"textToAll", noxTextToAll},
        {"nearestFoes", noxNearestFoes}, {"applyForce", noxApplyForce}, {"attack", noxAttack}, {"direction", noxDirection},
        {"directionOf", noxDirectionOf}, {"directionVector", noxDirectionVector}, {"newMark", noxNewMark}, {"madeSince", noxMadeSince},
        {"spellInfo", noxSpellInfo}, {"setCharmable", noxSetCharmable}, {"enchantTime", noxEnchantTime}, {"gameNumber", noxGameNumber}, {"traceRay", noxTraceRay},
        {"shoot", noxShoot},   {"summon", noxSummon},   {"abilitySound", noxAbilitySound}, {"setPosition", noxSetPosition},
        {"setVelocity", noxSetVelocity}, {"setOwner", noxSetOwner}, {"setTeam", noxSetTeam}, {"setNoCollide", noxSetNoCollide},
        {"setHitRadius", noxSetHitRadius}, {"setBurstScale", noxSetBurstScale},
        {"damageAt", noxDamageAt}, {"delete", noxDelete},
        {"thingType", noxThingType}, {"present", noxPresent}, {"living", noxLiving}, {"setCollide", noxSetCollide},
        {"backIntoPlay", noxBackIntoPlay}, {"nearSoulGate", noxNearSoulGate},
        {"position", noxPosition}, {"keepPresent", noxKeepPresent}, {"keepLiving", noxKeepLiving}, {"circle", noxCircle},
        {"damageAtEach", noxDamageAtEach}, {"tell", noxTell},
        // (a player's numbers and book, the quest's fields and rewards, access and teams)
        {"gold", noxGold},     {"setGold", noxSetGold}, {"mana", noxMana},       {"setMana", noxSetMana},
        {"setPlayerHealth", noxSetPlayerHealth}, {"strength", noxStrength}, {"setStrength", noxSetStrength},
        {"spellLevel", noxSpellLevel}, {"spells", noxSpells}, {"raiseSpell", noxRaiseSpell}, {"grantSpell", noxGrantSpell},
        {"abilityLevel", noxAbilityLevel}, {"setAbility", noxSetAbility}, {"fieldGuide", noxFieldGuide}, {"clearBook", noxClearBook},
        {"grantCreature", noxGrantCreature}, {"spellId", noxSpellId}, {"guideNumber", noxGuideNumber},
        {"revoke", noxRevoke}, {"sendRevokes", noxSendRevokes}, {"status", noxStatus}, {"gameOverAt", noxGameOverAt},
        {"setGameOverAt", noxSetGameOverAt}, {"exitGate", noxExitGate}, {"rewards", noxRewards}, {"rewardAt", noxRewardAt},
        {"setOnOff", noxSetOnOff}, {"unitSay", noxUnitSay}, {"setServerName", noxSetServerName}, {"questStage", noxQuestStage},
        {"exitTo", noxExitTo}, {"lock", noxLock}, {"unlock", noxUnlock}, {"allow", noxAllow}, {"isEnemy", noxIsEnemy},
        {"chests", noxChests}, {"giveItem", noxGiveItem}, {"createTeam", noxCreateTeam}, {"joinTeam", noxJoinTeam}, {"gameplay", noxGameplay},
        {"removeTeam", noxRemoveTeam}, {"soundAt", noxSoundAt}, {"gameRandom", noxGameRandom},
        // (seeded streams)
        {"mt19937", noxMt19937}, {"shuffle", noxShuffle}, {"mul32", noxMul32},
        // (teams and scores)
        {"teams", noxTeams},   {"teamScore", noxTeamScore}, {"changeLessons", noxChangeLessons}, {"addDeath", noxAddDeath},
        {"anyonePlaying", noxAnyonePlaying}, {"limits", noxLimits},
        // (the stock protocol)
        {"inform", noxInform}, {"informString", noxInformString}, {"flagStatus", noxFlagStatus}, {"timer", noxTimer},
        {"endMatch", noxEndMatch}, {"gameOver", noxGameOver},
    };
    // A stream's methods (nox.mt19937's): stream:next().
    if (luaL_newmetatable(L, kMtType)) {
        lua_createtable(L, 0, 1);
        lua_pushcfunction(L, mtNext, "next");
        lua_setfield(L, -2, "next");
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, "the stream's own");
        lua_setfield(L, -2, "__metatable");
        lua_setreadonly(L, -1, 1);
    }
    lua_pop(L, 1);
    lua_createtable(L, 0, static_cast<int>(std::size(kToolbox)) + 32);
    for (const auto& [name, fn] : kToolbox) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    // The parts in their own files (timers, events, the store, items, players, generation).
    addPartsToolbox(L, !ctx.modData.empty());
    lua_pushinteger(L, kScriptApi);
    lua_setfield(L, -2, "api");
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "nox");
    lua_pushcfunction(L, noxRequire, "require");
    lua_setglobal(L, "require");
    // An owning native mode's own functions (ScriptedMode::addOwnerFunction): `owner`.
    if (!ownerFns.empty()) {
        lua_createtable(L, 0, static_cast<int>(ownerFns.size()));
        for (auto& [name, fn] : ownerFns) {
            lua_pushlightuserdata(L, &fn); // (the map's node: it stays put)
            lua_pushcclosure(L, ownerCall, name.c_str(), 1);
            lua_setfield(L, -2, name.c_str());
        }
        lua_setreadonly(L, -1, 1);
        lua_setglobal(L, "owner");
    }
}

bool Impl::load(std::string& error)
{
    const fs::file_time_type stamp = package->newest();
    const auto source = package->read(package->manifest.serverScript);
    if (!source) {
        error = package->manifest.serverScript + " isn't there (or is over 1 MB)";
        return false;
    }
    std::string ui_;
    if (!package->manifest.uiScript.empty()) {
        const auto uiText = package->read(package->manifest.uiScript);
        if (!uiText || uiText->size() > uimsg::kMaxSource) {
            error = package->manifest.uiScript + " isn't there (or is over the UI's 256 KB)";
            return false;
        }
        ui_ = *uiText;
    }
    std::optional<LoadedAssets> newAssets;
    if (!loadAssets(error, newAssets)) {
        return false;
    }
    // Its own map generator's modules (gen/*.luau), compiled for its builds (a mod with a
    // mapPrefix's).
    std::shared_ptr<const GenModules> genModules;
    if (!package->manifest.info.mapPrefix.empty() && !package->folder.empty()) {
        std::map<std::string, std::string, std::less<>> sources;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(package->folder / "gen", ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == ".luau") {
                const std::string stem = e.path().stem().string();
                const auto text = package->read("gen/" + stem + ".luau");
                if (!text) {
                    error = "gen/" + stem + ".luau is over 1 MB";
                    return false;
                }
                sources.emplace(stem, *text);
            }
        }
        try {
            genModules = compileGenModules(sources, package->manifest.info.id, error);
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!genModules) {
            return false;
        }
    }
    if (!vm) {
        vm = std::make_unique<LuauVm>(limits);
        vm->setOwner(this);
    } else {
        vm->freshThread();
    }
    lua_State* L = vm->thread();
    installToolbox();
    const int oldRequired = requiredRef;
    lua_newtable(L);
    requiredRef = lua_ref(L, -1);
    lua_pop(L, 1);
    // (The old code's weather set aside while the new code runs: back if it fails; else none to
    // whoever had it, before anything the new code sets.)
    Weather oldWeather = std::exchange(parts.weather, Weather{});
    if (!vm->run(package->manifest.serverScript, *source, 1, error)) {
        lua_unref(L, requiredRef);
        requiredRef = oldRequired;
        parts.weather = std::move(oldWeather);
        return false;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_unref(L, requiredRef);
        requiredRef = oldRequired;
        error = package->manifest.serverScript + " must return its mode's table";
        parts.weather = std::move(oldWeather);
        return false;
    }
    oldWeather.reset(host);
    if (modeRef) {
        lua_unref(L, modeRef);
    }
    if (oldRequired) {
        lua_unref(L, oldRequired);
    }
    modeRef = lua_ref(L, -1);
    for (RuleRef& r : rules) { // (the rules looked up already: the new code's)
        if (r.ref) {
            lua_unref(L, r.ref);
        }
        r.ref = resolve(r.name);
    }
    const auto hasFn = [&](const char* name) {
        lua_rawgetfield(L, -1, name);
        const bool f = lua_isfunction(L, -1);
        lua_pop(L, 1);
        return f;
    };
    has = {.configure = hasFn("configure"), .start = hasFn("start"), .onJoin = hasFn("onJoin"), .onLeave = hasFn("onLeave"),
           .onChat = hasFn("onChat"), .onChoice = hasFn("onChoice"), .tick = hasFn("tick"), .console = hasFn("console"),
           .save = hasFn("save"), .restore = hasFn("restore"), .stop = hasFn("stop"), .onUiMessage = hasFn("onUiMessage"),
           .onKill = hasFn("onKill"), .respawnPoint = hasFn("respawnPoint"), .collide = hasFn("collide"), .mapLoaded = hasFn("mapLoaded"),
           .playerReady = hasFn("playerReady")};
    parts.events.look(L);
    lua_pop(L, 1);
    // The old code's timers and recorded events go with it; its store locks too.
    parts.scheduler.clear(L);
    parts.events.clear();
    if (parts.store) {
        parts.store->unlockAll(parts.storeOwner);
    }
    parts.generation.setModules(std::move(genModules));
    loadedAt = stamp;
    // Its UI, as NoX Core players get it: sent again to each when it (or its assets) changed.
    const bool assetsChanged = newAssets && keepAssets(std::move(*newAssets));
    const std::string oldUi = std::exchange(uiSource, std::move(ui_));
    if (uiSource != oldUi || assetsChanged) {
        resendUi();
    }
    return true;
}

bool Impl::keepAssets(LoadedAssets&& a)
{
    assetsAt = a.at;
    if (!a.changed) {
        return false;
    }
    assetsHash = a.hash;
    wireSet = std::move(a.wireSet);
    wireAssets = std::move(a.wireAssets);
    return true;
}

bool Impl::loadAssets(std::string& error, std::optional<LoadedAssets>& out)
{
    if (package->manifest.uiApi < 2 || package->manifest.uiScript.empty() || package->source || package->folder.empty()) {
        return true; // (only a UI of API 2 has assets; a built-in mode none)
    }
    const fs::path dir = package->folder / "assets";
    const fs::file_time_type stamp = newestAsset(dir);
    if (assetsHash && stamp == assetsAt) {
        return true;
    }
    // (Decoded here only to be packed: the decoded set goes once its wire form is made.)
    ModAssetsResult r = loadModAssets(dir);
    if (!r.assets) {
        error = r.error;
        return false;
    }
    const std::shared_ptr<const ModAssets> now = std::move(r.assets);
    out.emplace();
    out->at = stamp;
    out->hash = now->hash;
    if (assetsHash && *assetsHash == now->hash) {
        return true; // (touched, not changed)
    }
    // As the wire carries them (ui_proto.hpp): each asset's bytes NXZ-packed once, for everyone.
    uimsg::Writer set;
    set.u16(static_cast<std::uint16_t>(now->list.size()));
    set.u32(static_cast<std::uint32_t>(now->imageBytes));
    set.u32(static_cast<std::uint32_t>(now->soundBytes));
    std::vector<WireAsset> wire;
    wire.reserve(now->list.size());
    for (std::size_t i = 0; i < now->list.size(); ++i) {
        const ModAsset& a = now->list[i];
        std::vector<std::uint8_t> raw;
        uimsg::Writer h;
        h.u16(static_cast<std::uint16_t>(i));
        h.u8(static_cast<std::uint8_t>(a.kind));
        h.str(a.name);
        if (a.kind == ModAsset::Kind::Image) {
            h.u16(static_cast<std::uint16_t>(a.width));
            h.u16(static_cast<std::uint16_t>(a.height));
            raw = a.rgba;
        } else {
            h.u32(a.rate);
            h.u8(a.channels);
            h.u32(static_cast<std::uint32_t>(a.samples.size() / a.channels));
            raw.reserve(a.samples.size() * 2);
            for (const std::int16_t s : a.samples) {
                raw.push_back(static_cast<std::uint8_t>(s));
                raw.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(s) >> 8));
            }
        }
        WireAsset w;
        std::vector<std::uint8_t> packed = video::nxzCompress(raw);
        const bool nxz = packed.size() < raw.size();
        w.coded = nxz ? std::move(packed) : std::move(raw);
        h.u8(nxz ? 1 : 0);
        h.u32(static_cast<std::uint32_t>(w.coded.size()));
        w.header = std::move(h.out);
        wire.push_back(std::move(w));
    }
    out->wireSet = std::move(set.out);
    out->wireAssets = std::move(wire);
    out->changed = true;
    return true;
}

void Impl::onJoin(std::uint8_t peer)
{
    if (has.onJoin && begin("onJoin")) {
        pushPlayer(vm->thread(), peer);
        finish("onJoin", 1, 0);
    }
}

void Impl::playerReady(std::uint8_t peer)
{
    if (begin("playerReady")) {
        pushPlayer(vm->thread(), peer);
        finish("playerReady", 1, 0);
    }
}

void Impl::onLeave(std::uint8_t peer)
{
    choices.erase(peer);
    ui.erase(peer);
    parts.weather.left(peer);
    parts.drawScales.left(peer);
    if (has.onLeave && begin("onLeave")) {
        pushPlayer(vm->thread(), peer);
        finish("onLeave", 1, 0);
    }
}

bool Impl::onChat(std::uint8_t peer, std::u16string_view text)
{
    // An open choice's answer ("2"): taken, not relayed.
    if (const auto it = choices.find(peer); it != choices.end() && text.size() == 1 && text[0] >= u'1' && text[0] <= u'9' &&
                                            text[0] - u'0' <= it->second) {
        answer(peer, text[0] - u'0');
        return true;
    }
    if (!has.onChat || !begin("onChat")) {
        return false;
    }
    lua_State* L = vm->thread();
    pushPlayer(L, peer);
    const std::string utf8 = strings::toUtf8(text);
    lua_pushlstring(L, utf8.data(), utf8.size());
    if (!finish("onChat", 2, 1)) {
        return false;
    }
    const bool taken = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return taken;
}

// ------------------------------------------------------------------------- the engine's hooks

/// The engine's hooks inside the frame, each the script's: onKill(victim, killer, assist) after
/// a player's death (unit ids; nil none), respawnPoint(unit) -> x, y (nil: the game's start),
/// collide(thing, other) for a thing it watches (nox.watchCollide), mapLoaded(name) at each map's
/// load (the map's name; a script that takes none ignores it). The rest stay the game's
/// (ModeHooks' defaults).
class ScriptedMode::Impl::EngineHooks final : public server::ModeHooks {
public:
    explicit EngineHooks(Impl& impl) : impl_(impl) {}

    void mapLoaded(server::Server& server) override
    {
        if (impl_.has.mapLoaded) {
            impl_.guarded("mapLoaded", [&] {
                if (impl_.begin("mapLoaded")) {
                    const std::string& name = server.world().mapName;
                    lua_pushlstring(impl_.vm->thread(), name.data(), name.size());
                    impl_.finish("mapLoaded", 1, 0);
                }
            });
        }
    }

    void playerKilled(server::Fight&, server::ServerObject& victim, server::ServerObject* killer, server::ServerObject* assist) override
    {
        if (!impl_.has.onKill) {
            return;
        }
        impl_.guarded("onKill", [&] {
            if (!impl_.begin("onKill")) {
                return;
            }
            lua_State* L = impl_.vm->thread();
            lua_pushnumber(L, victim.extent);
            killer ? lua_pushnumber(L, killer->extent) : lua_pushnil(L);
            assist ? lua_pushnumber(L, assist->extent) : lua_pushnil(L);
            impl_.finish("onKill", 3, 0);
        });
    }

    std::optional<std::pair<float, float>> respawnPoint(server::Fight&, server::ServerObject& player) override
    {
        if (!impl_.has.respawnPoint) {
            return std::nullopt;
        }
        return impl_.guarded("respawnPoint", [&]() -> std::optional<std::pair<float, float>> {
            if (!impl_.begin("respawnPoint")) {
                return std::nullopt;
            }
            lua_State* L = impl_.vm->thread();
            lua_pushnumber(L, player.extent);
            if (!impl_.finish("respawnPoint", 1, 2)) {
                return std::nullopt;
            }
            std::optional<std::pair<float, float>> at;
            if (lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
                const double x = lua_tonumber(L, -2), y = lua_tonumber(L, -1);
                if (x >= 0 && x <= kMapExtent && y >= 0 && y <= kMapExtent) {
                    at = std::pair{static_cast<float>(x), static_cast<float>(y)};
                } else {
                    impl_.report("respawnPoint: a point off the map (the game's start used)");
                }
            }
            lua_pop(L, 2);
            return at;
        });
    }

    void collide(server::Fight&, server::ServerObject& object, server::ServerObject* other) override
    {
        if (!impl_.has.collide) {
            return;
        }
        impl_.guarded("collide", [&] {
            if (!impl_.begin("collide")) {
                return;
            }
            lua_State* L = impl_.vm->thread();
            lua_pushnumber(L, object.extent);
            other ? lua_pushnumber(L, other->extent) : lua_pushnil(L);
            impl_.finish("collide", 2, 0);
        });
    }

    // ---- The parts' (mod_events.hpp): the told ones recorded, the asked ones at once. A held-out
    // stock player (and its unit) is never in them.

    void objectKilled(server::Fight&, server::ServerObject& victim, server::ServerObject* by) override
    {
        impl_.parts.events.killed(victim, by && !impl_.hidden(*by) ? by : nullptr);
    }
    void destroyed(server::World&, server::ServerObject& object) override { impl_.parts.events.destroyed(object); }
    void damaged(server::Fight&, server::ServerObject& target, int amount) override
    {
        if (!impl_.hidden(target)) {
            impl_.parts.events.damaged(target, amount);
        }
    }
    void pickedUp(server::World&, server::ServerObject& taker, server::ServerObject& item, float x, float y) override
    {
        if (!impl_.hidden(taker)) {
            impl_.parts.events.pickedUp(taker, item, x, y);
        }
    }
    void dropped(server::World&, server::ServerObject& holder, server::ServerObject& item) override
    {
        if (!impl_.hidden(holder)) {
            impl_.parts.events.dropped(holder, item);
        }
    }
    void equipped(server::World&, server::ServerObject& player, server::ServerObject& item, bool worn) override
    {
        if (!impl_.hidden(player)) {
            impl_.parts.events.equipped(player, item, worn);
        }
    }
    void spellCast(server::World&, server::ServerObject& caster, std::uint32_t spell, server::ServerObject* target, float x, float y,
                   std::span<const std::uint32_t> made) override
    {
        if (!impl_.hidden(caster)) {
            impl_.parts.events.spellCast(caster, spell, target && !impl_.hidden(*target) ? target : nullptr, x, y, made);
        }
    }
    bool damage(server::Fight&, server::ServerObject& target, server::ServerObject* source, server::ServerObject* weapon, int& amount,
                int type) override
    {
        if (!impl_.parts.events.has().onDamage || impl_.hidden(target) || (source && impl_.hidden(*source))) {
            return true;
        }
        bool hit = true; // (the game's own, whatever goes wrong in the script)
        impl_.guarded("onDamage", [&] { hit = impl_.parts.events.damage(impl_.vm->thread(), target, source, weapon, amount, type); });
        return hit;
    }
    std::optional<CastPrice> castPrice(server::World&, const server::ServerObject& caster, std::uint32_t spell, int mana, bool trap) override
    {
        if (!impl_.parts.events.has().castCost || impl_.hidden(caster)) {
            return std::nullopt;
        }
        CastPrice price{mana, 0};
        bool priced = false;
        impl_.guarded("castCost", [&] { priced = impl_.parts.events.castCost(impl_.vm->thread(), caster, spell, mana, trap, price.mana, price.health); });
        return priced ? std::optional(price) : std::nullopt;
    }
    bool exitTouched(server::Fight&, server::ServerObject& exit, server::ServerObject& player) override
    {
        if (!impl_.parts.events.has().onExit || impl_.hidden(player)) {
            return true;
        }
        bool go = true;
        impl_.guarded("onExit", [&] { go = impl_.parts.events.exitTouched(impl_.vm->thread(), exit, player); });
        return go;
    }

private:
    Impl& impl_;
};

// ------------------------------------------------------------------------- ScriptedMode

ScriptedMode::ScriptedMode(std::shared_ptr<const ModPackage> package, ServerContext ctx, LuauVm::Limits limits)
    : impl_(std::make_unique<Impl>())
{
    impl_->package = std::move(package);
    impl_->ctx = std::move(ctx);
    impl_->limits = limits;
    impl_->rng.seed(impl_->ctx.seed == 0 ? 1u : impl_->ctx.seed);
}

ScriptedMode::~ScriptedMode() = default;

const std::vector<std::string>& ScriptedMode::errors() const noexcept
{
    return impl_->errors;
}

void ScriptedMode::setInstallEngineHooks(bool install) noexcept
{
    impl_->installEngineHooks = install;
}

void ScriptedMode::setF32Chop(std::optional<bool> chop) noexcept
{
    impl_->f32Chop = chop;
}

server::ModeHooks* ScriptedMode::engineHooks() noexcept
{
    return impl_->host ? impl_->engineHooks.get() : nullptr;
}

bool ScriptedMode::call(std::string_view name, int nresults, const std::function<int(lua_State*)>& pushArgs,
                        const std::function<void(lua_State*)>& readResults)
{
    Impl& impl = *impl_;
    if (!impl.vm) {
        return false;
    }
    const std::string fn(name);
    return impl.guarded(fn.c_str(), [&] { return impl.begin(fn.c_str()) && impl.callTop(fn.c_str(), nresults, pushArgs, readResults); });
}

bool ScriptedMode::has(Rule rule) const
{
    const Impl& impl = *impl_;
    return impl.vm && rule.index >= 0 && static_cast<std::size_t>(rule.index) < impl.rules.size() &&
           impl.rules[static_cast<std::size_t>(rule.index)].ref != 0;
}

ScriptedMode::Rule ScriptedMode::rule(std::string_view name)
{
    Impl& impl = *impl_;
    for (std::size_t i = 0; i < impl.rules.size(); ++i) {
        if (impl.rules[i].name == name) {
            return Rule{static_cast<int>(i)};
        }
    }
    impl.rules.push_back({std::string(name), impl.vm && impl.modeRef ? impl.resolve(std::string(name)) : 0});
    return Rule{static_cast<int>(impl.rules.size() - 1)};
}

bool ScriptedMode::call(Rule rule, int nresults, const std::function<int(lua_State*)>& pushArgs,
                        const std::function<void(lua_State*)>& readResults)
{
    Impl& impl = *impl_;
    if (!impl.vm || rule.index < 0 || static_cast<std::size_t>(rule.index) >= impl.rules.size()) {
        return false;
    }
    const Impl::RuleRef& r = impl.rules[static_cast<std::size_t>(rule.index)];
    if (r.ref == 0) {
        return false;
    }
    return impl.guarded(r.name.c_str(), [&] {
        lua_getref(impl.vm->thread(), r.ref);
        return impl.callTop(r.name.c_str(), nresults, pushArgs, readResults);
    });
}

void ScriptedMode::addOwnerFunction(std::string name, std::function<int(lua_State*)> fn)
{
    impl_->ownerFns[std::move(name)] = std::move(fn);
}

fs::path ScriptedMode::configure(server::GameHost::Options& o)
{
    Impl& impl = *impl_;
    std::string error;
    if (!impl.load(error)) {
        impl.report(error);
        throw std::runtime_error(impl.package->manifest.info.id + ": " + error);
    }
    // Its generated maps (mode.json's mapPrefix): named by its slot, the last game's swept.
    Generation& gen = impl.parts.generation;
    gen.setUp(impl.package->manifest.info.mapPrefix, impl.ctx.slot, o.mapsDir, impl.ctx.things);
    gen.sweepOwn();
    fs::path map;
    if (impl.has.configure && impl.begin("configure")) {
        lua_State* L = impl.vm->thread();
        lua_createtable(L, 0, 4);
        lua_createtable(L, 0, static_cast<int>(impl.package->manifest.info.options.size()));
        for (const OptionSpec& s : impl.package->manifest.info.options) {
            if (const auto v = impl.option(s.key)) {
                lua_pushlstring(L, v->data(), v->size());
                lua_setfield(L, -2, s.key.c_str());
            }
        }
        lua_setfield(L, -2, "options");
        lua_pushnumber(L, impl.ctx.seed);
        lua_setfield(L, -2, "seed");
        lua_pushinteger(L, impl.ctx.slot);
        lua_setfield(L, -2, "slot");
        lua_pushinteger(L, o.maxPlayers);
        lua_setfield(L, -2, "maxPlayers");
        if (!impl.finish("configure", 1, 1)) {
            throw std::runtime_error(impl.package->manifest.info.id + ": " + impl.errors.back());
        }
        if (lua_istable(L, -1)) {
            lua_rawgetfield(L, -1, "map");
            if (lua_isstring(L, -1)) {
                const std::string name = lua_tostring(L, -1);
                if (!insideFolder(name) || name.find('/') != std::string::npos) {
                    lua_pop(L, 2);
                    throw std::runtime_error(impl.package->manifest.info.id + ": configure's map is a map's name (\"Estate\")");
                }
                map = o.mapsDir / name / (name + ".map");
            }
            lua_pop(L, 1);
            lua_rawgetfield(L, -1, "maxPlayers");
            if (lua_isnumber(L, -1)) {
                o.maxPlayers = std::clamp(static_cast<int>(lua_tointeger(L, -1)), 1, 32);
            }
            lua_pop(L, 1);
            // Its first map generated now (nox.generate's table), in place of a map's name; its
            // onGenerated told at the first frame.
            const int top = lua_gettop(L);
            try {
                lua_rawgetfield(L, -1, "generate");
                if (lua_istable(L, -1)) {
                    if (!map.empty()) {
                        throw std::runtime_error("configure gives a map or generate, not both");
                    }
                    const Generation::Request r = gen.requestArg(L, lua_gettop(L));
                    Generation::Built b = gen.runNow(r, {});
                    if (!b.ok) {
                        throw std::runtime_error("configure's generate: " + b.error);
                    }
                    map = o.mapsDir / r.name / (r.name + ".map");
                    gen.told(r.tag, std::move(b));
                }
                lua_pop(L, 1);
                // The quest options it asks for (quest saves never on).
                applyHostOptions(L, -1, o, gen, map.empty() ? std::string() : map.stem().string());
            } catch (const std::exception& e) {
                lua_settop(L, top - 1);
                throw std::runtime_error(impl.package->manifest.info.id + ": " + e.what());
            }
        }
        lua_pop(L, 1);
    }
    // A map change to one of its maps still building waits for it (as the Delve's next level).
    if (gen.enabled()) {
        o.prepareMap = [&impl, prev = std::move(o.prepareMap)](const fs::path& next, const server::World& world) {
            if (prev) {
                prev(next, world);
            }
            impl.parts.generation.waitFor(next.stem().string(), world.mapName);
        };
    }
    // A mode that plays as a stock one (mode.json's looksLike): the world and every client in that
    // mode (its flags; CTF with team play), so the engine and the stock HUD work as in it; its
    // rules off: nothing scores natively (World::sandboxNoScoring: kills, captures), and its
    // lesson and time limits, kept as the host chose them and told to every client as the stock
    // mode's (MSG_GAME_SETTINGS, the settings records: the scoreboard's Lesson Limit), aren't the
    // game's to act on (World::modeOwnsLimits: no native win, timer or time-up). The script reads
    // the same limits (nox.limits) and scores, wins and times the match itself.
    if (const std::uint32_t looks = impl.package->manifest.looksLike) {
        o.gameFlags = (o.gameFlags & ~0x17F0u) | looks;
        if (looks == 0x20) {
            o.gameplayFlags |= 4u;
        }
        o.sandboxNoScoring = true;
        o.modeOwnsLimits = true;
    }
    // A NoX Core-only mod (mode.json's "stockClients": false): stock GAME.EXE players held out, told why and put out by the host; the script never hears
    // of them.
    if (impl.package->manifest.refuseStock) {
        o.stockRefusal = server::GameHost::Options::StockRefusal{
            "This game is a NoX Core mod (" + impl.package->manifest.info.name +
                "). Get the free NoX Core client from noxhub.net and join again.",
            15000};
    }
    // A mod that needs a newer NoX Core (mode.json's "minClientLevel"): an older one refused at its
    // join with the too-old notice, as the host's own --min-client-level (the higher of the two).
    o.minClientLevel = std::max(o.minClientLevel, impl.package->manifest.minClientLevel);
    // The host's team options a mod pins (mode.json's "gameplay"): set as given whatever the host
    // chose, and kept so for the game (GameHost::Options::gameplayPinned).
    if (const std::uint32_t pins = impl.package->manifest.gameplayPinned) {
        o.gameplayFlags = (o.gameplayFlags & ~pins) | impl.package->manifest.gameplayValues;
        o.gameplayPinned |= pins;
    }
    // A mod's pickup rules (mode.json's "pickup"): what a class can't use carried (never worn),
    // seconds under the weapons-respawn option.
    o.pickupAnyClass = o.pickupAnyClass || impl.package->manifest.pickupAnyClass;
    o.pickupDuplicates = o.pickupDuplicates || impl.package->manifest.pickupDuplicates;
    // The host's callbacks: each the script's when it has the hook (looked up as it's called, so a
    // reload's new hooks count). None of them runs per frame.
    o.onJoin = [&impl, prev = std::move(o.onJoin)](server::GameHost& h, std::uint8_t peer, server::ServerObject& unit) {
        if (prev) {
            prev(h, peer, unit);
        }
        impl.guarded("onJoin", [&] { impl.onJoin(peer); });
    };
    o.onLeave = [&impl, prev = std::move(o.onLeave)](server::GameHost& h, std::uint8_t peer, server::ServerObject& unit) {
        if (prev) {
            prev(h, peer, unit);
        }
        impl.guarded("onLeave", [&] { impl.onLeave(peer); });
    };
    o.onChat = [&impl, prev = std::move(o.onChat)](server::GameHost& h, std::uint8_t peer, std::u16string_view text) {
        if (prev && prev(h, peer, text)) {
            return true;
        }
        if (h.refused(peer)) {
            return false; // (a stock player held out: the mode never hears it; relayed as any line)
        }
        return impl.guarded("onChat", [&] { return impl.onChat(peer, text); });
    };
    // The UI's channel (MSG_NOXCPP_MODEUI): a player's map loaded, the UI due it (if it asked);
    // what its UI sends back.
    o.onClientReady = [&impl, prev = std::move(o.onClientReady)](server::GameHost& h, std::uint8_t peer) {
        if (prev) {
            prev(h, peer);
        }
        impl.clientReady(peer);
        impl.parts.weather.ready(h, peer); // (its join, or a map's load: its weather again, when the mode has any)
        // A mode's book (configure's bookIsTheMod, World::modeOwnsBook): its NoX Core client told
        // (kBook: a client of level 2 or above that named ModeUi), before the script's playerReady
        // grants. In a quest: not told (a quest's book lists only what was granted anyway).
        if (h.server().world().modeBook() && Weather::reaches(h, peer) && h.clientLevel(peer) >= uimsg::kBookLevel) {
            h.sendToIdentified(peer, uimsg::bookMessage(true));
        }
        // (A stock player held out: the host doesn't call us for one; asked again so the script
        // never hears of it, as with onJoin.)
        if (impl.has.playerReady && !h.refused(peer)) {
            impl.guarded("playerReady", [&] { impl.playerReady(peer); });
        }
    };
    o.noxcppMessages[uimsg::kOp] = [&impl](server::GameHost&, std::uint8_t peer, std::span<const std::uint8_t> m) {
        impl.guarded("onUiMessage", [&] { impl.onUiMessage(peer, m); });
    };
    // A thing nox.setDrawScale sized, first sent to a player: its scale to that player's client
    // (asked only for things with a scale; nothing runs per frame).
    o.onScaledSeen = [&impl, prev = std::move(o.onScaledSeen)](server::GameHost& h, std::uint8_t peer, server::ServerObject& thing) {
        if (prev) {
            prev(h, peer, thing);
        }
        impl.parts.drawScales.seen(h, peer, thing);
    };
    return map;
}

void ScriptedMode::attach(server::GameHost& host)
{
    impl_->host = &host;
    if (!impl_->assetsHash) {
        // (Hosted again after a detach let its assets go: read afresh; failing, the UI goes
        // without them and the error is the script's.)
        std::string error;
        std::optional<Impl::LoadedAssets> again;
        if (!impl_->loadAssets(error, again)) {
            impl_->report("assets: " + error);
        } else if (again) {
            impl_->keepAssets(std::move(*again));
        }
    }
    // The engine's hooks on the server (each the game's own unless the script has it), and the
    // first map's load told now: it came before us.
    impl_->engineHooks = std::make_unique<Impl::EngineHooks>(*impl_);
    if (impl_->installEngineHooks) {
        host.server().setModeHooks(impl_->engineHooks.get());
    }
    impl_->guarded("start", [&] {
        if (impl_->has.start && impl_->begin("start")) {
            impl_->finish("start", 0, 0);
        }
    });
    impl_->engineHooks->mapLoaded(host.server());
}

void ScriptedMode::tick(server::GameHost& host, std::uint32_t)
{
    Impl& impl = *impl_;
    // Once per game frame (hosts call between their own ticks, as often as every millisecond).
    const std::uint32_t frame = host.server().world().frameNow;
    if (frame == impl.lastFrame) {
        return;
    }
    impl.lastFrame = frame;
    // The frame's recorded events, then the timers due, then the builds finished: before tick.
    if (!impl.parts.events.idle()) {
        impl.guarded("events", [&] { impl.parts.events.deliver(impl.vm->thread()); });
    }
    if (impl.parts.scheduler.pending() != 0) {
        impl.guarded("timers", [&] { impl.parts.scheduler.run(impl.vm->thread(), frame); });
    }
    if (!impl.parts.generation.idle()) {
        impl.guarded("onGenerated", [&] { impl.parts.generation.poll(impl.vm->thread(), host.server().world().mapName); });
    }
    impl.guarded("tick", [&] {
        if (impl.has.tick && impl.begin("tick")) {
            lua_pushnumber(impl.vm->thread(), frame);
            impl.finish("tick", 1, 0);
        }
    });
    impl.pumpUi();
    if (impl.parts.weather.used()) {
        impl.parts.weather.flush(host); // (nox.weather's changes, once a frame)
    }
    if (impl.parts.drawScales.owing()) {
        impl.parts.drawScales.flush(host); // (draw scales that waited for room)
    }
    // Between games (nobody in), a changed script reloads by itself: looked at every 60 frames.
    if (++impl.sinceCheck >= 60) {
        impl.sinceCheck = 0;
        if (host.players().empty() && impl.package->newest() > impl.loadedAt) {
            std::string error;
            if (reload(false, error)) {
                impl.log("scripts changed: reloaded");
            } else {
                impl.loadedAt = impl.package->newest(); // (not again till it changes once more)
            }
        }
    }
}

void ScriptedMode::detach(server::GameHost& host)
{
    impl_->guarded("stop", [&] {
        if (impl_->has.stop && impl_->begin("stop")) {
            impl_->finish("stop", 0, 0);
        }
    });
    if (host.server().modeHooks() == impl_->engineHooks.get()) {
        host.server().setModeHooks(nullptr);
    }
    // The parts' work ended with the game: its builds waited for, its timers and events dropped,
    // its store's writes done and its locks given up.
    Parts& parts = impl_->parts;
    parts.generation.stop();
    if (impl_->vm) {
        parts.scheduler.clear(impl_->vm->thread());
    }
    parts.events.clear();
    if (parts.store) {
        parts.store->flush();
        parts.store->unlockAll(parts.storeOwner);
    }
    parts.weather.reset(&host); // (none to every player it was sent to)
    parts.drawScales.reset();   // (the things keep theirs; nothing owed is sent)
    impl_->host = nullptr;
    impl_->dropAssets(); // (the mod's assets kept only while it's hosted)
}

int ScriptedMode::sweepOwn()
{
    return impl_->parts.generation.sweepOwn();
}

void ScriptedMode::releaseBackground()
{
    impl_->parts.generation.release();
}

bool ScriptedMode::reload(bool keepState, std::string& error)
{
    error = "a Lua error outside the scripts' calls (see the errors)";
    return impl_->guarded("reload", [&] { return reloadUnguarded(keepState, error); });
}

bool ScriptedMode::reloadUnguarded(bool keepState, std::string& error)
{
    Impl& impl = *impl_;
    error.clear();
    int stateRef = 0;
    if (keepState && impl.has.save && impl.begin("save")) {
        if (!impl.finish("save", 0, 1)) {
            error = "save: " + impl.errors.back();
            return false;
        }
        stateRef = lua_ref(impl.vm->thread(), -1);
        lua_pop(impl.vm->thread(), 1);
    }
    const bool ok = impl.load(error);
    if (!ok) {
        impl.report("reload: " + error);
    }
    if (ok && stateRef && impl.has.restore && impl.begin("restore")) {
        lua_getref(impl.vm->thread(), stateRef);
        impl.finish("restore", 1, 0);
    }
    if (stateRef) {
        lua_unref(impl.vm->thread(), stateRef);
    }
    if (ok && impl.host && impl.has.start && impl.begin("start")) {
        impl.finish("start", 0, 0);
    }
    return ok;
}

std::optional<std::vector<std::string>> ScriptedMode::console(server::GameHost&, std::string_view line)
{
    Impl& impl = *impl_;
    if (line == "mode reload" || line == "mode reload keep") {
        std::string error;
        if (reload(line.ends_with("keep"), error)) {
            return std::vector<std::string>{impl.package->manifest.info.id + ": reloaded"};
        }
        return std::vector<std::string>{impl.package->manifest.info.id + ": reload failed, the old scripts kept: " + error};
    }
    return impl.guarded("console", [&] { return consoleLine(line); });
}

std::optional<std::vector<std::string>> ScriptedMode::consoleLine(std::string_view line)
{
    Impl& impl = *impl_;
    if (!impl.has.console || !impl.begin("console")) {
        return std::nullopt;
    }
    lua_State* L = impl.vm->thread();
    lua_pushlstring(L, line.data(), line.size());
    if (!impl.finish("console", 1, 1)) {
        return std::vector<std::string>{impl.errors.back()};
    }
    std::optional<std::vector<std::string>> out;
    if (lua_isstring(L, -1)) {
        out = std::vector<std::string>{lua_tostring(L, -1)};
    } else if (lua_istable(L, -1)) {
        out.emplace();
        const int n = std::min(lua_objlen(L, -1), 64);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            out->push_back(cellText(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return out;
}

} // namespace nox::modes::script
