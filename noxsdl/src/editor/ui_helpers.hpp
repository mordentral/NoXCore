// Small ImGui pieces the map editor's panels share: the hover help, a colour from its bytes, an SDL
// texture as ImGui's texture id, a held number field, and the system file dialog's answer kept for
// the next frame.
#pragma once

#include "editor/held_edit.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace noxsdl::editor {

/// The last item's tooltip, shown after the hover delay `flags` asks for (nothing for no text).
inline void help(const char* text, ImGuiHoveredFlags flags = ImGuiHoveredFlags_DelayNormal)
{
    if (text && *text && ImGui::IsItemHovered(flags)) {
        ImGui::SetTooltip("%s", text);
    }
}
inline void help(const std::string& text, ImGuiHoveredFlags flags = ImGuiHoveredFlags_DelayNormal)
{
    help(text.c_str(), flags);
}

/// A property row's label (dim, on the fields' baseline; its help when hovered), the field after it
/// from `fieldX` to the row's end.
inline void labelRow(const char* label, float fieldX, const char* helpText = nullptr, ImGuiHoveredFlags helpFlags = ImGuiHoveredFlags_DelayNormal)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    help(helpText, helpFlags);
    ImGui::SameLine(fieldX);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

inline ImU32 col(int r, int g, int b, int a = 255)
{
    return IM_COL32(r, g, b, a);
}

inline ImTextureID texId(SDL_Texture* t)
{
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(t));
}

/// An InputInt (with - / +) whose value is committed as an op (held_edit.hpp's HeldValue): its own
/// count while typed into or held, kept in [lo, hi]; then the value to commit, once. `keepStored`: a
/// stored value out of range is shown as it is till it's changed.
inline std::optional<int> heldInputInt(HeldValue<int>& held, const char* id, int current, int lo, int hi, bool keepStored)
{
    int& v = held.show(current);
    ImGui::InputInt(id, &v);
    if (!keepStored || v != current) v = std::clamp(v, lo, hi);
    return held.after(ImGui::IsItemActive(), ImGui::IsItemDeactivatedAfterEdit(), current);
}

/// What the system's file dialog picked, for the editor's next frame: `userdata` is a
/// std::pair<std::mutex*, std::optional<std::string>*> (the callback may come from another thread).
inline void SDLCALL storePickedFile(void* userdata, const char* const* files, int /*filter*/)
{
    if (!files || !files[0]) {
        return; // (cancelled, or an error)
    }
    auto* picked = static_cast<std::pair<std::mutex*, std::optional<std::string>*>*>(userdata);
    std::lock_guard lock(*picked->first);
    *picked->second = files[0];
}

} // namespace noxsdl::editor
