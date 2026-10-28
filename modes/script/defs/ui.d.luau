-- The UI API: the global `nox` a mode's UI script sees on a NoX Core client
-- (modes/script/src/script_ui.cpp registers it; a test checks that every function there is
-- declared here). Read by the map editor's Luau diagnostics and its Mod Reference panel. Each
-- function: one "--- " doc line, then its declaration on one line.

type NoxColour = string | { number }
type NoxWindowSpec = { title: string?, x: number?, y: number?, w: number?, h: number? }

declare nox: {
    --- # Windows
    --- Opens (or moves) the window `id`, framed and labelled as the server's, in the game's 640 x 480 units.
    window: (id: string, spec: NoxWindowSpec) -> (),
    --- Closes the window `id`.
    closeWindow: (id: string) -> (),
    --- # Drawing (inside ui.draw)
    --- Fills a box in the window being drawn; a colour is a name ("gold", "dim"...) or { r, g, b [, a] }.
    box: (x: number, y: number, w: number, h: number, colour: NoxColour?) -> (),
    --- Draws a box's outline in the window being drawn.
    outline: (x: number, y: number, w: number, h: number, colour: NoxColour?) -> (),
    --- Draws text at its top left in the window being drawn (at most 256 bytes).
    text: (text: string, x: number, y: number, colour: NoxColour?) -> (),
    --- How wide the client draws `text`, in the UI's units.
    textWidth: (text: string) -> number,
    --- # Talking to the server
    --- Sends a value (nil, booleans, numbers, strings, tables of them) to the server script's onUiMessage.
    send: (value: any) -> (),
    --- Answers the open choice (1-9), as the player saying its number would.
    answer: (index: number) -> (),
    --- # Pictures and sounds (UI API 2)
    --- Draws one of the mod's own pictures (assets\NAME.png) at its size, or scaled to w x h (UI API 2).
    image: (name: string, x: number, y: number, w: number?, h: number?) -> (),
    --- Draws a thing type's picture from the client's own art (thing.bin's name), centred on (x, y) (UI API 2).
    thing: (typeName: string, x: number, y: number) -> (),
    --- Plays one of the mod's own sounds (assets\NAME.wav), volume 0-100 (UI API 2).
    sound: (name: string, volume: number?) -> (),
    --- Plays one of the game's own sounds by name, volume 0-100 (UI API 2).
    gameSound: (name: string, volume: number?) -> (),
    --- # The client
    --- The window under the mouse and the mouse in its units; nothing when it's over none (UI API 2).
    mouse: () -> (string?, number?, number?),
    --- Our player's { name, class, level, health, maxHealth, mana, maxMana } (read-only); nil out of a game (UI API 2).
    player: () -> { name: string, class: string, level: number, health: number, maxHealth: number, mana: number, maxMana: number }?,
    --- The client's clock in milliseconds.
    time: () -> number,
    --- The mode's name, as its windows are labelled.
    mode: () -> string,
    --- The UI API this client runs.
    api: number,
}

-- The hooks the script's table may have (the client calls each one it has), for the Mod
-- Reference: "--- @hook <call> | <doc>".
--- @hook start() | The UI is in: open its windows.
--- @hook draw(window) | Each frame, each window: draw it (box, outline, text).
--- @hook click(window, x, y, button) | A click inside a window (its own units; button 1 left, 2 right with UI API 2): true takes it.
--- @hook key(window, key) | (UI API 2) A key ("0"-"9", arrows, "enter", "escape", "tab", "space", "f1"-"f12") with the mouse over a window: true takes it.
--- @hook onMessage(value) | A value the server script sent (nox.send).
--- @hook onView(view) | A view as data ({ kind = "say" | "choice" | "table", ... }): true draws it (else it's shown as text).
