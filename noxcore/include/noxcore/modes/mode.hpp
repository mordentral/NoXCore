// A game mode the core hosts without knowing it by name: what it is (ModeInfo), how a host makes
// its server side (ServerMode, a HostedMode) and a client its own side (ClientMode). Modes live
// outside the core (modes/: built in, and later scripts loaded from mods\) and register
// themselves in the registry (registry.hpp); hosts, the client and the lists ask the registry.
#pragma once

#include "noxcore/server/hosted_mode.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}
namespace nox::client {
class HeadlessClient;
}

namespace nox::modes {

/// One setting a mode offers: shown under its row in Host Custom, given to a dedicated server as
/// `--<legacyFlag>` or `--option key=value`. Read at set-up only, never in the frame.
struct OptionSpec {
    enum class Kind : std::uint8_t { Choice, Toggle, Number, Text };
    std::string key;   ///< "difficulty"
    std::string label; ///< "Difficulty"
    Kind kind = Kind::Toggle;
    std::vector<std::string> choices; ///< (Choice) their labels, by value 0..n-1
    int min = 0;                      ///< (Number)
    int max = 0;
    int defaultValue = 0; ///< what the mode does with the setting left alone
    /// the dedicated server's own flag for it ("--example-rounds"), kept working; empty: none. A Toggle's takes
    /// no value ("1"); the others' the next argument, as typed.
    std::string legacyFlag;
    bool hostPanel = true; ///< shown in Host Custom (false: the command line and tests only)
};

/// The settings chosen, by key, as typed (one left out: its default). Each mode reads its own the
/// way its flags always did (a number, a float, a name).
using OptionValues = std::map<std::string, std::string, std::less<>>;

/// A setting's text; none when it wasn't given.
inline std::optional<std::string_view> option(const OptionValues& values, std::string_view key)
{
    const auto it = values.find(key);
    return it != values.end() ? std::optional<std::string_view>(it->second) : std::nullopt;
}

/// What a mode is, as the lists and the hosts see it.
struct ModeInfo {
    std::string id;          ///< "example": the dedicated server's --mode, the log tag ("[example N]"), records' key
    std::string name;        ///< "Example Mode"
    std::string description; ///< the list's line
    int order = 0;           ///< position in the list (low first)
    int minPlayers = 1;
    int maxPlayers = 16;
    bool stockClients = false; ///< GAME.EXE players can join
    enum class Levels : std::uint8_t { MapFiles, Generated };
    Levels levels = Levels::MapFiles;
    std::vector<OptionSpec> options;
    /// Its NoX Core messages' opcodes (no two modes share one).
    std::vector<std::uint8_t> opcodes;
    /// Whether a map's name is one of its generated maps (kept out of the hosts' map lists and the
    /// quest's); none: it makes none.
    std::function<bool(std::string_view)> ownsMap;
    /// A mod's generated maps' prefix (mode.json's "mapPrefix", 2-3 of A-Z: its maps are
    /// <prefix>_<slot><TAG>); empty: none.
    std::string mapPrefix;
    /// the dedicated server's own flag that hosts it ("--example"), kept working beside "--mod ID";
    /// empty: none.
    std::string hostFlag;
    /// Hosted only by a player's own game, the host playing in it (noxcore.exe's: a campaign mode's
    /// host is its story's player), never by a dedicated server.
    bool playerHost = false;
    /// Loaded from the mods folder (mods.hpp's loadMods), not built into the program.
    bool fromMods = false;
};

/// What a host gives a mode it makes.
struct ServerContext {
    const things::ThingDb* things = nullptr;
    std::filesystem::path gameDir;
    std::filesystem::path mapsDir;
    std::uint32_t seed = 1;
    OptionValues options;
    /// A pool's slot (its maps numbered by it); 0: a host of its own.
    int slot = 0;
    /// The host's log (a line without its prefix; the host adds "<id>: ").
    std::function<void(std::string_view)> log;
    /// The mode's records store, from a host that keeps records (the bot manager's pool, the store
    /// its leaderboard reads; the user, 2026-10-24: no other host keeps any). None: no records.
    std::shared_ptr<void> records;
    /// Where the host keeps mods' own data (a script mode's nox.store: <modData>/<id>/): beside the
    /// program's mods folder, or the pool's folder. Empty: it keeps none.
    std::filesystem::path modData;
};

/// A mode's server side: the hosted game's set-up, its ticks between the game's (HostedMode), and
/// what hosts and supervisors ask of it.
class ServerMode : public server::HostedMode {
public:
    /// The game closed to newcomers (a run under way).
    virtual bool locked() const { return false; }
    /// Whether the lock changed since the last call: the hosts print "<id>: locked" / "<id>: open".
    virtual bool takeLockChanged() { return false; }
    /// How far a run has got, for a supervisor's status (a run's depth); 0: nothing to tell.
    virtual int progress() const { return 0; }
    /// The port the game listens on, once it's open (a mode's own bots join there).
    virtual void setServerPort(std::uint16_t port) { (void)port; }
    /// Its generated maps deleted (when its host stops); how many went.
    virtual int sweepOwn() { return 0; }
    /// Its host goes on a thread that mustn't wait (noxsdl's leaving a game): work still under way
    /// (levels being built) let go, to be waited for at the program's end (Mode::finishBackground).
    virtual void releaseBackground() {}
    /// Whether its host announces "<id>: open" as it starts (for a supervisor that reads it).
    virtual bool announcesOpen() const { return false; }
    /// A host console line: its reply lines when the mode takes it, none when it doesn't.
    virtual std::optional<std::vector<std::string>> console(server::GameHost& host, std::string_view line)
    {
        (void)host;
        (void)line;
        return std::nullopt;
    }

