// A mod's story campaign (mode.json's "campaign": ModeInfo::campaign) played as the co-op campaign
// plays the game's own: the co-op director's rules (the party, its saves under the campaign's own
// folder, the gathering at exits, the respawns, the journal and rewards shared, the scenes), and the
// mod's server script beside it as it runs in a game of its own (nox.onMapCall for the maps'
// LuauCall, its hooks, timers and store). The host's map is the campaign's (its first map, or a
// saved game's); the script's configure is run for its options, its map not used.
//
// One ModeHooks on the server for both (hooks_pair.hpp): the director's say and the script's, each
// hook by its rule. GetHost is the director's host, the host's own player (slot 0x1F): the co-op
// campaign is only a player's own game.
#pragma once

#include "noxcore/coop/coop.hpp"
#include "noxcore/modes/mode.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::modes::script {
class ScriptedMode;
}
namespace nox::server::detail {
class HooksPair;
}

namespace nox::coop {

/// The director's options for campaign `campaign`: `options` with the class the campaign's (the one
/// given when the campaign is played as it, else its first) and at most `maxPlayers` players.
CoopDirector::Options campaignOptions(const modes::CampaignInfo& campaign, int maxPlayers, CoopDirector::Options options);

class CoopCampaign final : public modes::ServerMode {
    /// (coop_links.hpp: its director's links, the director's own table.)
    template <class>
    friend struct server::Links;

public:
    /// `mod`: the mode that declares the campaign (its makeServer a script mode's); `ctx` its server
    /// side's; `options` the director's (campaignOptions'). Throws std::runtime_error when `mod`
    /// declares no campaign or its server side isn't a script mode.
    CoopCampaign(const modes::Mode& mod, const modes::ServerContext& ctx, CoopDirector::Options options);
    ~CoopCampaign() override;

    /// The director's options, then the script's (its configure run: a failing script throws, the
    /// game isn't hosted); the co-op campaign's own game kept whatever the script asked (a solo game
    /// with room for the guests, no quest and no class it forces, no map cycle or vote, the game's
    /// flags); the game told to NoX Core lists by the campaign's title. No map: the host's.
    std::filesystem::path configure(server::GameHost::Options& o) override;
    void attach(server::GameHost& host) override;
    void tick(server::GameHost& host, std::uint32_t nowMs) override;
    void detach(server::GameHost& host) override;
    std::optional<std::vector<std::string>> console(server::GameHost& host, std::string_view line) override;
    bool locked() const override;
    bool takeLockChanged() override;
    int sweepOwn() override;
    void releaseBackground() override;

    CoopDirector& director() noexcept { return director_; }
    modes::script::ScriptedMode& script() noexcept { return *script_; }
    const modes::CampaignInfo& campaign() const noexcept { return campaign_; }

private:
    modes::CampaignInfo campaign_;
    CoopDirector director_;
    std::unique_ptr<modes::script::ScriptedMode> script_;
    std::unique_ptr<server::detail::HooksPair> hooks_; ///< (on the server while attached)
};

} // namespace nox::coop
