--!strict
-- Deep Delve's own affixes (data/loot.luau's affixes): which gear kind each may roll on, and what
-- the game's six modifiers that fit no item became as affixes (by decision 2026-11-01): frost,
-- dread and grip on weapons, reflect, ward and resilience on armour. Each works only for a hero
-- wearing the piece (the wearer's bonus sums them: hero.luau), on the mod's monsters only (the
-- party's own hits are cancelled before any of this: combat.luau).
--
--   frost, dread  a hit by the hero's worn weapon (its blow, or its bow's arrow) on one of the
--                 mod's monsters may chill or frighten it: the game's enchantment put on a frame
--                 later (an asked hook changes nothing in the world);
--   grip          that hit's damage up;
--   reflect       a monster's missile about to hit the hero may be turned (onReflect: back the way
--                 it came, the hero's now, as the reflective shield turns one); a spell's thing
--                 with no missile (a boss's mist or fist) may be cancelled and its damage dealt to
--                 the monster a frame later;
--   ward          the hero's regeneration beat (regen.luau) cuts the harmful enchantments on it
--                 short as each comes on (its time set shorter in place, silently) and each poison
--                 step (the frames till it falls a level);
--   resilience    the hero's worn pieces lose less durability: a share of what a hit wore off
--                 given back a frame on (affixes.worn);
--   guard         a shield's own, rolled in its effectiveness slot's place (affixes.slotAffix): a
--                 monster's blow or missile from in front of the hero takes that share less
--                 (affixes.guarded, combat.luau).

local data = require("data/loot")
local tipData = require("data/tips")

local affixes = {}

-- Each gear type's kind (loot.gear's); a type not there by the game's record: armour with an
-- armour value, a weapon with a blow, else none.
local kindOf: { [string]: string } = {}
for _, g in data.gear do
	kindOf[g.type] = g.kind
end

function affixes.kindOf(thingType: string): string?
	local k = kindOf[thingType]
	if k then
		return k
	end
	local st = nox.itemStats(thingType)
	k = if st and st.armor and st.armor > 0 then "armour" elseif st and st.damageMin and st.damageMin > 0 then "weapon" else "none"
	kindOf[thingType] = k :: string
	return k
end

-- Whether an affix may go to a class (`classes` none: any class), and whether it may go on a type:
-- with `classes`, only on one of them may use by the game's record (a type with no record of its
-- classes: any).
function affixes.forClass(def: any, class: string?): boolean
	return not def.classes or class == nil or (def.classes :: any)[class] == true
end

local function forType(def: any, thingType: string): boolean
	if not def.classes then
		return true
	end
	local st = nox.itemStats(thingType)
	if not st or not st.classes then
		return true
	end
	for class in def.classes :: any do
		if (st.classes :: any)[class] then
			return true
		end
	end
	return false
end

-- Whether a type is a shield (data/tips.luau's kind).
function affixes.isShield(thingType: string): boolean
	local t = (tipData.items :: any)[thingType]
	return t ~= nil and t.kind == "shield"
end

-- The affixes that may go on a type (those with no kinds, and those its kind is one of, a shield's
-- own too; with classes, only on gear one of them may use), in data order, for `class` (rolled for
-- that class: only those it may have; nil: any); worked out once a type and class. `rolled`: only
-- those a rare's affixes are drawn from (not one rolled in a slot's place: affixes.slotAffix).
local allowedOf: { [string]: { any } } = {}

function affixes.allowed(thingType: string, class: string?, rolled: boolean?): { any }
	local key = (if class then thingType .. "|" .. class else thingType) .. (if rolled then "|rolled" else "")
	local list = allowedOf[key]
	if list then
		return list
	end
	local kind = affixes.kindOf(thingType)
	local shield = affixes.isShield(thingType)
	list = {}
	for _, def in data.affixes :: { any } do
		local kinds: any = def.kinds
		local fits = not kinds or (kind and kinds[kind]) or (shield and kinds.shield)
		if fits and forType(def, thingType) and affixes.forClass(def, class) and not (rolled and def.slot) then
			table.insert(list, def)
		end
	end
	allowedOf[key] = list
	return list
end

-- The affix a type's game slot is rolled as instead (data/loot's `slot`: a shield's guard, in its
-- effectiveness slot, where no armour quality does anything), for `class`; nil: none.
local slotOf: { [string]: any } = {}

function affixes.slotAffix(thingType: string, class: string?): any
	local key = if class then thingType .. "|" .. class else thingType
	local known = slotOf[key]
	if known ~= nil then
		return if known == false then nil else known
	end
	local found: any = false
	for _, def in affixes.allowed(thingType, class) do
		if def.slot then
			found = def
			break
		end
	end
	slotOf[key] = found
	return if found == false then nil else found
end

