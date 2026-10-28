// The mode lists' rows (mode_list.hpp).
#include "mode_list.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace noxsdl {

std::vector<ModeListRow> modeListRows(ModeListFor list, const nox::modes::Registry& registry, const nox::modes::ModsReport& report,
                                      const std::function<bool(std::string_view)>& soloScreens)
{
    // Those built into the program first (its modes by their order, Quest, the Map Editor), then the
    // mods folder's in the same order, then the folders that didn't load.
    std::vector<ModeListRow> rows;
    std::vector<ModeListRow> mods;
    for (const nox::modes::Mode* m : registry.all()) {
        if (!m->makeServer || (list == ModeListFor::GameModes && m->info.playerHost)) {
            continue;
        }
        ModeListRow r;
        r.fromMods = m->info.fromMods;
        r.id = m->info.id;
        r.name = m->info.name;
        r.description = m->info.description;
        r.players = m->info.minPlayers == m->info.maxPlayers ? std::to_string(m->info.maxPlayers)
                                                             : std::to_string(m->info.minPlayers) + "-" + std::to_string(m->info.maxPlayers);
        r.stock = m->info.stockClients ? "Yes" : "No";
        r.levels = m->info.levels == nox::modes::ModeInfo::Levels::Generated ? "Generated" : "Map files";
        for (const nox::modes::OptionSpec& s : m->info.options) {
            if (s.hostPanel && s.kind != nox::modes::OptionSpec::Kind::Text) {
                r.options.push_back(s); // (text settings: the dedicated server's command line only, for now)
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
    quest.name = "Quest";
    quest.description = "The original quest, stage after stage.";
    quest.stock = "Yes";
    quest.levels = "Map files";
    rows.push_back(std::move(quest));
    ModeListRow editor;
    editor.kind = ModeListRow::Kind::Editor;
    editor.name = "Map Editor";
    editor.levels = "Map files";
    if (list == ModeListFor::HostCustom) {
        // (A session is a live game: stock players play it as it's published, NoX Core's edit.)
        editor.description = "Build a map together with others.";
        editor.players = "1-32";
        editor.stock = "Yes";
    } else {
        editor.description = "Build and edit your own maps.";
        editor.players = "1";
    }
    rows.push_back(std::move(editor));
    rows.insert(rows.end(), std::make_move_iterator(mods.begin()), std::make_move_iterator(mods.end()));
    return rows;
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
    case nox::modes::OptionSpec::Kind::Toggle: return v != 0 ? "On" : "Off";
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
