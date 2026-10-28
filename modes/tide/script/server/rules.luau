-- Undead Tide's small rules: the XP curve, the weapons' waits, the difficulty in a stage.
--
-- Float figures as the server makes them: each step through nox.f32 (the server's float), as the
-- C++ stored a float at each step. Float constants as a C++ float literal is (float() below:
-- rounded to nearest once, whatever the server's rounding), worked out as the module loads. No
-- `^` and no sums of constants (the compiler would fold them its own way).
local data = require("data")

local f32 = nox.f32

-- The float a C++ float literal of `x` is: to nearest, a tie to the even one. Exact steps only
-- (frexp and ldexp scale by powers of two; the 24 bits' integer and its rest are exact), so the
-- server's rounding can't reach it. For the normal floats the tables hold.
local function float(x: number): number
    if x == 0 then
        return x
    end
    local sign = 1
    if x < 0 then
        sign = -1
        x = -x
    end
    local m, e = math.frexp(x) -- (x = m * 2 to the e, m in [0.5, 1))
    local scaled = m * 16777216 -- (2 to the 24: the float's 24 bits above the point)
    local kept = math.floor(scaled)
    local rest = scaled - kept
    if rest > 0.5 or (rest == 0.5 and kept % 2 == 1) then
        kept += 1
    end
    return sign * math.ldexp(kept, e - 24)
end

local rules = {}

rules.float = float

-- The XP from `level` to the next: cheap early, steeper on.
function rules.xpFor(level: number): number
    if level < 20 then
        return 5 + 10 * (level - 1)
    end
    return 195 + 13 * (level - 20)
end

-- A weapon's wait between shots in frames: its table's, less its waitPerLevel a level past the
-- first, 8% less a Haste level, never under 6 (whole frames, towards zero).
local WAIT = {}
local WAIT_PER_LEVEL = {}
for i, w in data.weapons do
    WAIT[i] = float(w.wait)
    WAIT_PER_LEVEL[i] = float(w.waitPerLevel)
end
local HASTE_STEP = float(0.08)
local SHORTEST = 6

function rules.cooldown(kind: number, level: number, haste: number): number
    local i = math.min(kind, #WAIT - 1) + 1
    local frames = f32(WAIT[i] - f32(WAIT_PER_LEVEL[i] * (level - 1)))
    frames = f32(frames * f32(1 - f32(HASTE_STEP * haste)))
    return math.floor(math.max(SHORTEST, frames))
end

-- A difficulty's numbers in a stage (2026-10-12: each guardian slain, the next land harder):
-- the tide 60% tougher and a quarter more of it each stage past the first, the elites from the
-- first minute with a trait more. Returns the tide's health and numbers times these, the minute
-- elites begin, the traits they have more.
local HEALTH = {}
local COUNT = {}
for i, d in data.difficulties do
    HEALTH[i] = float(d.health)
    COUNT[i] = float(d.count)
end
local STAGE_HEALTH = float(0.6)
local STAGE_COUNT = float(0.25)

function rules.hardness(difficulty: number, stage: number): (number, number, number, number)
    local i = math.min(difficulty, #HEALTH - 1) + 1
    local d = data.difficulties[i]
    local past = math.max(0, stage - 1)
    local health = f32(HEALTH[i] * f32(1 + f32(STAGE_HEALTH * past)))
    local count = f32(COUNT[i] * f32(1 + f32(STAGE_COUNT * past)))
    local eliteFrom = d.eliteFrom
    if stage > 1 then
        eliteFrom = 1
    end
    return health, count, eliteFrom, d.moreTraits + past
end

return rules
