// Host Custom > Map Editor's and the editor's File > Host a Session's session dialog, and the
// session's map written where it's published (session_setup.hpp).
#include "editor/session_setup.hpp"

#include "editor/editor_text.hpp"
#include "editor/name_label.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/sidecar.hpp"
#include "noxcore/mapedit/validate.hpp"
#include "noxcore/mapedit/writer.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <exception>
#include <iterator>
#include <optional>
#include <system_error>

namespace fs = std::filesystem;
namespace me = nox::mapedit;

namespace noxsdl::editor {

namespace {

/// The dialog's title (its popup's id too).
const char* title()
{
    return NOX_TRI("editor.session.setup_title", "Host a map editor session");
}

using nox::util::lowered;

/// A new map's floored area (as the editor's New dialog offers it).
constexpr int kSizes[] = {0, 16, 32, 48, 64, 96, 128, 192, 254};

std::string sizeLabel(int size)
{
    return size == 0 ? NOX_TR("editor.session.no_floor", "No floor")
                     : nox::l10n::format(NOX_TR("editor.session.floor_cells", "{n} x {n} cells"), {{"n", size}});
}

/// The session map's name a choice suggests: a user map's own (whatever its characters: '!TakRP'),
/// a stock map's copy name (suggestedMapName: never published under the stock name) and a copy's
/// name too for a user map named past the game's 8 characters ("estate_ed": it opens, but is
/// published only under a name that fits), "newmap" for a new map.
std::string suggestedName(const std::vector<SessionMapChoice>& maps, int selected, bool newMap)
{
    if (newMap || selected < 0 || selected >= static_cast<int>(maps.size())) {
        return newMap ? std::string("newmap") : std::string();
    }
    const SessionMapChoice& m = maps[static_cast<std::size_t>(selected)];
    return m.stock || me::mapNameProblem(m.name) ? me::suggestedMapName(m.name) : m.name;
}

void copyText(char* buf, std::size_t size, const std::string& text)
{
    std::snprintf(buf, size, "%s", text.c_str());
}

} // namespace

void SessionSetupDialog::open(std::vector<SessionMapChoice> maps, SessionSetup defaults, SessionListingNote listing)
{
    maps_ = std::move(maps);
    s_ = std::move(defaults);
    listing_ = std::move(listing);
    selected_ = -1;
    if (!s_.newMap && !s_.sourceMap.empty()) {
        const std::string want = lowered(s_.sourceMap);
        for (int i = 0; i < static_cast<int>(maps_.size()); ++i) {
            if (lowered(maps_[static_cast<std::size_t>(i)].name) == want) {
                selected_ = i;
                break;
            }
        }
    }
    search_[0] = 0;
    copyText(name_, sizeof name_, !s_.name.empty() ? s_.name : suggestedName(maps_, selected_, s_.newMap));
    copyText(password_, 9, s_.password); // (8 characters at most, as the game's)
    s_.maxPlayers = std::clamp(s_.maxPlayers, 2, 32);
    problem_.clear();
    open_ = true;
    justOpened_ = true;
    cancelled_ = false;
}

bool SessionSetupDialog::draw(SessionSetup& out)
{
    if (!open_) {
        return false;
    }
    if (justOpened_) {
        ImGui::OpenPopup(title());
        justOpened_ = false;
    }
    const float u = ImGui::GetFontSize(); // (sizes in lines of text: the editor's scale follows its font)
    ImGui::SetNextWindowSize(ImVec2(36 * u, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        // Closed from outside (another popup opened over the editor's frame): as Cancel.
        open_ = false;
        cancelled_ = true;
        return false;
    }

    // ---- the map to start from
    ImGui::SeparatorText(NOX_TRC("editor.session.setup_map", "Map"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("##search", NOX_TRC("editor.session.search_maps", "Search maps"), search_, sizeof search_);
    ImGui::BeginChild("##maps", ImVec2(0, 12 * ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_Borders);
    // A choice changed: the session's name follows it unless it was typed over (it still reads
    // the old choice's suggestion).
    const auto choose = [this](int index, bool newMap) {
        const bool followed = name_ == suggestedName(maps_, selected_, s_.newMap) || name_[0] == 0;
        selected_ = index;
        s_.newMap = newMap;
        if (followed) {
            copyText(name_, sizeof name_, suggestedName(maps_, selected_, s_.newMap));
        }
    };
    if (ImGui::Selectable(NOX_TRI("editor.session.new_blank_map", "New blank map"), s_.newMap)) {
        choose(-1, true);
    }
    const std::string q = lowered(search_);
    int shown = 0;
    for (int i = 0; i < static_cast<int>(maps_.size()); ++i) {
        const SessionMapChoice& m = maps_[static_cast<std::size_t>(i)];
        if (!q.empty() && lowered(m.name).find(q) == std::string::npos) {
            continue;
        }
        ++shown;
        const std::string label = m.name + (m.stock ? NOX_TR("editor.session.stock_copied", "   (stock: copied under a new name)") : std::string());
        if (verbatimSelectable(i, label, !s_.newMap && selected_ == i)) {
            choose(i, false);
        }
    }
    if (shown == 0) {
        ImGui::TextDisabled("%s", maps_.empty() ? NOX_TRC("editor.session.no_maps", "No maps found.") : NOX_TRC("editor.session.no_match", "No map matches."));
    }
    ImGui::EndChild();
    if (s_.newMap) {
        if (ImGui::BeginCombo(NOX_TRI("editor.session.floored_area", "Floored area"), sizeLabel(s_.newOptions.size).c_str())) {
            for (const int size : kSizes) {
                if (ImGui::Selectable(sizeLabel(size).c_str(), size == s_.newOptions.size)) {
                    s_.newOptions.size = size;
                }
            }
            ImGui::EndCombo();
        }
        help(NOX_TRC("editor.session.floored_area_tip",
                     "A square of floor in the middle of the map (the whole map is 256 x 256 wall cells), with a player start."));
    }

    // ---- the session's map name: what is published (never a stock map's)
    ImGui::InputText(NOX_TRI("editor.session.map_name", "Session map name"), name_, sizeof name_);
    help(NOX_TRC("editor.session.map_name_tip", "The map the session publishes, in your maps folder. A stock map is copied under this name."));
    const bool chosen = s_.newMap || (selected_ >= 0 && selected_ < static_cast<int>(maps_.size()));
    if (!chosen) {
        problem_ = NOX_TR("editor.session.choose_map", "Choose a map to start from.");
    } else if (const auto p = me::mapNameProblem(name_)) {
        problem_ = *p;
    } else if (me::isStockMapName(name_)) {
        problem_ = nox::l10n::format(NOX_TR("editor.session.stock_name", "{name} is a map that ships with the game: choose a new name"), {{"name", name_}});
    } else {
        problem_.clear();
    }
    if (!problem_.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(themeColours().error, "%s", problem_.c_str());
        ImGui::PopTextWrapPos();
    } else {
        // Hosting writes over a user map of this name unless it's the map chosen itself.
        const std::string lname = lowered(name_);
        const bool itself = !s_.newMap && !maps_[static_cast<std::size_t>(selected_)].stock &&
                            lowered(maps_[static_cast<std::size_t>(selected_)].name) == lname;
        const bool clash = std::ranges::any_of(maps_, [&](const SessionMapChoice& m) { return !m.stock && lowered(m.name) == lname; });
        if (itself) {
            ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.session.edits_itself", "Edits {name} itself: publishing writes over it."), {{"name", name_}}).c_str());
        } else if (clash) {
            ImGui::TextColored(themeColours().warning, "%s", NOX_TRC("editor.session.name_clash", "A map with this name is there: hosting replaces it."));
        } else {
            ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.session.writes", "Writes {name}\\{name}.map"), {{"name", name_}}).c_str());
        }
    }
    if (!s_.newMap && selected_ >= 0 && selected_ < static_cast<int>(maps_.size())) {
        const SessionMapChoice& m = maps_[static_cast<std::size_t>(selected_)];
        if (const auto p = m.stock ? std::nullopt : me::mapNameProblem(m.name)) {
            // (A map named before the limit: copied under a name that fits, as a stock map is.)
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.session.renamed_copy",
                                                               "{name} can't be published under its own name ({why}): the session copies it under a new one."),
                                                        {{"name", m.name}, {"why", *p}})
                                          .c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // ---- who may edit
    ImGui::SeparatorText(NOX_TRC("editor.session.who_may_edit", "Who may edit"));
    int mode = s_.editMode == me::session::EditMode::HostOnly ? 0 : 1;
    if (ImGui::RadioButton(NOX_TRI("editor.session.only_me", "Only me"), mode == 0)) {
        mode = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(NOX_TRI("editor.session.others_approve", "Others when I approve"), mode == 1)) {
        mode = 1;
    }
    help(NOX_TRC("editor.session.who_may_edit_tip", "NoX Core players ask to edit; you approve each one (once, or always for that player)."));
    s_.editMode = mode == 0 ? me::session::EditMode::HostOnly : me::session::EditMode::Approve;

    // ---- the running game
    ImGui::SeparatorText(NOX_TRC("editor.session.the_game", "The game"));
    ImGui::Checkbox(NOX_TRI("editor.session.cant_die_sandbox", "Players can't die (sandbox)"), &s_.playersCantDie);
    help(NOX_TRC("editor.session.cant_die_tip", "While editing, everyone plays the map in a sandbox; a play-test round can let them die."));
    ImGui::Checkbox(NOX_TRI("editor.session.show_live_players", "Show live players in the editor"), &s_.liveView);
    ImGui::Indent();
    ImGui::BeginDisabled(!s_.liveView);
    ImGui::Checkbox(NOX_TRI("editor.session.and_creatures", "...and creatures"), &s_.liveCreatures);
    ImGui::EndDisabled();
    ImGui::Unindent();
    ImGui::Checkbox(NOX_TRI("editor.session.auto_publish_changes", "Publish object changes automatically"), &s_.autoPublish);
    help(NOX_TRC("editor.session.auto_publish_tip",
                 "Object changes are written for players who join later; walls, floors and the script wait for a full publish."));
    const auto current = std::ranges::find_if(kSessionModes, [&](const auto& m) { return m.bits == s_.playTestMode; });
    if (ImGui::BeginCombo(NOX_TRI("editor.session.play_test_mode", "Play-test mode"),
                          nox::l10n::trImGui(current != std::end(kSessionModes) ? current->name : kSessionModes[0].name))) {
        for (const auto& [bits, label] : kSessionModes) {
            if (ImGui::Selectable(nox::l10n::trImGui(label), bits == s_.playTestMode)) {
                s_.playTestMode = bits;
            }
        }
        ImGui::EndCombo();
    }
    help(NOX_TRC("editor.session.play_test_mode_tip", "The mode a play-test round runs (the map must allow it)."));
    ImGui::InputText(NOX_TRI("editor.session.password", "Game password"), password_, 9); // (8 characters at most)
    help(NOX_TRC("editor.session.password_tip", "Asked of everyone joining (the game's own prompt for stock clients). Empty: none."));
    ImGui::SliderInt(NOX_TRI("editor.session.player_limit", "Player limit"), &s_.maxPlayers, 2, 32);
    s_.maxPlayers = std::clamp(s_.maxPlayers, 2, 32);

    // ---- where Host lists it (online only with an XWIS login: main.cpp's host_listing.hpp)
    if (!listing_.text.empty()) {
        ImGui::SeparatorText(NOX_TRC("editor.session.game_list", "Game list"));
        ImGui::PushTextWrapPos(0);
        if (listing_.warning) {
            ImGui::TextColored(themeColours().warning, "%s", listing_.text.c_str());
        } else {
            ImGui::TextUnformatted(listing_.text.c_str());
        }
        ImGui::PopTextWrapPos();
    }

    // ---- Host / Cancel
    ImGui::Separator();
    const ImVec2 button(8 * u, 0);
    bool host = false;
    ImGui::BeginDisabled(!problem_.empty());
    if (ImGui::Button(NOX_TRI("editor.session.host", "Host"), button)) {
        host = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.session.cancel", "Cancel"), button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        open_ = false;
        cancelled_ = true;
        ImGui::CloseCurrentPopup();
    }
    bool hosted = false;
    if (host && open_) {
        hosted = true;
        s_.name = name_;
        s_.password = password_;
        s_.sourceMap = s_.newMap ? std::string() : maps_[static_cast<std::size_t>(selected_)].name;
        if (s_.newMap && s_.newOptions.title.empty()) {
            s_.newOptions.title = s_.name;
        }
        out = s_;
        open_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return hosted;
}

std::string prepareSessionMap(const SessionSetup& s, const fs::path& mapsDir, const fs::path& gameMapsDir, const nox::things::ThingDb& things)
{
    try {
        if (const auto problem = me::mapNameProblem(s.name)) {
            return nox::l10n::format(NOX_TR("editor.session.cant_host", "Can't host \"{name}\": {why}"), {{"name", s.name}, {"why", *problem}});
        }
        if (me::isStockMapName(s.name)) { // (mapNameProblem's too: never a stock map's name)
            return nox::l10n::format(NOX_TR("editor.session.cant_host_stock", "Can't host \"{name}\": it is a map that ships with the game"),
                                     {{"name", s.name}});
        }
        if (s.newMap) {
            me::NewMapOptions options = s.newOptions;
            if (options.title.empty()) {
                options.title = s.name;
            }
            const me::MapDocument doc = me::newMap(things, options);
            me::Sidecar side;
            side.revision = 1;
            me::saveDocument(doc, things, mapsDir, s.name, &side);
            return {};
        }
        if (s.sourceMap.empty()) {
            return NOX_TR("editor.session.no_map_chosen", "No map chosen to start from.");
        }
        // The source: the maps folder's (the user's), else the game's.
        const auto find = [&](const fs::path& dir) -> std::optional<fs::path> {
            if (dir.empty()) {
                return std::nullopt;
            }
            const fs::path p = nox::findNoCase(dir, fs::path(s.sourceMap) / (s.sourceMap + ".map"));
            std::error_code ec;
            return fs::is_regular_file(p, ec) ? std::optional<fs::path>(p) : std::nullopt;
        };
        std::optional<fs::path> source = find(mapsDir);
        const bool inMapsDir = source.has_value();
        if (!source) {
            source = find(gameMapsDir);
        }
        if (!source) {
            return gameMapsDir.empty() ? nox::l10n::format(NOX_TR("editor.session.no_map_in", "No map {map} in {folder}"),
                                                          {{"map", s.sourceMap}, {"folder", pathText(mapsDir)}})
                                       : nox::l10n::format(NOX_TR("editor.session.no_map_in_either", "No map {map} in {folder} or {game_folder}"),
                                                          {{"map", s.sourceMap}, {"folder", pathText(mapsDir)}, {"game_folder", pathText(gameMapsDir)}});
        }
        const bool sourceStock = me::isStockMapName(s.sourceMap);
        if (inMapsDir && !sourceStock && lowered(s.sourceMap) == lowered(s.name)) {
            return {}; // (the session edits that map itself, as it is)
        }
        // Copied under the session's name: the document as read (saveDocument validates it), its
        // sidecar too (the script's source, the editor's settings), else a new one naming it.
        const me::MapDocument doc = me::MapDocument::load(*source, things);
        me::Sidecar side;
        side.origin = s.sourceMap;
        side.originStock = sourceStock;
        const fs::path sideFile = fs::path(*source).replace_extension(".nxe");
        std::error_code ec;
        if (fs::is_regular_file(sideFile, ec)) {
            try {
                const auto bytes = nox::readFileBytes(sideFile);
                side = me::Sidecar::fromJson(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                if (side.origin.empty()) {
                    side.origin = s.sourceMap;
                    side.originStock = sourceStock;
                }
            } catch (const std::exception&) {
                // (an unreadable sidecar is left behind: the editor opens a map without one)
            }
        }
        side.revision += 1;
        me::saveDocument(doc, things, mapsDir, s.name, &side);
        return {};
    } catch (const std::exception& ex) {
        return nox::l10n::format(NOX_TR("editor.session.cant_prepare", "Can't prepare the session's map {name}:\n{error}"),
                                 {{"name", s.name}, {"error", ex.what()}});
    }
}

} // namespace noxsdl::editor
