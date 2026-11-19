#include "gui/compare_view.hpp"

#include "noxcore/strings/l10n.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace videotools::gui {

void CompareView::controls(bool fillOption)
{
    int& m = s_.settings().compare;
    ImGui::RadioButton(NOX_TRI("content.compare.side_by_side", "Side by side"), &m, SideBySide);
    ImGui::SameLine();
    ImGui::RadioButton("video.bag", &m, BagOnly);
    ImGui::SameLine();
    ImGui::RadioButton("HD", &m, HdOnly);
    ImGui::SetItemTooltip("%s", NOX_TRC("content.compare.tab_tip", "Tab switches between video.bag and HD"));
    ImGui::SameLine(0, 20 * s_.scale());
    ImGui::Checkbox(NOX_TRI("content.compare.fit", "Fit"), &fit_);
    ImGui::SameLine();
    ImGui::BeginDisabled(fit_);
    ImGui::SetNextItemWidth(110 * s_.scale());
    static const std::string zoomFormat = sliderFormat(NOX_TR("content.compare.zoom", "zoom {n}x"));
    ImGui::SliderInt("##zoom", &s_.settings().zoom, 1, 16, zoomFormat.c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100 * s_.scale());
    ImGui::Combo("##bg", &s_.settings().background, [](void*, int i) { return backgroundName(i); }, nullptr, kBackgroundCount);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("content.compare.tint", "Tint"), &tint_);
    ImGui::SetItemTooltip("%s", NOX_TRC("content.compare.tint_tip", "Shade material-slot pixels (grey in the data; the game tints them) with sample colours"));
    if (fillOption) {
        ImGui::SameLine();
        ImGui::BeginDisabled(m == BagOnly);
        ImGui::Checkbox(NOX_TRI("content.compare.fill", "Fill from video.bag"), &fillFromBag_);
        ImGui::SetItemTooltip("%s", NOX_TRC("content.compare.fill_tip", "In the HD pane, frames the pack has no HD image for show the video.bag image, as NoX Core draws them"));
        ImGui::EndDisabled();
    }
}

void CompareView::keys()
{
    int& m = s_.settings().compare;
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Tab, false) && m != SideBySide) {
        m = m == BagOnly ? HdOnly : BagOnly;
    }
}

void CompareView::draw(const PaneView& bag, const PaneView& hd, ImVec2 size)
{
    if (mode() == SideBySide) {
        const float half = std::floor((size.x - ImGui::GetStyle().ItemSpacing.x) / 2);
        pane(bag, ImVec2(half, size.y));
        ImGui::SameLine();
        pane(hd, ImVec2(half, size.y));
    } else {
        pane(mode() == BagOnly ? bag : hd, size);
    }
}

void CompareView::pane(const PaneView& v, ImVec2 size)
{
    ImGui::BeginChild(v.title, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    drawBackground(dl, p0, p1, s_.settings().background, s_.scale());
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) {
        fit_ = false;
        s_.settings().zoom = std::clamp(s_.settings().zoom + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), 1, 16);
    }
    const Tex* t = v.image;
    const bool shows = t && t->texture;
    const int cw = v.canvasWidth > 0 ? v.canvasWidth : shows ? t->width : 0;
    const int ch = v.canvasHeight > 0 ? v.canvasHeight : shows ? t->height : 0;
    if (cw > 0 && ch > 0) {
        // Both at the logical size, so an HD image covers its original exactly.
        float z = float(s_.settings().zoom);
        if (fit_) {
            z = std::min((sz.x - 16) / float(cw), (sz.y - 16) / float(ch));
            z = z >= 1 ? std::floor(std::min(z, 16.0f)) : z;
        }
        const ImVec2 origin(std::floor(p0.x + (sz.x - float(cw) * z) / 2), std::floor(p0.y + (sz.y - float(ch) * z) / 2));
        dl->PushClipRect(p0, p1, true);
        if (v.under) {
            v.under(dl, origin, z);
        }
        if (shows) {
            const bool nearest = !v.hd || v.standIn; // video.bag pixels stay sharp
            const ImVec2 a(origin.x + v.x * z, origin.y + v.y * z);
            if (nearest) {
                dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
            }
            dl->AddImage(texRef(t->show(tint_)), a, ImVec2(a.x + float(t->width) * z, a.y + float(t->height) * z));
            if (nearest) {
                dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
            }
        }
        if (v.over) {
            v.over(dl, origin, z);
        }
        dl->PopClipRect();
    }
    const char* note = v.note;
    if (v.note) {
        // (the caller's)
    } else if (!t) {
        note = v.hd && !s_.packOpen() ? NOX_TRC("content.compare.no_pack", "No image pack open") : NOX_TRC("content.compare.loading", "Loading...");
    } else if (v.standIn && shows) {
        note = NOX_TRC("content.compare.stand_in", "No HD image in the pack for this frame: video.bag shown");
    } else if (t->missing) {
        note = v.hd ? NOX_TRC("content.compare.no_hd", "No HD image in the pack for this slot") : NOX_TRC("content.compare.no_bag_image", "No video.bag image here");
    } else if (t->empty) {
        note = NOX_TRC("content.compare.empty", "Empty image (0 x 0): draws nothing");
    } else if (!t->error.empty()) {
        note = t->error.c_str();
    }
    dl->AddRectFilled(ImVec2(p0.x + 4, p0.y + 4), ImVec2(p0.x + 8 + ImGui::CalcTextSize(v.title).x, p0.y + 6 + ImGui::GetTextLineHeight()),
                      IM_COL32(0, 0, 0, 150));
    dl->AddText(ImVec2(p0.x + 6, p0.y + 5), IM_COL32(235, 235, 240, 255), v.title);
    if (note) {
        const ImVec2 n(p0.x + 6, p0.y + 8 + ImGui::GetTextLineHeight());
        dl->AddRectFilled(ImVec2(n.x - 2, n.y - 1), ImVec2(n.x + 2 + ImGui::CalcTextSize(note).x, n.y + 1 + ImGui::GetTextLineHeight()),
                          IM_COL32(0, 0, 0, 150));
        dl->AddText(n, IM_COL32(220, 200, 120, 255), note);
    }
    ImGui::EndChild();
}

} // namespace videotools::gui
