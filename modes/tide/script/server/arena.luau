-- Undead Tide's arena as it goes on: the passives' health, the charmed armour's turns, the slain's
-- bodies. The director's frame keeps the clocks (every half second, each charm's next turn, each
-- body's time) and asks these as each comes due.
local util = require("util")

local f32 = nox.f32
local float = util.float

local FPS = 30 -- (frames a second)

local arena = {}

-- Vitality and Recovery (a runner's levels of them) for a unit of `base` most health before
-- Vitality: its most health (15% more a Vitality level, at most 65535) and the health it gets back
-- every half second (a point a Recovery level).
local VITALITY = float(0.15)
function arena.passiveHealth(base: number, vitality: number, recovery: number): (number, number)
    local most = math.min(65535, f32(base * f32(1 + f32(VITALITY * vitality))))
    return math.floor(most), recovery
end

-- A charmed piece of armour's turn, worn by a player in play (`warding` 1, else repelling): Warding
-- casts Shield on its wearer when it has none (the stock spell at power 3; again 20 s on),
-- Repulsion throws the tide within 140 off it (a force, as a push, its cyan sparks seen; again 8 s
-- on). Returns the frame of its next turn.
local NEAR = { monsters = true, notDead = true, notGone = true, enemiesOf = 0 }
function arena.charm(item: number, wearer: number, warding: number, frame: number): number
    local x, y = nox.position(wearer)
    if warding ~= 0 then
        if not nox.enchanted(wearer, "ENCHANT_SHIELD") then
            nox.cast("SPELL_SHIELD", wearer, wearer, x, y, 3)
        end
        return frame + 20 * FPS
    end
    NEAR.enemiesOf = wearer
    local pushed = false
    for _, o in nox.unitsNear(x, y, 140, NEAR) do
        nox.applyForce(o, x, y, 60)
        pushed = true
    end
    if pushed then
        nox.effect(0x83, x, y)
    end
    return frame + 8 * FPS
end

-- A slain creature's body, its time up: a zombie that got up joins the tide again (worth a gem of
-- 1, steered at once, its most health its own), any other body cleared.
function arena.bodyDue(id: number, monster: number, alive: number, most: number, frame: number)
    if monster ~= 0 and alive ~= 0 then
        owner.addFoe(id, 1, frame, most, 0)
    else
        nox.delete(id)
    end
end

-- A run over, its results read: what each of the party carries goes with it (all but what's kept
-- on death), as they go back to the lobby.
function arena.runOver(units: { number })
    for _, u in units do
        for _, item in nox.inventory(u) do
            if not owner.keptOnDeath(item.id) then
                nox.delete(item.id)
            end
        end
    end
end

return arena
