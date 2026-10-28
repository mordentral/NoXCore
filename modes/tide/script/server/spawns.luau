-- Undead Tide's spawns: the tide's clock (every 1.5-3 s more creatures about each player in play,
-- by shares growing with the minutes; a group of specials every 40 s from the third minute; a
-- wave of hard ones each minute, with an elite once the difficulty's minute is reached) and the
-- night's guardian. The director asks spawn when its next spawn is due, spawnBoss when the
-- guardian's time comes, and beginStage as each stage's arena begins.
--
-- Float figures as the server makes them (each step through nox.f32), float constants as C++
-- float literals are (float(), worked out as the module loads). The director's random stream
-- (owner.roll: its other draws share it) and its foes (owner.addFoe and the rest: its steering and
-- reaping use them) are still its own.
local data = require("data")
local rules = require("rules")

local f32 = nox.f32
local float = rules.float
local floor = math.floor

local FPS = 30 -- (frames a second)

-- The director's settings for the run (setOptions): creatures about one player at most, in all,
-- held about each (a bench's), whether dawn itself wins.
local options = { perView = 60, maxCreatures = 240, holdPerPlayer = 0, dawnWins = false, graceSeconds = 5 }

-- The clock's own: the next wave and the next specials (frames).
local nextWave = 0
local nextSpecial = 0

-- std::lround of a float: to nearest, a half away from zero (exact: a float plus a half is exact
-- in a double).
local function lround(v: number): number
    if v < 0 then
        return -floor(-v + 0.5)
    end
    return floor(v + 0.5)
end

-- A creature type's name as players read it: "OgreBrute" -> "Ogre Brute".
local function spacedName(t: string): string
    return (string.gsub(t, "(%l)(%u)", "%1 %2"))
end

-- A text line from the script's text table, its {fields} filled in one pass (fillText's).
local function fill(template: string, fields: { [string]: string }): string
    return (string.gsub(template, "{([%w_]+)}", function(name)
        return fields[name]
    end))
end

-- Each biome's creature sets (data.luau's).
local function creaturesFor(biome: string)
    return data.creatures[data.biomeCreatures[biome] or data.otherBiomeCreatures]
end