-- An affix's definition by id: the loot's, and the stats only uniques and sets carry
-- (data/uniques.luau's `stats`: never rolled, so not in `allowed`).
local defs: { [string]: any } = {}
for _, def in data.affixes :: { any } do
	defs[def.id] = def
end
for _, def in require("data/uniques").stats :: { any } do
	defs[def.id] = def
end

function affixes.def(id: string): any
	return defs[id]
end

-- A worn piece's affixes into a bonus table (`b`: the hero's, numbers by stat), each stat up to its
-- `most` once all are in (affixes.cap).
function affixes.add(b: any, list: { any })
	for _, a in list do
		local def = defs[a.id]
		if def and type(a.value) == "number" then
			b[def.stat] = (b[def.stat] or 0) + a.value
		end
	end
end

function affixes.cap(b: any)
	for _, def in data.affixes :: { any } do
		if def.most and (b[def.stat] or 0) > def.most then
			b[def.stat] = def.most
		end
	end
end

-- ---- On a hit by the hero

-- Whether a hit came by the hero's worn weapon (by the hit's cause): its blow (the weapon the item)
-- or its bow's shot (an arrow of the bow's own ammo the hero owns). A weapon's fire or shock beside
-- the blow is neither.
function affixes.byWeapon(b: any, unit: number, weapon: number?, cause: string): boolean
	if not weapon or not b.weapon then
		return false
	end
	if cause == "blow" then
		return weapon == b.weapon
	end
	if cause ~= "missile" or not b.ammo then
		return false
	end
	local o = nox.object(weapon)
	return o ~= nil and o.type == b.ammo and o.owner == unit
end

-- The worn weapon's id and its shot's type (a bow's arrow), for affixes.byWeapon: set on the bonus.
function affixes.noteWeapon(b: any, id: number, thingType: string)
	b.weapon = id
	local t = (tipData.items :: any)[thingType]
	b.ammo = if t then t.ammo else nil
end

-- The chances rolled for a hit on one of the mod's monsters (m: monsters.info's) by the hero's
-- weapon: the enchantments to put on it, a frame later.
-- A frost's or dread's enchantment put on a living monster for `frames`: never shorter than it
-- has, nor over one on for good.
local function putOn(target: number, hit: any, frames: number)
	if nox.living(target) then
		local left = nox.enchantTime(target, hit.enchantment)
		if not nox.enchanted(target, hit.enchantment) or (left > 0 and left < frames) then
			nox.enchant(target, hit.enchantment, frames, hit.power)
		end
	end
end

function affixes.onWeaponHit(b: any, target: number, m: any)
	for _, id in { "frost", "dread" } do
		local chance = b[id] or 0
		local hit = (data.hits :: any)[id]
		if chance > 0 and hit and not (hit.spare and hit.spare[m.tier]) and nox.random() * 100 < chance then
			nox.after(1, function()
				putOn(target, hit, hit.frames)
			end)
		end
	end
end

-- One of the mod's monsters (m: monsters.info's) frightened now as the dread affix frightens
-- (its fear, its spared tiers: never a boss) for `frames`; whether it may be (War Cry's Terror,
-- upgrades.luau).
function affixes.frighten(target: number, m: any, frames: number): boolean
	local hit = (data.hits :: any).dread
	if not hit or (hit.spare and hit.spare[m.tier]) then
		return false
	end
	putOn(target, hit, frames)
	return true
end

-- ---- On a hit on the hero

-- Whether the hero's reflection turns this one: rolled against its reflect.
function affixes.reflects(b: any): boolean
	local chance = b.reflect or 0
	return chance > 0 and nox.random() * 100 < chance
end

