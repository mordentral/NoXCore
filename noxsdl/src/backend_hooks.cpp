#include "backend_hooks.hpp"

namespace noxsdl::backend {

const std::filesystem::path& settingsDirectory(const std::filesystem::path& gameDir) { return gameDir; }
SDL_WindowFlags additionalWindowFlags() { return 0; }
std::optional<SDL_Rect> usableRenderArea(SDL_Renderer*) { return std::nullopt; }

} // namespace noxsdl::backend