    // ---- the dedicated server's --bench in the mode's game
    /// The players in, the bench's play begun (a run started as its portal would).
    virtual void benchBegin(server::GameHost& host) { (void)host; }
    /// Whether that play is under way (the bench waits for it, 20 s at most).
    virtual bool benchReady() const { return true; }
    /// How long the bench warms up before it measures.
    virtual std::uint32_t benchWarmMs() const { return 3000; }
    /// The mode's own line of the bench's report; none: none.
    virtual std::optional<std::string> benchReport(server::GameHost& host) const
    {
        (void)host;
        return std::nullopt;
    }
};

/// A mode's client side for one connection: made with the client, it takes its opcodes
/// (HeadlessClient::setNoxcppHandler) and keeps what the server told it.
class ClientMode {
public:
    virtual ~ClientMode() = default;
};

/// A mode as registered.
struct Mode {
    ModeInfo info;
    /// Its server side for a host; none: a mode that's only a list row (a native flow starts it).
    std::function<std::unique_ptr<ServerMode>(const ServerContext&)> makeServer;
    /// Its client side for a connection; none: it has none.
    std::function<std::unique_ptr<ClientMode>(client::HeadlessClient&)> makeClient;
    /// Its client side made with every client (bots' and tests' too). Off: only where a program
    /// asks for it (HeadlessClient::addModeSide: noxcore.exe's own connection), for a side that
    /// acts for its player (a guest's that sends its character to the host).
    bool everyClient = true;
    /// Every generated map of it in `mapsDir` deleted (a host's start-up sweep); how many went.
    std::function<int(const std::filesystem::path& mapsDir)> sweepAll;
    /// Work it left running after its hosts went (levels being built), waited for at quit.
    std::function<void()> finishBackground;
    /// Its records store for the game folder `gameDir`, for a host that shares one between its
    /// games (ServerContext::records); none: it keeps none.
    std::function<std::shared_ptr<void>(const std::filesystem::path& gameDir)> openRecords;
};

} // namespace nox::modes