local function pickOf(pool)
    return pool[owner.roll() % #pool + 1]
end

-- A point the tide may come in at: floor, no wall in its cell or about it. (Wall cells are 23
-- units square: the 3 x 3 cells about the point must hold no wall.)
local CELL = 23
local MAP = 5888 -- (256 cells: past it no floor)
local function openGround(x: number, y: number): boolean
    if x < 46 or y < 46 or x >= MAP or y >= MAP or nox.floorAt(x, y) < 0 then
        return false
    end
    local cx = floor(f32(x / CELL))
    local cy = floor(f32(y / CELL))
    for dx = -1, 1 do
        for dy = -1, 1 do
            if nox.wallAt(cx + dx, cy + dy) then
                return false
            end
        end
    end
    return true
end

-- Float constants (C++ float literals).
local DEGREE_TENTH = float(float(math.pi) / 1800) -- (pi_v<float> / 1800.0f, as the compiler folds it)
local MINUTE_GROWTH = float(0.2)
local QUEST_GROWTH = float(0.25)
local QUEST_GROWTH_SQUARED = float(0.015)
local PARTY_GROWTH = float(0.4)

-- The run as the director asks: the frame now, the seconds into the stage, the difficulty and
-- stage, the players in play.
type Now = { frame: number, seconds: number, difficulty: number, stage: number, party: number }

-- One creature of `type` near (px, py), just out of sight: its health the tide's (the quest's
-- health for a horde of the weak, a quarter more each minute and growing faster as the night
-- goes on; an elite's or the guardian's the network game's, a fifth more a minute past the first;
-- both 40% more a player in play past the first, and the difficulty's). Its id, or nil when no
-- spot was found.
local function spawnNear(now: Now, t: string, px: number, py: number, worth: number, rank: number): number?
    for _ = 1, 8 do
        local angle = f32((owner.roll() % 3600) * DEGREE_TENTH)
        -- Just out of a 640 x 480 view (its corners), and a little more.
        local d = 420 + owner.roll() % 100
        local x = f32(px + f32(nox.cosf(angle) * d))
        local y = f32(py + f32(nox.sinf(angle) * d))
        if openGround(x, y) then
            local id = nox.create(t, x, y)
            if not id then
                return nil
            end
            local o = nox.object(id)
            -- A creature sees the players from where it comes in (as a map's own MonsterXfer may
            -- set a creature's sight). (sightRange refuses what isn't a creature.)
            local ok, sight = pcall(nox.sightRange, id)
            if ok then
                nox.sightRange(id, math.max(sight, 450))
            end
            local minute = floor(now.seconds / 60)
            local most = o.maxHealth
            if most ~= 0 then
                local base = most
                local times = 1
                if minute >= 2 then
                    times = f32(1 + f32(MINUTE_GROWTH * (minute - 1)))
                end
                local def = nox.monsterInfo(o.type)
                if rank == 0 and def and def.questHealth > 0 then
                    base = def.questHealth
                    times = f32(f32(1 + f32(QUEST_GROWTH * minute)) + f32(f32(QUEST_GROWTH_SQUARED * minute) * minute))
                end
                times = f32(times * f32(1 + f32(PARTY_GROWTH * math.max(0, now.party - 1))))
                local health = rules.hardness(now.difficulty, now.stage)
                times = f32(times * health)
                most = floor(math.clamp(f32(base * times), 1, 65535))
                nox.setHealth(id, most, most)
            end
            owner.addFoe(id, worth, now.frame + 20 + id % 16, most, rank)
            return id
        end
    end
    return nil
end

-- A creature made an elite: stock enchantments for good (Hasted, Vampirism, a reflective shield,
-- as many as `traits`) and `healthTimes` its health.
local TRAITS = { "ENCHANT_HASTED", "ENCHANT_VAMPIRISM", "ENCHANT_REFLECTIVE_SHIELD" }
local function makeElite(id: number, traits: number, healthTimes: number)
    for i = 1, math.clamp(traits, 0, 3) do
        nox.enchant(id, TRAITS[i], 0, 3)
    end
    local most = nox.object(id).maxHealth
    if most ~= 0 then
        most = floor(math.min(65535, f32(most * healthTimes)))
        nox.setHealth(id, most, most)
        nox.touch(id)
    end
    owner.setFoeMost(id, nox.object(id).maxHealth)
end

local spawns = {}

-- The horde's rules (the director's native horde steers and reaps the tide each frame): each
-- creature looks every 1.5 s (45 frames, and its id modulo 16 more, so they don't all look on one
-- frame); one left past 1600 from every player goes (the guardian is brought back); an idle one
-- farther than 60 from the nearest player is sent to walk at it.
function spawns.setUp()
    owner.horde(45, 16, 1600, 60)
end

function spawns.setOptions(perView: number, maxCreatures: number, holdPerPlayer: number, dawnWins: boolean, graceSeconds: number)
    options = { perView = perView, maxCreatures = maxCreatures, holdPerPlayer = holdPerPlayer, dawnWins = dawnWins, graceSeconds = graceSeconds }
end

-- A stage's arena begun at `start`: the first spawn after the quiet (returned), the first hard wave
-- at the second minute (at the first when the difficulty's elites come then: they come with the
-- waves), the specials from the third minute.
function spawns.beginStage(start: number, difficulty: number, stage: number): number
    local _, _, eliteFrom = rules.hardness(difficulty, stage)
    nextWave = start + math.min(120, 60 * math.max(1, eliteFrom)) * FPS
    nextSpecial = start + 180 * FPS
    return start + options.graceSeconds * FPS
end

