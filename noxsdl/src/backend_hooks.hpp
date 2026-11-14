// Optional Mac client policy. Defaults preserve the upstream behavior on every platform.
#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>

namespace noxsdl::backend {

#if defined(SDL_PLATFORM_MACOS)
/// Directory for nox.cfg and noxcore.cfg; assets and factory default.cfg stay in gameDir.
/// The returned reference must remain valid for the duration of its use by the caller.
const std::filesystem::path& settingsDirectory(const std::filesystem::path& gameDir);
/// Additional creation flags only; the caller retains its resizable/hidden flags.
SDL_WindowFlags additionalWindowFlags();
/// Positive content bounds in output pixels; nullopt keeps the full output.
/// Called on the window render target, with the default viewport and scale.
std::optional<SDL_Rect> usableRenderArea(SDL_Renderer* renderer);
#else
inline const std::filesystem::path& settingsDirectory(const std::filesystem::path& gameDir) { return gameDir; }
inline SDL_WindowFlags additionalWindowFlags() { return 0; }
inline std::optional<SDL_Rect> usableRenderArea(SDL_Renderer*) { return std::nullopt; }
#endif

} // namespace noxsdl::backend
