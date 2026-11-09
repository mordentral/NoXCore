# Mouse ownership regression checks

Run the standalone, asset-free policy tests:

```sh
cmake -S noxsdl/tests/mouse_handover -B build/mouse-tests
cmake --build build/mouse-tests
ctest --test-dir build/mouse-tests --output-on-failure
```

Desktop acceptance checks (windowed mode, with game assets):

- In the starting menus, only the Nox cursor appears inside the window. Moving
  outside shows the desktop pointer; returning restores the Nox cursor.
- Start a game: the Nox cursor stays confined to the window. Open Escape, Options,
  or key bindings: the Nox cursor remains inside, but can move out to the desktop.
  Resume: confinement returns without an extra click. Repeat several times.
- Switch applications or minimize while playing or dragging: capture and held
  actions release. Return to the game and check capture resumes.
- Leave an editor play-test for the menus: the Nox cursor follows the mouse.
  The editor itself keeps its system pointer; embedded play-tests retain their
  explicit capture/release controls.
- Hidden `--screenshot` runs never capture the desktop mouse.

The automated checks cover ownership policy; the desktop checks require a real
window system and are not established by those tests alone.