-- The clock's turn: creatures about each player in play (`units`, the director's order), the
-- specials, the waves. Returns the frame the next is due.
function spawns.spawn(frame: number, seconds: number, difficulty: number, stage: number, party: number, biome: string, units: { number }): number
    local now: Now = { frame = frame, seconds = seconds, difficulty = difficulty, stage = stage, party = party }
    -- (A slower trickle the first minute; quicker from the eighth.)
    local next = frame + (if seconds < 60 then 3 * FPS elseif seconds < 480 then 2 * FPS else 45)
    local kinds = creaturesFor(biome)
    local minute = floor(seconds / 60)
    local foes = owner.foeCount()
    local function room()
        return foes < options.maxCreatures
    end
    local function near(t, x, y, worth, rank)
        local id = spawnNear(now, t, x, y, worth, rank or 0)
        if id then
            foes += 1
        end
        return id
    end
    local _, more, eliteFrom, moreTraits = rules.hardness(difficulty, stage)
    local at = {}
    for i, u in units do
        local o = nox.object(u)
        at[i] = { x = o.x, y = o.y }
    end
    for i, u in units do
        local x, y = at[i].x, at[i].y
        -- Those about it now (the grid's): live creatures within its view and the ring.
        local about = nox.countNear(x, y, 560, { monsters = true, notDead = true, notGone = true })
        local want
        if options.holdPerPlayer > 0 then
            want = math.min(40, options.holdPerPlayer - about)
        else
            -- A trickle at first (one every 2 s, a dozen about at most), building each minute; the
            -- difficulty's more.
            local cap = math.min(options.perView, lround(f32((8 + 10 * minute) * more)))
            want = math.min(lround(f32((1 + (minute * 2) // 3) * more)), cap - about, lround(f32(10 * more)))
        end
        -- The middling joining it from the first minute, the hard sprinkled in from the sixth (a
        -- little more each minute, a quarter at most).
        local midShare = math.min(55, minute * 12)
        local hardShare = if minute >= 6 then math.min(25, (minute - 5) * 3) else 0
        local n = 0
        while n < want and room() do
            local d = owner.roll() % 100
            if d < hardShare then
                near(pickOf(kinds.hard), x, y, 10)
            else
                local mid = d < hardShare + midShare
                near(pickOf(if mid then kinds.mid else kinds.low), x, y, if mid then 3 else 1)
            end
            n += 1
        end
    end
    -- The specials: a small group of one kind about each player every 40 s from the third minute,
    -- more as the night goes on (and the harder).
    if #kinds.special > 0 and nextSpecial <= frame and options.holdPerPlayer == 0 and minute >= 3 then
        nextSpecial = frame + 40 * FPS
        local kind = pickOf(kinds.special)
        local count = math.max(1, lround(f32(math.min(4, 1 + minute // 5) * more)))
        for i in units do
            local n = 0
            while n < count and room() do
                near(kind, at[i].x, at[i].y, 15)
                n += 1
            end
        end
        nox.textToAll(fill(if count > 1 then data.text.specialMany else data.text.specialOne, { creature = spacedName(kind) }))
    end
    -- The waves: each minute the harder ones, more of them as it goes on (the middling ones till
    -- the fifth minute, then the hard ones).
    if nextWave <= frame and options.holdPerPlayer == 0 then
        nextWave = frame + 60 * FPS
        local hard = pickOf(if minute < 5 then kinds.mid else kinds.hard)
        local name = spacedName(hard)
        nox.textToAll(fill(data.text.wave, { creature = name }))
        for i in units do
            local count = lround(f32(math.min(5, 1 + minute // 2) * more))
            local n = 0
            while n < count and room() do
                near(hard, at[i].x, at[i].y, 10)
                n += 1
            end
            -- From the third minute (sooner the harder) an elite with each wave, one a player:
            -- more traits as it goes on (and the harder), three times the health and more from the
            -- eighth minute (five times by the twentieth); it drops a chest (a free upgrade).
            if minute >= eliteFrom then
                local e = near(hard, at[i].x, at[i].y, 25, 1)
                if e then
                    local times = f32(3 + f32(math.clamp(f32(minute - 8), 0, 12) / 6))
                    makeElite(e, math.min(3, 1 + minute // 6 + moreTraits), times)
                end
            end
        end
        if minute >= eliteFrom then
            nox.textToAll(fill(data.text.elite, { creature = name }))
        end
    end
    return next
end

-- The night's guardian by biome: the biome's worst.
local function guardianFor(biome: string): string
    return data.guardians[biome] or data.otherGuardian
end

local GUARDIAN_PARTY = float(3)

-- The night's guardian comes, near the first player in play (`near`; none: none comes): its
-- health (the minutes' already in) six times, three more a player in play past the first; swift
-- and vampiric (no reflective shield: the casters' missiles would turn on them). Returns its id
-- (nil when it found no spot) and its name.
function spawns.spawnBoss(frame: number, seconds: number, difficulty: number, stage: number, party: number, biome: string, near: number?): (number?, string)
    local t = guardianFor(biome)
    local name = spacedName(t)
    if not near then
        return nil, name
    end
    local now: Now = { frame = frame, seconds = seconds, difficulty = difficulty, stage = stage, party = party }
    local o = nox.object(near)
    local b = spawnNear(now, t, o.x, o.y, 60, 2)
    if b then
        makeElite(b, 2, f32(6 + f32(GUARDIAN_PARTY * math.max(0, party - 1))))
        nox.textToAll(fill(if options.dawnWins then data.text.guardianComesDawn else data.text.guardianComes, { guardian = name }))
    end
    return b, name
end

return spawns
