// The content browser (panels.hpp).
#include "gui/panels.hpp"

#include "gui/textures.hpp"

#include "editor/theme_colours.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

namespace videotools::gui {

using content::FileKind;
using content::Record;
using content::RecordList;
using content::Section;

void ContentBrowser::setSearch(std::string_view text)
{
    std::snprintf(search_, sizeof search_, "%.*s", int(text.size()), text.data());
}

void ContentBrowser::open(const Record& r)
{
    if (r.ref.list == RecordList::View) {
        ws_.focus(r.ref.index == 0 ? panel::kImages : panel::kSprites);
        return;
    }
    ws_.select(r.ref);
}

void ContentBrowser::row(const Record& r, bool showSection)
{
    ImGui::PushID(&r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // A sound (an Audio.bag sample, a sound event, a dialog line): a play button where an icon goes.
    const bool sound = r.ref.list == RecordList::Sample || r.ref.list == RecordList::SoundEvent || r.ref.list == RecordList::SoundEventAvnt ||
                       (r.ref.list == RecordList::File && r.ref.kind == FileKind::Dialog);
    if (sound) {
        if (ImGui::InvisibleButton("##play", ImVec2(rowH_, rowH_))) {
            if (r.ref.list == RecordList::Sample) {
                playSample(ws_, r.name);
            } else if (r.ref.list == RecordList::File) {
                if (const auto* p = recordFile(ws_.files, r.ref)) {
                    playSoundFile(ws_, *p);
                }
            } else {
                playSoundEvent(ws_, r.name);
            }
        }
        const ImVec2 b0 = ImGui::GetItemRectMin();
        const float h = rowH_ * 0.22f;
        const ImVec2 c(b0.x + rowH_ * 0.5f, b0.y + rowH_ * 0.5f);
        dl->AddTriangleFilled(ImVec2(c.x - h * 0.8f, c.y - h), ImVec2(c.x - h * 0.8f, c.y + h), ImVec2(c.x + h, c.y),
                              ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled));
        ImGui::SetItemTooltip("Play");
        ImGui::SameLine(0, 4);
    }
    const bool selected = ws_.selected && *ws_.selected == r.ref;
    const ImVec2 size(0, rowH_);
    if (ImGui::Selectable("##r", selected, ImGuiSelectableFlags_AllowDoubleClick, size)) {
        open(r);
    }
    if (selected && ws_.scrollContentToSelection) {
        ImGui::SetScrollHereY(0.3f);
        ws_.scrollContentToSelection = false;
    }
    const ImVec2 a = ImGui::GetItemRectMin();
    float x = a.x + 2;
    if (r.icon >= 0) {
        drawFit(dl, ws_.session.sd().get(r.icon), ImVec2(x, a.y + 1), ImVec2(rowH_ - 2, rowH_ - 2));
    }
    if (!sound) {
        x += rowH_ + 4;
    }
    const float textY = a.y + (rowH_ - ImGui::GetTextLineHeight()) / 2;
    dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(ImGuiCol_Text), r.name.c_str());
    std::string sub = r.detail;
    if (showSection) {
        const content::Section* s = ws_.index.sectionOf(r.ref);
        sub = (s ? s->name : std::string(content::fileKindLabel(r.ref.kind))) + (sub.empty() ? "" : "  ·  " + sub);
    }
    if (!sub.empty()) {
        const float nameW = ImGui::CalcTextSize(r.name.c_str()).x;
        const float right = ImGui::GetItemRectMax().x - 4;
        const float subW = ImGui::CalcTextSize(sub.c_str()).x;
        const float sx = std::max(x + nameW + 12, right - subW);
        dl->PushClipRect(ImVec2(x + nameW + 8, a.y), ImGui::GetItemRectMax(), true);
        dl->AddText(ImVec2(sx, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        dl->PopClipRect();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s\n%s", r.name.c_str(), sub.c_str());
    }
    ImGui::PopID();
}

void ContentBrowser::sectionList(const Section& s, const std::vector<std::uint32_t>* only)
{
    const int n = only ? int(only->size()) : int(s.records.size());
    ImGuiListClipper clipper;
    clipper.Begin(n, rowH_ + ImGui::GetStyle().ItemSpacing.y);
    // (The selection scrolled into view: its row is drawn even when the clipper would skip it.)
    if (ws_.scrollContentToSelection && ws_.selected) {
        for (int i = 0; i < n; ++i) {
            if (s.records[only ? (*only)[std::size_t(i)] : std::size_t(i)].ref == *ws_.selected) {
                clipper.IncludeItemByIndex(i);
                break;
            }
        }
    }
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            row(s.records[only ? (*only)[std::size_t(i)] : std::size_t(i)], false);
        }
    }
}

