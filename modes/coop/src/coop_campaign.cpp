// A mod's campaign played as the co-op campaign (coop_campaign.hpp).
#include "noxcore/coop/coop_campaign.hpp"

#include "noxcore/modes/script/scripted_mode.hpp"
#include "noxcore/server/server.hpp"

#include "hooks_pair.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nox::coop {

CoopDirector::Options campaignOptions(const modes::CampaignInfo& campaign, int maxPlayers, CoopDirector::Options options)
{
    if (!(campaign.classes & (1u << options.campaignClass))) {
        for (std::uint8_t c = 0; c < 3; ++c) {
            if (campaign.classes & (1u << c)) {
                options.campaignClass = c; // (its first: the class screen offers its classes alone)
                break;
            }
        }
    }
    options.maxPlayers = std::min(options.maxPlayers, std::max(maxPlayers, 1));
    return options;
}

modes::OptionValues campaignScriptOptions(const modes::Mode& mod, const modes::OptionValues& options)
{
    modes::OptionValues out;
    const std::string prefix = mod.info.id + ".";
    for (const auto& [key, value] : options) {
        if (key.find('.') == std::string::npos) {
            out.emplace(key, value); // (the co-op row's)
        }
    }
    for (const modes::OptionSpec& s : mod.info.options) {
        out.erase(s.key); // (its own key: its own setting alone, else its default)
    }
    for (const auto& [key, value] : options) {
        if (key.size() > prefix.size() && key.starts_with(prefix)) {
            out[key.substr(prefix.size())] = value;
        }
    }
    return out;
}

namespace {

/// The director's options for `mod`'s campaign: its id and title told to NoX Core players (Hello).
CoopDirector::Options withCampaign(CoopDirector::Options options, const modes::Mode& mod)
{
    options.campaignId = mod.info.id;
    options.campaignTitle = mod.info.campaign ? mod.info.campaign->title : mod.info.name;
    return options;
}

} // namespace

CoopCampaign::CoopCampaign(const modes::Mode& mod, const modes::ServerContext& ctx, CoopDirector::Options options)
    : director_(withCampaign(std::move(options), mod))
{
    if (!mod.info.campaign || !mod.makeServer) {
        throw std::runtime_error(mod.info.id + ": not a campaign");
    }
    campaign_ = *mod.info.campaign;
    // (Its script made with its own settings, as on a row of its own: campaignScriptOptions.)
    modes::ServerContext own = ctx;
    own.options = campaignScriptOptions(mod, ctx.options);
    std::unique_ptr<modes::ServerMode> made = mod.makeServer(own);
    if (auto* script = dynamic_cast<modes::script::ScriptedMode*>(made.get())) {
        script_.reset(script);
        (void)made.release();
    } else {
        throw std::runtime_error(mod.info.id + ": a campaign's mod is a script mode");
    }
    script_->setInstallEngineHooks(false); // (the pair's, at attach)
}

CoopCampaign::~CoopCampaign() = default;

std::filesystem::path CoopCampaign::configure(server::GameHost::Options& o)
{
    (void)director_.configure(o);
    // What the co-op campaign's game is, kept: the script's configure may ask a game of its own (its
    // map, a quest, a stock mode's look) as it would hosted alone.
    const auto solo = o.solo;
    const auto soloGuests = o.soloGuests;
    const auto maxPlayers = o.maxPlayers;
    const auto gameFlags = o.gameFlags;
    const auto quest = o.quest;
    const auto questScreens = o.questScreens;
    const auto questSaves = o.questSaves;
    const auto questMapsOnly = o.questMapsOnly;
    const auto forceClass = o.forceClass; // (a quest's forced class: the party's is the host's)
    const auto mapCycleOn = o.mapCycleOn;
    const auto mapVote = o.mapVote;
    const auto sandboxNoScoring = o.sandboxNoScoring;
    const auto modeOwnsLimits = o.modeOwnsLimits;
    (void)script_->configure(o); // (its options, callbacks and the maps the mod ships: Options::modeMaps)
    o.solo = solo;
    o.soloGuests = soloGuests;
    o.maxPlayers = maxPlayers;
    o.gameFlags = gameFlags;
    o.quest = quest;
    o.questScreens = questScreens;
    o.questSaves = questSaves;
    o.questMapsOnly = questMapsOnly;
    o.forceClass = forceClass;
    o.mapCycleOn = mapCycleOn;
    o.mapVote = mapVote;
    o.sandboxNoScoring = sandboxNoScoring; // (a stock mode's look: none here)
    o.modeOwnsLimits = modeOwnsLimits;
    o.modeName = campaign_.title; // (NoX Core lists' Mode column: the campaign, not "Co-op Campaign")
    return {};
}

void CoopCampaign::attach(server::GameHost& host)
{
    director_.attach(host); // (its hooks and its saves on the server)
    script_->attach(host);  // (its engine hooks made, not installed)
    hooks_ = std::make_unique<server::detail::HooksPair>(director_, *script_->engineHooks());
    host.server().setModeHooks(hooks_.get());
}

void CoopCampaign::tick(server::GameHost& host, std::uint32_t nowMs)
{
    director_.tick(host, nowMs);
    script_->tick(host, nowMs);
}

void CoopCampaign::detach(server::GameHost& host)
{
    script_->detach(host);
    if (host.server().modeHooks() == hooks_.get()) {
        host.server().setModeHooks(&director_); // (the director's own detach takes its hooks and saves off)
    }
    director_.detach(host);
    hooks_.reset();
}

std::optional<std::vector<std::string>> CoopCampaign::console(server::GameHost& host, std::string_view line)
{
    if (auto r = script_->console(host, line)) {
        return r;
    }
    return director_.console(host, line);
}

bool CoopCampaign::locked() const
{
    return script_->locked();
}

bool CoopCampaign::takeLockChanged()
{
    return script_->takeLockChanged();
}

int CoopCampaign::sweepOwn()
{
    return script_->sweepOwn();
}

void CoopCampaign::releaseBackground()
{
    script_->releaseBackground();
}

} // namespace nox::coop
