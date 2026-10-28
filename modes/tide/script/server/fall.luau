-- Undead Tide's slain and its guardian left far behind: the director's horde reaps the tide each
-- frame and asks fell as a creature of it falls, comeBack as the guardian is found past the cull
-- distance from every player.
local data = require("data")
local util = require("util")

local f32 = nox.f32

local fall = {}

-- A soul gem's look by its worth: the bigger as it grows.
local function gemType(worth: number): string
    return if worth >= 10 then "RedOrb" elseif worth >= 3 then "GreenOrb" else "BlueOrb"
end

-- A creature of the tide slain (`worth` its gems', `rank` 0 the tide's, 1 an elite, 2 the guardian;
-- `minute` the stage's): its slayer's runner credited, a soul gem dropped (or merged into one
-- within 80, its worth added: fewer lights on the screen), an elite's chest, the guardian's fall
-- noted, now and then a pickup or a piece of armour; the body kept a while.
function fall.fell(id: number, worth: number, rank: number, minute: number)
    local by = owner.slayer(id)
    if by then
        owner.addKill(by)
    end
    local x, y = nox.position(id)
    local at, gem, had, look = owner.gemNear(x, y, 80)
    if at then
        local merged = math.min(255, had + worth)
        if gemType(merged) ~= look then
            local gx, gy = nox.position(gem)
            nox.delete(gem)
            local g = nox.create(gemType(merged), gx, gy)
            if g then
                nox.setNoCollide(g, true)
                gem = g
            end
        end
        owner.setGem(at, gem, merged)
    else
        local g = nox.create(gemType(worth), x, y)
        if g then
            nox.setNoCollide(g, true)
            owner.addGem(g, worth)
        end
    end
    -- An elite's chest (a stone one: a touch opens it, quest or not): a free upgrade. The
    -- guardian slain: the stage won at the frame's end.
    if rank == 1 then
        local chests = data.chests
        owner.eliteSlain(nox.create(chests[owner.roll() % #chests + 1], x, f32(y + 16)))
    elseif rank == 2 then
        owner.guardianSlain()
    end
    -- A pickup now and then: the guardian's always; by worth, 25 or more (an elite) 40%, 10 or more
    -- 5%, 3 or more 1.5%, else 0.5% (in thousandths); its kind by the table's weights.
    local chance = if rank == 2 then 1000 elseif worth >= 25 then 400 elseif worth >= 10 then 50 elseif worth >= 3 then 15 else 5
    if owner.roll() % 1000 < chance then
        local total = 0
        for _, p in data.pickups do
            total += p.weight
        end
        local pick = owner.roll() % total
        local kind = 1
        while kind < #data.pickups and pick >= data.pickups[kind].weight do
            pick -= data.pickups[kind].weight
            kind += 1
        end
        owner.dropPickup(kind - 1, f32(x + 10), f32(y + 10))
    end
    -- Armour: worth 10 or more 12.5% (25-149 of 1000), the rest 1% (2-11): from the quest's tables
    -- (its kind 0x10), better as the stage goes on, only what a class in the run may wear (drawn
    -- again a few times, else none); one in three charmed (warding or repelling). (The reward's
    -- stage held at 1000, nox.rewardAt's most: every stage from 10 gives armour as 1000 does, its
    -- slot 0x10 and its enchantments' slot 4, so past a stage's 999th minute it drops as before.)
    local r = owner.roll() % 1000
    if r >= (if worth >= 10 then 25 else 2) and r < (if worth >= 10 then 150 else 12) then
        local item
        local tries = 0
        local stage = math.min(1 + minute, 1000)
        while tries < 6 and not item do
            item = nox.rewardAt(f32(x - 12), y, stage, 0x10)
            if item and not owner.wearable(item) then
                nox.delete(item)
                item = nil
            end
            tries += 1
        end
        if item and owner.roll() % 3 == 0 then
            local warding = owner.roll() % 2 == 0
            owner.charm(item, warding)
            local said = if warding then data.text.wardingDrop else data.text.repulsionDrop
            nox.textToAll(util.fill(said, { item = util.spacedName(nox.object(item).type) }))
        end
    end
    owner.addBody(id)
end

-- The guardian left far behind: brought back just out of the nearest player's sight (450 from it,
-- on open ground; eight tries).
function fall.comeBack(id: number, nearest: number)
    local nx, ny = nox.position(nearest)
    for _ = 1, 8 do
        local a = f32((owner.roll() % 3600) * util.DEGREE_TENTH)
        local x = f32(nx + f32(nox.cosf(a) * 450))
        local y = f32(ny + f32(nox.sinf(a) * 450))
        if util.openGround(x, y) then
            nox.setPosition(id, x, y)
            return
        end
    end
end

return fall
