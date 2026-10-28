-- Undead Tide's maps as the generator builds them (modes/common's mapgen): a stage's arena (open
-- ground in a biome, its scenery for cover, nothing on it: the tide brings the creatures) and the
-- lobby (a chat map, the rules' signs, the records board, the portal). The director asks these as
-- it writes each, with no host (any thread).
local data = require("data")

local params = {}

local U32 = 4294967296

-- A stage's arena: by its seed (mixed by a multiplicative hash: neighbouring seeds give unlike
-- arenas), in `biome` ("": one drawn by the biomes' weights, not the shop, nor the lava's: its floor
-- burns), its corner distance `radius` in wall cells (even, 10 to 110).
function params.arena(seed: number, biome: string, radius: number)
    local p = {
        title = data.text.arenaTitle,
        author = data.text.mapAuthor,
        gameFlags = 2, -- (a quest map: the quest's own list holds it)
        minPlayers = 1,
        maxPlayers = 6,
        centerX = 128,
        centerY = 128,
        radius = math.clamp(radius - radius % 2, 10, 110),
        seed = (nox.mul32(seed, 2654435761) + 17) % U32,
        playerStarts = 6,
        items = {},
        questExit = false,
    }
    if biome == "" then
        local places, total = {}, 0
        for _, b in owner.biomes() do
            if b.name ~= "Shop" and b.name ~= "Lava" then
                table.insert(places, b)
                total += math.max(1, b.weight)
            end
        end
        -- (A weighted draw: the seed picks a point in the weights' total; the biome whose share holds it.)
        local pick = ((nox.mul32(seed, 2246822519) + 3) % U32) % total
        for _, b in places do
            pick -= math.max(1, b.weight)
            if pick < 0 then
                biome = b.name
                break
            end
        end
    end
    p.biome = biome
    return p
end

-- The lobby: a chat map (a quest may begin from it), a sign for each rule (west to east), the
-- records' board in the middle (the game's largest sign, a shop's notice board), the portal.
function params.lobby()
    local signs = {}
    for i = 1, #data.rules do
        table.insert(signs, if i % 2 == 1 then "Sign1" else "Sign2")
    end
    return {
        title = data.text.lobbyTitle,
        author = data.text.mapAuthor,
        gameFlags = 0x80000000,
        minPlayers = 1,
        maxPlayers = 16,
        radius = 14,
        floorName = "CobblestoneGrayDark",
        wallSet = "DungeonStone",
        ambient = 150,
        playerStarts = 8,
        items = {},
        questExit = true, -- (the portal: inert outside a quest, watched by the director)
        signs = signs,
        centre = { "SignShop01" },
        seed = 3,
    }
end

return params
