-- Delving Mode's rules that have moved into script: what the difficulty, the depth and the act
-- make of a level's numbers (its monsters' count and health, its elites, a boss's traits and
-- health, a champion guardian's), the gold it pays, the blessings' raises and what comes back
-- each beat. The director calls each where its C++ figured it.
--
-- Float figures as the C++ made them: on the server's frame, each step through nox.f32 (the
-- server's float); while a level's built (its own rounding to nearest, on any thread), through
-- nearest() below. Float constants as a C++ float literal is (nearest(): rounded once, in exact
-- steps), worked out as the module loads. No `^`, no sums of constants (the compiler would fold
-- them its own way).
local data = require("data")

local f32 = nox.f32

-- The float nearest `x` (a tie to the even one), as a C++ float literal of it is, or as a float
-- operation rounding to nearest makes an exact result. Exact steps only (frexp and ldexp scale by
-- powers of two; the 24 bits' integer and its rest are exact), so no thread's rounding reaches
-- it. For the normal floats the figures here make.
local function nearest(x: number): number
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

-- A whole number rounded as C's lround: to nearest, a half away from zero (math.round's).
local round = math.round

local HEALTH = {}
local COUNT = {}
for i, d in data.difficulties do
    HEALTH[i] = nearest(d.health)
    COUNT[i] = nearest(d.count)
end
-- A difficulty by its number (from 0; one past the last its last).
local function hard(difficulty: number): number
    return math.min(difficulty, #HEALTH - 1) + 1
end

local rules = {}

-- The difficulty's more monsters: a level of `had` holds this many (more of its own kinds; at
-- most 120). Figured as the level's built (to nearest).
function rules.monsters(difficulty: number, had: number): number
    local count = COUNT[hard(difficulty)]
    if had == 0 or not (count > 1) then
        return had
    end
    return math.min(120, round(nearest(had * count)))
end

-- Whether the difficulty makes the monsters tougher, and a monster's health so made (at most
-- 0xFFFF). On the server's frame.
function rules.toughens(difficulty: number): boolean
    return HEALTH[hard(difficulty)] > 1
end
function rules.health(difficulty: number, maxHealth: number): number
    return math.min(65535, round(f32(maxHealth * HEALTH[hard(difficulty)])))
end

-- A fight's elites at `depth`: whether it has any (a fight from the difficulty's depth on), how
-- many at the most (one, another every few levels deeper, at most a few), and of how many traits.
-- (fight: 1 a fight's level, 0 any other.)
function rules.elites(difficulty: number, depth: number, fight: number): (boolean, number, number)
    local d = data.difficulties[hard(difficulty)]
    if fight == 0 or depth < d.eliteFrom then
        return false, 0, 0
    end
    return true, math.min(d.eliteCap, (depth - d.eliteFrom) // d.eliteEvery + 1), d.eliteTraits
end

-- A champion's or guardian's traits: an elite room's champion one (two from act 3), a guardian
-- one in act 2 (two from act 3), a lone champion one; the harder difficulties one more each.
-- (room: DelveRoom's number.)
local ELITE_ROOM = 1
local GUARDIAN_ROOM = 5
function rules.bossTraits(room: number, act: number, difficulty: number): number
    local traits = 1
    if room == ELITE_ROOM then
        traits = if act >= 3 then 2 else 1
    elseif room == GUARDIAN_ROOM then
        traits = math.min(2, act - 1)
    end
    return traits + data.difficulties[hard(difficulty)].eliteTraits - 1
end

-- A boss's health: its own `times` over (at most 0xFFFF).
function rules.bossHealth(maxHealth: number, times: number): number
    return math.min(65535, maxHealth * times)
end

-- Tough: half again its health (at most 0xFFFF).
function rules.tough(maxHealth: number): number
    return math.min(65535, maxHealth * 3 // 2)
end

-- What comes back each beat: Regenerating's a fiftieth of its health a second, Renewal's a
-- fortieth of each every two seconds (one at the least).
function rules.regenerate(maxHealth: number): number
    return math.max(1, maxHealth // 50)
end
function rules.renew(most: number): number
    return math.max(1, most // 40)
end

-- A blessing raising a number by a quarter `count` times (Vigor, Wisdom, Might), at most `most`
-- (the number's largest).
function rules.raise(base: number, count: number, most: number): number
    return math.min(most, base * (4 + count) // 4)
end

-- The gold paid: Swift done (its level's depth), Fortune at a level's start.
function rules.swiftGold(depth: number): number
    return 20 + 10 * depth
end
function rules.fortuneGold(depth: number): number
    return 10 + 5 * depth
end

-- A champion guardian (a bot boss) in act `act`, of `maxHealth` and `maxMana` as it joined: its
-- health one and the act's number times, a caster's mana three times (each at most 0xFFFF); its
-- spells' level (2, 3 from act 3); whether it has its class's late spell (data's
-- championLateSpell, the Death Ray: act 3 on); whether it plays hard (act 3 on, or the
-- difficulty's hard bots).
function rules.champion(act: number, difficulty: number, maxHealth: number, maxMana: number): (number, number, number, boolean, boolean)
    local late = act >= 3
    return math.min(65535, maxHealth * (1 + act)), math.min(65535, maxMana * 3), if late then 3 else 2, late,
        late or data.difficulties[hard(difficulty)].hardBots
end

-- A run's record as it's filed (it went as deep as `depth`, on `difficulty`): the act it reached
-- (six levels each), the difficulty's word (one past the last: the first's).
function rules.record(depth: number, difficulty: number): (number, string)
    local d = data.difficulties[difficulty + 1] or data.difficulties[1]
    return (depth - 1) // 6 + 1, d.name
end

return rules
