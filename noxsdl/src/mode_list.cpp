// The mode lists' rows (mode_list.hpp).
#include "mode_list.hpp"

#include "noxcore/strings/l10n.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace noxsdl {

namespace {

/// A built-in mode's own words (its registry's English in modes/: kept English there, as hosts and
/// the dedicated server have them), shown translated. A mod's text is its own.
std::string builtInText(const std::string& en)
{
    static constexpr nox::l10n::Key kTexts[] = {
        NOX_TRK("modes.coop.name", "Co-op Campaign"),
        NOX_TRK("modes.coop.description", "The story campaign, with friends."),
        NOX_TRK("modes.delve.description", "Endless generated levels to fight through."),
        NOX_TRK("modes.tide.description", "Survive an endless tide of the undead."),
        NOX_TRK("modes.coop.difficulty", "Difficulty"),
        NOX_TRK("modes.coop.difficulty_easy", "Easy"),
        NOX_TRK("modes.coop.difficulty_normal", "Normal"),
        NOX_TRK("modes.coop.difficulty_hard", "Hard"),
        NOX_TRK("modes.coop.difficulty_nightmare", "Nightmare"),
        NOX_TRK("modes.coop.campaign", "Campaign"),
        NOX_TRK("modes.coop.class_warrior", "Warrior"),
        NOX_TRK("modes.coop.class_wizard", "Wizard"),
        NOX_TRK("modes.coop.class_conjurer", "Conjurer"),
    };
    for (const auto& k : kTexts) {
        if (en == k.en) {
            return nox::l10n::tr(k);
        }
    }
    return en;
}

} // namespace

std::vector<ModeListRow> modeListRows(ModeListFor list, const nox::modes::Registry& registry, const nox::modes::ModsReport& report,
                                      const std::function<bool(std::string_view)>& soloScreens)
{
    // Those built into the program first (its modes by their order, Quest, the Map Editor), then the
    // mods folder's in the same order, then the folders that didn't load.
    std::vector<ModeListRow> rows;
    std::vector<ModeListRow> mods;
    const std::vector<const nox::modes::Mode*> all = registry.all();
    for (const nox::modes::Mode* m : all) {
        if (!m->makeServer || (list == ModeListFor::GameModes && m->info.playerHost) || m->info.campaign) {
            continue; // (a campaign: its mode's Campaign choice lists it, below)
        }
        ModeListRow r;
        r.fromMods = m->info.fromMods;
        r.id = m->info.id;
        r.name = m->info.name;
        r.description = m->info.description;
        r.players = m->info.minPlayers == m->info.maxPlayers ? std::to_string(m->info.maxPlayers)
                                                             : std::to_string(m->info.minPlayers) + "-" + std::to_string(m->info.maxPlayers);
        r.stock = m->info.stockClients ? NOX_TR("menus.modes.yes", "Yes") : NOX_TR("menus.modes.no", "No");
        r.levels = m->info.levels == nox::modes::ModeInfo::Levels::Generated ? NOX_TR("menus.modes.levels_generated", "Generated")
                                                                             : NOX_TR("menus.modes.levels_map_files", "Map files");
        for (const nox::modes::OptionSpec& s : m->info.options) {
            if (s.hostPanel && s.kind != nox::modes::OptionSpec::Kind::Text) {
                r.options.push_back(s); // (text settings: the dedicated server's command line only, for now)
            }
        }
        if (!r.fromMods) {
            // (What's shown only: an option's key and its values' numbers stay as they are.)
            r.name = builtInText(r.name);
            r.description = builtInText(r.description);
            for (nox::modes::OptionSpec& s : r.options) {
                s.label = builtInText(s.label);
                for (auto& c : s.choices) {
                    c = builtInText(c);
                }
            }
        }
        // A mode that runs campaigns: the mods' campaigns beside its own, its first setting (none
        // installed: no choice, the row as it always was).
        if (m->info.runsCampaigns) {
            nox::modes::OptionSpec choice{.key = "campaign", .label = NOX_TR("modes.coop.campaign", "Campaign"), .kind = nox::modes::OptionSpec::Kind::Choice};
            for (const nox::modes::Mode* c : all) {
                if (c->info.campaign && c->makeServer) {
                    if (r.campaigns.empty()) {
                        r.campaigns.emplace_back();
                        choice.choices.push_back(NOX_TR("menus.modes.campaign_own", "The game's own"));
                    }
                    r.campaigns.push_back(c->info.id);
                    choice.choices.push_back(c->info.campaign->title);
                }
            }
            if (!r.campaigns.empty()) {
                r.options.insert(r.options.begin(), std::move(choice));
            }
        }
        r.soloScreens = soloScreens && soloScreens(m->info.id);
        (r.fromMods ? mods : rows).push_back(std::move(r));
    }
    for (const auto& b : report.broken) {
        ModeListRow r;
        r.kind = ModeListRow::Kind::Broken;
        r.fromMods = true;
        r.id = b.folder.filename().string();
        r.name = r.id;
        r.description = b.why;
        mods.push_back(std::move(r));
    }
    ModeListRow quest;
    quest.kind = ModeListRow::Kind::Quest;
    quest.name = NOX_TR("menus.modes.quest_name", "Quest");
    quest.description = NOX_TR("menus.modes.quest_description", "The original quest, stage after stage.");
    quest.stock = NOX_TR("menus.modes.yes", "Yes");
    quest.levels = NOX_TR("menus.modes.levels_map_files", "Map files");
    rows.push_back(std::move(quest));
    ModeListRow editor;
    editor.kind = ModeListRow::Kind::Editor;
    editor.name = NOX_TR("menus.modes.editor_name", "Map Editor");
    editor.levels = NOX_TR("menus.modes.levels_map_files", "Map files");
    if (list == ModeListFor::HostCustom) {
        // (A session is a live game: stock players play it as it's published, NoX Core's edit.)
        editor.description = NOX_TR("menus.modes.editor_description_hosted", "Build a map together with others.");
        editor.players = "1-32";
        editor.stock = NOX_TR("menus.modes.yes", "Yes");
    } else {
        editor.description = NOX_TR("menus.modes.editor_description_local", "Build and edit your own maps.");
        editor.players = "1";
    }
    rows.push_back(std::move(editor));
    rows.insert(rows.end(), std::make_move_iterator(mods.begin()), std::make_move_iterator(mods.end()));
    return rows;
}

