// A program's window icon (the title bar's and the taskbar's): the .ico cmake's nox_app_icon
// embedded in it (cmake/NoxWindowsResources.cmake): noxcore.exe's noxsdl/res/noxcore.ico,
// noxcontent's videotools/res/noxcontent.ico.
//
// On Windows the exe carries the same icon as a resource: SDL registers its window class with it
// before any window opens, and Windows picks the size it shows from the icon's own sizes, sharper
// than SDL_SetWindowIcon's one picture made into every size. So there the window keeps the class
// icon, and the embedded copy is set only if the class has none. Elsewhere (X11, Wayland) the
// embedded copy is set: its largest size as the surface, the rest as its alternate images.
#pragma once

struct SDL_Window;

namespace noxsdl {

/// Gives `window` the icon (see above); logs what it did. False if it had to and couldn't.
bool setWindowIcon(SDL_Window* window);

} // namespace noxsdl
