// Undead Tide as a mode (mode.hpp): its registry entry, and its server side for any host (noxsdl's
// LocalHost, a dedicated server) through ServerMode alone. The run itself is the
// TideDirector's (tide.hpp), set up as it always was (tide_host.hpp).
#include "noxcore/modes/registry.hpp"
#include "noxcore/client/tide_client.hpp"
#include "noxcore/net/messages.hpp"
#include "noxcore/server/tide.hpp"
#include "noxcore/server/tide_host.hpp"
#include "noxcore/server/tide_records.hpp"

#include "mode_options.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace nox::modes {

namespace {

namespace fs = std::filesystem;

using detail::text;
using detail::textOption;

class TideMode final : public ServerMode {
public:
    explicit TideMode(const ServerContext& ctx)
        : ctx_(ctx)
    {
    }

    fs::path configure(server::GameHost::Options& o) override
    {
        // The director's options as each host's own flags always made them (the defaults left
        // alone; a pool's server its own maps, UTD_N and its lobby UTD_LN).
        server::TideDirector::Options d;
        d.mapsDir = o.mapsDir;
        detail::poolMaps(ctx_, "UTD_", d.mapName, d.lobbyName);
        const OptionValues& v = ctx_.options;
        d.seed = detail::seedOption(ctx_);
        if (const auto b = option(v, "biome")) {
            d.biome = text(*b);
        }
        if (const auto m = option(v, "minutes")) {
            d.winSeconds = static_cast<std::uint32_t>(std::max(0.25, std::atof(text(*m).c_str())) * 60.0);
        }
        if (const auto m = option(v, "boss")) {
            d.bossSeconds = static_cast<std::uint32_t>(std::max(0.1, std::atof(text(*m).c_str())) * 60.0);
        }
        if (const auto n = option(v, "perView")) {
            d.perView = std::clamp(std::atoi(text(*n).c_str()), 1, 1000);
        }
        if (const auto n = option(v, "max")) {
            d.maxCreatures = std::clamp(std::atoi(text(*n).c_str()), 1, 4000);
        }
        if (const auto n = option(v, "hold")) {
            d.holdPerPlayer = std::clamp(std::atoi(text(*n).c_str()), 0, 1000);
        }
        // Its records: only the store a host that keeps them hands it (the bot manager's pool);
        // any other host keeps none.
        records_ = detail::recordsOf<server::TideRecords>(ctx_);
        d.records = records_.get();
        // (A script failure told in the host's log; none: the standard error.)
        d.log = ctx_.log;
        director_.emplace(*ctx_.things, d);
        return server::setUpTideHost(*director_, o, [log = ctx_.log](const server::TideDirector& t) {
            if (log) {
                log("arena generated (" + t.biome() + ")");
            }
        });
    }
    void attach(server::GameHost&) override {}
    void detach(server::GameHost& host) override
    {
        if (director_) {
            director_->detach(host);
        }
    }
    void tick(server::GameHost& host, std::uint32_t) override
    {
        if (director_ && director_->tick(host)) {
            lockChanged_ = true;
        }
    }
    bool locked() const override { return director_ && director_->locked(); }
    bool takeLockChanged() override { return std::exchange(lockChanged_, false); }
    int sweepOwn() override { return director_ ? director_->sweepOwn() : 0; }

