-- Delving Mode's acts: six levels in one biome, its rooms in rows that branch, drawn from a seed
-- (each act's own std::mt19937, as the C++ drew them), and a guardian's fork of two next acts.
--
-- An act's rows are lists of rooms by their numbers (DelveRoom): row 1 a fight; rows 2-4 fights,
-- mysteries, elites, a shop or so and a blessing or two (no elite after an elite, two side by side
-- never the same); row 5 campfires and shops (a mystery now and then); row 6 the guardian. Any way
-- through it meets one shop at the most, one blessing at the most (every act one at the least),
-- and one campfire.
local FIGHT, ELITE, MYSTERY, SHOP, CAMPFIRE, GUARDIAN, BLESSING = 0, 1, 2, 3, 4, 5, 6
local LENGTH = 6
-- The rows a missing blessing goes in, the act's middle first (from 0).
local MIDDLE_FIRST = { 2, 3, 1 }

local mul32 = nox.mul32

-- A whole number as the u32 it wraps to.
local function u32(x: number): number
    return x % 4294967296
end

-- The biomes by their names and weights (the map generator's, in its order), asked once.
local biomes = nil
local function biomeList()
    if biomes == nil then
        biomes = owner.biomes()
    end
    return biomes
end

-- A row's rooms: row + 1 of them (from 0), the guardian's one.
local function width(row: number): number
    if row >= LENGTH - 1 then
        return 1
    end
    return row + 1
end

local acts = {}
acts.width = width

-- A biome by its weight (rare ones less often), none of `notBiomes` (a set by name), never the
-- shop: a weighted draw, walking the places until the roll is used up.
local function drawBiome(rng, notBiomes): string
    local all = biomeList()
    local total = 0
    local last = nil
    for _, b in all do
        if b.name ~= "Shop" and not notBiomes[b.name] then
            total += math.max(1, b.weight)
            last = b.name
        end
    end
    local roll = rng:next() % total
    for _, b in all do
        if b.name ~= "Shop" and not notBiomes[b.name] then
            roll -= math.max(1, b.weight)
            if roll < 0 then
                return b.name
            end
        end
    end
    return last
end

-- Act `number` drawn from `seed`, its biome none of `notBiomes` (a set by name): its biome and
-- its rows (from 1, each a list of rooms).
function acts.roll(number: number, seed: number, notBiomes): (string, { { number } })
    local rng = nox.mt19937(u32(mul32(seed, 0x9E3779B1) + mul32(u32(number), 7919) + 3))
    local biome = drawBiome(rng, notBiomes)
    local rows = { { FIGHT }, {}, {}, {}, {}, { GUARDIAN } }
    -- Rows 2-4 by weight: fight 4, mystery 3, elite 2, shop 1, blessing 1. A room never the same
    -- as the one beside it; no elite where an elite leads (room i of a row comes from rooms i - 1
    -- and i of the one before); two elites an act, three from act 3; two shops and two blessings,
    -- never two of either on one way through (shops[r][i]: the most shops on a way to room i of
    -- row r, it with them; blessings[r][i] the same). (Rows and rooms from 0 below, as drawn.)
    local elites = if number >= 3 then 3 else 2
    local eliteCount, shopCount, blessingCount = 0, 0, 0
    local shops = { [0] = { [0] = 0 } }
    local blessings = { [0] = { [0] = 0 } }
    local function mostBefore(most, r: number, i: number): number
        local before = most[r - 1]
        local a, b = before[i - 1], before[i]
        local m = 0
        if a ~= nil and a > m then
            m = a
        end
        if b ~= nil and b > m then
            m = b
        end
        return m
    end
    -- Whether room `k` may go next in `row` (its rows' counts so far, what leads into it).
    local function fits(row, k: number, afterElite: boolean, shopsIn: number, blessingsIn: number): boolean
        return (#row == 0 or row[#row] ~= k) and (k ~= ELITE or (not afterElite and eliteCount < elites))
            and (k ~= SHOP or (shopCount < 2 and shopsIn == 0)) and (k ~= BLESSING or (blessingCount < 2 and blessingsIn == 0))
    end
    for r = 1, 3 do
        local row = rows[r + 1]
        local before = rows[r]
        shops[r] = {}
        blessings[r] = {}
        for i = 0, width(r) - 1 do
            local afterElite = (i > 0 and before[i] == ELITE) or (i < #before and before[i + 1] == ELITE)
            local shopsIn = mostBefore(shops, r, i)
            local blessingsIn = mostBefore(blessings, r, i)

            local k = FIGHT
            local found = false
            -- Up to 50 draws for a room that fits; none fits: a fight, else a mystery.
            local tries = 0
            while tries < 50 and not found do
                local w = rng:next() % 11
                k = if w < 4 then FIGHT elseif w < 7 then MYSTERY elseif w < 9 then ELITE elseif w < 10 then SHOP else BLESSING
                found = fits(row, k, afterElite, shopsIn, blessingsIn)
                tries += 1
            end
            if not found then
                k = if fits(row, FIGHT, afterElite, shopsIn, blessingsIn) then FIGHT else MYSTERY
            end
            if k == ELITE then
                eliteCount += 1
            elseif k == SHOP then
                shopCount += 1
            elseif k == BLESSING then
                blessingCount += 1
            end
            table.insert(row, k)
            shops[r][i] = shopsIn + (if k == SHOP then 1 else 0)
            blessings[r][i] = blessingsIn + (if k == BLESSING then 1 else 0)
        end
    end
    -- Every act a blessing at the least: none drawn, a fight or mystery made one, row 3's (the
    -- act's middle) else row 4's or 2's.
    for _, r in MIDDLE_FIRST do
        local row = rows[r + 1]
        local can = {}
        if blessingCount == 0 then
            for i, k in row do
                if k == FIGHT or k == MYSTERY then
                    table.insert(can, i)
                end
            end
        end
        if #can > 0 then
            row[can[rng:next() % #can + 1]] = BLESSING
            blessingCount = 1
        end
    end
    -- Row 5, before the guardian: every other room a campfire (so every way has one to rest at),
    -- the rest shops (but where a way in has had one: a mystery) and now and then a mystery.
    local fires = rng:next() % 2
    for i = 0, width(4) - 1 do
        local k = CAMPFIRE
        if i % 2 ~= fires then
            k = if rng:next() % 3 == 0 or mostBefore(shops, 4, i) ~= 0 then MYSTERY else SHOP
        end
        table.insert(rows[5], k)
    end
    return biome, rows
end

local function sameRows(a, b): boolean
    for r = 1, LENGTH do
        if #a[r] ~= #b[r] then
            return false
        end
        for i, k in a[r] do
            if b[r][i] ~= k then
                return false
            end
        end
    end
    return true
end

-- A guardian's fork: two next acts (number `number`), other biomes than `notBiome` and each
-- other's, their rooms not the same (the right drawn again with other seeds while they match).
function acts.fork(number: number, seed: number, notBiome: string): (string, { { number } }, string, { { number } })
    local leftBiome, left = acts.roll(number, seed, { [notBiome] = true })
    local notRight = { [notBiome] = true, [leftBiome] = true }
    local rightBiome, right = acts.roll(number, u32(seed + 1), notRight)
    local k = 2
    while k < 20 and sameRows(left, right) do
        rightBiome, right = acts.roll(number, u32(seed + k), notRight)
        k += 1
    end
    return leftBiome, left, rightBiome, right
end

return acts