void ContentBrowser::draw()
{
    rowH_ = std::round(ImGui::GetTextLineHeight() * 1.45f);
    const float boxW = ImGui::CalcTextSize("Changed only").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::SetNextItemWidth(-boxW);
    ImGui::InputTextWithHint("##search", "Search every file's records", search_, sizeof search_);
    ImGui::SameLine();
    ImGui::Checkbox("Changed only", &changedOnly_);
    ImGui::SetItemTooltip("Only the records the content pack changes (with the search, those that match it)");
    const std::string text = search_;
    if (text != searchUsed_) {
        searchUsed_ = text;
        results_ = text.empty() ? std::vector<content::RecordRef>{} : ws_.index.search(text);
    }
    std::vector<content::RecordRef> changed;
    if (changedOnly_ && ws_.changedRecords) {
        changed = ws_.changedRecords();
        if (!text.empty()) {
            std::erase_if(changed, [&](const content::RecordRef& r) { return std::ranges::find(results_, r) == results_.end(); });
        }
    }
    const std::vector<content::RecordRef>& shown = changedOnly_ ? changed : results_;
    ImGui::BeginChild("tree", ImVec2(0, 0), ImGuiChildFlags_None);
    if (!text.empty() || changedOnly_) {
        ImGui::TextDisabled("%zu %s%s", shown.size(), changedOnly_ ? "changed" : "found", shown.size() >= 5000 ? " (the first 5000)" : "");
        ImGuiListClipper clipper;
        clipper.Begin(int(shown.size()), rowH_ + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                if (const Record* r = ws_.index.find(shown[std::size_t(i)])) {
                    row(*r, true);
                }
            }
        }
        ImGui::EndChild();
        ws_.scrollContentToSelection = false;
        return;
    }
    const auto& theme = noxsdl::editor::themeColours();
    for (const Section& s : ws_.index.sections()) {
        ImGui::PushID(&s);
        const bool holdsSelection = ws_.selected && ws_.selected->kind == s.kind && ws_.scrollContentToSelection &&
                                    std::ranges::any_of(s.records, [&](const Record& r) { return r.ref == *ws_.selected; });
        if (holdsSelection) {
            ImGui::SetNextItemOpen(true);
        }
        const std::string label = std::format("{}###s", s.name);
        const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("000000").x);
        if (!s.loaded) {
            ImGui::TextColored(theme.error, "missing");
            if (const auto* st = ws_.files.statusOf(s.kind); st && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s: %s", st->path.string().c_str(), st->error.c_str());
            }
        } else {
            ImGui::TextDisabled("%6zu", s.records.size());
        }
        if (open) {
            if (s.groups.empty()) {
                sectionList(s, nullptr);
            } else {
                for (const auto& g : s.groups) {
                    ImGui::PushID(&g);
                    const bool groupHolds = holdsSelection && std::ranges::any_of(g.records, [&](std::uint32_t i) { return s.records[i].ref == *ws_.selected; });
                    if (groupHolds) {
                        ImGui::SetNextItemOpen(true);
                    }
                    const bool gOpen = ImGui::TreeNodeEx(std::format("{}###g", g.name).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("000000").x);
                    ImGui::TextDisabled("%6zu", g.records.size());
                    if (gOpen) {
                        sectionList(s, &g.records);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ws_.scrollContentToSelection = false; // (done, or nowhere to scroll to)
}

} // namespace videotools::gui
