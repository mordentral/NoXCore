-- Delving Mode's words about the run: a level's line, a gate's sign, where a gate leads, an act as
-- a guardian's gate tells it. Each from the run as the director hands it (owner.run's table):
--   { depth, room, mystery (-1: none known), gates (whether the level has its two), biome (the
--     level's), difficulty, act, fork (nil, or the guardian's two acts) }
-- an act: { number, biome, rows (six, each its rooms by number), path (its lane at each row) }.
local data = require("data")
local words = require("words")

local text = data.text
local fill = words.fill

local told = {}

-- The rooms' numbers (DelveRoom), and an act's length.
local FIGHT, ELITE, MYSTERY, SHOP, CAMPFIRE, GUARDIAN, BLESSING = 0, 1, 2, 3, 4, 5, 6
local LENGTH = 6

-- A whole number's digits, as the C++'s std::to_string.
local function num(n: number): string
    return string.format("%d", n)
end

-- (C's remainder: its sign the dividend's.)
local function rem(a: number, b: number): number
    return math.fmod(a, b)
end

-- "a fight", "an elite", "a mystery", "a shop", "a campfire", "the guardian"
function told.roomPhrase(room: number): string
    local r = data.rooms[room + 1]
    return if r then r.phrase else "a room"
end

-- "an elite", "2 elites"
local function counted(n: number, room: number): string
    if n == 1 then
        return told.roomPhrase(room)
    end
    local r = data.rooms[room + 1]
    local many = if r then (r.many or r.name .. "s") else "?s"
    return fill(text.actCount, { count = num(n), rooms = many })
end

