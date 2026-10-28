// The editor's panel images for the floating panels (panel_textures.hpp).
#include "editor/panel_textures.hpp"

#include <cstdint>

namespace noxsdl::editor {

TextureMirror& panelTextures()
{
    static TextureMirror mirror;
    return mirror;
}

void panelTextureSet(SDL_Texture* texture, const void* pixels, int w, int h, int pitch, SDL_PixelFormat format)
{
    if (!texture) return;
    TexturePixels p;
    p.data = static_cast<const std::uint8_t*>(pixels);
    p.width = w;
    p.height = h;
    p.pitch = pitch;
    p.bytesPerPixel = 4;
    p.format = static_cast<std::uint32_t>(format);
    panelTextures().setPixels(reinterpret_cast<std::uintptr_t>(texture), p);
}

void panelTextureUpdate(SDL_Texture* texture, const SDL_Rect& rect, const void* pixels, int pitch)
{
    if (!texture) return;
    panelTextures().updatePixels(reinterpret_cast<std::uintptr_t>(texture), TexelRect{rect.x, rect.y, rect.w, rect.h},
                                 static_cast<const std::uint8_t*>(pixels), pitch);
}

void panelTextureForget(SDL_Texture* texture)
{
    if (!texture) return;
    panelTextures().remove(reinterpret_cast<std::uintptr_t>(texture));
}

} // namespace noxsdl::editor
