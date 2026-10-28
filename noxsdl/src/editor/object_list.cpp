// The map editor's Object List panel (object_tools.hpp): every object on the map, live with the
// document, searchable, filterable by folder text or what's in view, sortable by any column,
// multi-select (Ctrl toggles, Shift a range), go to on double click, delete. The old editor's
// ObjectListDialog (extent, place, name, script name; go to, edit), dockable and improved.
#include "editor/object_tools.hpp"

#include "editor/map_editor.hpp"
#include "editor/ui_helpers.hpp"

#include "noxcore/util/text.hpp"

#include <imgui.h>

#include <algorithm>
#include <map>

namespace me = nox::mapedit;

namespace noxsdl {

void ObjectTools::objectListPanel()
{
    if (!ImGui::Begin("Object List", &showList)) {
        ImGui::End();
        return;
    }
    const float scale = ed_.imgui_->scale();
    // The rows, made again whenever the document changes.
    if (rowsRevision_ != ed_.history_.revision() || rows_.empty() != ed_.doc_.objects.empty()) {
        rowsRevision_ = ed_.history_.revision();
        rows_.clear();
        std::map<me::ObjId, std::size_t> items;
        for (const auto& [id, o] : ed_.doc_.objects) {
            if (o.parent != 0) ++items[o.parent];
        }
        for (const auto& [id, o] : ed_.doc_.objects) {
            if (o.parent != 0) continue;
            ListRow r;
            r.id = id;
            r.extent = o.header.extent;
            r.type = o.type;
            r.scriptName = o.header.details ? o.header.details->scriptName : std::string();
            r.team = o.header.details ? o.header.details->team : 0;
            const auto* e = catalog_.find(o.type);
            r.folder = e ? e->folder : std::string();
            r.displayName = e ? e->displayName : std::string();
            r.x = o.header.x;
            r.y = o.header.y;
            r.items = items[id];
            r.search = nox::util::lowered(r.type + ' ' + r.displayName + ' ' + r.scriptName + ' ' + r.folder + " #" + std::to_string(r.extent));
            rows_.push_back(std::move(r));
        }
        rowsRevision_ = ed_.history_.revision();
    }
    ImGui::SetNextItemWidth(200 * scale);
    ImGui::InputTextWithHint("##listsearch", "search (type, game name, script name, folder, #extent)", listSearch_, sizeof listSearch_);
    ImGui::SameLine();
    ImGui::Checkbox("In view", &listInView_);
    editor::help("Only the objects the map panel shows.");
    // The rows shown.
    const std::string q = nox::util::lowered(listSearch_);
    std::vector<const ListRow*> shown;
    const auto& cam = ed_.camera_;
    const float x0 = cam.left(), y0 = cam.top(), x1 = cam.left() + ed_.panelW_ / cam.zoom, y1 = cam.top() + ed_.panelH_ / cam.zoom;
    for (const auto& r : rows_) {
        if (listInView_ && (r.x < x0 || r.x > x1 || r.y < y0 || r.y > y1)) continue;
        bool ok = true;
        for (std::size_t at = 0; at < q.size() && ok;) {
            const auto end = q.find(' ', at);
            const std::string word = q.substr(at, end == std::string::npos ? std::string::npos : end - at);
            ok = word.empty() || r.search.find(word) != std::string::npos;
            at = end == std::string::npos ? q.size() : end + 1;
        }
        if (ok) shown.push_back(&r);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu objects", shown.size(), rows_.size());
    const auto& sel = ed_.selection_.objects;
    ImGui::BeginDisabled(sel.empty());
    if (ImGui::SmallButton("Go to")) ed_.frameSelection();
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) ed_.deleteSelection();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Select listed")) {
        me::Selection s;
        for (const auto* r : shown) s.objects.insert(r->id);
        ed_.setSelection(std::move(s));
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;
    if (ImGui::BeginTable("##objects", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Extent", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 44 * scale);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Script name", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 40 * scale);
        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 40 * scale);
        ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 38 * scale);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
            sortColumn_ = specs->Specs[0].ColumnIndex;
            sortAscending_ = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
        }
        const int col = sortColumn_;
        std::ranges::stable_sort(shown, [&](const ListRow* a, const ListRow* b) {
            bool less = false, greater = false;
            switch (col) {
            case 1: less = a->type < b->type; greater = b->type < a->type; break;
            case 2: less = a->scriptName < b->scriptName; greater = b->scriptName < a->scriptName; break;
            case 3: less = a->x < b->x; greater = b->x < a->x; break;
            case 4: less = a->y < b->y; greater = b->y < a->y; break;
            case 5: less = a->team < b->team; greater = b->team < a->team; break;
            case 6: less = a->folder < b->folder; greater = b->folder < a->folder; break;
            default: less = a->extent < b->extent; greater = b->extent < a->extent; break;
            }
            return sortAscending_ ? less : greater;
        });
        static int anchor = -1; // (Shift-click: the range from the last click)
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(shown.size()));
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const ListRow& r = *shown[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = sel.contains(r.id);
                const std::string label = std::to_string(r.extent) + "##o" + std::to_string(r.id);
                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    const ImGuiIO& io = ImGui::GetIO();
                    me::Selection s = ed_.selection_;
                    if (io.KeyShift && anchor >= 0 && anchor < static_cast<int>(shown.size())) {
                        for (int k = std::min(anchor, i); k <= std::max(anchor, i); ++k) s.objects.insert(shown[static_cast<std::size_t>(k)]->id);
                    } else if (io.KeyCtrl) {
                        if (selected) s.objects.erase(r.id);
                        else s.objects.insert(r.id);
                        anchor = i;
                    } else {
                        s = {};
                        s.objects.insert(r.id);
                        anchor = i;
                    }
                    ed_.setSelection(std::move(s));
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ed_.frameSelection(); // (go to)
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    const std::string carried = r.items ? "\n" + std::to_string(r.items) + " items carried" : std::string();
                    const std::string shownName = r.displayName.empty() ? std::string() : "\"" + r.displayName + "\"\n";
                    ImGui::SetTooltip("%s #%u\n%s%s%s\nDouble click: go to it", r.type.c_str(), r.extent, shownName.c_str(), r.folder.c_str(),
                                      carried.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.type.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.scriptName.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", r.x);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", r.y);
                ImGui::TableNextColumn();
                if (r.team) ImGui::Text("%d", r.team);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.folder.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace noxsdl
