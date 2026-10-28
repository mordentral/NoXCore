-- Delving Mode's run as it goes from level to level: each run's seed, the gate the party came
-- through into a depth, the run as that makes it (its act, the fork its guardian opens, the act
-- left behind, the way taken through the act), the levels built ahead, and Swift's gold as the
-- party leaves in time. The director keeps the state and asks these as the map changes.
--
-- An act here: { number, biome, rows (from 1, each a list of rooms), path (from 1, each a lane
-- from 0) }; a fork: two of them.
local acts = require("acts")
local rules = require("rules")

local LENGTH = 6
local SWIFT = 4

local mul32 = nox.mul32

local function u32(x: number): number
    return x % 4294967296
end

local function copyAct(a)
    local rows = table.create(LENGTH)
    for r = 1, LENGTH do
        rows[r] = table.clone(a.rows[r])
    end
    return { number = a.number, biome = a.biome, rows = rows, path = table.clone(a.path) }
end

local function newAct(number: number, biome: string, rows)
    return { number = number, biome = biome, rows = rows, path = {} }
end

local run = {}

-- A run's seed after the first (the first is the server's): run `runs` of the server's `seed`.
function run.seed(seed: number, runs: number): number
    return u32(mul32(bit32.bxor(seed, mul32(runs, 0x9E3779B9)), 2246822519) + runs)
end

-- What the party came through into `depth` (from the act it's in: its number, how far its way
-- goes, whether its guardian's fork is drawn (1 or 0), how many gates the level had): -1 none (a run's
-- first level, or a new act with no fork), -2 the same level made again, 0 the one way on, or
-- -3: the gate most of the party took (the director counts them).
function run.choiceInto(depth: number, actNumber: number, pathSize: number, hasFork: number, gates: number): number
    if depth <= 1 then
        return -1
    end
    if (depth - 1) // LENGTH + 1 ~= actNumber then
        return if hasFork ~= 0 then -3 else -1 -- (a new act: the fork's gate the party took)
    end
    local row = (depth - 1) % LENGTH
    if row > 0 and pathSize <= row then
        return if gates == 2 then -3 else 0 -- (the next row: the gate into it)
    end
    return -2
end

-- The ways a level at `depth` may be come into from the one before, to build ahead: a gate each
-- (its two rows' rooms, or a guardian's two acts), else the one way on (-1: none to choose).
-- (hasFork: 1 or 0.)
function run.ahead(depth: number, hasFork: number)
    if depth <= 1 then
        return { -1 }
    end
    local row = (depth - 2) % LENGTH -- (the level the party's in)
    if row == LENGTH - 1 then
        return if hasFork ~= 0 then { 0, 1 } else { -1 }
    elseif row == LENGTH - 2 then
        return { 0 }
    end
    return { 0, 1 }
end

-- The run at `depth`, the party having come in by `choice` (choiceInto's, or a gate), from the
-- run as it stands (its seed, act, fork or nil, last fork's choice), with a test's or host's
-- biome for every act ("": drawn) and room for every level (-1: the act's). The plan: its act
-- (its way taken through it), its fork, the last fork's choice, the act left behind with the gate
-- out of it (nil: none), and whether it's a new run.
function run.plan(depth: number, choice: number, runSeed: number, act, fork, lastChoice: number, biome: string, room: number)
    local p = { act = copyAct(act), fork = nil, lastChoice = lastChoice, finished = nil, finishedChoice = -1, fresh = false }
    if fork ~= nil then
        p.fork = { copyAct(fork[1]), copyAct(fork[2]) }
    end
    depth = math.max(depth, 1)
    local actNumber = (depth - 1) // LENGTH + 1
    local actSeed = u32(runSeed + mul32(depth, 977))
    -- An act's fork is drawn as the act begins (its map shows both ways from the start); its
    -- guardian's gates open onto it.
    local function drawFork()
        local number = p.act.number + 1
        local lb, lr, rb, rr = acts.fork(number, u32(mul32(runSeed, 31) + mul32(u32(p.act.number), 7919)), p.act.biome)
        p.fork = { newAct(number, lb, lr), newAct(number, rb, rr) }
    end
    if depth == 1 then
        local b, rows = acts.roll(1, actSeed, {})
        p.act = newAct(1, b, rows)
        if biome ~= "" then
            p.act.biome = biome
        end
        p.lastChoice = -1
        p.fresh = true
        drawFork()
    elseif actNumber ~= act.number then
        -- A new act: the fork's way the party took (none made: one of its own).
        if fork ~= nil and choice >= 0 then
            p.lastChoice = choice
            p.finished = copyAct(act)
            p.finishedChoice = choice
            p.act = copyAct(fork[math.min(choice, 1) + 1])
        else
            p.finished = copyAct(act)
            p.finishedChoice = -1
            local b, rows = acts.roll(actNumber, actSeed, { [act.biome] = true })
            p.act = newAct(actNumber, b, rows)
        end
        p.act.number = actNumber
        if biome ~= "" then
            p.act.biome = biome
        end
        drawFork()
    elseif fork == nil then
        drawFork()
    end
    if room >= 0 then
        -- (A test's: every level the one room, the guardian's row too.)
        for r = 0, LENGTH - 1 do
            p.act.rows[r + 1] = table.create(acts.width(r), room)
        end
    end
    -- The way through the act: the room the gate the party took leads to (the left the lane it
    -- was on, the right the next; the guardian's row its one). A level made again keeps its room.
    local path = p.act.path
    local row = (depth - 1) % LENGTH
    if row == 0 then
        p.act.path = { 0 }
    elseif #path <= row then
        -- Rows the path lacks take the lane before; this row's lane is that lane (the left gate)
        -- or one more (the right).
        local fill = if #path == 0 then 0 else path[#path]
        while #path < row do
            table.insert(path, fill)
        end
        local lane = path[#path] + math.max(choice, 0)
        table.insert(path, math.min(lane, acts.width(row) - 1) % 256)
    else
        while #path > row + 1 do
            table.remove(path)
        end
    end
    return p
end

-- Swift done as the party leaves through a gate in time (the map changes as the last goes
-- through, before a level's frame sees it): the gold it pays as the next level opens (0: none).
-- `running`: in a run (1 or 0); the level's objective and whether it began (1 or 0), how it stands (0 open), the
-- frame now and its deadline; the depth come to and the one left.
function run.swift(running: number, objective: number, started: number, state: number, frame: number, deadline: number, depth: number, left: number): number
    if running ~= 0 and objective == SWIFT and started ~= 0 and state == 0 and frame <= deadline and depth > left then
        return rules.swiftGold(left)
    end
    return 0
end

return run