-- Whether a spell's hit on the hero with no missile is turned back: by a thing (`weapon`) a
-- monster (`who`) owns (a boss's mist or fist), not the caster itself; rolled as reflects.
function affixes.turnsBack(b: any, who: number, weapon: number?): boolean
	if not weapon or weapon == who or weapon == 0 then
		return false
	end
	return affixes.reflects(b)
end

-- ---- Guard: a shield's front
--
-- The cosine of the guard's arc either side of the hero's facing, worked out once.
local guardCos = math.cos(math.rad(data.guard.arc))

-- The share of a monster's hit the hero takes past its guard (1: all of it): less by its guard
-- when the hit is a blow or a missile (its cause) and the monster behind it (`who`) stands within
-- the arc of the hero's facing.
function affixes.guarded(b: any, unit: number, who: number?, cause: string): number
	local g = b.guard or 0
	if g <= 0 or not who or who == unit or not (data.guard.causes :: any)[cause] or not nox.present(unit) or not nox.present(who) then
		return 1
	end
	local x, y = nox.position(unit)
	local wx, wy = nox.position(who)
	local dx, dy = wx - x, wy - y
	local len = math.sqrt(dx * dx + dy * dy)
	if len <= 0 then
		return 1
	end
	local fx, fy = nox.directionVector(nox.direction(unit))
	if (dx * fx + dy * fy) / len < guardCos then
		return 1
	end
	return 1 - g / 100
end

-- ---- Resilience: the worn pieces wear less
--
-- A piece wears as the game has it (a hit taken wears the armour, a blow the weapon); a frame after
-- a hit the hero takes or deals with its weapon, each worn piece's durability is read against what
-- it was, and the resilience's share of what it lost given back (nox.setDurability), the parts of
-- a point kept on the piece till they make a whole one. One look a frame at most, at the hero's own
-- worn pieces. `h.wear`: item id -> { was (its durability then), part (a point's share owed) }.

local function lookAtWear(h: any)
	h.wearDue = nil
	local r = h.bonus and h.bonus.resilience or 0
	if r <= 0 or not nox.present(h.unit) then
		h.wear = nil
		return
	end
	local old = h.wear or {}
	local now: { [number]: any } = {}
	for _, it in nox.inventory(h.unit) do
		local f = nox.item(it.id)
		if f and f.worn and f.maxDurability > 0 then
			local seen = old[it.id]
			local d = f.durability
			local part = if seen then seen.part else 0
			if seen and d < seen.was and d > 0 then
				part += (seen.was - d) * r / 100
				local back = math.floor(part)
				if back >= 1 then
					part -= back
					d = math.min(f.maxDurability, d + back)
					nox.setDurability(it.id, d)
				end
			end
			now[it.id] = { was = d, part = part }
		end
	end
	h.wear = now
end

-- A hit the hero took or dealt with its weapon: its worn pieces looked at a frame on (once).
function affixes.worn(h: any)
	if h.wearDue or not h.bonus or (h.bonus.resilience or 0) <= 0 then
		return
	end
	h.wearDue = true
	nox.after(1, function()
		lookAtWear(h)
	end)
end

-- Its worn pieces noted as they are now (put on or taken off: what they lose from here on).
function affixes.noteWear(h: any)
	if h.bonus and (h.bonus.resilience or 0) > 0 then
		lookAtWear(h)
	else
		h.wear = nil
	end
end

-- ---- Ward: the harmful enchantments on the hero cut short (the regeneration beat's)

-- Each one is cut once, as it's first seen (or put on again for longer): its frames left set
-- shorter in place (nox.setEnchantTime: no sound, it runs out by itself at the shortened end). A
-- poison step likewise: the frames till its level falls a step (nox.setPoison, the level kept, so
-- it bites as the game's at that level) cut by the ward's share times loot.wardPoison, at each new
-- step (the game's every 1000 frames, or a fresh poisoning). (That count is the one a slowness
-- steps down by too, so a slowness on the hero then steps sooner as well.)
-- `h.ward`: enchantment (or "poison") -> { ends (the frame it now runs out or steps), full (the frame
-- it would have) }.

local function cut(left: number, pct: number): number
	return math.max(1, math.floor(left * (1 - pct / 100) + 0.5))
end

function affixes.ward(h: any)
	local pct = h.bonus and h.bonus.ward or 0
	local now = nox.frame()
	local w = h.ward
	if pct <= 0 then
		if w then
			h.ward = nil
		end
		return
	end
	if not w then
		w = {}
		h.ward = w
	end
	for _, e in data.ward do
		local on = nox.enchanted(h.unit, e)
		local left = if on then nox.enchantTime(h.unit, e) else 0
		local seen = w[e]
		if not on or left == 0 then
			-- (gone, or on for good: nothing to shorten)
			w[e] = nil
		elseif not seen or now + left > seen.ends + 1 then
			-- (new, or put on again for longer: cut by the ward's share, in place)
			local ends = cut(left, pct)
			nox.setEnchantTime(h.unit, e, ends)
			w[e] = { ends = now + ends, full = now + left }
		end
	end
	local level, step = nox.poison(h.unit)
	local seen = w.poison
	if level == 0 or step <= 0 then
		w.poison = nil
	elseif not seen or now + step > seen.ends + 1 then
		-- (a new step: begun since, or poisoned afresh)
		local ends = cut(step, pct * data.wardPoison)
		if ends < step then
			nox.setPoison(h.unit, level, ends)
		end
		w.poison = { ends = now + ends, full = now + step }
	end
end

-- What "dd ward" says of a hero: "ENCHANTMENT ends E full F" for each noted (frames from now;
-- "poison" for its step).
function affixes.wardLines(h: any): { string }
	local out = {}
	local now = nox.frame()
	for e, s in h.ward or {} do
		table.insert(out, string.format("%s ends %d full %d", e, s.ends - now, s.full - now))
	end
	table.sort(out)
	return if #out > 0 then out else { "none" }
end

return affixes
