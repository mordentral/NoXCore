// A mod's campaign as noxcore.exe's menus, save folders and hosting see it: one rule for all of
// them (registry.hpp's installedCampaign: exactly the lowercase plain id of an installed mod that
// declares a campaign). No SDL: the tests build it.
#pragma once

#include "noxcore/modes/registry.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace noxsdl {

/// The installed campaign mod `campaign` (its mode id) names, in `registry` (the process's by
/// default); null for the game's own (empty) or anything that isn't one, another case included.
const nox::modes::Mode* campaignMod(std::string_view campaign, const nox::modes::Registry& registry = nox::modes::registry());

/// A campaign's id as a player typed it (the command line): lower-cased, since a mod's id is
/// always lowercase (mode.json's rule); anything else in it left for campaignMod to refuse.
std::string typedCampaignId(std::string_view typed);

/// Mod `campaign`'s campaign title (mode.json's "campaign"); empty when it's none (or `campaign` is
/// empty: the game's own).
std::string campaignTitleOf(std::string_view campaign, const nox::modes::Registry& registry = nox::modes::registry());

/// Where mod `campaign`'s map `name` is: the one it ships (its maps\<name>\<name>.map: the index its
/// games find maps by, Mode::maps), else the game's maps folder's.
std::filesystem::path campaignMapPath(const std::filesystem::path& gameDir, std::string_view campaign, const std::string& name,
                                      const nox::modes::Registry& registry = nox::modes::registry());

/// Where a solo-screens mode's saves go for `campaign`, `modeRoot` being the mode's own: an
/// installed campaign mod's own folder's saves\ (registry.hpp's campaignSavesFolder), else, where
/// that can't be written (a read-only install), <modeRoot>\Mods\<id> (`fellBack`); the mode's own
/// root for the game's own campaign and for an id that names no installed campaign mod.
struct CampaignSaveRoot {
    std::filesystem::path root;
    bool fellBack = false;
};
CampaignSaveRoot campaignSaveRoot(const std::filesystem::path& modeRoot, std::string_view campaign,
                                  const nox::modes::Registry& registry = nox::modes::registry());

} // namespace noxsdl
