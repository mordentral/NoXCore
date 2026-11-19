// The map editor's Content Browser panel and the Object tool's options (object_tools.hpp): the
// folder tree, the thumbnail grid, search, class filters, favourites, recently used, user
// categories, prefabs, the floors / walls / edges the floor, wall and edge tools paint with, the details card.
#include "editor/object_tools.hpp"

#include "editor/map_editor.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"
#include "sprite_cache.hpp"

#include "noxcore/mapedit/paint.hpp"
#include "noxcore/mapedit/walls.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>

namespace fs = std::filesystem;
namespace me = nox::mapedit;
namespace oc = nox::things::ObjectClass;

namespace noxsdl {

namespace {

constexpr const char* kFolderFavourites = "Favourites";
constexpr const char* kFolderRecent = "Recently Used";
constexpr const char* kFolderPrefabs = "Prefabs";
constexpr const char* kFloors = "Floors";
constexpr const char* kWalls = "Walls";
constexpr const char* kEdges = "Edges";
constexpr const char* kUser = "User Categories";

using editor::help;
using editor::texId;

/// The class filter chips: the CLASS bits each takes (its label: chipLabel).
constexpr std::uint32_t kChips[] = {
    oc::Monster,
    oc::Weapon | oc::Armor | oc::Food | oc::Wand | oc::Key | oc::InfoBook | oc::Treasure,
    oc::Door | oc::Exit | oc::Transporter | oc::Elevator | oc::ElevatorShaft | oc::Hole,
    oc::Trigger,
    oc::Dangerous | oc::Fire,
    oc::Light,
    oc::Immobile,
};

const char* chipLabel(std::size_t i)
{
    switch (i) {
    case 0: return NOX_TRI("editor.browser.chip.creatures", "Creatures");
    case 1: return NOX_TRI("editor.browser.chip.items", "Items");
    case 2: return NOX_TRI("editor.browser.chip.doors", "Doors & exits");
    case 3: return NOX_TRI("editor.browser.chip.triggers", "Triggers");
    case 4: return NOX_TRI("editor.browser.chip.hazards", "Hazards");
    case 5: return NOX_TRI("editor.browser.chip.lights", "Lights");
    default: return NOX_TRI("editor.browser.chip.immobile", "Immobile");
    }
}

/// A folder's name over the grid: the browser's own folders (kept in English: their keys) in the
/// language in use, the catalog's as they are.
void folderHeading(const std::string& folder)
{
    const std::string_view f = folder;
    const std::string_view user = kUser;
    const char* own = f == kFolderFavourites ? NOX_TRC("editor.browser.folder.favourites", "Favourites")
                      : f == kFolderRecent   ? NOX_TRC("editor.browser.folder.recent", "Recently Used")
                      : f == kFolderPrefabs  ? NOX_TRC("editor.browser.folder.prefabs", "Prefabs")
                      : f == kFloors         ? NOX_TRC("editor.browser.folder.floors", "Floors")
                      : f == kWalls          ? NOX_TRC("editor.browser.folder.walls", "Walls")
                      : f == kEdges          ? NOX_TRC("editor.browser.folder.edges", "Edges")
                                             : nullptr;
    if (own) {
        ImGui::TextDisabled("%s", own);
    } else if (f.starts_with(user) && (f.size() == user.size() || f[user.size()] == '/')) {
        ImGui::TextDisabled("%s%s", NOX_TRC("editor.browser.folder.user", "User Categories"), folder.c_str() + user.size());
    } else {
        ImGui::TextDisabled("%s", me::folderText(folder).c_str());
    }
}

/// One cell of a grid: a picture (fitted, centred) over its name. True when clicked.
bool cell(const char* id, SDL_Texture* tex, ImVec2 uv0, ImVec2 uv1, float texW, float texH, const std::string& name, bool selected, float size,
          bool waiting)
{
    ImGui::PushID(id);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float labelH = ImGui::GetTextLineHeight();
    const bool clicked = ImGui::InvisibleButton("##cell", ImVec2(size, size + labelH + 2));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 frame = selected ? ImGui::GetColorU32(ImGuiCol_HeaderActive) : hovered ? ImGui::GetColorU32(ImGuiCol_HeaderHovered)
                                                                                        : ImGui::GetColorU32(ImGuiCol_FrameBg);
    dl->AddRectFilled(at, ImVec2(at.x + size, at.y + size), frame, 4);
    if (tex && texW > 0 && texH > 0) {
        const float k = std::min((size - 4) / texW, (size - 4) / texH);
        const float w = texW * k, h = texH * k;
        const ImVec2 p0(at.x + (size - w) / 2, at.y + (size - h) / 2);
        dl->AddImage(texId(tex), p0, ImVec2(p0.x + w, p0.y + h), uv0, uv1);
    } else {
        // No picture (yet): its name's first letters.
        const std::string initials = name.substr(0, 2);
        const ImVec2 ts = ImGui::CalcTextSize(initials.c_str());
        dl->AddText(ImVec2(at.x + (size - ts.x) / 2, at.y + (size - ts.y) / 2), ImGui::GetColorU32(waiting ? ImGuiCol_TextDisabled : ImGuiCol_Text),
                    initials.c_str());
    }
    if (selected) dl->AddRect(at, ImVec2(at.x + size, at.y + size), ImGui::ColorConvertFloat4ToU32(editor::themeColours().highlight), 4.0f, ImDrawFlags_None, 2.0f);
    // The name, cut to the cell.
    std::string label = name;
    while (label.size() > 1 && ImGui::CalcTextSize(label.c_str()).x > size) label.pop_back();
    if (label.size() < name.size() && label.size() > 2) label = label.substr(0, label.size() - 1) + "~";
    const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(at.x + (size - ls.x) / 2, at.y + size + 1), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    ImGui::PopID();
    return clicked;
}

} // namespace

std::string ObjectTools::displayName(const me::ContentEntry& e) const
{
    return e.displayName; // (the game's name for it: thingDisplayName; empty for none)
}

bool ObjectTools::thumbnail(std::size_t thing, float size)
{
    if (!thumbs_) {
        ImGui::Dummy(ImVec2(size, size));
        return false;
    }
    const auto c = thumbs_->get(thing);
    if (!c.texture) {
        ImGui::Dummy(ImVec2(size, size));
        return false;
    }
    ImGui::Image(texId(c.texture), ImVec2(size, size), ImVec2(c.u0, c.v0), ImVec2(c.u1, c.v1));
    return true;
}

void ObjectTools::drawThumbnail(const std::string& type, float size)
{
    const auto* e = catalog_.find(type);
    if (!e) {
        ImGui::Dummy(ImVec2(size, size));
        return;
    }
    thumbnail(e->thing, size);
}

bool ObjectTools::drawThumbnailAt(ImDrawList* dl, const std::string& type, const ImVec2& at, float size)
{
    const auto* e = catalog_.find(type);
    if (!e || !thumbs_) {
        return false;
    }
    const auto c = thumbs_->get(e->thing);
    if (!c.texture) {
        return false;
    }
    dl->AddImage(texId(c.texture), at, ImVec2(at.x + size, at.y + size), ImVec2(c.u0, c.v0), ImVec2(c.u1, c.v1));
    return true;
}

void ObjectTools::browse(const std::string& folder)
{
    folder_ = folder;
    focusFolder_ = true;
    showBrowser = true;
    search_[0] = 0;
    ImGui::SetWindowFocus(NOX_TRI("editor.window.content_browser", "Content Browser"));
}

void ObjectTools::pickType(const std::string& type)
{
    if (!catalog_.find(type)) {
        ed_.status(nox::l10n::format(NOX_TR("editor.browser.status.no_type", "No object type {type}"), {{"type", type}}));
        return;
    }
    type_ = type;
    prefab_ = -1;
    if (ed_.tool_ != MapEditor::Tool::Object) {
        ed_.lastTool_ = ed_.tool_;
        ed_.tool_ = MapEditor::Tool::Object;
    }
    // (A type that turns starts facing as it was last turned; a door hangs on a quarter.)
    if (const auto* e = catalog_.find(type); e && e->xfer == "DoorXfer") facing_ = std::round(facing_ / 90.0f) * 90.0f;
}

void ObjectTools::pickPaint(const std::string& folder, int index)
{
    const auto tool = [&](MapEditor::Tool t) {
        if (ed_.tool_ != t) {
            ed_.lastTool_ = ed_.tool_;
            ed_.tool_ = t;
        }
    };
    if (folder == kFloors) {
        ed_.brush_.floor = static_cast<std::uint8_t>(index);
        tool(MapEditor::Tool::Floor);
        ed_.status(nox::l10n::format(NOX_TR("editor.browser.floor_brush", "Floor brush: {name}"), {{"name", ed_.things_.floors[static_cast<std::size_t>(index)].name}}));
    } else if (folder == kWalls) {
        ed_.wallMaterial_ = index;
        ed_.wallVariantMin_ = 0;
        ed_.wallVariantMax_ = std::max(0, me::wallRandomVariations(ed_.things_, index) - 1);
        tool(MapEditor::Tool::Wall);
        ed_.status(nox::l10n::format(NOX_TR("editor.browser.wall_material", "Wall material: {name}"), {{"name", ed_.things_.walls[static_cast<std::size_t>(index)].name}}));
    } else if (folder == kEdges) {
        ed_.edgeSet_ = index;
        tool(MapEditor::Tool::Edge);
        ed_.status(nox::l10n::format(NOX_TR("editor.browser.edge_set", "Edge set: {name}"), {{"name", ed_.things_.edges[static_cast<std::size_t>(index)].name}}));
    }
}

std::vector<const me::ContentEntry*> ObjectTools::shown()
{
    std::vector<const me::ContentEntry*> out;
    const std::string_view q = search_;
    const auto keep = [&](const me::ContentEntry* e) {
        if (!e || (!e->placeable && !showAll_)) return false;
        if (classMask_ != 0 && (e->classes & classMask_) == 0) return false;
        return q.empty() || me::matchesQuery(*e, q, displayName(*e));
    };
    if (!q.empty() && folder_ != kFolderFavourites && folder_ != kFolderRecent && !folder_.starts_with(kUser)) {
        // A search looks through everything.
        for (const auto& e : catalog_.entries) {
            if (keep(&e)) out.push_back(&e);
        }
        return out;
    }
    if (folder_ == kFolderFavourites) {
        for (const auto& f : favourites_) {
            if (const auto* e = catalog_.find(f); keep(e)) out.push_back(e);
        }
    } else if (folder_ == kFolderRecent) {
        for (const auto& r : recent_) {
            if (const auto* e = catalog_.find(r); keep(e)) out.push_back(e);
        }
    } else if (folder_.starts_with(std::string(kUser) + "/")) {
        const std::string name = folder_.substr(std::string(kUser).size() + 1);
        for (const auto& c : userCats_) {
            if (c.name != name) continue;
            for (const auto& n : c.evaluate(catalog_)) {
                if (const auto* e = catalog_.find(n); keep(e)) out.push_back(e);
            }
        }
    } else {
        for (const auto* e : catalog_.inFolder(folder_, recursive_)) {
            if (keep(e)) out.push_back(e);
        }
    }
    return out;
}

void ObjectTools::panels()
{
    if (thumbs_) thumbs_->pump(ed_.ctx_.renderer);
    // A drag from the browser: its ghost follows the mouse; let go over the map, it's placed there.
    dragType_.clear();
    dragPrefab_ = -1;
    if (const ImGuiPayload* p = ImGui::GetDragDropPayload(); p && p->IsDataType("NOX_THING") && p->DataSize == sizeof(int)) {
        const int v = *static_cast<const int*>(p->Data);
        if (v >= 0 && static_cast<std::size_t>(v) < ed_.things_.things.size()) dragType_ = ed_.things_.things[static_cast<std::size_t>(v)].name;
        else if (v < 0) dragPrefab_ = -v - 1;
        if (ed_.panelW_ > 0 && ImGui::Begin(NOX_TRI("editor.window.map", "Map"))) {
            const ImRect r(ImVec2(ed_.panelX_, ed_.panelY_), ImVec2(ed_.panelX_ + ed_.panelW_, ed_.panelY_ + ed_.panelH_));
            if (ImGui::BeginDragDropTargetCustom(r, ImGui::GetID("##mapdrop"))) {
                if (const ImGuiPayload* got = ImGui::AcceptDragDropPayload("NOX_THING")) {
                    const int g = *static_cast<const int*>(got->Data);
                    if (const auto mw = ed_.mouseWorld()) {
                        if (g >= 0) {
                            pickType(ed_.things_.things[static_cast<std::size_t>(g)].name);
                        } else {
                            prefab_ = -g - 1;
                            ed_.tool_ = MapEditor::Tool::Object;
                        }
                        placeAt(placePoint(mw->x, mw->y));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        if (ed_.panelW_ > 0) ImGui::End();
    }
    if (showBrowser) browserPanel();
    if (showList) objectListPanel();
    prefabDialog();
}

void ObjectTools::folderTree()
{
    const auto leaf = [&](const char* label, const std::string& path, const char* tip = nullptr) {
        ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (folder_ == path) f |= ImGuiTreeNodeFlags_Selected;
        ImGui::TreeNodeEx(label, f);
        if (ImGui::IsItemClicked()) folder_ = path;
        if (tip) help(tip);
        if (folder_ == path && focusFolder_) {
            ImGui::SetScrollHereY();
            focusFolder_ = false;
        }
    };
    leaf(NOX_TRI("editor.browser.tree.favourites", "* Favourites"), kFolderFavourites,
         NOX_TRC("editor.browser.tree.favourites_help", "The types you starred (right click a thumbnail)."));
    leaf(NOX_TRI("editor.browser.folder.recent", "Recently Used"), kFolderRecent, NOX_TRC("editor.browser.tree.recent_help", "The last 30 types placed."));
    leaf(NOX_TRI("editor.browser.folder.prefabs", "Prefabs"), kFolderPrefabs,
         NOX_TRC("editor.browser.tree.prefabs_help", "Saved selections, placed again with their links (Edit > Save Selection as Prefab)."));
    ImGui::Separator();
    // The catalog's folders, nested by their paths (parents come before children).
    const auto& folders = catalog_.folders;
    const auto drawChildren = [&](auto&& self, const std::string& parent) -> void {
        for (const auto& f : folders) {
            const auto slash = f.rfind('/');
            const std::string p = slash == std::string::npos ? std::string() : f.substr(0, slash);
            if (p != parent) continue;
            if (!showAll_ && f.starts_with("Hidden")) continue;
            const auto fi = static_cast<std::size_t>(&f - folders.data());
            const std::string& name = catalog_.folderNames[fi]; // (as shown: the path stays its id)
            const bool hasChildren = std::ranges::any_of(folders, [&](const std::string& g) { return g.size() > f.size() && g.starts_with(f) && g[f.size()] == '/'; });
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
            if (folder_ == f) flags |= ImGuiTreeNodeFlags_Selected;
            if (focusFolder_ && folder_.starts_with(f)) ImGui::SetNextItemOpen(true);
            const std::size_t count = catalog_.inFolder(f, true).size();
            const bool open = ImGui::TreeNodeEx(f.c_str(), flags, "%s  (%zu)", name.c_str(), count);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) folder_ = f;
            if (folder_ == f && focusFolder_) {
                ImGui::SetScrollHereY();
                focusFolder_ = false;
            }
            if (open) {
                self(self, f);
                ImGui::TreePop();
            }
        }
    };
    drawChildren(drawChildren, std::string());
    ImGui::Separator();
    leaf(NOX_TRI("editor.browser.folder.floors", "Floors"), kFloors, NOX_TRC("editor.browser.tree.floors_help", "thing.bin's floors: a click picks the floor brush's."));
    leaf(NOX_TRI("editor.browser.folder.walls", "Walls"), kWalls,
         NOX_TRC("editor.browser.tree.walls_help", "thing.bin's wall materials: a click picks the wall tool's."));
    leaf(NOX_TRI("editor.browser.folder.edges", "Edges"), kEdges, NOX_TRC("editor.browser.tree.edges_help", "thing.bin's edge sets: a click picks the edge tool's."));
    ImGui::Separator();
    const bool userOpen = ImGui::TreeNodeEx(NOX_TRI("editor.browser.folder.user", "User Categories"),
                                            ImGuiTreeNodeFlags_SpanAvailWidth | (folder_ == kUser ? ImGuiTreeNodeFlags_Selected : 0));
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) folder_ = kUser;
    help(NOX_TRC("editor.browser.tree.user_help",
                 "Your own categories: the old editor's categories.xml rules (NAME_BEGIN, NAME_CONTAIN, OBJECT_CLASS, ANYTHING...)."));
    if (userOpen) {
        for (const auto& c : userCats_) leaf(c.name.c_str(), std::string(kUser) + "/" + c.name);
        ImGui::TreePop();
    }
}

void ObjectTools::itemGrid()
{
    const float size = cell_ * ed_.imgui_->scale();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float labelH = ImGui::GetTextLineHeight() + 2;
    const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (size + spacing)));
    const auto rows = [&](int count, auto&& draw) {
        const int n = (count + perRow - 1) / perRow;
        ImGuiListClipper clip;
        clip.Begin(n, size + labelH + ImGui::GetStyle().ItemSpacing.y);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                for (int c = 0; c < perRow; ++c) {
                    const int i = r * perRow + c;
                    if (i >= count) break;
                    if (c > 0) ImGui::SameLine();
                    draw(i);
                }
            }
        }
    };
    // Floors, walls and edges: the floor, wall and edge tools' pickers.
    if (folder_ == kFloors || folder_ == kWalls || folder_ == kEdges) {
        const auto& things = ed_.things_;
        const int count = folder_ == kFloors ? static_cast<int>(things.floors.size()) : folder_ == kWalls ? static_cast<int>(things.walls.size())
                                                                                                          : static_cast<int>(things.edges.size());
        std::vector<int> list;
        const std::string q = nox::util::lowered(search_);
        for (int i = 0; i < count; ++i) {
            const std::string& name = folder_ == kFloors ? things.floors[static_cast<std::size_t>(i)].name
                                      : folder_ == kWalls ? things.walls[static_cast<std::size_t>(i)].name
                                                          : things.edges[static_cast<std::size_t>(i)].name;
            if (q.empty() || nox::util::lowered(name).find(q) != std::string::npos) list.push_back(i);
        }
        rows(static_cast<int>(list.size()), [&](int k) {
            const int i = list[static_cast<std::size_t>(k)];
            SDL_Texture* t = nullptr;
            std::string name;
            bool selected = false;
            if (folder_ == kFloors) {
                t = ed_.floorThumb(i, 0);
                name = things.floors[static_cast<std::size_t>(i)].name;
                selected = ed_.brush_.floor == i;
            } else if (folder_ == kWalls) {
                t = ed_.wallThumb(i, 0, 0) ? ed_.wallThumb(i, 0, 0) : ed_.wallThumb(i, 1, 0);
                name = things.walls[static_cast<std::size_t>(i)].name;
                selected = ed_.wallMaterial_ == i;
            } else {
                const auto& set = things.edges[static_cast<std::size_t>(i)];
                const auto dirs = me::edgeDirections(set.rows, set.cols, me::kEdgeSESW);
                t = ed_.edgeThumb(i, dirs.empty() ? 0 : dirs.front(), ed_.brush_.floor, ed_.edgeFloor_ == ed_.brush_.floor ? -1 : ed_.edgeFloor_);
                name = set.name;
                selected = ed_.edgeSet_ == i;
            }
            float w = 0, h = 0;
            if (t) SDL_GetTextureSize(t, &w, &h);
            if (cell(std::to_string(i).c_str(), t, ImVec2(0, 0), ImVec2(1, 1), w, h, name, selected, size, false)) pickPaint(folder_, i);
            help(name);
        });
        return;
    }
    if (folder_ == kFolderPrefabs) {
        if (ImGui::Button(NOX_TRI("editor.browser.prefab.save", "Save Selection as Prefab..."))) ed_.commands_.run("prefab.save");
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("editor.browser.reload", "Reload"))) reloadPrefabs();
        help(NOX_TRC("editor.browser.prefab.reload_help", "Prefabs live in the user folder's prefabs\\ (one .nxprefab file each)."));
        if (prefabs_.empty())
            ImGui::TextDisabled("%s", NOX_TRC("editor.browser.prefab.none", "No prefabs yet: select things on the map, then Save Selection as Prefab."));
        rows(static_cast<int>(prefabs_.size()), [&](int i) {
            const auto& p = prefabs_[static_cast<std::size_t>(i)];
            // Its picture: its first object's thumbnail.
            SDL_Texture* t = nullptr;
            ImVec2 uv0, uv1(1, 1);
            float w = 0, h = 0;
            for (const auto& e : p.clip.objects) {
                if (const auto* ce = catalog_.find(e.object.type); ce && thumbs_) {
                    const auto c = thumbs_->get(ce->thing);
                    t = c.texture;
                    uv0 = ImVec2(c.u0, c.v0);
                    uv1 = ImVec2(c.u1, c.v1);
                    w = h = editor::ThingThumbnails::kSize;
                    break;
                }
            }
            if (cell(("p" + std::to_string(i)).c_str(), t, uv0, uv1, w, h, p.name, prefab_ == i, size, !t)) {
                prefab_ = i;
                if (ed_.tool_ != MapEditor::Tool::Object) {
                    ed_.lastTool_ = ed_.tool_;
                    ed_.tool_ = MapEditor::Tool::Object;
                }
                mode_ = Mode::Place;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                help(p.name + "\n" +
                     nox::l10n::format(NOX_TR("editor.browser.prefab.counts", "{objects} objects, {walls} walls, {tiles} floor tiles, {waypoints} waypoints"),
                                       {{"objects", p.objectCount()},
                                        {"walls", p.clip.walls.size()},
                                        {"tiles", p.clip.tiles.size()},
                                        {"waypoints", p.clip.waypoints.size()}}) +
                     (p.cellAligned() ? "\n" + NOX_TR("editor.browser.prefab.cell_aligned", "(placed by whole cells: it holds walls or floor)") : std::string()));
            }
            if (ImGui::BeginDragDropSource()) {
                const int v = -i - 1;
                ImGui::SetDragDropPayload("NOX_THING", &v, sizeof v);
                ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.browser.prefab.name", "Prefab {name}"), {{"name", p.name}}).c_str());
                ImGui::EndDragDropSource();
            }
        });
        return;
    }
    if (folder_ == kUser) {
        ImGui::TextWrapped("%s", NOX_TRC("editor.browser.user.about",
                                         "Your categories come from categories.xml in the user folder (the old community editor's format: "
                                         "<Category Name=\"..\"> with <Include Rule=\"NAME_BEGIN\">Tree</Include> and Exclude lines; rules OBJECT_CLASS, "
                                         "NAME_BEGIN, NAME_ENDING, NAME_CONTAIN, NAME_EQUAL, ANYTHING)."));
        static char path[260] = {};
        ImGui::SetNextItemWidth(-90 * ed_.imgui_->scale());
        ImGui::InputTextWithHint("##catpath", NOX_TRC("editor.browser.user.path_hint", "path to a categories.xml"), path, sizeof path);
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("editor.browser.user.import", "Import")) && path[0]) runStep(std::string("categories:") + path);
        if (ImGui::Button(NOX_TRI("editor.browser.reload", "Reload"))) loadUserCategories();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.browser.user.count", "{n} categories"), {{"n", userCats_.size()}}).c_str());
        categoriesEditor();
        return;
    }
    const auto list = shown();
    if (list.empty()) {
        ImGui::TextDisabled("%s", search_[0]                         ? NOX_TRC("editor.browser.no_match", "Nothing matches.")
                                  : folder_ == kFolderFavourites ? NOX_TRC("editor.browser.no_favourites", "No favourites: right click a thumbnail to star it.")
                                                                 : NOX_TRC("editor.browser.empty", "Nothing here."));
        return;
    }
    rows(static_cast<int>(list.size()), [&](int i) {
        const auto* e = list[static_cast<std::size_t>(i)];
        editor::ThingThumbnails::Cell c;
        if (thumbs_) c = thumbs_->get(e->thing);
        const bool selected = prefab_ < 0 && type_ == e->name;
        if (cell(std::to_string(e->thing).c_str(), c.texture, ImVec2(c.u0, c.v0), ImVec2(c.u1, c.v1), editor::ThingThumbnails::kSize,
                 editor::ThingThumbnails::kSize, e->name, selected, size, !c.none)) {
            pickType(e->name);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            const std::string shownName = e->displayName.empty() ? std::string() : "\"" + e->displayName + "\"\n";
            ImGui::SetTooltip("%s\n%s%s\n%s%s%s", e->name.c_str(), shownName.c_str(), me::folderText(e->folder).c_str(), e->xfer.c_str(), e->placeable ? "" : "\n",
                              e->placeable ? "" : NOX_TRC("editor.browser.not_placeable", "(not placeable: the game's save drops it)"));
        }
        if (ImGui::BeginDragDropSource()) {
            const int v = static_cast<int>(e->thing);
            ImGui::SetDragDropPayload("NOX_THING", &v, sizeof v);
            thumbnail(e->thing, 48);
            ImGui::SameLine();
            ImGui::TextUnformatted(e->name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("##cellmenu")) {
            const bool fav = favourites_.contains(e->name);
            if (ImGui::MenuItem(fav ? NOX_TRI("editor.browser.unfavourite", "Remove from Favourites") : NOX_TRI("editor.browser.favourite", "Add to Favourites"))) {
                if (fav) favourites_.erase(e->name);
                else favourites_.insert(e->name);
                saveState();
            }
            if (ImGui::MenuItem(NOX_TRI("editor.browser.place_at_centre", "Place at the View's Centre"))) placeAt(placePoint(ed_.camera_.x, ed_.camera_.y), e->name);
            if (ImGui::MenuItem(NOX_TRI("editor.browser.select_type", "Select All of This Type on the Map"))) ed_.setSelection(me::selectType(ed_.doc_, e->name));
            if (ImGui::MenuItem(NOX_TRI("editor.browser.show_folder", "Show Its Folder"))) browse(e->folder);
            if (ImGui::BeginMenu(NOX_TRI("editor.browser.add_to_category", "Add to Category"))) {
                // (A NAME_EQUAL rule for it in one of the user's categories, or a new one.)
                for (std::size_t k = 0; k < userCats_.size(); ++k) {
                    if (ImGui::MenuItem((userCats_[k].name + "##cat" + std::to_string(k)).c_str())) {
                        userCats_[k].rules.push_back({true, "NAME_EQUAL", e->name});
                        saveUserCategories();
                    }
                }
                if (!userCats_.empty()) ImGui::Separator();
                if (ImGui::MenuItem(NOX_TRI("editor.browser.new_category", "New Category"))) {
                    userCats_.push_back({uniqueCategoryName(NOX_TR("editor.browser.new_category", "New Category")), {{true, "NAME_EQUAL", e->name}}});
                    saveUserCategories();
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    });
}

std::string ObjectTools::uniqueCategoryName(const std::string& base) const
{
    std::string name = base;
    for (int n = 2; std::ranges::any_of(userCats_, [&](const auto& c) { return c.name == name; }); ++n) {
        name = base + " " + std::to_string(n);
    }
    return name;
}

void ObjectTools::saveUserCategories()
{
    // The user folder's categories.xml (the old editor's format, read back by loadUserCategories).
    if (writeDir_.empty()) {
        return; // (a test's run: nothing written)
    }
    std::error_code ec;
    fs::create_directories(writeDir_, ec);
    std::ofstream out(writeDir_ / "categories.xml", std::ios::binary | std::ios::trunc);
    out << me::categoriesXml(userCats_);
    ed_.status(out ? nox::l10n::format(NOX_TR("editor.browser.user.saved", "Saved {n} categories (categories.xml)"), {{"n", userCats_.size()}})
                   : NOX_TR("editor.browser.user.save_failed", "categories.xml couldn't be written"));
}

void ObjectTools::categoriesEditor()
{
    // The categories editor: each category's name and rules (Include / Exclude, the rule, its
    // text), new and deleted ones; every change saved to categories.xml at once (a name or a
    // pattern when its field is left).
    static constexpr const char* kRules[] = {"NAME_BEGIN", "NAME_ENDING", "NAME_CONTAIN", "NAME_EQUAL", "OBJECT_CLASS", "ANYTHING"};
    const float s = ed_.imgui_->scale();
    ImGui::SeparatorText(NOX_TRC("editor.browser.user.edit", "Edit categories"));
    bool changed = false;
    std::optional<std::size_t> removeCat;
    for (std::size_t k = 0; k < userCats_.size(); ++k) {
        auto& c = userCats_[k];
        ImGui::PushID(static_cast<int>(k));
        const bool open = ImGui::TreeNodeEx("##cat", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap, "%s  (%zu)", c.name.c_str(),
                                            c.evaluate(catalog_).size());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120 * s + ImGui::GetCursorPosX());
        if (ImGui::SmallButton(NOX_TRI("editor.browser.user.show", "Show"))) browse(std::string(kUser) + "/" + c.name);
        ImGui::SameLine();
        if (ImGui::SmallButton(NOX_TRI("editor.common.delete", "Delete"))) removeCat = k;
        if (open) {
            char name[96];
            std::snprintf(name, sizeof name, "%s", c.name.c_str());
            ImGui::SetNextItemWidth(200 * s);
            if (ImGui::InputText(NOX_TRI("editor.browser.user.name", "Name"), name, sizeof name) && name[0]) {
                c.name = name;
            }
            changed = changed || ImGui::IsItemDeactivatedAfterEdit();
            std::optional<std::size_t> removeRule;
            for (std::size_t r = 0; r < c.rules.size(); ++r) {
                auto& rule = c.rules[r];
                ImGui::PushID(static_cast<int>(r));
                ImGui::SetNextItemWidth(90 * s);
                int inc = rule.include ? 0 : 1;
                const char* incItems[] = {NOX_TRC("editor.browser.user.include", "Include"), NOX_TRC("editor.browser.user.exclude", "Exclude")};
                if (ImGui::Combo("##inc", &inc, incItems, 2)) {
                    rule.include = inc == 0;
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130 * s);
                if (ImGui::BeginCombo("##kind", rule.kind.c_str())) {
                    for (const char* k2 : kRules) {
                        if (ImGui::Selectable(k2, rule.kind == k2)) {
                            rule.kind = k2;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (rule.kind != "ANYTHING") {
                    char pattern[128];
                    std::snprintf(pattern, sizeof pattern, "%s", rule.pattern.c_str());
                    ImGui::SetNextItemWidth(160 * s);
                    if (ImGui::InputTextWithHint("##pattern", rule.kind == "OBJECT_CLASS" ? "MONSTER | NPC" : NOX_TRC("editor.browser.user.pattern_hint", "type name text"),
                                                 pattern, sizeof pattern)) {
                        rule.pattern = pattern;
                    }
                    changed = changed || ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton("x")) removeRule = r;
                ImGui::PopID();
            }
            if (removeRule) {
                c.rules.erase(c.rules.begin() + static_cast<std::ptrdiff_t>(*removeRule));
                changed = true;
            }
            if (ImGui::SmallButton(NOX_TRI("editor.browser.user.add_rule", "Add Rule"))) {
                c.rules.push_back({true, "NAME_CONTAIN", ""});
                changed = true;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removeCat) {
        userCats_.erase(userCats_.begin() + static_cast<std::ptrdiff_t>(*removeCat));
        changed = true;
    }
    if (ImGui::Button(NOX_TRI("editor.browser.new_category", "New Category"))) {
        userCats_.push_back({uniqueCategoryName(NOX_TR("editor.browser.new_category", "New Category")), {{true, "NAME_CONTAIN", ""}}});
        changed = true;
    }
    help(NOX_TRC("editor.browser.user.new_help", "A new category in the user folder's categories.xml (right click a thumbnail: Add to Category)."));
    if (changed) {
        saveUserCategories();
    }
}

void ObjectTools::detailsCard(bool tall)
{
    // The paint folders: what the floor, wall or edge tool paints with now.
    if (folder_ == kFloors || folder_ == kWalls || folder_ == kEdges) {
        const auto& t = ed_.things_;
        if (folder_ == kFloors && ed_.brush_.floor < t.floors.size()) {
            const auto& f = t.floors[ed_.brush_.floor];
            ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.browser.floor_brush", "Floor brush: {name}"), {{"name", f.name}}).c_str());
            ImGui::TextDisabled("%s", nox::l10n::format(f.frames > 1 ? NOX_TR("editor.browser.floor_variants_animated", "{cols} x {rows} variants, animated")
                                                                     : NOX_TR("editor.browser.floor_variants", "{cols} x {rows} variants"),
                                                        {{"cols", f.cols}, {"rows", f.rows}})
                                          .c_str());
        } else if (folder_ == kWalls && ed_.wallMaterial_ >= 0 && static_cast<std::size_t>(ed_.wallMaterial_) < t.walls.size()) {
            ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.browser.wall_material", "Wall material: {name}"),
                                                     {{"name", t.walls[static_cast<std::size_t>(ed_.wallMaterial_)].name}})
                                       .c_str());
            ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.browser.wall_variations", "{n} random variations"),
                                                        {{"n", me::wallRandomVariations(t, ed_.wallMaterial_)}})
                                          .c_str());
        } else if (folder_ == kEdges && ed_.edgeSet_ >= 0 && static_cast<std::size_t>(ed_.edgeSet_) < t.edges.size()) {
            const auto& e = t.edges[static_cast<std::size_t>(ed_.edgeSet_)];
            ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.browser.edge_set", "Edge set: {name}"), {{"name", e.name}}).c_str());
            ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.browser.edge_shapes", "{cols} x {rows} ({n} shapes)"),
                                                        {{"cols", e.cols}, {"rows", e.rows}, {"n", 2 * (e.cols + e.rows)}})
                                          .c_str());
        }
        ImGui::TextWrapped("%s", NOX_TRC("editor.browser.paint_click", "A click picks it for its tool and switches to that tool."));
        return;
    }
    const me::ContentEntry* e = prefab_ < 0 ? catalog_.find(type_) : nullptr;
    if (!e) {
        if (prefab_ >= 0 && prefab_ < static_cast<int>(prefabs_.size())) {
            const auto& p = prefabs_[static_cast<std::size_t>(prefab_)];
            ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.browser.prefab.name", "Prefab {name}"), {{"name", p.name}}).c_str());
            ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.browser.prefab.card_counts", "{objects} objects, {walls} walls, {tiles} tiles, {waypoints} waypoints"),
                                                        {{"objects", p.objectCount()},
                                                         {"walls", p.clip.walls.size()},
                                                         {"tiles", p.clip.tiles.size()},
                                                         {"waypoints", p.clip.waypoints.size()}})
                                          .c_str());
        } else {
            ImGui::TextDisabled("%s", NOX_TRC("editor.browser.card_empty", "Pick a type to see it here; click on the map to place it (Object tool)."));
        }
        return;
    }
    const auto& def = ed_.things_.things[e->thing];
    const float big = (tall ? 96 : 80) * ed_.imgui_->scale();
    thumbnail(e->thing, big);
    if (!tall) ImGui::SameLine();
    ImGui::BeginGroup();
    const bool fav = favourites_.contains(e->name);
    if (ImGui::SmallButton(fav ? NOX_TRI("editor.browser.unstar", "Unstar##fav") : NOX_TRI("editor.browser.star", "Star##fav"))) {
        if (fav) favourites_.erase(e->name);
        else favourites_.insert(e->name);
        saveState();
    }
    help(fav ? NOX_TRC("editor.browser.unfavourite", "Remove from Favourites") : NOX_TRC("editor.browser.favourite", "Add to Favourites"));
    ImGui::SameLine();
    ImGui::TextUnformatted(e->name.c_str());
    if (!e->displayName.empty()) {
        ImGui::Text("\"%s\"", e->displayName.c_str());
        help(NOX_TRC("editor.browser.game_name_help", "The name the game shows for it."));
    }
    ImGui::TextDisabled("%s", me::folderText(e->folder).c_str());
    ImGui::Text("%s%s", e->xfer.c_str(), e->placeable ? "" : NOX_TRC("editor.browser.card_not_placeable", "  (not placeable: the game's object save drops it)"));
    ImGui::TextWrapped("CLASS %s", std::string(def.attribute("CLASS").value_or("-")).c_str());
    if (!e->subclasses.empty()) {
        std::string s;
        for (const auto& n : e->subclasses) s += (s.empty() ? "" : " ") + n;
        ImGui::TextWrapped("SUBCLASS %s", s.c_str());
    }
    std::string stats;
    for (const char* k : {"EXTENT", "HEALTH", "WORTH", "WEIGHT", "SPEED", "MASS"}) {
        if (const auto v = def.lastAttribute(k)) stats += std::string(stats.empty() ? "" : ", ") + k + " " + std::string(*v);
    }
    if (!stats.empty()) ImGui::TextWrapped("%s", stats.c_str());
    if (monsters_) {
        if (const auto* m = monsters_->find(e->name)) {
            ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.browser.monster_stats", "monster.bin (solo): health {health}, speed {speed}"),
                                                     {{"health", m->health}, {"speed", m->speed}})
                                       .c_str());
        }
    }
    ImGui::EndGroup();
}

