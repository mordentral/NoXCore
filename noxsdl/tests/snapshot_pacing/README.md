# Match Server

The fifth Smoothing choice draws the latest state when a server update arrives,
including NoXCore movement substeps. It adds no easing or interpolation buffer.
Default local hosting (including solo play) supplies 60 updates per second;
servers can supply 30, 60, 90 or 120. Rendering performance, packet arrivals and
VSync can reduce the displayed rate. Multiple updates received together produce
one frame of the latest state, not a burst of historical frames.

The original four choices and the default (Smooth Network) keep their behavior.
Lock to 30 FPS follows only base game frames. Both update-driven modes allow a
redraw after 50 ms without an update, as the original lock did. Menus without a
live game retain their normal rendering behavior. This option does not change
the server's update rate or the VSync setting.

The setting is saved as `Smoothing = 4`; `--smoothing server` selects it for a run.

Run the standalone tests without SDL or game assets:

```sh
cmake -S noxsdl/tests/snapshot_pacing -B build/snapshot-pacing-tests
cmake --build build/snapshot-pacing-tests
ctest --test-dir build/snapshot-pacing-tests --output-on-failure
```

Desktop acceptance:

- Select Match Server in Options > Smoothing and check that it persists after
  closing Options and restarting.
- In solo play, compare Lock to 30 FPS with Match Server: the latter should show
  the local server's extra movement updates (normally about 60 FPS).
- Repeat with server update rates of 30, 90 and 120, and with VSync on and off.
- Switch modes, pause/resume and reconnect. Confirm that rendering continues and
  that the existing smoothing modes and menu behavior remain unchanged.

The Windows and Linux CI jobs run this standalone suite on every push and pull request.
