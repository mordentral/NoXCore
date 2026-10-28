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

constexpr const char* kTitle = "Host a map editor session";

using nox::util::lowered;

/// A new map's floored area (as the editor's New dialog offers it).
constexpr int kSizes[] = {0, 16, 32, 48, 64, 96, 128, 192, 254};

std::string sizeLabel(int size)
{
    return size == 0 ? std::string("No floor") : std::to_string(size) + " x " + std::to_string(size) + " cells";
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
        ImGui::OpenPopup(kTitle);
        justOpened_ = false;
    }
    const float u = ImGui::GetFontSize(); // (sizes in lines of text: the editor's scale follows its font)
    ImGui::SetNextWindowSize(ImVec2(36 * u, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        // Closed from outside (another popup opened over the editor's frame): as Cancel.
        open_ = false;
        cancelled_ = true;
        return false;
    }

    // ---- the map to start from
    ImGui::SeparatorText("Map");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("##search", "Search maps", search_, sizeof search_);
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
    if (ImGui::Selectable("New blank map", s_.newMap)) {
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
        const std::string label = m.name + (m.stock ? "   (stock: copied under a new name)" : "");
        if (verbatimSelectable(i, label, !s_.newMap && selected_ == i)) {
            choose(i, false);
        }
    }
    if (shown == 0) {
        ImGui::TextDisabled(maps_.empty() ? "No maps found." : "No map matches.");
    }
    ImGui::EndChild();
    if (s_.newMap) {
        if (ImGui::BeginCombo("Floored area", sizeLabel(s_.newOptions.size).c_str())) {
            for (const int size : kSizes) {
                if (ImGui::Selectable(sizeLabel(size).c_str(), size == s_.newOptions.size)) {
                    s_.newOptions.size = size;
                }
            }
            ImGui::EndCombo();
        }
        help("A square of floor in the middle of the map (the whole map is 256 x 256 wall cells), with a player start.");
    }

    // ---- the session's map name: what is published (never a stock map's)
    ImGui::InputText("Session map name", name_, sizeof name_);
    help("The map the session publishes, in your maps folder. A stock map is copied under this name.");
    const bool chosen = s_.newMap || (selected_ >= 0 && selected_ < static_cast<int>(maps_.size()));
    if (!chosen) {
        problem_ = "Choose a map to start from.";
    } else if (const auto p = me::mapNameProblem(name_)) {
        problem_ = *p;
    } else if (me::isStockMapName(name_)) {
        problem_ = std::string(name_) + " is a map that ships with the game: choose a new name";
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
            ImGui::TextDisabled("Edits %s itself: publishing writes over it.", name_);
        } else if (clash) {
            ImGui::TextColored(themeColours().warning, "A map with this name is there: hosting replaces it.");
        } else {
            ImGui::TextDisabled("Writes %s\\%s.map", name_, name_);
        }
    }
    if (!s_.newMap && selected_ >= 0 && selected_ < static_cast<int>(maps_.size())) {
        const SessionMapChoice& m = maps_[static_cast<std::size_t>(selected_)];
        if (const auto p = m.stock ? std::nullopt : me::mapNameProblem(m.name)) {
            // (A map named before the limit: copied under a name that fits, as a stock map is.)
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s can't be published under its own name (%s): the session copies it under a new one.",
                                m.name.c_str(), p->c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // ---- who may edit
    ImGui::SeparatorText("Who may edit");
    int mode = s_.editMode == me::session::EditMode::HostOnly ? 0 : 1;
    if (ImGui::RadioButton("Only me", mode == 0)) {
        mode = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Others when I approve", mode == 1)) {
        mode = 1;
    }
    help("NoX Core players ask to edit; you approve each one (once, or always for that player).");
    s_.editMode = mode == 0 ? me::session::EditMode::HostOnly : me::session::EditMode::Approve;

    // ---- the running game
    ImGui::SeparatorText("The game");
    ImGui::Checkbox("Players can't die (sandbox)", &s_.playersCantDie);
    help("While editing, everyone plays the map in a sandbox; a play-test round can let them die.");
    ImGui::Checkbox("Show live players in the editor", &s_.liveView);
    ImGui::Indent();
    ImGui::BeginDisabled(!s_.liveView);
    ImGui::Checkbox("...and creatures", &s_.liveCreatures);
    ImGui::EndDisabled();
    ImGui::Unindent();
    ImGui::Checkbox("Publish object changes automatically", &s_.autoPublish);
    help("Object changes are written for players who join later; walls, floors and the script wait for a full publish.");
    const auto current = std::ranges::find_if(kSessionModes, [&](const auto& m) { return m.bits == s_.playTestMode; });
    if (ImGui::BeginCombo("Play-test mode", current != std::end(kSessionModes) ? current->name : "Arena")) {
        for (const auto& [bits, label] : kSessionModes) {
            if (ImGui::Selectable(label, bits == s_.playTestMode)) {
                s_.playTestMode = bits;
            }
        }
        ImGui::EndCombo();
    }
    help("The mode a play-test round runs (the map must allow it).");
    ImGui::InputText("Game password", password_, 9); // (8 characters at most)
    help("Asked of everyone joining (the game's own prompt for stock clients). Empty: none.");
    ImGui::SliderInt("Player limit", &s_.maxPlayers, 2, 32);
    s_.maxPlayers = std::clamp(s_.maxPlayers, 2, 32);

    // ---- where Host lists it (online only with an XWIS login: main.cpp's host_listing.hpp)
    if (!listing_.text.empty()) {
        ImGui::SeparatorText("Game list");
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
    if (ImGui::Button("Host", button)) {
        host = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
            return "Can't host \"" + s.name + "\": " + *problem;
        }
        if (me::isStockMapName(s.name)) { // (mapNameProblem's too: never a stock map's name)
            return "Can't host \"" + s.name + "\": it is a map that ships with the game";
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
            return "No map chosen to start from.";
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
            return "No map " + s.sourceMap + " in " + pathText(mapsDir) + (gameMapsDir.empty() ? "" : " or " + pathText(gameMapsDir));
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
        return "Can't prepare the session's map " + s.name + ":\n" + ex.what();
    }
}

} // namespace noxsdl::editor
