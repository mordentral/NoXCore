--!strict
-- Rare and champion powers (by decision 2026-11-01; their numbers data/monsters.luau's `powers`). A
-- rare rolls one, a champion two, from those that fit its kind (a power's `notOn`); magic and normal
-- monsters and the act bosses none. Each power's word goes in front of the monster's name on its
-- plate ("Frostbound Grimmaw"), and each one it has makes its death's loot a little better.
--
-- How each works, all through the game's own spells, enchantments and effects:
--   on it for good   swift (the game's Haste), mirrored (the reflective shield), burning's look
--                    (the fire motes of fire protection)
--   its beat         burning, blinking, warding, festering, summoner: one timer per monster that has one,
--                    every powerBeat.every frames, doing nothing while no player is within
--                    powerBeat.wake; each power then on its own clock. It reads the units near it
--                    (nox.unitsNear, nox.countNear), never the world.
--   its hits         frostbound, vampiric: combat.luau tells `struck` of each hit of a powered
--                    monster on a hero (the hit's final amount); the slow or the heal follows the
--                    next frame (onDamage may change nothing in the world).
--   its death        splitting, stormcharged: monsters.luau tells `died`.
--   rising again     a zombie's beat stops at each of its deaths and starts again, as it was
--                    made, each time it stands (`rose`); its death powers go off at its first
--                    death alone.
--
-- A summoner's minions (by decision 2026-11-02) are made through monsters.luau as normal ones of the
-- area's weakest types, their entries marked `minion` and `master` (the summoner's id): main.luau
-- gives no experience or loot for them. Its own are kept in its entry (`minions`, a handful at
-- most); they crumble away at its death, or once it's no longer the mod's.
--
-- Each power adds to its monster's experience (data's powerExperience) and its death's loot
-- (powerLoot).
--
-- Damage a power deals goes through the engine with the monster as its source, so combat.luau
-- scales it as the monster's own hits (its tier, level and the party) with the hero's resistance,
-- and cancels it on the mod's monsters; a power only ever aims at the monster's foes. The
-- lightning at a death has no living source: its amount is worked out here from the dead
-- monster's own factors (the corpse its source, for the hero's resistance).

local data = require("data/monsters")
local scale = require("scale")

local powers = {}

local MSG_BLUE_SPARKS = 0x81
local MSG_YELLOW_SPARKS = 0x82
local MSG_COUNTERSPELL_EXPLOSION = 0x87
local MSG_TELEPORT = 0x89
local MSG_SMOKE_BLAST = 0x8A

local FLAME = 1
local SHOCK = 9

local PLAYERS: any = { players = true, alive = true }

-- The powers by id, and their order (data's), built once.
local byId: { [string]: any } = {}
local order: { string } = {}
for _, p in data.powers :: any do
	byId[p.id] = p
	table.insert(order, p.id)
end

-- The ones that act on the monster's own beat.
local ACTIVE: { [string]: boolean } = { burning = true, blinking = true, warding = true, festering = true, summoner = true }

-- The monsters with a power that acts by itself: id -> { entry, timer, at (power id -> the frame
-- it may act next) }.
local live: { [number]: any } = {}

-- monsters.luau, handed over at its load (attach): its index and its spawn.
local host: any = nil

-- The powers' own random stream: their rolls never move the mode's own (nox.random), so the
-- packs, loot and everything else drawn after a powered monster come out as they would without it.
local rng = nox.mt19937(0x0D0E1A57)

-- 1 to n from the stream.
local function roll(n: number): number
	return rng:next() % n + 1
end

-- What the powers have done since the counts were cleared (for "dd powers" and the tests).
local counts: { [string]: number } = {}

-- The map's number (one more at each new map): what a power left to the next frames (a slow, a
-- heal, a death's lightning) does nothing in a map after its own.
local mapSerial = 1

local function count(what: string, n: number?)
	counts[what] = (counts[what] or 0) + (n or 1)
end

function powers.attach(monsters: any)
	host = monsters
end

-- A power's strength at an area level (1 at the first, `perLevel` more each level past it).
local function strength(def: any, level: number): number
	return 1 + (def.perLevel or 0) * math.max(0, level - 1)
end
powers.strength = strength

-- Whether a power fits a monster type (its `notOn` kinds).
local function fits(def: any, thingType: string): boolean
	if def.notOn then
		for _, kind in def.notOn do
			local set = (data.powerKinds :: any)[kind]
			if set and set[thingType] then
				return false
			end
		end
	end
	return true
end
powers.fits = fits

-- The power's definition by id, or nil.
function powers.def(id: string): any
	return byId[id]
end

-- The ids in data's order.
function powers.ids(): { string }
	return order
end

-- ---- The beat

local function stop(id: number)
	local st = live[id]
	if st then
		live[id] = nil
		nox.cancelTimer(st.timer)
	end
end

-- Whether the monster `st` was made for is still the one at `id`, alive.
local function still(id: number, st: any): boolean
	return host ~= nil and host.info(id) == st.entry and nox.living(id)
end

-- burning: each foe within reach takes its fire.
local function burn(id: number, e: any, def: any, x: number, y: number)
	local hit, took = 0, 0
	local amount = math.max(1, math.floor(def.damage * strength(def, e.level) + 0.5))
	for _, u in nox.unitsNear(x, y, def.reach, { enemiesOf = id, alive = true } :: any) do
		took += nox.damage(u, id, amount, FLAME)
		local ux, uy = nox.position(u)
		nox.effect(MSG_YELLOW_SPARKS, ux, uy)
		hit += 1
	end
	if hit > 0 then
		count("burns", hit)
		count("burnt", took)
	end
end

-- blinking: to the side of a foe it sees (no wall between), between near and far: the nearest.
-- (nox.nearestFoes lists only creatures, a hero's summons; a monster's foes are the heroes too.)
local function blink(id: number, def: any, x: number, y: number): boolean
	local best: number?, bestD = nil, math.huge
	for _, f in nox.unitsNear(x, y, def.far, { enemiesOf = id, alive = true } :: any) do
		local fx, fy = nox.position(f)
		local dx, dy = x - fx, y - fy
		local d = dx * dx + dy * dy
		if d > def.near * def.near and d < bestD and nox.traceRay(x, y, fx, fy, 1) then
			best, bestD = f, d
		end
	end
	if not best then
		return false
	end
	local f: number = best
	local fx, fy = nox.position(f)
	local d = math.sqrt(bestD)
	-- (its landing: `land` from the foe on its own side, where there's room and no wall between)
	local px, py = nox.clearPointNear(fx + (x - fx) / d * def.land, fy + (y - fy) / d * def.land, 30)
	if not nox.traceRay(fx, fy, px, py, 1) then
		return false
	end
	nox.effect(MSG_TELEPORT, x, y)
	nox.setPosition(id, px, py)
	nox.effect(MSG_TELEPORT, px, py)
	local sound = nox.spellSound("SPELL_BLINK", 0)
	if sound then
		nox.sound(id, sound)
	end
	nox.direction(id, nox.directionOf(fx - px, fy - py))
	count("blinks")
	return true
end

-- warding: the game's Force Field on the mod's other monsters near it that have none.
local function ward(id: number, def: any, x: number, y: number)
	local given = 0
	for _, m in nox.unitsNear(x, y, def.reach, { monsters = true, alive = true } :: any) do
		if given >= def.most then
			break
		end
		if m ~= id and host.info(m) and not nox.enchanted(m, "ENCHANT_SHIELD") then
			local mx, my = nox.position(m)
			if nox.cast("SPELL_SHIELD", m, m, mx, my, def.power) then
				nox.effect(MSG_BLUE_SPARKS, mx, my)
				given += 1
			end
		end
	end
	if given > 0 then
		count("wards", given)
	end
end

-- festering: the game's Toxic Cloud, its own, where it stands.
local function fester(id: number, def: any, x: number, y: number)
	if nox.countNear(x, y, def.reach, { enemiesOf = id, alive = true } :: any) == 0 then
		return
	end
	if nox.cast("SPELL_TOXIC_CLOUD", id, nil, x, y, 0) then
		count("clouds")
	end
end

-- summoner: its minions still its own and alive (the list in its entry kept to those).
local function minionsOf(id: number, e: any): { number }
	local mine: { number } = {}
	for _, m in e.minions or {} do
		local c = host.info(m)
		if c and c.master == id and nox.living(m) then
			table.insert(mine, m)
		end
	end
	e.minions = mine
	return mine
end

-- summoner: while a foe is near, up to `count` minions round it (never past `most` alive); whether
-- any came (its cooldown then starts).
local function summon(id: number, e: any, def: any, x: number, y: number): boolean
	if nox.countNear(x, y, def.reach, { enemiesOf = id, alive = true } :: any) == 0 then
		return false
	end
	local mine = minionsOf(id, e)
	local room = math.min(def.count, def.most - #mine)
	local kinds: { string } = host.weakTypes(def.kinds)
	if room <= 0 or #kinds == 0 then
		return false
	end
	local made = 0
	local party = scale.factor("health")
	for k = 1, room do
		local a = (k / room) * 2 * math.pi + roll(100) * 0.02
		local cid = host.spawn(kinds[roll(#kinds)], x + def.ring * math.cos(a), y + def.ring * math.sin(a), "normal", e.level, nil, {})
		local c = cid and host.info(cid)
		if cid and c then
			c.minion = true
			c.master = id
			c.experience = 0
			c.drops = 0
			local most = math.clamp(math.floor(c.base * def.health + 0.5), 1, 65535)
			c.base = most
			local now = math.clamp(math.floor(most * party + 0.5), 1, 65535)
			nox.setHealth(cid, now, now)
			nox.touch(cid)
			local cx, cy = nox.position(cid)
			nox.effect(MSG_SMOKE_BLAST, cx, cy)
			table.insert(mine, cid)
			made += 1
		end
	end
	if made == 0 then
		return false
	end
	local sound = nox.spellSound(def.sound, 0)
	if sound then
		nox.sound(id, sound)
	end
	count("calls")
	count("minions", made)
	return true
end

-- summoner: its minions crumble away (at its death, or once it's no longer the mod's): out of the
-- index first, so nothing of their going counts.
local function dismiss(id: number, e: any)
	local list = e.minions
	e.minions = nil
	if not list then
		return
	end
	for _, m in list do
		local c = host.info(m)
		if c and c.master == id then
			host.forget(m)
			if nox.present(m) then
				local mx, my = nox.position(m)
				nox.effect(MSG_SMOKE_BLAST, mx, my)
				nox.delete(m)
				count("dismissed")
			end
		end
	end
end

local function think(id: number)
	local st = live[id]
	if not st then
		return
	end
	if not still(id, st) then
		stop(id)
		return
	end
	local x, y = nox.position(id)
	local beat: any = data.powerBeat
	if nox.countNear(x, y, beat.wake, PLAYERS) == 0 then
		return
	end
	local now = nox.frame()
	local e = st.entry
	for _, p in e.powers do
		local def = byId[p]
		if ACTIVE[p] and now >= (st.at[p] or 0) then
			if p == "burning" then
				st.at[p] = now + def.beat
				burn(id, e, def, x, y)
			elseif p == "blinking" then
				-- (none to go to: looked for again on the next beat)
				if blink(id, def, x, y) then
					st.at[p] = now + def.every
					x, y = nox.position(id)
				end
			elseif p == "warding" then
				st.at[p] = now + def.every
				ward(id, def, x, y)
			elseif p == "festering" then
				st.at[p] = now + def.every
				fester(id, def, x, y)
			elseif p == "summoner" then
				-- (no foe near, or its most out: looked at again on the next beat)
				if summon(id, e, def, x, y) then
					st.at[p] = now + def.every
				end
			end
		end
	end
end

-- Its beat started (any one it had stopped first): each power's clock from now.
local function beatFor(id: number, e: any)
	stop(id)
	local st = { entry = e, at = {} :: { [string]: number }, timer = 0 }
	st.timer = nox.every(data.powerBeat.every, think, id)
	live[id] = st
end

-- ---- Rolling them

-- `want` (a list of ids, the console's) or `n` of the powers that fit `thingType`, drawn at random
-- without repeats.
local function draw(thingType: string, n: number): { string }
	local pool: { string } = {}
	for _, p in order do
		if fits(byId[p], thingType) then
			table.insert(pool, p)
		end
	end
	local out: { string } = {}
	for _ = 1, math.min(n, #pool) do
		table.insert(out, (table.remove(pool, roll(#pool)) :: string))
	end
	return out
end

-- The list with its `noun` powers last (their words nearest the name on the plate).
local function nounsLast(list: { string }): { string }
	local out: { string } = {}
	for _, p in list do
		if not byId[p].noun then
			table.insert(out, p)
		end
	end
	for _, p in list do
		if byId[p].noun then
			table.insert(out, p)
		end
	end
	return out
end

-- The plate's text: the powers' words, then the name (64 bytes at most: a label's room).
local function plateOf(e: any): string
	local words: { string } = {}
	for _, p in e.powers do
		table.insert(words, byId[p].name)
	end
	table.insert(words, tostring(e.name))
	local text = table.concat(words, " ")
	if #text > 64 then
		text = string.sub(text, 1, 64)
	end
	return text
end

-- A monster just set up as its tier (monsters.applyTier): `n` powers rolled for it (or `want`'s,
-- every one there; {}: none), put on, its plate's text and its loot's and experience's shares set in
-- its entry.
function powers.give(id: number, e: any, n: number, want: { string }?)
	local list = nounsLast(if want then want else draw(e.type, n))
	if #list == 0 then
		return
	end
	e.powers = list
	local active = false
	for _, p in list do
		local def = byId[p]
		if def.enchant then
			nox.enchant(id, def.enchant, 0)
		end
		if def.look then
			nox.enchant(id, def.look, 0)
		end
		if ACTIVE[p] then
			active = true
		end
	end
	e.drops += #list * data.powerLoot.drops
	-- (each power `share` of the tier's experience more, added up)
	e.experience = math.max(1, math.floor(e.experience * (1 + #list * data.powerExperience.share) + 0.5))
	if e.name then
		e.plate = plateOf(e)
	end
	if active then
		beatFor(id, e)
	end
end

-- A risen monster of the mod's (a zombie stood up again: monsters.standing) with powers: they work
-- again as they did when it was made (by decision 2026-11-10): its beat started afresh (each power's
-- clock from now, no minions), and its powers' enchantments put back should any be off (the engine
-- keeps a zombie's through its death).
function powers.rose(id: number, e: any)
	local list = e.powers
	if not list or not nox.living(id) then
		return
	end
	local active = false
	for _, p in list do
		local def = byId[p]
		for _, ench in { def.enchant, def.look } do
			if ench and not nox.enchanted(id, ench) then
				nox.enchant(id, ench, 0)
			end
		end
		if ACTIVE[p] then
			active = true
		end
	end
	if active then
		e.minions = nil
		beatFor(id, e)
		count("risen")
	end
end

-- ---- Its hits

-- A powered monster's hit on a hero (combat.luau's onDamage: `amount` the hit as it lands, after
-- the scaling): its slow and its heal the next frame.
function powers.struck(e: any, monster: number, target: number, amount: number)
	local list = e.powers
	if not list or amount <= 0 then
		return
	end
	local serial = mapSerial
	for _, p in list do
		local def = byId[p]
		if p == "frostbound" then
			local frames = math.floor(def.slow * strength(def, e.level) + 0.5)
			nox.after(1, function()
				if serial == mapSerial and nox.living(target) then
					nox.enchant(target, "ENCHANT_SLOWED", frames)
					count("slows")
				end
			end)
		elseif p == "vampiric" then
			local heal = math.max(1, math.floor(amount * def.share * strength(def, e.level) + 0.5))
			nox.after(1, function()
				if serial == mapSerial and nox.living(monster) then
					nox.heal(monster, heal)
					count("drunk", heal)
				end
			end)
		end
	end
end

-- ---- Its death

local function split(id: number, e: any, def: any, x: number, y: number)
	for k = 1, def.count do
		local a = (k / def.count) * 2 * math.pi + roll(100) * 0.005
		local cid = host.spawn(e.type, x + 28 * math.cos(a), y + 28 * math.sin(a), "normal", e.level, nil, {})
		local c = cid and host.info(cid)
		if c then
			local most = math.clamp(math.floor(e.base * def.health + 0.5), 1, 65535)
			c.base = most
			c.experience = math.max(1, math.floor(e.experience * def.experience + 0.5))
			c.copy = true
			local party = scale.factor("health")
			local now = math.clamp(math.floor(most * party + 0.5), 1, 65535)
			nox.setHealth(cid, now, now)
			nox.touch(cid)
			local cx, cy = nox.position(cid)
			nox.effect(MSG_TELEPORT, cx, cy)
			count("splits")
		end
	end
end

local function storm(id: number, e: any, def: any, x: number, y: number)
	-- (the dead monster's own factors: its tier, level and the party's; a corpse no longer counts
	-- as the mod's in combat.luau, which then takes only the hero's resistance off)
	local amount = math.max(1, math.floor(def.damage * strength(def, e.level) * e.damage * scale.factor("damage") + 0.5))
	for k = 0, 5 do
		local a = k * math.pi / 3
		nox.effect(MSG_BLUE_SPARKS, x + def.reach * 0.6 * math.cos(a), y + def.reach * 0.6 * math.sin(a))
	end
	nox.effect(MSG_BLUE_SPARKS, x, y)
	local serial = mapSerial
	nox.after(def.warn, function()
		if serial ~= mapSerial then
			return
		end
		nox.effect(MSG_COUNTERSPELL_EXPLOSION, x, y)
		local sound = nox.spellSound("SPELL_LIGHTNING", 0)
		if sound then
			nox.soundAt(x, y, sound)
		end
		local source: number? = if nox.present(id) then id else nil
		local hit = 0
		for _, u in nox.unitsNear(x, y, def.reach, PLAYERS) do
			nox.damage(u, source, amount, SHOCK)
			hit += 1
		end
		count("bursts")
		if hit > 0 then
			count("shocked", hit)
		end
	end)
end

-- A powered monster of the mod's died (monsters.killed, its entry already out of the index, but a
-- zombie's, kept till its body goes): its timer stopped, its minions gone; its death's powers, but
-- for a zombie's later deaths (`again`: it has died before; only its first gives anything).
function powers.died(id: number, e: any, again: boolean?)
	stop(id)
	local list = e.powers
	if not list then
		return
	end
	if e.minions then
		dismiss(id, e)
	end
	if again then
		return
	end
	local o = nox.object(id)
	if not o then
		return
	end
	for _, p in list do
		local def = byId[p]
		if p == "splitting" then
			split(id, e, def, o.x, o.y)
		elseif p == "stormcharged" then
			storm(id, e, def, o.x, o.y)
		end
	end
end

-- A monster no longer the mod's (charmed away, gone; `e` its entry): its timer stopped, its
-- minions gone.
function powers.forgot(id: number, e: any?)
	stop(id)
	if e and e.minions then
		dismiss(id, e)
	end
end

-- A new map: every timer stopped (the old monsters gone with their map).
function powers.reset()
	mapSerial += 1
	for id in live do
		stop(id)
	end
end

-- ---- For the console

-- "dd powers": each live powered monster ("id type tier powers drops D xp X [minions IDS] beat B":
-- D its death's loot rolls past the first, its tier's and its powers'; X its death's experience for
-- one player, its powers' share in it; IDS a summoner's minions alive now, comma separated, or "-";
-- B 1 while its beat runs, else 0: a zombie lying dead), how many beats run, then what they've done.
function powers.lines(): { string }
	local out = {}
	for id, e in host.all() do
		if e.powers then
			local words = { tostring(id), tostring(e.type), tostring(e.tier), table.concat(e.powers, ","), "drops", tostring(e.drops), "xp",
				tostring(e.experience) }
			if table.find(e.powers, "summoner") then
				local mine = minionsOf(id, e)
				local ids: { string } = {}
				for _, m in mine do
					table.insert(ids, tostring(m))
				end
				table.insert(words, "minions")
				table.insert(words, if #ids > 0 then table.concat(ids, ",") else "-")
			end
			table.insert(words, "beat")
			table.insert(words, if live[id] then "1" else "0")
			table.insert(out, table.concat(words, " "))
		end
	end
	table.sort(out)
	table.insert(out, "beating " .. tostring(powers.beating()))
	local done: { string } = {}
	for k, n in counts do
		table.insert(done, k .. " " .. tostring(n))
	end
	table.sort(done)
	table.insert(out, "done: " .. (if #done > 0 then table.concat(done, ", ") else "nothing"))
	return out
end

-- "dd powers clear": the counts back to none.
function powers.clear()
	table.clear(counts)
end

-- How many powered monsters have a beat now (for "dd powers").
function powers.beating(): number
	local n = 0
	for _ in live do
		n += 1
	end
	return n
end

return powers
