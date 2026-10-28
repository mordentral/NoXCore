-- Delving Mode's lobby: what's said and done as players come in and walk about it. The director
-- watches (its watchers: the group's arrivals, a zone on each sign, the board and the portal) and
-- calls these as each happens; the director's own state (the records, the run's lock) is the
-- owner's.
local data = require("data")
local words = require("words")

local text = data.text
local fill = words.fill

local lobby = {}

-- How long a sign's line stays over it, in frames.
local SIGN_SHOWN = 150

-- A player come in (its unit; whether it's on NoX Core, 1 or 0): one on the stock client is told,
-- once a visit, that NoX Core plays the Delve best (its sign says so too, but only to one who
-- walks up to it).
function lobby.arrive(unit: number, identified: number)
    if identified == 0 then
        nox.tell(unit, text.clientGreeting)
    end
end

-- A player near sign `index` (west to east, from 1; `sign` its id): its line spoken (the
-- director's zone: at most every so often).
function lobby.sign(index: number, sign: number)
    nox.unitSay(sign, data.rules[index], SIGN_SHOWN)
end

-- The deepest delves as told (the owner's records, the first few), line by line: each run's place,
-- depth, act, date and party (a few of each named: a line's length), and where the full board is.
function lobby.recordLines(): { string }
    local shown, full = owner.records(data.board.shown)
    if #shown == 0 then
        return { text.recordsNone, full }
    end
    local lines = { text.recordsTitle }
    local named = data.board.named
    for i, r in shown do
        local party = ""
        for k = 1, math.min(#r.party, named) do
            local m = r.party[k]
            party = party .. (if party == "" then "" else text.listComma) .. fill(text.recordMember, { name = m.name, class = m.class })
        end
        if #r.party > named then
            party ..= fill(text.recordMore, { count = tostring(#r.party - named) })
        end
        table.insert(lines, fill(text.recordLine, { place = tostring(i), depth = tostring(r.depth), act = tostring(r.act), date = r.date, party = party }))
    end
    table.insert(lines, full)
    return lines
end

-- The records told to one player (its unit), the board's and "?record"'s.
function lobby.records(unit: number)
    for _, line in lobby.recordLines() do
        nox.tell(unit, line)
    end
end

-- When the board last told each player (by name) the records, as its frame with the low bit set
-- (never 0); kept for good, as the server runs.
local boardToldAt: { [string]: number } = {}

-- A player come up to the board (its unit and name, the frame): the records told it, at most every
-- so often each.
function lobby.board(unit: number, name: string, frame: number)
    local at = boardToldAt[name]
    -- (Frames as the server counts them: a whole 32 bits, wrapping.)
    local since = frame - (at or 0)
    if since < 0 then
        since += 4294967296
    end
    if at == nil or data.every.board < since then
        lobby.records(unit)
        boardToldAt[name] = bit32.bor(frame, 1)
    end
end

-- Someone not busy stepping into the portal: the delve begins, everyone in it (the owner's lock:
-- the run's party, the game closed), the quest begun with everyone in (OTQuest.rul's line).
function lobby.portal()
    nox.textToAll(text.begins)
    owner.begin()
    owner.startSoloQuest()
end

return lobby
