// Small ImGui pieces the map editor's panels share: the hover help, a colour from its bytes, an SDL
// texture as ImGui's texture id, a held number field, and the system file dialog's answer kept for
// the next frame.
#pragma once

#include "editor/file_pick.hpp"
#include "editor/held_edit.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <optional>

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

/// The system's file dialog's answer, into the FilePick that asked (file_pick.hpp: it may come from
/// another thread, and after the panel that opened the dialog is gone).
inline void SDLCALL storePickedFile(void* userdata, const char* const* files, int /*filter*/)
{
    FilePick::answer(userdata, files);
}

/// The system's dialog for a file to open (one), its answer for `pick.take()` on a later frame.
/// `filters` must outlive the dialog (a static table).
inline void openFileDialog(FilePick& pick, SDL_Window* window, const SDL_DialogFileFilter* filters, int filterCount,
                           const char* start = nullptr)
{
    SDL_ShowOpenFileDialog(storePickedFile, pick.ask(), window, filters, filterCount, start, false);
}

/// The system's dialog for a file to save as, its answer for `pick.take()` on a later frame.
inline void saveFileDialog(FilePick& pick, SDL_Window* window, const SDL_DialogFileFilter* filters, int filterCount,
                           const char* start = nullptr)
{
    SDL_ShowSaveFileDialog(storePickedFile, pick.ask(), window, filters, filterCount, start);
}

} // namespace noxsdl::editor
