// The mode lists' rows (menus_modes.cpp): Host Custom's, the modes hosted for others, and the main
// menu's Game Modes, the same list played on this computer. Both are laid out as the game list:
// first what's built into the program (its modes, then Quest and the Map Editor), then the mods
// folder's modes, then each mods folder that didn't load, greyed with why. No SDL here (so it's testable alone).
#pragma once

#include "noxcore/modes/mode.hpp"
#include "noxcore/modes/mods.hpp"
#include "noxcore/modes/registry.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

/// Which list: Host Custom's (the Host Quest button: hosted, others may join) or the main menu's
/// Game Modes (its Solo Quest button: on this computer, unlisted).
enum class ModeListFor { HostCustom, GameModes };

/// One row of a mode list.
struct ModeListRow {
    enum class Kind {
        Mode,   ///< a NoX Core mode (the registry's)
        Broken, ///< a mods folder that didn't load (`description`: why)
        Quest,  ///< the game's own quest
        Editor, ///< the map editor (Host Custom's: a session others join)
    };
    Kind kind = Kind::Mode;
    std::string id; ///< the mode's (a broken folder's name; empty for Quest and the editor)
    std::string name;
    std::string description;
    std::string players;
    std::string stock;
    std::string levels;
    bool soloScreens = false; ///< started through the solo screens (a campaign mode)
    bool fromMods = false;    ///< the mods folder's (ModeInfo::fromMods, or a folder that didn't load)
    std::vector<nox::modes::OptionSpec> options; ///< the settings shown under it
    /// A mode that runs campaigns (ModeInfo::runsCampaigns) with a mod's installed: the Campaign
    /// choice's (options' "campaign", first) mode ids by its value, "" the mode's own. Empty: none.
    std::vector<std::string> campaigns;
    /// By the same value: each campaign's title (the choice's label) and its mod's description;
    /// the mode's own's empty.
    std::vector<std::string> campaignTitles;
    std::vector<std::string> campaignAbout;
    std::vector<std::string> campaignStock; ///< (its Stock clients column: a mod that refuses them, "No")
    /// By the same value: the settings a campaign's mod shows on its own (its Host Custom ones),
    /// shown after the row's own while it's chosen, each key "<mod id>.<its key>" (a mode's own
    /// keys are a-z, 0-9 and _: never one of these); the mode's own: none.
    std::vector<std::vector<nox::modes::OptionSpec>> campaignOptions;
    /// Built into the program (the lists put these first and mark them): Quest, the Map Editor and
    /// the modes registerBuiltinModes adds.
    bool builtIn() const { return !fromMods; }
};

/// The list's rows: the modes built into the program that have a server side (Game Modes: those a
/// player's own game alone doesn't host) by their order, Quest and the Map Editor; then the mods
/// folder's modes the same way; then each folder `report` says didn't load. `soloScreens` says
/// whether a mode starts through the solo screens (mode_ui.hpp; none: never). A mod's campaign
/// (ModeInfo::campaign) is no row of its own: each mode that runs campaigns lists them in its
/// Campaign choice, its first setting (with none installed, no such choice).
std::vector<ModeListRow> modeListRows(ModeListFor list, const nox::modes::Registry& registry, const nox::modes::ModsReport& report,
                                      const std::function<bool(std::string_view)>& soloScreens = {});

/// The campaign `set` (a row's settings) chooses on `row` (its Campaign choice): its mode id; empty
/// for the row's own campaign, or a row without the choice.
std::string chosenCampaign(const ModeListRow& row, const nox::modes::OptionValues& set);
/// Its value (0: the row's own campaign, or a row without the choice).
std::size_t chosenCampaignIndex(const ModeListRow& row, const nox::modes::OptionValues& set);

/// The settings shown under `row` with `set`: its own, then the chosen campaign's (campaignOptions:
/// none for the row's own campaign). How many, and the `k`th (null past the end).
std::size_t shownOptionCount(const ModeListRow& row, const nox::modes::OptionValues& set);
const nox::modes::OptionSpec* shownOption(const ModeListRow& row, const nox::modes::OptionValues& set, std::size_t k);

/// What `row` is hosted with from its settings `set`: the chosen campaign's mode id as the option
/// "campaign" (none for the row's own), its own settings kept, any other campaign's left out. A row
/// without the Campaign choice: `set` without "campaign", as always.
nox::modes::OptionValues hostedModeOptions(const ModeListRow& row, nox::modes::OptionValues set);

/// The key a row is chosen again by, next time the list opens: a mode's id, "@quest", "@editor";
/// empty for a broken folder (never kept).
std::string modeListKey(const ModeListRow& row);
/// The row `key` names, else Quest's (the first row if there's none).
int modeListRowOf(const std::vector<ModeListRow>& rows, std::string_view key);

/// A setting's value in `set` (its default when not set).
int modeOptionValue(const nox::modes::OptionValues& set, const nox::modes::OptionSpec& s);
/// A value as the panel shows it (a choice's label, On / Off, the number).
std::string modeOptionText(const nox::modes::OptionSpec& s, int v);
/// The setting stepped once (`by` -1 back, +1 on): a choice and a number go round their range.
int stepModeOption(const nox::modes::OptionSpec& s, int v, int by);

} // namespace noxsdl
