// The modes built into this program (modes/, NOX_MODES), registered in the process's registry
// (registry.hpp). Call once at start-up, before any game is hosted; again does nothing.
#pragma once

namespace nox::modes {

void registerBuiltinModes();

} // namespace nox::modes
