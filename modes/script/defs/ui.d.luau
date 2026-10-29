-- The UI API: the global `nox` a mode's UI script sees on a NoX Core client
-- (modes/script/src/script_ui.cpp registers it; a test checks that every function there is
-- declared here). Read by the map editor's Luau diagnostics and its Mod Reference panel. Each
-- function: one "--- " doc line, then its declaration on one line.

type NoxColour = string | { number }
-- frame, clickThrough and hud are UI API 3's: frame = false draws no frame, title or background
-- (any y on the screen); clickThrough = true leaves clicks, the wheel and the mouse over it to the
-- game; hud = true hides it whenever the game hides its HUD and draws it under the framed windows.
-- anchor (UI API 3 and up): "topleft", "top", "topright", "left", "right", "bottomleft", "bottom"
-- or "bottomright" keeps the window's distance (x, y in the 640 x 480) to that edge or corner of
-- the whole view on a wider or taller screen; none or "center": centred with the 640 x 480.
type NoxWindowSpec = { title: string?, x: number?, y: number?, w: number?, h: number?, frame: boolean?, clickThrough: boolean?, hud: boolean?, anchor: string? }
type NoxTooltipLine = { any } | string -- { text, colour? }, or a white line's text
-- keep is UI API 4's: true keeps the label while its object is out of the client's world.
type NoxLabelOptions = { show: string?, near: number?, offset: number?, keep: boolean? }

declare nox: {
    --- # Windows
    --- Opens (or moves) the window `id`, framed and labelled as the server's (UI API 3: or bare, click-through, a HUD piece), in the game's 640 x 480 units.
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
    --- Draws a line from (x1, y1) to (x2, y2) in the window being drawn, width 1-8 (UI API 3).
    line: (x1: number, y1: number, x2: number, y2: number, colour: NoxColour?, width: number?) -> (),
    --- Shows { { text, colour? }, ... } (16 lines, 128 bytes each) at the mouse over everything this frame, kept on the screen (inside ui.draw; UI API 3).
    tooltip: (lines: { NoxTooltipLine }) -> (),
    --- Draws the game's own icon for a spell ("SPELL_FIREBALL") or a warrior's ability ("ABILITY_WARCRY") at its top left, its size or w x h; an unknown name draws nothing (UI API 4).
    spellIcon: (name: string, x: number, y: number, w: number?, h: number?) -> (),
    --- Marks a part of a frameless window that takes the mouse this frame; with any, the rest of it is click-through (inside ui.draw; 32 a window; UI API 4).
    hitArea: (x: number, y: number, w: number, h: number) -> (),
    --- # World labels (UI API 3)
    --- Labels an object (its net code: the server's nox.object(id).netCode) with up to 64 bytes; opts { show = "always" | "alt" | "near", near = 150, offset = 4, keep = false (UI API 4: kept while it's away, till nox.clearLabel or a map's load: clear it when your server says its object is gone) }; false over 128 labels (UI API 3).
    label: (object: number, text: string, colour: NoxColour?, opts: NoxLabelOptions?) -> boolean,
    --- Takes the label off an object (UI API 3).
    clearLabel: (object: number) -> (),
    --- Takes every label off (UI API 3).
    clearLabels: () -> (),
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
--- @hook tick(nowMs) | (UI API 4) Once a frame before the draws, the HUD shown or hidden: windows may open, move and close here.
--- @hook draw(window) | Each frame, each window: draw it (box, outline, text).
--- @hook click(window, x, y, button) | A click inside a window (its own units; button 1 left, 2 right with UI API 2): true takes it.
--- @hook key(window, key) | (UI API 2) A key ("0"-"9", arrows, "enter", "escape", "tab", "space", "f1"-"f12") with the mouse over a window: true takes it.
--- @hook drag(window, phase, x, y, button) | (UI API 3) A press in a window moved 3 units or more: phase "start" (at the press), "move", "end" (let go), in that window's units.
--- @hook mouseUp(window, x, y, button) | (UI API 3) A button let go over a window (pressed in one of the UI's windows), in its units.
--- @hook wheel(window, x, y, steps) | (UI API 3) The wheel over a window: steps > 0 up, < 0 down (the UI's, taken or not).
--- @hook onMessage(value) | A value the server script sent (nox.send).
--- @hook onView(view) | A view as data ({ kind = "say" | "choice" | "table", ... }): true draws it (else it's shown as text).