std::string chosenCampaign(const ModeListRow& row, const nox::modes::OptionValues& set)
{
    if (row.campaigns.empty() || row.options.empty() || row.options.front().key != "campaign") {
        return {};
    }
    const int v = modeOptionValue(set, row.options.front());
    return v >= 0 && v < static_cast<int>(row.campaigns.size()) ? row.campaigns[static_cast<std::size_t>(v)] : std::string();
}

std::string modeListKey(const ModeListRow& row)
{
    switch (row.kind) {
    case ModeListRow::Kind::Quest: return "@quest";
    case ModeListRow::Kind::Editor: return "@editor";
    case ModeListRow::Kind::Broken: return {};
    default: return row.id;
    }
}

int modeListRowOf(const std::vector<ModeListRow>& rows, std::string_view key)
{
    int quest = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == ModeListRow::Kind::Broken) {
            continue;
        }
        if (!key.empty() && modeListKey(rows[i]) == key) {
            return static_cast<int>(i);
        }
        if (rows[i].kind == ModeListRow::Kind::Quest) {
            quest = static_cast<int>(i);
        }
    }
    return quest;
}

int modeOptionValue(const nox::modes::OptionValues& set, const nox::modes::OptionSpec& s)
{
    if (const auto v = nox::modes::option(set, s.key)) {
        return std::atoi(std::string(*v).c_str());
    }
    return s.defaultValue;
}

std::string modeOptionText(const nox::modes::OptionSpec& s, int v)
{
    switch (s.kind) {
    case nox::modes::OptionSpec::Kind::Choice:
        return v >= 0 && v < static_cast<int>(s.choices.size()) ? s.choices[static_cast<std::size_t>(v)] : std::to_string(v);
    case nox::modes::OptionSpec::Kind::Toggle: return v != 0 ? NOX_TR("menus.modes.option_on", "On") : NOX_TR("menus.modes.option_off", "Off");
    default: return std::to_string(v);
    }
}

int stepModeOption(const nox::modes::OptionSpec& s, int v, int by)
{
    switch (s.kind) {
    case nox::modes::OptionSpec::Kind::Choice: {
        const int n = std::max(1, static_cast<int>(s.choices.size()));
        return ((v + by) % n + n) % n;
    }
    case nox::modes::OptionSpec::Kind::Toggle: return v != 0 ? 0 : 1;
    case nox::modes::OptionSpec::Kind::Number:
        if (v + by > s.max) {
            return s.min;
        }
        if (v + by < s.min) {
            return s.max;
        }
        return v + by;
    default: return v;
    }
}

} // namespace noxsdl