void ObjectTools::browserPanel()
{
    if (!ImGui::Begin(NOX_TRI("editor.window.content_browser", "Content Browser"), &showBrowser)) {
        ImGui::End();
        return;
    }
    const float scale = ed_.imgui_->scale();
    // The search and filters.
    ImGui::SetNextItemWidth(220 * scale);
    ImGui::InputTextWithHint("##search", NOX_TRC("editor.browser.search_hint", "search (name, game name, folder, xfer, class)"), search_, sizeof search_);
    help(NOX_TRC("editor.browser.search_help",
                 "Every word must match: the type name, the name the game shows (\"Potion of Restore Health\"), the folder, the XFER or a CLASS / SUBCLASS "
                 "name. A search looks through every folder."));
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.browser.show_all", "Show all"), &showAll_);
    help(NOX_TRC("editor.browser.show_all_help", "Also the types the game's object save drops (no menu icon: effects, missiles, generators' markers)."));
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.browser.subfolders", "Sub-folders"), &recursive_);
    help(NOX_TRC("editor.browser.subfolders_help", "A folder shows what its sub-folders hold too."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * scale);
    if (ImGui::SliderFloat("##cell", &cell_, 40, 160, "%.0f px")) saveState();
    help(NOX_TRC("editor.browser.cell_help", "Thumbnail size."));
    for (std::size_t i = 0; i < std::size(kChips); ++i) {
        const std::uint32_t classes = kChips[i];
        ImGui::SameLine();
        const bool on = (classMask_ & classes) == classes && classMask_ != 0;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        if (ImGui::SmallButton(chipLabel(i))) classMask_ = on ? (classMask_ & ~classes) : (classMask_ | classes);
        if (on) ImGui::PopStyleColor();
        help(NOX_TRC("editor.browser.chip_help", "Only types of these classes (click again to drop the filter)."));
    }
    if (thumbs_) {
        if (const auto n = thumbs_->pending()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.browser.thumbs_pending", "thumbnails: {n} to draw"), {{"n", n}}).c_str());
        }
    }
    // The tree | the grid | the details card.
    const float avail = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##tree", ImVec2(220 * scale, avail), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    folderTree();
    ImGui::EndChild();
    ImGui::SameLine();
    // Wide: the card beside the grid; narrow: under it.
    const float cardW = 250 * scale;
    const float rest = ImGui::GetContentRegionAvail().x;
    const bool beside = rest - cardW > 4 * (cell_ * scale + ImGui::GetStyle().ItemSpacing.x);
    const float gridW = beside ? rest - cardW - ImGui::GetStyle().ItemSpacing.x : rest;
    const float cardH = 104 * scale;
    ImGui::BeginGroup();
    ImGui::BeginChild("##grid", ImVec2(gridW, beside ? avail : std::max(80.0f, avail - cardH - ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_Borders);
    if (search_[0] && folder_ != kFloors && folder_ != kWalls && folder_ != kEdges) {
        ImGui::TextDisabled("%s", NOX_TRC("editor.browser.search_results", "Search results"));
    } else {
        folderHeading(folder_);
    }
    itemGrid();
    ImGui::EndChild();
    if (beside) ImGui::SameLine();
    ImGui::BeginChild("##card", ImVec2(0, beside ? avail : 0), ImGuiChildFlags_Borders);
    detailsCard(beside);
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::End();
}

// --- The Object tool's options -----------------------------------------------------------------------

void ObjectTools::scatterPanel()
{
    ImGui::SliderFloat(NOX_TRI("editor.object.scatter.radius", "Radius"), &scatter_.radius, 23, 460, "%.0f");
    ImGui::SliderFloat(NOX_TRI("editor.object.scatter.density", "Density"), &scatter_.density, 0.05f, 3,
                       NOX_TRC("editor.object.scatter.density_format", "%.2f a tile"));
    help(NOX_TRC("editor.object.scatter.density_help", "Objects a dab tries to place per floor tile (46 x 46 diamond) inside the radius."));
    ImGui::SliderFloat(NOX_TRI("editor.object.scatter.spacing", "Spacing"), &scatter_.spacing, 4, 200, "%.0f");
    help(NOX_TRC("editor.object.scatter.spacing_help", "The least distance between scattered objects, and from objects already on the map (Poisson disc)."));
    ImGui::Checkbox(NOX_TRI("editor.object.scatter.random_facing", "Random facing"), &scatter_.randomFacing);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.object.scatter.avoid_walls", "Avoid walls"), &scatter_.avoidWalls);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.object.scatter.only_floor", "Only on floor"), &scatter_.avoidHoles);
    ImGui::TextDisabled("%s", NOX_TRC("editor.object.scatter.list", "The list (a weighted pick each time):"));
    int remove = -1;
    for (std::size_t i = 0; i < scatter_.types.size(); ++i) {
        auto& t = scatter_.types[i];
        ImGui::PushID(static_cast<int>(i));
        if (const auto* e = catalog_.find(t.type)) {
            thumbnail(e->thing, ImGui::GetFrameHeight());
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(70 * ed_.imgui_->scale());
        ImGui::DragFloat("##w", &t.weight, 0.05f, 0, 100, "%.2f");
        ImGui::SameLine();
        ImGui::TextUnformatted(t.type.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) scatter_.types.erase(scatter_.types.begin() + remove);
    ImGui::BeginDisabled(type_.empty());
    if (ImGui::Button(NOX_TRI("editor.object.scatter.add_type", "Add the Browser's Type"))) {
        if (std::ranges::none_of(scatter_.types, [&](const auto& t) { return t.type == type_; })) scatter_.types.push_back({type_, 1});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.object.scatter.add_folder", "Add the Folder"))) {
        for (const auto* e : catalog_.inFolder(folder_, recursive_)) {
            if (e->placeable && std::ranges::none_of(scatter_.types, [&](const auto& t) { return t.type == e->name; })) scatter_.types.push_back({e->name, 1});
        }
    }
    help(NOX_TRC("editor.object.scatter.add_folder_help", "Every placeable type of the browser's folder, weight 1 each (trees, rocks, flowers...)."));
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.common.clear", "Clear"))) scatter_.types.clear();
    if (scatter_.types.empty()) ImGui::TextDisabled("%s", NOX_TRC("editor.object.scatter.empty", "(empty: the browser's type)"));
    ImGui::TextWrapped("%s", NOX_TRC("editor.object.scatter.about",
                                     "Drag on the map: one stroke is one change (undo takes it all back). The same stroke places the same objects every time "
                                     "(seeded by the stroke count, never the clock)."));
}

void ObjectTools::toolPanel()
{
    ImGui::SeparatorText(NOX_TRC("editor.object.heading", "Object"));
    int mode = static_cast<int>(mode_);
    ImGui::RadioButton(NOX_TRI("editor.object.place", "Place"), &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(NOX_TRI("editor.object.scatter_brush", "Scatter brush"), &mode, 1);
    mode_ = static_cast<Mode>(mode);
    if (mode_ == Mode::Place) {
        if (prefab_ >= 0 && prefab_ < static_cast<int>(prefabs_.size())) {
            ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.object.placing_prefab", "Placing prefab {name}"),
                                                     {{"name", prefabs_[static_cast<std::size_t>(prefab_)].name}})
                                       .c_str());
            if (ImGui::SmallButton(NOX_TRI("editor.object.back_to_type", "Back to the browser's type"))) prefab_ = -1;
        } else if (const auto* e = catalog_.find(type_)) {
            thumbnail(e->thing, 40 * ed_.imgui_->scale());
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(e->name.c_str());
            ImGui::TextDisabled("%s", me::folderText(e->folder).c_str());
            ImGui::EndGroup();
        } else {
            ImGui::TextDisabled("%s", NOX_TRC("editor.object.pick_type", "Pick a type in the Content Browser (or drag one onto the map)."));
        }
        ImGui::SliderFloat(NOX_TRI("editor.object.facing", "Facing"), &facing_, 0, 359, NOX_TRC("editor.object.facing_format", "%.0f deg"));
        help(NOX_TRC("editor.object.facing_help",
                     "New doors, creatures, NPCs and sentries face this way (0 east, 90 south). Q and E turn it a step: a door a quarter, a creature "
                     "an eighth, a sentry 5 degrees. With the Select tool, Q and E turn the selection."));
        ImGui::Checkbox(NOX_TRI("editor.object.random_from_folder", "Random from the folder"), &randomFromFolder_);
        help(NOX_TRC("editor.object.random_from_folder_help", "Each click places a random type from the browser's folder (the old editor's Randomize)."));
        ImGui::Checkbox(NOX_TRI("editor.object.right_deletes", "Right click deletes"), &rightDeletes_);
        help(NOX_TRC("editor.object.right_deletes_help", "A right click deletes the object under the mouse (the old editor's place-mode binding)."));
        ImGui::TextWrapped("%s", NOX_TRC("editor.object.place_about",
                                         "Click to place (again and again: multi-place); Alt+click takes the type under the mouse. Objects snap as the "
                                         "toolbar's Snap says; doors always snap to the grid, as the old editor placed them. Each placement is one change."));
    } else {
        scatterPanel();
    }
    ImGui::SeparatorText(NOX_TRC("editor.object.new_objects", "New objects"));
    ImGui::TextWrapped("%s", NOX_TRC("editor.object.new_objects_about",
                                     "Each is the record the game's own save writes for a thing just made: the type's fresh state (MonsterCreate's numbers "
                                     "with monster.bin's solo table, a weapon's durability, a plate's colours...), its next free extent, no script id "
                                     "(the load gives one)."));
}

} // namespace noxsdl
