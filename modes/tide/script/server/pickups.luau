-- Undead Tide's pickups (2026-10-12): what each does as a player in play touches it. The
-- director's block finds the touch and takes the pickup away, then asks taken: usePickup, then the
-- kind's sound, its flash and its line (the table's: data.luau's pickups).
--
-- Through the engine's own primitives (nox.heal, cast, enchant, damage, unitsNear), and the
-- director's for what is still its own (owner: the runner's weapons, its gems, its XP).
local data = require("data")
local util = require("util")

local FPS = 30 -- (frames a second)

-- The tide about the player: the live enemy creatures within `reach` of it, in the grid's order.
local function tideAbout(unit: number, x: number, y: number, reach: number): { number }
    return nox.unitsNear(x, y, reach, { monsters = true, alive = true, enemiesOf = unit })
end

local effects = {}

function effects.heal(unit, me)
    nox.heal(unit, math.max(1, (me.maxHealth * 2) // 5))
end
function effects.meat(unit, me)
    nox.heal(unit, math.max(1, me.maxHealth // 5))
end
function effects.cure(unit, me)
    nox.cast("SPELL_CURE_POISON", unit, unit, me.x, me.y, 3) -- (on the player itself)
    nox.heal(unit, math.max(1, me.maxHealth // 4))
end
function effects.haste(unit)
    nox.enchant(unit, "ENCHANT_HASTED", 15 * FPS, 3)
end
function effects.shield(unit, me)
    if not nox.enchanted(unit, "ENCHANT_SHIELD") then
        nox.cast("SPELL_SHIELD", unit, unit, me.x, me.y, 3)
    end
end
function effects.vampire(unit)
    nox.enchant(unit, "ENCHANT_VAMPIRISM", 20 * FPS, 3)
end
function effects.invisible(unit)
    -- (Its 8 s not ended by the player's own weapons: their show of the attacker passes it by
    -- till then.)
    nox.enchant(unit, "ENCHANT_INVISIBLE", 8 * FPS, 3)
    owner.hide(unit, 8 * FPS)
end
function effects.invulnerable(unit)
    nox.enchant(unit, "ENCHANT_INVULNERABLE", 6 * FPS, 3)
end
function effects.arcane(unit)
    owner.readyAll(unit) -- (every weapon ready to fire again at once)
end
function effects.magnet(unit)
    owner.drawGems(unit) -- (every gem on the ground: its worth the player's)
end
function effects.bomb(unit, me)
    for _, c in tideAbout(unit, me.x, me.y, 350) do
        nox.damage(c, unit, 150, 0xB)
    end
    nox.effect(0x81, me.x, me.y)
    nox.sound(unit, 0x57) -- (a meteor's fall)
end
function effects.freeze(unit, me)
    for _, c in tideAbout(unit, me.x, me.y, 450) do
        nox.enchant(c, "ENCHANT_FREEZE", 5 * FPS, 3)
    end
end
function effects.fear(unit, me)
    for _, c in tideAbout(unit, me.x, me.y, 400) do
        nox.enchant(c, "ENCHANT_AFRAID", 6 * FPS, 3)
    end
end
function effects.ruby(unit)
    owner.gainXp(unit, 15)
end
function effects.emerald(unit)
    owner.gainXp(unit, 30)
end
function effects.diamond(unit)
    owner.gainXp(unit, 60)
end

-- Each kind's effect, by its place in the table (the director's kind numbers, from 0).
local byKind = {}
for i, p in data.pickups do
    byKind[i] = effects[p.effect] or error("pickup " .. i .. ": no effect " .. tostring(p.effect))
end

local pickups = {}

-- The pickup of `kind` used by the player whose unit is `unit`.
function pickups.usePickup(kind: number, unit: number)
    local use = byKind[kind + 1]
    if use then
        use(unit, nox.object(unit))
    end
end

-- A pickup taken by a player in play (the director's pickups touch it; it's gone): what it does,
-- its spell's sound, a flash where it lay and its line to the player.
function pickups.taken(kind: number, unit: number, x: number, y: number)
    pickups.usePickup(kind, unit)
    local p = data.pickups[kind + 1]
    local sound = nox.spellSound(p.sound, 0)
    if sound then
        nox.sound(unit, sound)
    end
    nox.effect(0x82, x, y)
    nox.tell(unit, p.says)
end

-- A soul gem taken: its worth the taker's XP.
function pickups.gemTaken(unit: number, worth: number)
    owner.gainXp(unit, worth)
end

-- A soul gem's reach by the player's Magnet level: 70, half again a level.
local GEM_REACH = util.float(data.gems.reach)
local GEM_MAGNET = util.float(data.gems.magnet)
function pickups.gemReach(magnet: number): number
    return nox.f32(GEM_REACH * nox.f32(1 + nox.f32(GEM_MAGNET * magnet)))
end

return pickups
