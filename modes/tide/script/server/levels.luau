-- Undead Tide's levels and picks: soul gems' XP (Wisdom's share), each level's celebration, a
-- level's offer (three drawn from the runner's weapons' next levels, its class's new weapons and
-- its passives' next levels; a heal when nothing's left), and what a pick does.
--
-- Each on the runner in hand (the player whose rule is running: its numbers through owner.xp and
-- owner.kit, its changes through owner.setXp, setWeapon and setPassive); the offer's own
-- bookkeeping, its window (noxsdl's) and its time are still the director's.
local data = require("data")
local rules = require("rules")

local FPS = 30 -- (frames a second)
local WISDOM = 6 -- (TidePassive's Wisdom, from 1)
local MOST_WEAPONS = 6

-- A text line from the script's text table, its {fields} filled in one pass (fillText's).
local function fill(template: string, fields: { [string]: string }): string
    return (string.gsub(template, "{([%w_]+)}", function(name)
        return fields[name]
    end))
end

local CLASS_BITS = { warrior = 1, wizard = 2, conjurer = 4 }
local weaponClasses = {}
for i, w in data.weapons do
    local bits = 0
    for _, c in w.classes do
        bits += CLASS_BITS[c]
    end
    weaponClasses[i] = bits
end

local levels = {}

-- XP as soul gems give it to the player whose unit is `unit` (Wisdom: 15% more a level, the
-- hundredths carried over); each level reached celebrated (its sound and glow, a moment's safety
-- while choosing, a fifth of its health back) and banked for a pick. (The runner's numbers set as
-- they end: nothing the celebration does reads them.)
function levels.gainXp(unit: number, xp: number)
    local now, carry, level, banked, passives = owner.xp()
    local cents = xp * (100 + 15 * passives[WISDOM]) + carry
    now += cents // 100
    carry = cents % 100
    while now >= rules.xpFor(level) do
        now -= rules.xpFor(level)
        level += 1
        banked += 1
        nox.sound(unit, 0x386)
        local me = nox.object(unit)
        local glow = nox.create("LevelUp", me.x, me.y)
        if glow then
            owner.addGlow(glow)
        end
        nox.enchant(unit, "ENCHANT_INVULNERABLE", 3 * FPS, 1)
        nox.heal(unit, math.max(1, nox.object(unit).maxHealth // 5))
    end
    owner.setXp(now, carry, level, banked)
end

-- A level's offer for the runner in hand (`class`: its unit's class, 0-2; `level`: the level it's
-- for): its picks, as kind (0 a weapon, 1 a passive, 2 a heal), which and level, three at most,
-- then the stock client's line for them.
function levels.offerPicks(class: number, level: number): ({ number }, string)
    local weapons, passives = owner.kit()
    local all = {}
    local held = {}
    for _, w in weapons do
        held[w.kind] = true
        if w.level < data.weaponMost then
            table.insert(all, { 0, w.kind, w.level + 1 })
        end
    end
    if #weapons < MOST_WEAPONS then
        -- A new one of its class's, not held yet.
        local bit = bit32.lshift(1, math.min(class, 2))
        for k = 0, #data.weapons - 1 do
            if bit32.band(weaponClasses[k + 1], bit) ~= 0 and not held[k] then
                table.insert(all, { 0, k, 1 })
            end
        end
    end
    for k = 0, #data.passives - 1 do
        if passives[k + 1] < data.passives[k + 1].most then
            table.insert(all, { 1, k, passives[k + 1] + 1 })
        end
    end
    -- (A shuffle: each place swapped with a random one at or before it.)
    for i = #all, 2, -1 do
        local j = owner.roll() % i + 1
        all[i], all[j] = all[j], all[i]
    end
    while #all > 3 do
        table.remove(all)
    end
    if #all == 0 then
        all[1] = { 2, 0, 0 }
    end
    local t = data.text
    local line = fill(t.offerTitle, { level = tostring(level), count = tostring(#all) })
    local flat = {}
    for i, p in all do
        local what
        if p[1] == 2 then
            what = t.pickHeal
        else
            local name = if p[1] == 0 then data.weapons[p[2] + 1].name else data.passives[p[2] + 1].name
            what = if p[3] == 1 and p[1] == 0 then fill(t.pickNew, { name = name }) else fill(t.pickLevel, { name = name, level = tostring(p[3]) })
        end
        line ..= fill(t.offerItem, { number = tostring(i), pick = what })
        table.insert(flat, p[1])
        table.insert(flat, p[2])
        table.insert(flat, p[3])
    end
    return flat, line
end

-- A pick taken by the runner in hand (`unit` its unit, nil: none now): a weapon added or raised, a
-- passive raised (applied at once), or a heal. Returns the line it's told.
function levels.pick(unit: number?, kind: number, which: number, level: number): string
    local t = data.text
    local told
    if kind == 0 then
        owner.setWeapon(which, level)
        told = fill(t.kitItem, { name = data.weapons[which + 1].name, level = tostring(level) })
    elseif kind == 1 then
        owner.setPassive(which, level)
        told = fill(t.kitItem, { name = data.passives[which + 1].name, level = tostring(level) })
    else
        told = t.healTaken
        if unit then
            nox.heal(unit, nox.object(unit).maxHealth)
        end
    end
    return fill(t.youTake, { what = told })
end

return levels
