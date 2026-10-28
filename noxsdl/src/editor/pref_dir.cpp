// The user's settings folder for the map editor (editor_settings.hpp's editorPrefDir): SDL's
// pref path, apart from the settings themselves so they build without SDL.
#include "editor/editor_settings.hpp"

#include <SDL3/SDL.h>

namespace noxsdl::editor {

std::filesystem::path editorPrefDir()
{
    std::filesystem::path dir;
    if (char* pref = SDL_GetPrefPath("noxcpp", "noxsdl")) {
        dir = std::filesystem::path(reinterpret_cast<const char8_t*>(pref)) / "editor";
        SDL_free(pref);
    } else {
        dir = std::filesystem::current_path() / "noxsdl_editor";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace noxsdl::editor
