-- Delving Mode's draws as a level is played: what its monsters carry, its shopkeepers' names, its
-- barrels' contents and its elites (as it begins), a champion's or guardian's traits, the Hunt's
-- quarry, an altar's three blessings and a champion guardian's name. Each from its own
-- std::mt19937, seeded by the run and the level (the level's serial: each written counts one), as
-- the C++ drew them. The director does what they say in the world.
local data = require("data")
local rules = require("rules")

local mul32 = nox.mul32

local function u32(x: number): number
    return x % 4294967296
end

-- A stream seeded `run` * 2654435761 + `serial` * `times` + `plus` (u32 arithmetic).
local function stream(run: number, serial: number, times: number, plus: number)
    return nox.mt19937(u32(mul32(run, 2654435761) + mul32(serial, times) + plus))
end

-- A barrel's or crate's contents: nothing, low loot, a small creature, a gem.
local NOTHING, LOOT, MONSTER, GEM = 0, 1, 2, 3
-- The elites' traits, by their numbers: swift, vampiric, reflective, tough, regenerating.
local TRAITS = 5

local draws = {}

-- `count` traits of the five, drawn (the five shuffled, the first so many).
local function traits(rng, count: number)
    local all = { 0, 1, 2, 3, 4 }
    nox.shuffle(rng, all)
    local out = {}
    for i = 1, math.min(count, TRAITS) do
        out[i] = all[i]
    end
    return out
end

-- The difficulty's more monsters as a level of `had` is built (its seed, depth and attempt):
-- which of its own (from 1) are copied on, in order, to the rule's count (rules.monsters).
function draws.more(difficulty: number, seed: number, depth: number, attempt: number, had: number)
    local want = rules.monsters(difficulty, had)
    local out = {}
    if want <= had then
        return out
    end
    local rng = nox.mt19937(u32(mul32(seed, 2891336453) + mul32(u32(depth), 7) + attempt))
    for i = 1, want - had do
        out[i] = rng:next() % had + 1
    end
    return out
end

-- The level begun: one stream for its draws, in the director's order (each asked where the
-- world's things are gathered for it).
local level = nil
function draws.beginLevel(run: number, serial: number)
    level = stream(run, serial, 1, 0)
end

-- Of `n` monsters (in the world's order, chests aside), which carry loot: a third of them.
function draws.carriers(n: number)
    local carry = table.create(n)
    for i = 1, n do
        carry[i] = level:next() % 3 == 0
    end
    return carry
end

-- `n` shopkeepers' names.
function draws.keepers(n: number)
    local names = table.create(n)
    local all = data.shopkeepers
    for i = 1, n do
        names[i] = all[level:next() % #all + 1]
    end
    return names
end

-- `n` barrels' and crates' contents: low loot one in three, a small creature one in seven, a gem
-- one in sixty, else nothing.
function draws.barrels(n: number)
    local holds = table.create(n)
    for i = 1, n do
        local roll = level:next() % 420
        holds[i] = if roll < 7 then GEM elseif roll < 67 then MONSTER elseif roll < 207 then LOOT else NOTHING
    end
    return holds
end

-- A fight's elites among `foes` (the difficulty's rule, rules.elites): which of the foes (from
-- 1), each one's traits.
function draws.elites(foes: number, difficulty: number, depth: number, fight: number)
    local elites = {}
    local on, most, count = rules.elites(difficulty, depth, fight)
    if on then
        local order = table.create(foes)
        for i = 1, foes do
            order[i] = i
        end
        nox.shuffle(level, order)
        for i = 1, math.min(most, foes) do
            elites[i] = { foe = order[i], traits = if count > 0 then traits(level, count) else {} }
        end
    end
    return elites
end

-- A champion's or guardian's traits (its level's own stream).
function draws.bossTraits(run: number, serial: number, count: number)
    if count <= 0 then
        return {}
    end
    return traits(stream(run, serial, 31, 7), count)
end

-- The Hunt's quarry of `can` foes (from 1), made an elite of two traits.
function draws.hunt(run: number, serial: number, can: number)
    local rng = stream(run, serial, 131, 3)
    local quarry = rng:next() % can + 1
    return quarry, traits(rng, 2)
end

-- An altar's offer to the player named `name`: of `can` blessings it may have (from 1), three in
-- the order drawn (its own draw: the same three each time it comes up to this altar). Its seed
-- has the name's FNV-1a hash.
function draws.altar(run: number, serial: number, name: string, can: number)
    local h = 2166136261 -- (FNV-1a's offset basis; its prime 16777619)
    for i = 1, #name do
        h = mul32(bit32.bxor(h, string.byte(name, i)), 16777619)
    end
    local rng = nox.mt19937(u32(mul32(run, 2654435761) + mul32(serial, 977) + h))
    local order = {}
    for i = 1, can do
        order[i] = i
    end
    nox.shuffle(rng, order)
    local out = {}
    for i = 1, math.min(3, can) do
        out[i] = order[i]
    end
    return out
end

-- A champion guardian: its name (one of the champions', none of `taken`'s, 32 tries), its 22-digit
-- made-up serial, its brain's seed.
function draws.champion(run: number, serial: number, taken)
    local rng = stream(run, serial, 17, 1)
    local all = data.champions
    local name = ""
    local tries = 0
    while tries < 32 and (name == "" or taken[name]) do
        name = all[rng:next() % #all + 1]
        tries += 1
    end
    local digits = table.create(22)
    for i = 1, 22 do
        digits[i] = string.char(48 + rng:next() % 10)
    end
    return name, table.concat(digits), rng:next()
end

return draws
