-- Undead Tide's words about the run as it goes: the tide coming, the land it rises in, the minutes,
-- the fallen and those back, the run's end and its results. The director asks each where its own
-- C++ told them; owner.announce tells everyone (the engine's line to all, as the C++ sent it).
local data = require("data")
local util = require("util")

local fill = util.fill
local text = data.text

local told = {}

-- A clock as the lines show it: minutes and seconds ("3:05").
local function clock(seconds: number): string
    local m = seconds // 60
    local s = seconds % 60
    return tostring(m) .. (if s < 10 then ":0" else ":") .. tostring(s)
end
told.clock = clock

local function difficultyName(d: number): string
    return data.difficulties[d + 1].name
end

-- The portal taken: the tide's coming, at the difficulty set.
function told.coming(difficulty: number)
    owner.announce(fill(text.coming, { difficulty = difficultyName(difficulty) }))
end

-- A NoX Core player's choice of difficulty at the gate.
function told.setsDifficulty(player: string, difficulty: number)
    local d = data.difficulties[difficulty + 1]
    owner.announce(fill(text.setsDifficulty, { player = player, difficulty = d.name, about = d.about }))
end

-- A stage's arena begun: the land, the difficulty and the goal (dawn's or the guardian's).
function told.rises(stage: number, land: string, difficulty: number, dawnWins: boolean, dawn: number, guardian: number)
    local where = fill(if stage > 1 then text.risesStage else text.rises,
        { stage = tostring(stage), land = if land == "" then text.darkLand else land, difficulty = difficultyName(difficulty) })
    if dawnWins then
        owner.announce(fill(text.goalDawn, { where = where, dawn = clock(dawn), guardian = clock(guardian) }))
    else
        owner.announce(fill(text.goalGuardian, { where = where, guardian = clock(guardian) }))
    end
end

-- A minute gone in a stage: the time left till dawn, or the guardian still standing.
function told.minute(minute: number, seconds: number, dawn: number, guardianSlain: number)
    if seconds < dawn then
        owner.announce(fill(text.minuteDawn, { clock = clock(minute * 60), left = clock(dawn - seconds) }))
    elseif guardianSlain == 0 then
        owner.announce(fill(text.minuteGuardian, { clock = clock(minute * 60) }))
    end
end

-- A player out of lives: the tide takes it.
function told.taken(player: string)
    owner.announce(fill(text.tideTakes, { player = player }))
end

-- An offer left unanswered: its first taken for the player.
function told.pickedForYou(unit: number)
    nox.tell(unit, text.pickedForYou)
end

-- The run's end: how it ended, then the results (the elites, the guardian, a line for each runner:
-- `runners` {name, level, kills, kit = {{kind, level}...}} in the records' order).
function told.ended(won: boolean, guardianSlain: boolean, guardianCame: boolean, guardian: string, seconds: number, stage: number,
    total: number, elites: number, runners)
    if won and guardianSlain then
        owner.announce(fill(text.endGuardianDawn, { guardian = guardian, clock = clock(seconds) }))
    elseif won then
        owner.announce(fill(text.endDawn, { clock = clock(seconds) }))
    elseif stage > 1 then
        owner.announce(fill(text.endLostStage, { stage = tostring(stage), clock = clock(seconds), total = clock(total) }))
    else
        owner.announce(fill(text.endLost, { clock = clock(seconds) }))
    end
    -- (Past the first stage: the stage reached and the guardians slain before it, then this
    -- stage's own.)
    local earlier = stage - 1
    local before = if earlier > 0
        then fill(if earlier == 1 then text.beforeOne else text.beforeMany, { stage = tostring(stage), count = tostring(earlier) })
        else ""
    local slain = fill(if elites == 1 then text.eliteOne else text.eliteMany, { count = tostring(elites) })
    local fate = if not guardianCame then text.fateNeverCame
        else fill(if guardianSlain then text.fateSlain else text.fateStanding, { guardian = guardian })
    owner.announce(fill(text.results, { elites = slain, before = before, fate = fate }))
    for _, r in runners do
        local kit = ""
        for _, w in r.kit do
            kit ..= (if kit == "" then "" else text.listSeparator) .. fill(text.kitItem, { name = data.weapons[w.kind + 1].name, level = tostring(w.level) })
        end
        owner.announce(fill(text.runnerLine, { player = r.name, level = tostring(r.level), kills = tostring(r.kills), kit = kit }))
    end
end

-- A run of the records as its line tells it (the board's, TideRecords::describe's): `r` {outcome
-- (0 fell, 1 dawn, 2 guardian slain), seconds, stage, difficulty, biome, date, kills, party =
-- {{name, class, level}...}}. How it ended, a difficulty past the first, the party's best four by
-- name (each to 20 letters: the line stays within one text message), the rest counted, the kills,
-- the biome, the date.
local NAMED = 4
function told.record(r): string
    local at = clock(math.max(0, r.seconds))
    local outcome = if r.outcome == 2 then fill(text.recordGuardian, { clock = at })
        elseif r.outcome == 1 then fill(text.recordDawn, { clock = at })
        elseif r.stage > 1 then fill(text.recordStage, { stage = tostring(r.stage), clock = at })
        else fill(text.recordFell, { clock = at })
    local on = ""
    if r.difficulty ~= "" and r.difficulty ~= data.difficulties[1].name then
        on = fill(text.recordOn, { difficulty = string.sub(r.difficulty, 1, 12) })
    end
    local members = ""
    for i = 1, math.min(#r.party, NAMED) do
        local m = r.party[i]
        members ..= (if i == 1 then "" else text.listSeparator)
            .. fill(text.recordMember, { name = string.sub(m.name, 1, 20), class = m.class, level = tostring(m.level) })
    end
    local more = if #r.party > NAMED then fill(text.recordMore, { count = tostring(#r.party - NAMED) }) else ""
    local biome = if r.biome == "" then "" else fill(text.recordBiome, { biome = string.sub(r.biome, 1, 20) })
    local date = if r.date == "" then "" else fill(text.recordDate, { date = string.sub(r.date, 1, 10) })
    return fill(text.recordLine, { outcome = outcome, on = on, members = members, more = more, kills = tostring(r.kills), biome = biome, date = date })
end

-- The run's end, its results told: the players still in play kept safe while they read them (11 s).
function told.keptSafe(units: { number })
    for _, u in units do
        nox.enchant(u, "ENCHANT_INVULNERABLE", 11 * 30, 1)
    end
end

return told
