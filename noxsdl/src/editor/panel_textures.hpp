// The editor's own panel images (thumbnails, previews, the minimap: textures of the main window's
// renderer that ImGui::Image draws) registered for the floating panels: a floating panel's window
// has a renderer of its own, which draws a copy made from the pixels kept here
// (texture_mirror.hpp, imgui_host.cpp). Register a texture when it's made, tell of each update,
// forget it before it's destroyed; an image nobody registered draws as a grey placeholder in a
// floating panel (the main window draws it as ever).
#pragma once

#include "editor/texture_mirror.hpp"

#include <SDL3/SDL.h>

namespace noxsdl::editor {

/// The registry (one for the process; the ImGui host makes the copies).
TextureMirror& panelTextures();

/// `texture`'s whole picture: `w` x `h` pixels of `format` (4 bytes each), rows `pitch` apart.
void panelTextureSet(SDL_Texture* texture, const void* pixels, int w, int h, int pitch, SDL_PixelFormat format = SDL_PIXELFORMAT_RGBA32);
/// Part of `texture` updated (as SDL_UpdateTexture(texture, &rect, pixels, pitch)).
void panelTextureUpdate(SDL_Texture* texture, const SDL_Rect& rect, const void* pixels, int pitch);
/// `texture` about to be destroyed.
void panelTextureForget(SDL_Texture* texture);

} // namespace noxsdl::editor