-- A list as told: "a, b and c".
function told.list(items: { string }): string
    local s = ""
    for i, item in items do
        s ..= (if i == 1 then "" elseif i == #items then text.listAnd else text.listComma) .. item
    end
    return s
end

-- The room at `row` (0-5, kept within) in lane `lane` (kept within the row; none in it: the
-- guardian's row's guardian, else a fight), as DelveAct::room.
function told.actRoom(act, row: number, lane: number): number
    row = math.clamp(row, 0, LENGTH - 1)
    local r = act.rows[row + 1]
    if #r == 0 then
        return if row >= LENGTH - 1 then GUARDIAN else FIGHT
    end
    return r[math.min(math.max(lane, 0), #r - 1) + 1]
end

-- A biome by its name, any case (the map generator's, owner.biome: its title and climate; nil:
-- none such), each kept as first asked.
local biomes: { [string]: any } = {}
local NONE = {}
local function biome(name: string)
    local b = biomes[name]
    if b == nil then
        local title, climate = owner.biome(name)
        b = if title then { title = title, climate = climate } else NONE
        biomes[name] = b
    end
    return if b == NONE then nil else b
end
-- "the Forest", "the Frozen Wastes"
function told.placeName(name: string): string
    local b = biome(name)
    return if b then b.title else fill(text.placeUnknown, { biome = name })
end

-- What the biome's air does as players are told it, and its gate's warning ("" when mild).
local function climate(name: string)
    local b = biome(name)
    return if b then data.climates[b.climate] else nil
end
function told.climateLine(name: string): string
    local c = climate(name)
    return if c then c.line else ""
end
function told.climateTag(name: string): string
    local c = climate(name)
    return if c then c.tag else ""
end

-- An act as a guardian's gate tells it: its first room, then its rows 2-4's rooms by kind.
function told.describe(act): string
    local kinds = {}
    for _, k in { ELITE, MYSTERY, FIGHT, SHOP, BLESSING, CAMPFIRE } do
        local n = 0
        for r = 2, 4 do
            for _, room in act.rows[r] do
                if room == k then
                    n += 1
                end
            end
        end
        if n > 0 then
            table.insert(kinds, counted(n, k))
        end
    end
    local ways = if #kinds == 0 then "" else fill(text.actWays, { rooms = told.list(kinds) })
    return fill(text.actPlan, { first = told.roomPhrase(told.actRoom(act, 0, 0)), ways = ways })
end

-- The run now (owner.run's table), kept till its key changes (owner.runKey: a level written, the
-- difficulty set): not read again for each sign. Read only.
local kept, keptKey = nil, nil
function told.run()
    local key = owner.runKey()
    if key ~= keptKey then
        kept = owner.run()
        keptKey = key
    end
    return kept
end

-- The level's lane in its act's row (the act's path's last).
local function lane(run): number
    local path = run.act.path
    return if #path == 0 then 0 else path[#path]
end

-- A difficulty's name (one past the four: the first's).
local function difficultyName(d: number): string
    local all = data.difficulties
    local one = all[d + 1] or all[1]
    return one.name
end

-- The level's line: its depth, act and difficulty, where, what the room is (a mystery's, once
-- known, its kind's), the ways on, the air.
function told.levelLine(run): string
    local act = run.act
    local what = if run.room == MYSTERY and run.mystery >= 0 then data.mysteries[run.mystery + 1] else data.rooms[run.room + 1].told
    local row = rem(run.depth - 1, LENGTH)
    local ways = ""
    if run.room ~= GUARDIAN and run.gates then
        ways = fill(text.levelGates, {
            left = told.roomPhrase(told.actRoom(act, row + 1, lane(run))),
            right = told.roomPhrase(told.actRoom(act, row + 1, lane(run) + 1)),
        })
    elseif run.room ~= GUARDIAN then
        ways = fill(text.levelAhead, { next = told.roomPhrase(told.actRoom(act, row + 1, 0)) })
    end
    local air = told.climateLine(run.biome)
    return fill(text.levelLine, {
        depth = num(run.depth),
        act = num(act.number),
        difficulty = if run.difficulty ~= 0 then fill(text.levelDifficulty, { difficulty = difficultyName(run.difficulty) }) else "",
        place = told.placeName(act.biome),
        what = what,
        ways = ways,
        air = if air == "" then "" else fill(text.levelAir, { air = air }),
    })
end

-- Where gate `gate` (0 the left) leads, as "X takes the LEFT gate: ..." says it: the act's place
-- (a guardian's), else the room.
function told.gateWhere(run, gate: number): string
    if run.room == GUARDIAN then
        local to = if run.fork then run.fork[gate + 1] else nil
        return if to then told.placeName(to.biome) else ""
    end
    return told.roomPhrase(told.actRoom(run.act, rem(run.depth - 1, LENGTH) + 1, lane(run) + gate))
end

-- What gate `gate`'s sign says: a guardian's, the act it leads to; else the room it leads to and
-- that room's own two ways on (or the guardian).
function told.gateLine(run, gate: number): string
    local side = if gate == 0 then text.gateLeft else text.gateRight
    if run.room == GUARDIAN then
        local to = if run.fork then run.fork[gate + 1] else nil
        if not to then
            return ""
        end
        return fill(text.gateToAct, { side = side, place = told.placeName(to.biome), climate = told.climateTag(to.biome), act = told.describe(to) })
    end
    local act = run.act
    local nextRow = rem(run.depth - 1, LENGTH) + 1
    local at = lane(run) + gate
    local after = ""
    if nextRow + 1 == LENGTH - 1 then
        after = text.thenGuardian
    elseif nextRow + 1 < LENGTH - 1 then
        after = fill(text.thenRooms, {
            left = told.roomPhrase(told.actRoom(act, nextRow + 1, at)),
            right = told.roomPhrase(told.actRoom(act, nextRow + 1, at + 1)),
        })
    end
    return fill(text.gateToRoom, { side = side, room = told.roomPhrase(told.actRoom(act, nextRow, at)), ["then"] = after })
end

-- The bonus objective's kinds (DelveObjective).
local PURGE, HUNT, SEALS, SWIFT = 1, 2, 3, 4

-- What the bonus objective asks (its kind; the Hunt's quarry by name; Swift's seconds), as told.
function told.objectiveText(kind: number, quarry: string, seconds: number): string
    if kind == PURGE then
        return text.objPurge
    elseif kind == HUNT then
        return fill(text.objHunt, { quarry = quarry })
    elseif kind == SEALS then
        return text.objSeals
    elseif kind == SWIFT then
        -- (C's division and remainder: toward 0.)
        local minutes = if seconds >= 0 then math.floor(seconds / 60) else -math.floor(-seconds / 60)
        local left = rem(seconds, 60)
        return fill(text.objSwift, { minutes = num(minutes), seconds = (if left < 10 then "0" else "") .. num(left) })
    end
    return ""
end

return told