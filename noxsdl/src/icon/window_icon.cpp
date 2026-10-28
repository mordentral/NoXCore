#include "window_icon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern const unsigned char kWindowIcon[]; // the program's .ico (nox_app_icon embeds it)
extern const std::size_t kWindowIconSize;

namespace noxsdl {

namespace {

std::uint32_t le16(const unsigned char* p)
{
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8;
}

std::uint32_t le32(const unsigned char* p)
{
    return le16(p) | le16(p + 2) << 16;
}

/// The .ico's PNG entries as surfaces, largest first (the .ico holds only PNGs).
std::vector<SDL_Surface*> iconImages()
{
    std::vector<SDL_Surface*> images;
    const unsigned char* d = kWindowIcon;
    const std::size_t n = kWindowIconSize;
    if (n < 6 || le16(d) != 0 || le16(d + 2) != 1) {
        return images;
    }
    const std::size_t count = le16(d + 4);
    for (std::size_t i = 0; i < count && 6 + 16 * (i + 1) <= n; ++i) {
        const unsigned char* entry = d + 6 + 16 * i;
        const std::size_t size = le32(entry + 8);
        const std::size_t at = le32(entry + 12);
        if (size < 8 || at > n || size > n - at || std::memcmp(d + at, "\x89PNG", 4) != 0) {
            continue;
        }
        if (SDL_Surface* s = SDL_LoadPNG_IO(SDL_IOFromConstMem(d + at, size), true)) {
            images.push_back(s);
        }
    }
    std::sort(images.begin(), images.end(), [](const SDL_Surface* a, const SDL_Surface* b) { return a->w > b->w; });
    return images;
}

} // namespace

bool setWindowIcon(SDL_Window* window)
{
#ifdef _WIN32
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd && GetClassLongPtrW(hwnd, GCLP_HICON) != 0) {
        SDL_Log("window icon: the exe's (its icon resource)");
        return true;
    }
#endif
    std::vector<SDL_Surface*> images = iconImages();
    if (images.empty()) {
        SDL_Log("window icon: couldn't read the embedded icon (%s)", SDL_GetError());
        return false;
    }
    SDL_Surface* icon = images.front();
    for (std::size_t i = 1; i < images.size(); ++i) {
        SDL_AddSurfaceAlternateImage(icon, images[i]); // (takes its own reference)
        SDL_DestroySurface(images[i]);
    }
    const bool ok = SDL_SetWindowIcon(window, icon);
    SDL_Log("window icon: %zu sizes, %dx%d the largest%s%s", images.size(), icon->w, icon->h, ok ? "" : "; not set: ",
            ok ? "" : SDL_GetError());
    SDL_DestroySurface(icon);
    return ok;
}

} // namespace noxsdl