    std::optional<std::vector<std::string>> console(server::GameHost& game, std::string_view line) override
    {
        // "tide start" begins the run as the portal does; "tide give NAME WEAPON LEVEL" / "tide
        // passive NAME PASSIVE LEVEL" set a player's, for trying them: the names as they're told,
        // run together, any case: magicmissile, forceofnature, haste...; level 0 takes a weapon
        // away; "tide xp NAME XP" as soul gems give it.
        if (!director_ || !line.starts_with("tide ")) {
            return std::nullopt;
        }
        std::vector<std::string> out;
        std::istringstream in{std::string(line.substr(5))};
        std::string what, name, which;
        int level = 1;
        in >> what;
        const auto squash = [](std::string_view s) {
            std::string r;
            for (const char c : s) {
                if (c != ' ') {
                    r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            return r;
        };
        if (what == "start") {
            director_->beginRun(game);
        } else if (what == "xp" && (in >> name >> level)) {
            out.emplace_back(director_->giveXp(game, name, level) ? "given" : "no such player in the arena");
        } else if ((what == "give" || what == "passive") && (in >> name >> which >> level)) {
            bool done = false;
            const std::size_t count = what == "give" ? server::kTideWeapons : server::kTidePassives;
            for (std::size_t k = 0; k < count && !done; ++k) {
                const std::string_view label = what == "give" ? server::tideWeaponName(static_cast<server::TideWeapon>(k))
                                                              : server::tidePassiveName(static_cast<server::TidePassive>(k));
                // (the name, run together, or its start: "fist" for Fist of Vengeance)
                if (squash(label).starts_with(squash(which)) || (what == "give" && k == 0 && squash(which) == "sword")) {
                    const bool ok = what == "give" ? director_->giveWeapon(name, static_cast<server::TideWeapon>(k), level)
                                                   : director_->givePassive(name, static_cast<server::TidePassive>(k), level);
                    out.emplace_back(ok ? "given" : "no such player in the arena");
                    done = true;
                }
            }
            if (!done) {
                out.push_back("no " + std::string(what == "give" ? "weapon" : "passive") + " named " + which);
            }
        } else {
            out.emplace_back("tide start | tide give NAME WEAPON LEVEL | tide passive NAME PASSIVE LEVEL | tide xp NAME XP");
        }
        return out;
    }

    // (The bench: the run begun as the portal begins it, everyone in the arena; the tide's own a
    // while to gather; then how many creatures stand about each player.)
    void benchBegin(server::GameHost& host) override
    {
        if (director_) {
            director_->beginRun(host);
        }
    }
    bool benchReady() const override { return director_ && director_->state() == server::TideDirector::State::Running && !director_->runners().empty(); }
    std::uint32_t benchWarmMs() const override { return 15000; }
    std::optional<std::string> benchReport(server::GameHost& host) const override
    {
        if (!director_) {
            return std::nullopt;
        }
        // Those about each player as its client would see them (a 640 x 480 view, a margin).
        double inView = 0;
        int counted = 0;
        for (const auto& [peer, p] : host.players()) {
            if (const server::ServerObject* u = host.server().world().byExtent(p.extent)) {
                int n = 0;
                host.server().world().unitsInRect(u->x - 360, u->y - 280, u->x + 360, u->y + 280, [&](server::ServerObject& o) {
                    n += o.monster && !(o.flags & server::ObjFlag::Dead) ? 1 : 0;
                });
                inView += n;
                ++counted;
            }
        }
        char line[160];
        std::snprintf(line, sizeof line, "undead tide: %zu creatures of the tide, %.1f live creatures in a player's view (at the end)",
                      director_->creatures(), counted ? inView / counted : 0.0);
        return std::string(line);
    }

private:
    ServerContext ctx_;
    std::shared_ptr<server::TideRecords> records_; ///< (before the director: it outlives it)
    std::optional<server::TideDirector> director_;
    bool lockChanged_ = false;
};

} // namespace

void registerMode_tide(Registry& registry)
{
    Mode m;
    m.info.id = "tide";
    m.info.name = "Undead Tide";
    m.info.description = "Survive an endless tide of the undead.";
    m.info.order = 20;
    m.info.minPlayers = 1;
    m.info.maxPlayers = 16;
    m.info.stockClients = true;
    m.info.levels = ModeInfo::Levels::Generated;
    m.info.opcodes = {static_cast<std::uint8_t>(net::Op::NoxcppModeF5)};
    m.info.ownsMap = [](std::string_view name) { return server::isTideMapName(name); };
    m.info.hostFlag = "--undead-tide";
    // (the dedicated server's own flags, as they always read; none shown in Host Custom yet.)
    m.info.options = {
        textOption("seed", "Seed", "--tide-seed"),
        textOption("biome", "Biome", "--tide-biome"),
        textOption("minutes", "Night length (minutes)", "--tide-minutes"),
        textOption("boss", "Guardian comes (minutes)", "--tide-boss"),
        textOption("perView", "Creatures about a player", "--tide-per-view"),
        textOption("max", "Creatures in all", "--tide-max"),
        textOption("hold", "Creatures held about each (bench)", "--tide-hold"),
    };
    m.makeServer = [](const ServerContext& ctx) { return std::make_unique<TideMode>(ctx); };
    m.makeClient = [](client::HeadlessClient& c) { return std::make_unique<client::TideClient>(c); };
    m.sweepAll = [](const fs::path& mapsDir) { return server::TideDirector::sweep(mapsDir); };
    m.openRecords = [](const fs::path& gameDir) { return std::static_pointer_cast<void>(std::make_shared<server::TideRecords>(gameDir / "tide_records.txt")); };
    registry.add(std::move(m));
}

} // namespace nox::modes
