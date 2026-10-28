// Changes, Problems and Log (panels.hpp).
#include "gui/panels.hpp"

#include "editor/theme_colours.hpp"

#include <imgui.h>

namespace videotools::gui {

void drawChanges(Workspace&, const std::vector<ChangeRow>& rows)
{
    if (rows.empty()) {
        ImGui::TextDisabled("No changes: everything is as the game's files have it.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("changes", 4, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch, 0.25f);
    ImGui::TableSetupColumn("Reach", ImGuiTableColumnFlags_WidthStretch, 0.15f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Revert").x + ImGui::GetStyle().FramePadding.x * 2);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(int(rows.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& r = rows[std::size_t(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(r.what.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) && r.show) {
                r.show();
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.detail.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", r.reach.c_str());
            ImGui::TableNextColumn();
            if (r.revert && ImGui::SmallButton("Revert")) {
                r.revert();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void drawProblems(Workspace&, const std::vector<ProblemRow>& rows)
{
    const auto& theme = noxsdl::editor::themeColours();
    if (rows.empty()) {
        ImGui::TextColored(theme.ok, "No problems.");
        return;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        ImGui::PushID(int(i));
        ImGui::TextColored(r.error ? theme.error : theme.warning, "%s", r.error ? "error" : "warning");
        ImGui::SameLine();
        if (r.show) {
            if (ImGui::Selectable(r.text.c_str())) {
                r.show();
            }
        } else {
            ImGui::TextWrapped("%s", r.text.c_str());
        }
        ImGui::PopID();
    }
}

void drawLog(Workspace& ws)
{
    const auto& lines = ws.session.messages();
    if (lines.empty()) {
        ImGui::TextDisabled("Nothing yet.");
        return;
    }
    ImGuiListClipper clipper;
    clipper.Begin(int(lines.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            ImGui::TextUnformatted(lines[std::size_t(i)].c_str());
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) {
        ImGui::SetScrollHereY(1.0f); // (follows the newest line unless scrolled back)
    }
}

} // namespace videotools::gui
