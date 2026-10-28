// A list row showing a name exactly as it is: map names can hold '#' (and "##" in an ImGui label
// would hide the rest and become the row's id), so the text is drawn apart from the row's id.
#pragma once

#include <imgui.h>

#include <string_view>

namespace noxsdl::editor {

/// A selectable row with `text` drawn verbatim, its id `id` (unique in the list).
inline bool verbatimSelectable(int id, std::string_view text, bool selected, ImGuiSelectableFlags flags = 0)
{
    ImGui::PushID(id);
    const bool hit = ImGui::Selectable("##row", selected, flags, ImVec2(0, ImGui::GetTextLineHeight()));
    ImGui::GetWindowDrawList()->AddText(ImGui::GetItemRectMin(), ImGui::GetColorU32(ImGuiCol_Text), text.data(), text.data() + text.size());
    ImGui::PopID();
    return hit;
}

} // namespace noxsdl::editor
