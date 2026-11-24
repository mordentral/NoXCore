// A mod's campaign in noxcore.exe (campaign_mod.hpp).
#include "campaign_mod.hpp"

#include "noxcore/server/mode_maps.hpp"
#include "noxcore/util/text.hpp"

#include <utility>

namespace noxsdl {

const nox::modes::Mode* campaignMod(std::string_view campaign, const nox::modes::Registry& registry)
{
    return campaign.empty() ? nullptr : nox::modes::installedCampaign(registry, campaign);
}

std::string typedCampaignId(std::string_view typed)
{
    return nox::util::lowered(typed);
}

std::string campaignTitleOf(std::string_view campaign, const nox::modes::Registry& registry)
{
    const nox::modes::Mode* mod = campaignMod(campaign, registry);
    return mod ? mod->info.campaign->title : std::string();
}

std::filesystem::path campaignMapPath(const std::filesystem::path& gameDir, std::string_view campaign, const std::string& name,
                                      const nox::modes::Registry& registry)
{
    // (The maps the mod ships, as its games find them: its package's index, GameHost::findMap's.)
    const nox::modes::Mode* mod = campaignMod(campaign, registry);
    if (mod && mod->maps) {
        if (const nox::server::ModeMaps::Entry* e = mod->maps->find(name)) {
            return e->map;
        }
    }
    return gameDir / "maps" / name / (name + ".map");
}

CampaignSaveRoot campaignSaveRoot(const std::filesystem::path& modeRoot, std::string_view campaign, const nox::modes::Registry& registry)
{
    const nox::modes::Mode* mod = campaignMod(campaign, registry);
    if (!mod) {
        return {modeRoot, false};
    }
    if (std::filesystem::path own = nox::modes::campaignSavesFolder(*mod); !own.empty()) {
        return {std::move(own), false};
    }
    return {modeRoot / std::filesystem::path("Mods") / mod->info.id, true};
}

} // namespace noxsdl
