// Mac client policy. Other platforms use the original paths and window flags.
#pragma once

#include <SDL3/SDL.h>

#include <filesystem>

namespace noxsdl::backend {

#if defined(SDL_PLATFORM_MACOS)
/// Directory for nox.cfg and noxcore.cfg; assets and factory default.cfg stay in gameDir.
/// The returned reference must remain valid for the duration of its use by the caller.
const std::filesystem::path& settingsDirectory(const std::filesystem::path& gameDir);
/// Additional creation flags only; the caller retains its resizable/hidden flags.
SDL_WindowFlags additionalWindowFlags();
#else
inline const std::filesystem::path& settingsDirectory(const std::filesystem::path& gameDir) { return gameDir; }
inline SDL_WindowFlags additionalWindowFlags() { return 0; }
#endif

} // namespace noxsdl::backend
