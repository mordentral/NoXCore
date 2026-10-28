// The mode lists' rows (menus_modes.cpp): Host Custom's, the modes hosted for others, and the main
// menu's Game Modes, the same list played on this computer. Both are laid out as the game list:
// first what's built into the program (its modes, then Quest and the Map Editor), then the mods
// folder's modes, then each mods folder that didn't load, greyed with why. No SDL here (so it's testable alone).
#pragma once

#include "noxcore/modes/mode.hpp"
#include "noxcore/modes/mods.hpp"
#include "noxcore/modes/registry.hpp"

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
    /// Built into the program (the lists put these first and mark them): Quest, the Map Editor and
    /// the modes registerBuiltinModes adds.
    bool builtIn() const { return !fromMods; }
};

/// The list's rows: the modes built into the program that have a server side (Game Modes: those a
/// player's own game alone doesn't host) by their order, Quest and the Map Editor; then the mods
/// folder's modes the same way; then each folder `report` says didn't load. `soloScreens` says
/// whether a mode starts through the solo screens (mode_ui.hpp; none: never).
std::vector<ModeListRow> modeListRows(ModeListFor list, const nox::modes::Registry& registry, const nox::modes::ModsReport& report,
                                      const std::function<bool(std::string_view)>& soloScreens = {});

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
