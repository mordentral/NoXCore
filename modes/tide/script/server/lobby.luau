-- Undead Tide's lobby: its signs speak the rules, its records board tells the best runs. The
-- director's lobby zones (modes/common's watchers, data.lobby's reaches and waits) ask these as a
-- player comes near.
local data = require("data")
local told = require("told")
local util = require("util")

local FPS = 30 -- (frames a second)
local BOARD_WAIT = 20 * FPS -- (the board tells one player at most every 20 s)
local BOARD_RUNS = 5 -- (the best runs it tells)

local lobby = {}

-- The i-th sign from the west speaks the i-th rule (a bubble over it for 5 s).
function lobby.sign(i: number, id: number)
    nox.unitSay(id, data.rules[i], 5 * FPS)
end

-- When the board last told each player (by unit; a new lobby's load forgets them).
local toldAt: { [number]: number } = {}

-- The board to a player come up to it (its own text is a string table's id: a stock client could
-- show nothing else): the best runs, at most every 20 s. 1 told, 0 not yet (the director asks
-- again as the wait ends, while it stays).
function lobby.board(unit: number, frame: number): number
    local at = toldAt[unit]
    if at and at ~= 0 and frame - at < BOARD_WAIT then
        return 0
    end
    toldAt[unit] = frame
    local best, full = owner.topRecords(BOARD_RUNS)
    if #best == 0 then
        nox.tell(unit, data.text.boardEmpty)
        nox.tell(unit, full)
        return 1
    end
    nox.tell(unit, data.text.boardTitle)
    for i, run in best do
        nox.tell(unit, util.fill(data.text.boardLine, { place = tostring(i), run = told.record(run) }))
    end
    nox.tell(unit, full)
    return 1
end

-- The lobby loaded afresh: who was told when, forgotten.
function lobby.loaded()
    table.clear(toldAt)
end

return lobby
