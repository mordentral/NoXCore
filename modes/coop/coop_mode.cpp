// The co-op campaign as a mode (mode.hpp): its registry entry. Its server side is the
// CoopDirector itself (a ServerMode), its client side the CoopClient. It's hosted only from
// noxcore.exe, where the host plays the story: the solo screens choose the campaign's class and
// its saved game (noxcore.exe's co-op UI, modes/coop/ui), then the game is hosted with the class
// as the option "class".
#include "noxcore/modes/registry.hpp"
#include "noxcore/coop/coop.hpp"
#include "noxcore/coop/coop_campaign.hpp"
#include "noxcore/coop/coop_client.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace nox::modes {

void registerMode_coop(Registry& registry)
{
    Mode m;
    m.info.id = "coop";
    m.info.name = "Co-op Campaign";
    m.info.description = "The story campaign, with friends.";
    m.info.order = 30;
    m.info.minPlayers = 1;
    m.info.maxPlayers = coop::CoopDirector::Options{}.maxPlayers;
    m.info.stockClients = true; // (as quest characters)
    m.info.levels = ModeInfo::Levels::MapFiles;
    m.info.opcodes = {coop::coopmsg::kOp};
    m.info.playerHost = true;
    m.info.options = {
        {.key = "class", .label = "Campaign", .kind = OptionSpec::Kind::Choice, .choices = {"Warrior", "Wizard", "Conjurer"},
         .defaultValue = 1, .legacyFlag = {}, .hostPanel = false}, // (the class screen's choice)
        // (The party's scaling: Normal as it always was; Easy below it, Hard and Nightmare above.)
        {.key = "difficulty", .label = "Difficulty", .kind = OptionSpec::Kind::Choice, .choices = {"Easy", "Normal", "Hard", "Nightmare"},
         .defaultValue = 1, .legacyFlag = {}},
    };
    // A mod's campaign (mode.json's "campaign") beside the game's own: Host Custom's row offers each
    // as its Campaign choice, hosted with the option "campaign" naming the mod (each also a row of
    // its own in the mode lists, hosted the same way).
    m.info.runsCampaigns = true;
    m.makeServer = [](const ServerContext& ctx) -> std::unique_ptr<ServerMode> {
        coop::CoopDirector::Options o;
        if (const auto c = option(ctx.options, "class")) {
            o.campaignClass = static_cast<std::uint8_t>(std::clamp(std::atoi(std::string(*c).c_str()), 0, 2));
        }
        if (const auto d = option(ctx.options, "difficulty")) {
            o.difficulty = std::clamp(std::atoi(std::string(*d).c_str()), 0, 3);
        }
        o.seed = ctx.seed;
        if (const auto c = option(ctx.options, "campaign"); c && !c->empty()) {
            // (The process's mods loaded; exactly its id, as the game data and the saves take it.)
            const Mode* mod = installedCampaign(nox::modes::registry(), *c);
            if (!mod) {
                throw std::runtime_error("no campaign \"" + std::string(*c) + "\" among the mods");
            }
            return std::make_unique<coop::CoopCampaign>(*mod, ctx, coop::campaignOptions(*mod->info.campaign, mod->info.maxPlayers, o));
        }
        return std::make_unique<coop::CoopDirector>(o);
    };
    m.makeClient = [](client::HeadlessClient& c) { return std::make_unique<coop::CoopClient>(c); };
    m.everyClient = false; // (a guest's side sends its character: noxcore.exe's own connection's alone)
    registry.add(std::move(m));
}

} // namespace nox::modes
