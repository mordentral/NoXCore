// Delving Mode as a mode (mode.hpp): its registry entry, and its server side for any host (noxsdl's
// LocalHost, a dedicated server) through ServerMode alone. The run itself is the
// DelveDirector's (delve.hpp), set up as it always was (delve_host.hpp).
#include "noxcore/modes/registry.hpp"
#include "noxcore/client/delve_client.hpp"
#include "noxcore/net/messages.hpp"
#include "noxcore/server/delve.hpp"
#include "noxcore/server/delve_host.hpp"
#include "noxcore/server/delve_records.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/teleport.hpp"

#include "mode_options.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nox::modes {

namespace {

namespace fs = std::filesystem;

using detail::text;
using detail::textOption;

// Levels still being built when their host went (DelveDirector::releaseBuilds): waited for at the
// program's end (Mode::finishBackground); those finished let go as each host goes, so a long
// session hosting game after game doesn't gather them.
std::mutex& releasedMutex()
{
    static std::mutex m;
    return m;
}
std::vector<std::function<bool(bool)>>& releasedBuilds()
{
    static std::vector<std::function<bool(bool)>> v;
    return v;
}

class DelveMode final : public ServerMode {
public:
    // (The director is made at once: a pool's server sweeps its own maps before its set-up.)
    explicit DelveMode(const ServerContext& ctx)
        : ctx_(ctx)
    {
        server::DelveDirector::Options d;
        d.mapsDir = ctx.mapsDir;
        detail::poolMaps(ctx, "DEL_", d.mapName, d.lobbyName);
        d.seed = detail::seedOption(ctx);
        if (const auto b = option(ctx.options, "biome")) {
            d.biome = text(*b); // (every act in it, to try one)
        }
        // Its records: only the store a host that keeps them hands it (the bot manager's pool);
        // any other host keeps none.
        records_ = detail::recordsOf<server::DelveRecords>(ctx);
        d.records = records_.get();
        director_.emplace(*ctx.things, d);
    }

    fs::path configure(server::GameHost::Options& o) override
    {
        // (The lobby written, the options a run's: delve_host.hpp. Conjurers: every player a
        // conjurer in a run whatever its character, World::forceClass.)
        const auto conjurers = option(ctx_.options, "conjurers");
        return server::setUpDelveHost(*director_, o, conjurers && *conjurers != "0", [this](const server::DelveDirector& d) {
            if (ctx_.log) {
                char line[200];
                std::snprintf(line, sizeof line, "depth %d generated: act %d, %s, %s (deepest %d)", d.depth(), d.act().number, d.act().biome.c_str(),
                              std::string(server::delveRoomName(d.level().room)).c_str(), d.deepest());
                ctx_.log(line);
            }
        });
    }
    void attach(server::GameHost&) override {}
    void detach(server::GameHost& host) override { director_->detach(host); }
    void tick(server::GameHost& host, std::uint32_t) override
    {
        if (director_->tick(host)) {
            lockChanged_ = true;
        }
    }
    bool locked() const override { return director_->locked(); }
    bool takeLockChanged() override { return std::exchange(lockChanged_, false); }
    int progress() const override { return director_->depth(); }
    void setServerPort(std::uint16_t port) override { director_->setServerPort(port); } // (a bot boss joins here)
    int sweepOwn() override { return director_->sweepOwn(); }
    void releaseBackground() override
    {
        std::lock_guard lock(releasedMutex());
        auto& released = releasedBuilds();
        std::erase_if(released, [](const std::function<bool(bool)>& f) { return !f(false); });
        if (auto builds = director_->releaseBuilds(); builds(false)) {
            released.push_back(std::move(builds));
        }
    }
    bool announcesOpen() const override { return true; } // (a supervisor reads it: open in the lobby, locked in a run)

    // (The bench: the lobby, or with the "benchlevel" option a run's first level, begun as a
    // player stepping onto the portal begins it.)
    void benchBegin(server::GameHost& host) override
    {
        const auto level = option(ctx_.options, "benchlevel");
        benchLevel_ = level && *level != "0";
        if (!benchLevel_) {
            return;
        }
        server::World& world = host.server().world();
        const server::ServerObject* portal = nullptr;
        for (const auto& [extent, o] : world.objects()) {
            if (o.typeName == "GauntletExitA") {
                portal = &o;
                break;
            }
        }
        for (const auto& [peer, p] : host.players()) {
            if (server::ServerObject* u = world.byExtent(p.extent); u && portal) {
                server::setPosition(world, *u, portal->x + 18, portal->y + 18, host.server().clock().frame()); // (in front: the room's side)
                break;
            }
        }
    }
    bool benchReady() const override { return !benchLevel_ || director_->state() == server::DelveDirector::State::Running; }
    std::uint32_t benchWarmMs() const override { return benchLevel_ ? 8000 : 3000; }

private:
    ServerContext ctx_;
    std::shared_ptr<server::DelveRecords> records_; ///< (before the director: it outlives it)
    std::optional<server::DelveDirector> director_;
    bool lockChanged_ = false;
    bool benchLevel_ = false;
};

} // namespace

void registerMode_delve(Registry& registry)
{
    Mode m;
    m.info.id = "delve";
    m.info.name = "Delve";
    m.info.description = "Endless generated levels to fight through.";
    m.info.order = 10;
    m.info.minPlayers = 1;
    m.info.maxPlayers = 16;
    m.info.stockClients = true;
    m.info.levels = ModeInfo::Levels::Generated;
    m.info.opcodes = {static_cast<std::uint8_t>(net::Op::NoxcppModeF4)};
    m.info.ownsMap = [](std::string_view name) { return server::isGeneratedMapName(name); };
    m.info.hostFlag = "--delve";
    // (the dedicated server's own flags, as they always read; none shown in Host Custom yet.)
    m.info.options = {
        textOption("seed", "Seed", "--delve-seed"),
        textOption("biome", "Biome", "--delve-biome"),
        {.key = "conjurers", .label = "Conjurers only", .kind = OptionSpec::Kind::Toggle, .legacyFlag = "--delve-conjurers", .hostPanel = false},
        textOption("slot", "Pool slot", "--delve-slot"),
        {.key = "benchlevel", .label = "Bench a level", .kind = OptionSpec::Kind::Toggle, .legacyFlag = "--delve-bench-level", .hostPanel = false},
    };
    m.makeServer = [](const ServerContext& ctx) { return std::make_unique<DelveMode>(ctx); };
    m.makeClient = [](client::HeadlessClient& c) { return std::make_unique<client::DelveClient>(c); };
    m.sweepAll = [](const fs::path& mapsDir) { return server::DelveDirector::sweep(mapsDir); };
    m.finishBackground = [] {
        std::vector<std::function<bool(bool)>> waits;
        {
            std::lock_guard lock(releasedMutex());
            waits.swap(releasedBuilds());
        }
        for (auto& wait : waits) {
            wait(true);
        }
    };
    m.openRecords = [](const fs::path& gameDir) { return std::static_pointer_cast<void>(std::make_shared<server::DelveRecords>(gameDir / "delve_records.txt")); };
    registry.add(std::move(m));
}

} // namespace nox::modes
