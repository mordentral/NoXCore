-- Delving Mode's levels: what each level of a run holds, drawn from its seed (its own
-- std::mt19937s, as the C++ drew them): a fight's length, monsters and potions; the room the act's
-- way reaches (an elite's champion, a shop, a campfire, an altar, a mystery's find, the guardian
-- and its fork); the gates on; traps, puzzles and vaults; a fight's bonus objective; obelisks,
-- chests and gold. The map generator builds it (its params, below as a table of the same names).
local data = require("data")

local FIGHT, ELITE, MYSTERY, SHOP, CAMPFIRE, GUARDIAN, BLESSING = 0, 1, 2, 3, 4, 5, 6
local STASH, PURSE, SHRINE, OBELISK, SKIRMISH, FIRE, CHAMPION = 0, 1, 2, 3, 4, 5, 6
local NONE, PURGE, HUNT, SEALS, SWIFT = 0, 1, 2, 3, 4
local LENGTH = 6
local SEAL = "OrbPoolWithOrb"

local mul32 = nox.mul32

local function u32(x: number): number
    return x % 4294967296
end

-- The biomes (the map generator's, in its order), asked once; by name.
local biomes = nil
local byName = nil
local function biomeList()
    if biomes == nil then
        biomes = owner.biomes()
        byName = {}
        for _, b in biomes do
            byName[b.name] = b
        end
    end
    return biomes
end
local function findBiome(name: string)
    biomeList()
    return byName[name]
end

-- A biome by its weight (none of `notBiomes`, never the shop): acts.luau's draw.
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

-- The stock creatures by tier (1-5, clamped), and the depth's tier: 1-2, 3-5, 6-9, 10-14, then 15 on.
local function tier(t: number)
    return data.tiers[math.clamp(t, 1, 5)]
end
local function tierOf(depth: number): number
    if depth <= 2 then
        return 1
    elseif depth <= 5 then
        return 2
    elseif depth <= 9 then
        return 3
    elseif depth <= 14 then
        return 4
    end
    return 5
end

-- Each act's guardians (the last list for every act past them).
local function guardians(act: number)
    return data.guardians[math.clamp(act, 1, #data.guardians)]
end

local NO_BIOMES = {}

local levels = {}
levels.tierOf = tierOf

-- A fight at `depth` (from 1): a longer level and more, tougher monsters as it deepens, in a
-- biome drawn by weight. Its params (the title the director's).
function levels.base(depth: number, seed: number)
    depth = math.max(depth, 1)
    local rng = nox.mt19937(u32(mul32(seed, 2654435761) + depth))
    local p = {}
    p.gameFlags = 2 -- (a quest map: the quest's own list holds it)
    p.questExit = true
    p.seed = rng:next()
    -- A long winding level, longer as it deepens: 9 segments, one more a level, at most 22.
    p.segments = math.min(8 + depth, 22)
    p.radius = 16
    p.biome = drawBiome(rng, NO_BIOMES)
    -- Composition over health: more monsters as it deepens, mostly the depth's tier.
    local count = math.min(8 + 2 * depth, 70)
    local t = tierOf(depth)
    local monsters = {}
    for i = 1, count do
        local lower = t > 1 and rng:next() % 3 == 0
        local pool = tier(if lower then t - 1 else t)
        monsters[i] = pool[rng:next() % #pool + 1]
    end
    p.monsters = monsters
    -- A potion for every few monsters; mana for the casters from depth 3.
    local items = {}
    for i = 0, count // 4 do
        items[i + 1] = if depth >= 3 and i % 2 == 1 then "BluePotion" else "RedPotion"
    end
    p.items = items
    return p
end

-- A level's bonus objective made `objective`: Seals' three seals first among the things placed,
-- Swift's time (seconds).
function levels.setObjective(level, objective: number)
    local items = {}
    if objective == SEALS then
        items = { SEAL, SEAL, SEAL }
    end
    for _, it in level.params.items do
        if it ~= SEAL then
            table.insert(items, it)
        end
    end
    level.params.items = items
    level.objective = objective
    level.objectiveSeconds = 0
    if objective == SWIFT then
        level.objectiveSeconds = 60 + 10 * level.params.segments
    end
end

-- A guardian's level made a bot boss's of `class` (a duel: no monsters, its spot free).
function levels.setBotBoss(level, class: number)
    level.botBoss = class
    if #level.params.centre > 0 then
        level.params.centre[1] = "RewardMarkerPlus" -- (its spot; the monster stands in only if need be)
    end
    level.params.monsters = {}
end

-- The room at `lane` of `row` (from 0) of an act's rows (lanes past the row's last clamped; an
-- empty row a fight, the guardian's the guardian).
local function roomAt(rows, row: number, lane: number): number
    local r = rows[math.clamp(row, 0, LENGTH - 1) + 1]
    if #r == 0 then
        return if row >= LENGTH - 1 then GUARDIAN else FIGHT
    end
    return r[math.min(math.max(lane, 0), #r - 1) + 1]
end
levels.roomAt = roomAt

-- The level at `depth` in an act (its number, biome, rows and the way taken so far: path, from
-- 0), from `seed`; a guardian's gates to the fork's two biomes (nil: none). The level: its room,
-- mystery (-1: none), boss and its health's times, the boss's rare drop, objective and its
-- seconds, bot boss's class (-1: none), and params.
function levels.level(number: number, biome: string, rows, path, depth: number, seed: number, forkLeft: string?, forkRight: string?)
    depth = math.max(depth, 1)
    local level = { room = FIGHT, mystery = -1, boss = "", bossHealthTimes = 1, bossDrop = 0, objective = NONE, objectiveSeconds = 0, botBoss = -1 }
    local row = (depth - 1) % LENGTH
    local lane = if #path > row then path[row + 1] else 0
    level.room = roomAt(rows, row, lane)
    local p = levels.base(depth, seed) -- (a fight's: its length, monsters and potions)
    level.params = p
    p.biome = biome
    p.centre = {}
    p.gates = {}
    p.chests = {}
    local rng = nox.mt19937(u32(mul32(seed, 2246822519) + mul32(depth, 31) + 9))
    local t = tierOf(depth)
    local function draw(tierWanted: number): string
        local pool = tier(tierWanted)
        return pool[rng:next() % #pool + 1]
    end
    local function room(radius: number) -- (a room of its own in place of a winding level)
        p.segments = 0
        p.radius = radius
        p.centerX = 128
        p.centerY = 128
    end
    local kind = level.room
    if kind == ELITE then
        -- Shorter, half the monsters but a tier up, a champion at the end with its spoils.
        p.segments = math.max(4, p.segments - 3)
        local n = math.max(4, #p.monsters // 2)
        local monsters = {}
        for i = 0, n - 1 do
            monsters[i + 1] = draw(if i % 3 == 0 then t else t + 1)
        end
        p.monsters = monsters
        level.boss = draw(t + 1)
        level.bossHealthTimes = 2
        p.centre = { level.boss, "RewardMarkerPlus", "RewardMarkerPlus" }
    elseif kind == SHOP then
        room(12)
        p.biome = "Shop"
        p.monsters = {}
        p.items = {}
        p.centre = { "Shopkeeper" }
    elseif kind == CAMPFIRE then
        room(12)
        p.monsters = {}
        p.items = {}
        p.centre = { "CampFire" }
    elseif kind == BLESSING then -- (an altar, its candles)
        room(12)
        p.monsters = {}
        p.items = {}
        p.centre = { "DunMirAltar1", "Candleabra1Immobile", "Candleabra1Immobile" }
    elseif kind == MYSTERY then
        -- A room of its own, what it holds rolled: the two rare (a campfire's spell, a lone
        -- champion with a rare drop) one in ten each, the five common, something small, the
        -- rest. A fire only on a way through with no mystery before it: with row 5's campfire,
        -- two fires at the most on any way.
        local mysteryBefore = false
        for r = 0, row - 1 do
            local l = if #path > r then path[r + 1] else 0
            mysteryBefore = mysteryBefore or roomAt(rows, r, l) == MYSTERY
        end
        local roll = rng:next() % 20
        local mystery
        if roll < 2 then
            mystery = if mysteryBefore then STASH else FIRE
        elseif roll < 4 then
            mystery = CHAMPION
        else
            mystery = (roll - 4) % 5 -- (Stash, Purse, Shrine, Obelisk, Skirmish)
        end
        level.mystery = mystery
        room(12)
        p.monsters = {}
        p.items = {}
        if mystery == STASH then -- (a couple of potions left behind)
            p.items = { "RedPotion", if depth >= 3 then "BluePotion" else "RedPotion" }
        elseif mystery == PURSE then -- (a little gold)
            p.items = { "RewardMarker@20" }
        elseif mystery == SHRINE then -- (waters that heal, once each)
            p.centre = { "Fountain" }
        elseif mystery == OBELISK then -- (mana)
            local b = findBiome(biome)
            p.centre = { if b and b.built then "Obelisk" else "ObeliskPrimitive" }
        elseif mystery == SKIRMISH then -- (a few of the tier below, a small find)
            room(14)
            -- (Three or four: each time round, whether there's another is drawn again, before
            -- the creature, as the C++ loop's test drew it.)
            local monsters = {}
            local i = 0
            while i < 3 + rng:next() % 2 do
                monsters[i + 1] = draw(math.max(1, t - 1))
                i += 1
            end
            p.monsters = monsters
            p.items = { "RewardMarker@60" } -- (gold or a potion)
        elseif mystery == FIRE then -- (a campfire's: a spell strengthened)
            p.centre = { "CampFire" }
        else -- CHAMPION (alone, strong, a rare thing on it)
            room(16)
            level.boss = draw(t + 1)
            level.bossHealthTimes = 3
            level.bossDrop = 3
            p.centre = { level.boss }
        end
    elseif kind == GUARDIAN then
        -- A short way to an arena: the guardian, its escorts, its spoils, and the fork's gates
        -- (opened when it dies).
        p.segments = 4
        p.endRadius = 11
        p.questExit = false
        local keep = math.max(3, #p.monsters // 3)
        local monsters = {}
        for i = 1, keep do
            monsters[i] = p.monsters[i] or ""
        end
        p.monsters = monsters
        local pool = guardians(number)
        level.boss = pool[rng:next() % #pool + 1]
        level.bossHealthTimes = math.min(8, 2 + number)
        p.centre = { level.boss, "RewardMarkerPlus", "RewardMarkerPlus", "RewardMarkerPlus" }
        -- From act 2, half the time, a champion of a class played by a bot instead; the monster
        -- guardian is its stand-in, made only if the bot can't come.
        if number >= 2 and rng:next() % 2 == 0 then
            levels.setBotBoss(level, rng:next() % 3)
        end
        if forkLeft ~= nil then
            p.gates = {
                { type = "GauntletExitA", toBiome = forkLeft, sign = "Sign1" },
                { type = "GauntletExitB", toBiome = forkRight, sign = "Sign2" },
            }
        end
    end
    -- Rows 1-4: two gates on, to the next row's rooms at this lane (the left) and the next (the
    -- right); a shop's shows its floor before it.
    if row < LENGTH - 2 and kind ~= GUARDIAN then
        p.questExit = false
        local function to(l: number): string
            return if roomAt(rows, row + 1, l) == SHOP then "Shop" else ""
        end
        p.gates = {
            { type = "GauntletExitA", toBiome = to(lane), sign = "Sign1" },
            { type = "GauntletExitB", toBiome = to(lane + 1), sign = "Sign2" },
        }
    end
    -- Traps and puzzles on a winding level: more and nastier deeper; a vault on most.
    p.depth = depth
    if p.segments > 0 then
        if kind == FIGHT then
            p.traps = math.min(6, 1 + depth // 2)
            p.puzzles = 1
            p.ambushes = if depth >= 2 then 1 else 0
            p.doors = if depth >= 2 and rng:next() % 2 == 0 then 1 else 0
        elseif kind == ELITE then
            p.traps = math.min(6, 2 + depth // 2)
            p.puzzles = 1
            p.ambushes = 1
            p.doors = 1
        elseif kind == GUARDIAN then
            p.traps = 1
        end
    end
    -- A fight's bonus objective, from depth 2 (its own draw).
    if kind == FIGHT and depth >= 2 then
        local orng = nox.mt19937(u32(mul32(seed, 747796405) + mul32(depth, 13) + 5))
        levels.setObjective(level, 1 + orng:next() % 4)
    end
    -- A mana obelisk now and then in a route chamber.
    if p.segments > 0 and kind ~= GUARDIAN then
        p.obelisks = rng:next() % 2
    end
    -- Loot on every level with a fight: chests (filled as the level starts) and gold to spend at
    -- the shops (reward markers of gold or gems alone, always placed).
    if kind ~= SHOP and kind ~= CAMPFIRE and kind ~= MYSTERY and kind ~= BLESSING then
        local b = findBiome(biome)
        local chests = 1 + rng:next() % 2
        for _ = 1, chests do
            if b and #b.chests > 0 then
                table.insert(p.chests, b.chests[rng:next() % #b.chests + 1])
            else
                table.insert(p.chests, "CryptChest" .. tostring(1 + rng:next() % 4))
            end
        end
        table.insert(p.items, "RewardMarker@20")
        table.insert(p.items, "RewardMarker@20")
    end
    return level
end

-- The lobby: a chat map (a quest may begin from it), a sign for each rule (west to east), the
-- records' board in the middle (the game's largest sign, a shop's notice board on a stone base, as
-- Undead Tide's lobby has; a sign's own text is a string table id, so it can't carry the table),
-- the portal (inert outside a quest, watched by the director). Its author the generator's own.
function levels.lobby()
    local signs = {}
    for i = 1, #data.rules do
        table.insert(signs, if i % 2 == 1 then "Sign1" else "Sign2")
    end
    return {
        title = data.text.lobbyTitle,
        gameFlags = 0x80000000,
        minPlayers = 1,
        maxPlayers = 16,
        radius = 14,
        floorName = "OakWoodFloor",
        ambient = 190,
        playerStarts = 8,
        items = {},
        questExit = true,
        signs = signs,
        centre = { "SignShop01" },
        seed = 1,
    }
end

return levels
