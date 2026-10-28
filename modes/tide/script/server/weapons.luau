-- Undead Tide's auto-weapons: each player's weapons fired by the server with the player as their
-- caster, its wait over (a blow, a stock spell, arrows and bolts, a haul, the tide pushed or pulled
-- or held, summoned creatures, Force of Nature's ring of orbs).
--
-- The frame's continuous work is the director's native blocks' (modes/common's ready timers): each
-- weapon's wait counted down, the ring turned and harming while it stands, the summons and the
-- missiles in flight kept count of. This script sets their ways up (setUp) and is asked only as a
-- weapon is ready (shoot: it fires, and says when the weapon is ready next) or a weapon's shape is
-- wanted at a new level (shape). What falls one at a time is the script's own.
local data = require("data")
local rules = require("rules")
local util = require("util")

local f32 = nox.f32
local float = util.float
-- A thing's place and size (nox.position, with no table; one being deleted still has its place, as the
-- C++ read it: nox.object then).
local function pos(id: number): (number, number, number)
    local ok, x, y, r = pcall(nox.position, id)
    if ok then
        return x, y, r
    end
    local o = nox.object(id)
    return o.x, o.y, o.radius
end
local floor = math.floor

local FPS = 30 -- (frames a second)

-- The weapons by their numbers (TideWeapon's order; the table's).
local K = {
    swing = 0, magicMissile = 1, arrows = 2, forceOfNature = 3, stun = 18, shockwave = 20, gather = 21,
    cleave = 30, crossbow = 31, warCry = 32, harpoon = 33, wolves = 34, bear = 35, wisps = 36,
}
local RING_MOST = 5 -- (the ring's most orbs: more strength turns it faster)
local RING_SPRITE = "MagicEnergy" -- (a plain looping sprite; the harm is the ring's)
local RING_LIFE = 4 * FPS -- (a DeathBall's 4 s)
local RING_HARM_AFTER = 10 -- (no harm in its first frames, as a DeathBall's)
local SUMMONS = 1 -- (the kept lists' groups: the summons, counted together; the missiles)
local MISSILES = 2
local SUMMONS_MOST = 4 -- (four summoned in all at most, by heads)
local SUMMON_SIGHT = 380 -- (summoned only with a foe in sight this near)
-- (The ring's type number, asked once the host is there and kept.)
local ringType
local function types()
    if not ringType then
        ringType = nox.thingType(RING_SPRITE)
    end
end

local WEAPONS = data.weapons
local MOST = data.weaponMost

-- What falls one at a time (a weapon's `out`: the things of its kind out, at most one) kept by the
-- director's kept lists, a list for each kind (OUT_KEY on), one group.
local FALLING = 3
local OUT_KEY = 100
local outKeys: { [string]: number } = {}

-- Why a fire was declined, for the director to test while it waits (ready_timers.hpp's Wait): the
-- frame it's looked at again, the test, its figures, and a second test with its own retry. Each
-- test names a reason the fire is declined whenever it holds, with nothing done before it.
local NO_FOE, NONE_NEAR, HOLDER_ENCHANTED, KEPT_ANY, KEPT_AT_LEAST, OUT_OF_REACH, ALL_UNDER = 1, 2, 3, 4, 5, 6, 7
-- (next, test, range, tested, reach, key, least, enchant + 1, enchant2 + 1, also, its retry)
local function wait(next: number, test: number, range: number?, tested: number?, reach: number?, key: number?, least: number?,
    enchant: number?, enchant2: number?, also: number?, alsoRetry: number?)
    return next, test, range or 0, tested or 0, reach or 0, key or 0, least or 0, if enchant then enchant + 1 else 0,
        if enchant2 then enchant2 + 1 else 0, also or 0, alsoRetry or 0
end
-- The enchantments' bits by name (asked once).
local bits: { [string]: number } = {}
local function bit(name: string): number
    local v = bits[name]
    if v == nil then
        v = owner.enchantBit(name)
        bits[name] = v
    end
    return v
end

local function clampLevel(level: number): number
    return math.clamp(level, 1, MOST)
end

-- A hostile thing done by the player (as the stock hostile cast): invisibility ends, and the map
-- start's invulnerability for good (which an attack ends) with it; a timed one stays. Not before
-- the Invisibility pickup's time.
local function showAttacker(p, frame: number)
    if frame < p.hiddenUntil then
        return
    end
    local invisible, invulnerable = bit("ENCHANT_INVISIBLE"), bit("ENCHANT_INVULNERABLE")
    if nox.enchanted(p.unit, invisible) then
        nox.enchant(p.unit, invisible, -1)
    end
    if nox.enchanted(p.unit, invulnerable) and nox.enchantTime(p.unit, invulnerable) == 0 then
        nox.enchant(p.unit, invulnerable, -1)
    end
end

-- The spells by number, their sounds and whether they're hostile (flag 0x20), asked once each.
local spellNumbers: { [string]: number } = {}
local function spellNo(name: string): number
    local v = spellNumbers[name]
    if v == nil then
        v = owner.spellNumber(name)
        spellNumbers[name] = v
    end
    return v
end
local sounds: { [string]: string | false } = {}
local hostile: { [string]: boolean } = {}
local function isHostile(name: string): boolean
    local v = hostile[name]
    if v == nil then
        local info = nox.spellInfo(name)
        v = info ~= nil and bit32.band(info.flags, 0x20) ~= 0
        hostile[name] = v
    end
    return v
end

-- A spell's own sound at the player (none: none).
local function spellSound(unit: number, spell: string)
    local sound = sounds[spell]
    if sound == nil then
        sound = nox.spellSound(spell, 0) or false
        sounds[spell] = sound
    end
    if sound then
        nox.sound(unit, sound)
    end
end

-- gamedata.bin's numbers the weapons read, asked once a stage (forget) and kept.
local numbers: { [string]: number } = {}
local function gameNumber(name: string): number
    local v = numbers[name]
    if v == nil then
        v = nox.gameNumber(name)
        numbers[name] = v
    end
    return v
end
local missileCounts: { [number]: number } = {}
local function missileCount(level: number): number
    local v = missileCounts[level]
    if v == nil then
        v = nox.gameNumber("MagicMissileCount", level - 1)
        missileCounts[level] = v
    end
    return v
end

-- A weapon's wait (the rule's), kept as it's worked out: the same figures for the same weapon,
-- level and Haste in this VM (its rounding the host's).
local cooldowns: { [number]: number } = {}
local function cooldown(p, kind: number, level: number): number
    local key = (kind * 256 + level) * 256 + p.haste
    local v = cooldowns[key]
    if v == nil then
        v = rules.cooldown(kind, level, p.haste)
        cooldowns[key] = v
    end
    return v
end

-- The type's name ends with `out` (a weapon's one-at-a-time kind).
local function ofKind(out: string?, id: number): boolean
    if not out then
        return false
    end
    local o = nox.object(id)
    return o ~= nil and string.sub(o.type, -#out) == out
end

-- A spell weapon cast at `target` (nil: about or on the player): the stock spell at the weapon's
-- level as its power, the player its caster, its facing turned to the foe for the cast alone. One
-- of each falling kind (the table's `out`) at a time; a shield only while without one.
-- (Its second answer: refused for one of its kind out.)
local function castWeapon(p, kind: number, level: number, target: number?): (boolean, boolean)
    local sp = WEAPONS[kind + 1]
    if not sp.spell or sp.aim == "custom" then
        return false, false
    end
    local unit = p.unit
    if sp.out and owner.kept(unit, outKeys[sp.out]) > 0 then
        return false, true
    end
    if sp.aim == "onSelf" and nox.enchanted(unit, bit(sp.enchant)) then
        return false, false
    end
    local mark = nox.newMark()
    -- Only a hostile spell (flag 0x20) shows the player, as the stock cast by a user.
    if isHostile(sp.spell) then
        showAttacker(p, p.frame)
    end
    local facing = nox.direction(unit)
    local tx, ty
    if target then
        tx, ty = pos(target)
        nox.direction(unit, nox.directionOf(f32(tx - p.x), f32(ty - p.y)))
    end
    local atSelf = sp.aim == "atSelf" or sp.aim == "onSelf" or not target
    local went
    if atSelf then
        went = nox.cast(spellNo(sp.spell), unit, unit, p.x, p.y, clampLevel(level))
    else
        went = nox.cast(spellNo(sp.spell), unit, target, tx, ty, clampLevel(level))
    end
    nox.direction(unit, facing)
    if sp.out then
        for _, e in nox.madeSince(mark) do
            if ofKind(sp.out, e) then
                owner.keep(unit, outKeys[sp.out], e)
            end
        end
    end
    return went, false
end

-- The conjurer's summonings: creatures of the player's own (as a stock summoning ends), brought
-- back as they fall after the wait; four in all at most, counted by heads. Wolves: one, one more
-- every two levels; a bear (a BlackBear; from level 3 a Bear); wisps: one, one more from level 3.
-- (The director's kept lists count them; asked only when fewer are out than the weapon's want, the
-- four aren't out, its wait is over and a foe's in sight within 380.)
local function summonWant(kind: number, level: number): number
    return if kind == K.wolves then 1 + level // 2 elseif kind == K.bear then 1 else 1 + (if level >= 3 then 1 else 0)
end
local function summon(p, kind: number, level: number, have: number, total: number): number
    local unit = p.unit
    local want = summonWant(kind, level)
    local t = if kind == K.wolves then "Wolf" elseif kind == K.bear then (if level >= 3 then "Bear" else "BlackBear") else "WillOWisp"
    local n = have
    while n < want and total < SUMMONS_MOST do
        local a = f32((owner.roll() % 3600) * util.DEGREE_TENTH)
        local x = f32(p.x + f32(nox.cosf(a) * 40))
        local y = f32(p.y + f32(nox.sinf(a) * 40))
        if util.openGround(x, y) then
            local c = nox.summon(t, x, y, unit, nox.direction(unit))
            if c then
                owner.keep(unit, kind, c)
                total += 1
            end
        end
        n += 1
    end
    spellSound(unit, WEAPONS[kind + 1].icon.spell)
    return p.frame + cooldown(p, kind, level)
end

-- Force of Nature: a ring of orbs about the player for 4 s, then its wait; while it stands each orb
-- harms the tide about it as a DeathBall would, and the ring turns with the player (the director's
-- orbit group). Its size: the orbs out of their harm's reach of the player, a little wider for more
-- of them; its strength (levels and Multishot): up to five on the ring, the rest of it speed.
local RING_GAP = float(6)
local radii: { [number]: number } = {} -- (by orbs, a stage's: kept with the numbers)
local function ringRadius(count: number): number
    local v = radii[count]
    if v == nil then
        v = f32(f32(gameNumber("DeathBallOutRadius") + 25) + f32(RING_GAP * math.max(0, count - 1)))
        radii[count] = v
    end
    return v
end
local TURN = float(0.06)
local TURN_MORE = float(0.02)
local made = {} -- (the orbs of a cast, reused)
local function castRing(p, kind: number, level: number): ...number
    local unit = p.unit
    local frame = p.frame
    local strength = clampLevel(level) + p.multishot
    local count = math.min(strength, RING_MOST)
    local radius = ringRadius(count)
    local sight = f32(radius + 200)
    if #nox.nearestFoes(unit, sight, 1, 4) == 0 then
        return wait(frame + 10, NO_FOE, sight, 4)
    end
    showAttacker(p, frame)
    local angle = owner.orbitAngle(unit)
    table.clear(made)
    for k = 0, count - 1 do
        local a = f32(angle + f32(f32(util.TWO_PI * k) / count))
        local x = f32(p.x + f32(nox.cosf(a) * radius))
        local y = f32(p.y + f32(nox.sinf(a) * radius))
        local m = nox.create(RING_SPRITE, x, y, unit)
        if m then
            nox.setNoCollide(m, true)
            table.insert(made, m)
        end
    end
    owner.orbit(unit, made, RING_LIFE, RING_HARM_AFTER, ringType, gameNumber("DeathBallOutRadius"), gameNumber("DeathBallInRadius"),
        floor(gameNumber("DeathBallNearbyDamage")), 2)
    spellSound(unit, "SPELL_FORCE_OF_NATURE")
    return frame + RING_LIFE + cooldown(p, kind, level)
end

-- (Tables a fire fills afresh each time, kept: the targets, the filters of foes about the player,
-- a blow's attack.)
local scratch = {}
local ALIVE_FOES = { monsters = true, alive = true, enemiesOf = 0 }
local STANDING_FOES = { monsters = true, notDead = true, notGone = true, enemiesOf = 0 }
local BLOW = { damage = 0, type = 0, range = 0, area = true, sides = 0xFF, x = 0, y = 0 }

-- The table's numbers made floats once.
local REACH = {}
for k, w in WEAPONS do
    REACH[k] = float(w.reach)
end

-- A spell weapon (the table): at the nearest foe (Multishot: one more cast at the next nearest every
-- two levels, past the first, but for those one at a time); on the nearest foes not under it yet
-- (one more every two levels); about or on the player, a foe within its reach; on the strongest foe
-- near. Earthquake's reach its spell's. No target: looked at again in 5 frames; a cast that didn't
-- go likewise.
local function castSpell(p, sp, kind: number, level: number): ...number
    local unit = p.unit
    local frame = p.frame
    local reach = if REACH[kind + 1] < 0 then gameNumber("EarthquakeRange") else REACH[kind + 1]
    local more = (p.multishot + 1) // 2
    local targets = scratch
    table.clear(targets)
    if sp.aim == "atFoe" then
        targets = nox.nearestFoes(unit, reach, if sp.out then 1 else 1 + more, 8)
    elseif sp.aim == "onFoe" then
        -- (Never the guardian: Freeze and the like would hold dawn's fight still. Stun takes the
        -- foes it can hold first: a monster heavier than 15 is only slowed.)
        local near = nox.nearestFoes(unit, reach, 8, 8)
        if kind == K.stun then
            local light, heavy = {}, {}
            for _, t in near do
                table.insert(if nox.object(t).mass <= 15 then light else heavy, t)
            end
            near = light
            for _, t in heavy do
                table.insert(near, t)
            end
        end
        for _, t in near do
            local under = nox.enchanted(t, bit(sp.enchant)) or (kind == K.stun and nox.enchanted(t, bit("ENCHANT_SLOWED")))
            if t ~= p.boss and not under and #targets < 1 + (level - 1) // 2 + more then
                table.insert(targets, t)
            end
        end
    elseif sp.aim == "atSelf" or sp.aim == "onSelf" then
        if (sp.aim ~= "onSelf" or not nox.enchanted(unit, bit(sp.enchant))) and #nox.nearestFoes(unit, reach, 1, 8) > 0 then
            table.insert(targets, unit)
        end
    elseif sp.aim == "onStrongest" then
        local best, bestMost = nil, 0
        for _, t in nox.nearestFoes(unit, reach, 8, 8) do
            if t ~= p.boss and not nox.enchanted(t, bit(sp.enchant)) then
                local most = nox.object(t).maxHealth
                if best == nil or most > bestMost then
                    best, bestMost = t, most
                end
            end
        end
        if best then
            table.insert(targets, best)
        end
    end
    if #targets == 0 then
        -- (Why, for the director's wait: no foe in sight; every one in sight already under it, or
        -- the guardian; the player under its own.)
        if sp.aim == "onFoe" then
            return wait(frame + 5, ALL_UNDER, reach, 8, nil, nil, nil, bit(sp.enchant), if kind == K.stun then bit("ENCHANT_SLOWED") else nil)
        elseif sp.aim == "onStrongest" then
            return wait(frame + 5, ALL_UNDER, reach, 8, nil, nil, nil, bit(sp.enchant))
        elseif sp.aim == "onSelf" then
            -- (Under its own already, or no foe in sight.)
            local test = if nox.enchanted(unit, bit(sp.enchant)) then HOLDER_ENCHANTED else NO_FOE
            return wait(frame + 5, test, reach, 8, nil, nil, nil, bit(sp.enchant), nil, if test == NO_FOE then HOLDER_ENCHANTED else NO_FOE, 5)
        elseif sp.out then
            -- (No foe in sight; one of its kind out would decline it too.)
            return wait(frame + 5, NO_FOE, reach, 8, nil, outKeys[sp.out], nil, nil, nil, KEPT_ANY, 5)
        end
        return wait(frame + 5, NO_FOE, reach, 8)
    end
    local went, held = false, true
    for _, t in targets do
        local w, out = castWeapon(p, kind, level, if t == unit then nil else t)
        went = w or went
        held = held and out
    end
    if went then
        return frame + cooldown(p, kind, level)
    elseif held then
        -- (One of its kind still out; no foe in sight would decline it too.)
        return wait(frame + 5, KEPT_ANY, reach, 8, nil, outKeys[sp.out], nil, nil, nil, NO_FOE, 5)
    end
    return frame + 5
end

-- About the player: the tide within reach thrown back and hurt, drawn in, or stunned.
local function aboutPlayer(p, kind: number, level: number): ...number
    local unit = p.unit
    local frame = p.frame
    local reach = f32(REACH[kind + 1] + 10 * (level - 1)) -- (10 more a level)
    ALIVE_FOES.enemiesOf = unit
    local near = nox.unitsNear(p.x, p.y, reach, ALIVE_FOES)
    if #near == 0 then
        return wait(frame + 5, NONE_NEAR, reach)
    end
    showAttacker(p, frame)
    for _, o in near do
        if kind == K.shockwave then
            nox.applyForce(o, p.x, p.y, 70 + 10 * level)
            nox.damage(o, unit, 15 + 8 * level, 0xB) -- (impact, the player's)
        elseif kind == K.gather then
            nox.applyForce(o, p.x, p.y, -(40 + 8 * level))
        else
            nox.enchant(o, bit("ENCHANT_HELD"), (2 + level // 2) * FPS, level)
        end
    end
    if kind == K.warCry then
        local sound = nox.abilitySound(2, 0) -- (War Cry's own)
        if sound then
            nox.sound(unit, sound)
        end
    else
        spellSound(unit, if kind == K.gather then "SPELL_PULL" else "SPELL_PUSH")
        nox.effect(0x81, p.x, p.y)
    end
    return frame + cooldown(p, kind, level)
end

-- A blow's reach past the player's own radius (Cleave's wider).
local function blowReach(kind: number, level: number): number
    return if kind == K.cleave then 60 + 10 * level else 40 + 8 * level
end

-- The rest, at the nearest foe: a blow, magic missiles, arrows, bolts, a haul. No foe: looked at
-- again in 5 frames.
local function atNearest(p, kind: number, level: number): ...number
    local unit = p.unit
    local frame = p.frame
    local range = REACH[kind + 1]
    local target = nox.nearestFoes(unit, range, 1, 4)[1]
    if not target then
        -- (No foe in sight; for a blow, one out of its reach, for magic missiles all of theirs out,
        -- would decline it too.)
        if kind == K.swing or kind == K.cleave then
            return wait(frame + 5, NO_FOE, range, 4, blowReach(kind, level), nil, nil, nil, nil, OUT_OF_REACH, 3)
        elseif kind == K.magicMissile then
            return wait(frame + 5, NO_FOE, range, 4, nil, K.magicMissile, floor(missileCount(level)), nil, nil, KEPT_AT_LEAST, 5)
        end
        return wait(frame + 5, NO_FOE, range, 4)
    end
    local tx, ty, tr = pos(target)
    local dx = f32(tx - p.x)
    local dy = f32(ty - p.y)
    local facing = nox.direction(unit)
    if kind == K.swing or kind == K.cleave then
        -- A blow all round it (the player's attack, as a war hammer's area), the weapon's damage
        -- and reach. Cleave: wider, harder, slower, the struck thrown back. Out of reach: looked at
        -- again in 3 frames.
        local cleave = kind == K.cleave
        local beyond = blowReach(kind, level)
        local reach = f32(p.radius + beyond)
        if f32(f32(math.sqrt(f32(f32(dx * dx) + f32(dy * dy)))) - tr) > reach then
            return wait(frame + 3, OUT_OF_REACH, range, 4, beyond, nil, nil, nil, nil, NO_FOE, 5)
        end
        showAttacker(p, frame)
        if cleave then
            STANDING_FOES.enemiesOf = unit
            for _, o in nox.unitsNear(p.x, p.y, reach, STANDING_FOES) do
                nox.applyForce(o, p.x, p.y, 50 + 8 * level)
            end
        end
        local damage = if cleave then 45 + 12 * (level - 1) else 30 + 10 * (level - 1)
        -- Faced at the foe for the blow, then turned back.
        nox.direction(unit, nox.directionOf(dx, dy))
        BLOW.damage, BLOW.range, BLOW.x, BLOW.y = damage, reach, p.x, p.y
        nox.attack(unit, BLOW)
        nox.direction(unit, facing)
        -- Its flash where it struck (yellow sparks every client draws).
        local fx, fy = pos(target)
        nox.effect(0x82, math.max(0, fx), math.max(0, f32(fy - 10)))
    elseif kind == K.magicMissile then
        -- The spell itself at the weapon's level as its power, after the foe; its own count of
        -- missiles in the air holds (the director's kept list of them).
        local most = floor(missileCount(level))
        if owner.kept(unit, K.magicMissile) >= most then
            return wait(frame + 5, KEPT_AT_LEAST, range, 4, nil, K.magicMissile, most, nil, nil, NO_FOE, 5)
        end
        showAttacker(p, frame)
        local mark = nox.newMark()
        nox.direction(unit, nox.directionOf(dx, dy))
        nox.cast(spellNo("SPELL_MAGIC_MISSILE"), unit, target, tx, ty, level)
        nox.direction(unit, facing)
        for _, e in nox.madeSince(mark) do
            owner.keep(unit, K.magicMissile, e)
        end
    elseif kind == K.arrows then
        -- Arrows loosed for it (as a bow looses one, from no bow or quiver): two, one more a level,
        -- fanned 8 apart.
        showAttacker(p, frame)
        local base = nox.directionOf(dx, dy)
        local n = clampLevel(level) + 1 + p.multishot
        for k = 0, n - 1 do
            nox.shoot("ArcherArrow", unit, bit32.band(base + (2 * k - (n - 1)) * 4, 0xFF))
        end
        nox.sound(unit, 0x375) -- (a bow's)
    elseif kind == K.crossbow then
        -- Heavy bolts, one at each of the nearest foes: one, one more a level and a Multishot's.
        showAttacker(p, frame)
        for _, f in nox.nearestFoes(unit, range, clampLevel(level) + p.multishot, 8) do
            local fx, fy = pos(f)
            nox.shoot("ArcherBolt", unit, nox.directionOf(f32(fx - p.x), f32(fy - p.y)))
        end
        nox.sound(unit, 0x375)
    elseif kind == K.harpoon then
        -- The nearest foe hauled to the player's side and hurt.
        showAttacker(p, frame)
        local d = f32(math.sqrt(f32(f32(dx * dx) + f32(dy * dy))))
        local keepAt = f32(f32(p.radius + tr) + 6)
        if d > keepAt and target ~= p.boss then -- (the guardian won't be hauled)
            nox.setPosition(target, f32(p.x + f32(f32(dx / d) * keepAt)), f32(p.y + f32(f32(dy / d) * keepAt)))
        end
        nox.damage(target, unit, 25 + 10 * level, 0)
        local sound = nox.abilitySound(3, 0)
        if sound then
            nox.sound(unit, sound)
        end
        local fx, fy = pos(target)
        nox.effect(0x82, math.max(0, fx), math.max(0, fy))
    end
    return frame + cooldown(p, kind, level)
end

local weapons = {}

-- The director's blocks set up: the ring an orbit group, the summons kept lists (one group, four
-- in all at most, each waiting for a foe in sight within 380, the walls tested for the 8 nearest),
-- the magic missiles in flight a kept list; the rest fire as they're ready.
function weapons.setUp()
    owner.weaponWay(K.forceOfNature, 1)
    for _, k in { K.wolves, K.bear, K.wisps } do
        owner.weaponWay(k, 2, SUMMONS, SUMMONS_MOST, SUMMON_SIGHT, 8)
        owner.keptRule(k, 1, "", SUMMONS)
    end
    owner.keptRule(K.magicMissile, 0, "MagicMissile", MISSILES)
    -- (One list for each kind out at a time; none a suffix of another, so a thing is of one alone,
    -- as one list shared by them all, each looking for its own kind, had it.)
    local outs = {}
    for _, w in WEAPONS do
        if w.out and not outKeys[w.out] then
            table.insert(outs, w.out)
            outKeys[w.out] = OUT_KEY + #outs
            owner.keptRule(outKeys[w.out], 0, "", FALLING)
        end
    end
    for _, a in outs do
        for _, b in outs do
            if a ~= b and string.sub(a, -#b) == b then
                error("weapons: the kind out at a time " .. b .. " is a suffix of " .. a)
            end
        end
    end
end

-- A weapon's shape at a level and Multishot (the director's blocks ask once for each and keep it):
-- the ring's orbs, radius and turn a frame; the summons' want.
function weapons.shape(kind: number, level: number, multishot: number): (number, number, number, number)
    if kind == K.forceOfNature then
        local strength = clampLevel(level) + multishot
        local count = math.min(strength, RING_MOST)
        local step = f32(f32(TURN + f32(TURN_MORE * (strength - 1))) + f32(TURN_MORE * math.max(0, strength - RING_MOST)))
        return count, ringRadius(count), step, 0
    end
    return 0, 0, 0, summonWant(kind, level)
end

-- A ready weapon fired: the player's (its unit, place and size, the Invisibility pickup's time,
-- Multishot, Haste), the weapon's kind and level, when it was ready, and for summons how many of
-- its kind are out and how many in all. Returns the frame it's ready next, and when it was declined
-- why (the director's wait: see wait above).
local p = {} -- (the player's view, reused)
function weapons.shoot(frame: number, boss: number, unit: number, x: number, y: number, radius: number, hiddenUntil: number,
    multishot: number, haste: number, kind: number, level: number, readyAt: number, have: number, total: number): ...number
    types()
    p.frame, p.boss, p.unit, p.x, p.y, p.radius = frame, boss, unit, x, y, radius
    p.hiddenUntil, p.multishot, p.haste = hiddenUntil, multishot, haste
    if kind == K.forceOfNature then
        return castRing(p, kind, level)
    elseif kind == K.wolves or kind == K.bear or kind == K.wisps then
        return summon(p, kind, level, have, total)
    end
    local sp = WEAPONS[kind + 1]
    if sp.aim ~= "custom" then
        return castSpell(p, sp, kind, level)
    elseif kind == K.shockwave or kind == K.gather or kind == K.warCry then
        return aboutPlayer(p, kind, level)
    end
    return atNearest(p, kind, level)
end

-- A new lobby or a new arena: the numbers asked afresh (what's out is the director's blocks',
-- forgotten with them).
function weapons.forget(_everything: number)
    table.clear(numbers)
    table.clear(radii)
    table.clear(missileCounts)
end

return weapons
