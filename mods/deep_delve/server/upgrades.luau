--!strict
-- The stock abilities' upgrades (by decision 2026-11-02): a choice of two beside each ability's
-- last node on the tree (data/tree.luau's `excludes`: one of each pair at a time, tree.canTake),
-- skill nodes (a warrior's alone; another class gets the stats' worth). Their numbers are
-- data/warrior.luau's skills; Ambush and Hunter's Mark, Tread Lightly's and Eye of the Wolf's,
-- are warrior.luau's (they read the warrior's own enchantments).
--
-- Each works off the engine's news of a warrior's stock ability (mode.onAbility, told at the next
-- frame's start), so nothing about the ability itself changes, and nothing at all for a warrior
-- without the upgrade or for any other class:
--   Berserker Charge  Trample: its end against a creature or a wall ("hit", "wall") strikes the
--                     creature it hit and the monsters about where it stopped, and knocks them
--                     aside off the charge's line (its facing). The game's charge stops at the
--                     first creature it meets, so this is an impact where it stops.
--                     Blood Rush: a kill by any blow of the warrior's within a few seconds of the
--                     charge's end (any end but its death) readies it again, once a charge
--                     (nox.setAbilityCooldown 0). The game already readies it on a kill by the
--                     charge itself.
--   War Cry           Rallying Cry: its start gives every party member about the warrior (the
--                     warrior too) more damage and less taken for a while (hero.rally, counted
--                     by hero.dealt and hero.taken) and heals them a little.
--                     Terror: its start frightens every monster about the warrior (the dread
--                     affix's fear: affixes.frighten; never a boss).
--   Harpoon           Reel and Rend: the creature it hooks bleeds (Rend's bleed, warrior.bleed)
--                     and is held a moment once the pull ends (the hold would stop the pull).
--                     Chain: as it hooks a monster, the nearest other near it is struck and
--                     pulled toward the warrior.
--
-- Their hits are the warrior's skill hits (warrior.strike: quiet, the mod's monsters alone, so
-- never a party member or its creatures), shares of the warrior's blow as the actives' are
-- (actives.blow x hero.dealt); Execute and Hunter's Mark count on each (warrior.factor).

local data = require("data/warrior")
local affixes = require("affixes")
local actives = require("actives")
local hero = require("hero")
local monsters = require("monsters")
local warrior = require("warrior")

local upgrades = {}

local FPS = 30
local U: any = data.skills
-- The upgrades' skill keys, by the ability they upgrade.
local KEYS = { "trample", "bloodRush", "rallyingCry", "terror", "reelAndRend", "chain" }

-- A hero's own state for its upgrades.
local function state(h: any): any
	local s = h.upgradeState
	if not s then
		s = { counts = {} }
		h.upgradeState = s
	end
	return s
end

local function count(s: any, what: string, n: number?)
	s.counts[what] = (s.counts[what] or 0) + (n or 1)
end

-- Whether the hero has an upgrade (its skills: a warrior's alone).
local function has(h: any, key: string): boolean
	return h.bonus ~= nil and h.bonus.skills ~= nil and h.bonus.skills[key] ~= nil
end

-- `pct` of the warrior's blow (its damage bonus in it, Rallying Cry's too).
local function share(h: any, pct: number): number
	return actives.blow(h) * hero.dealt(h, nil) * pct / 100
end

-- A skill hit on one of the mod's monsters (Execute and Hunter's Mark on it); what it took.
local function hit(h: any, id: number, amount: number, kind: number): number
	local before = nox.object(id)
	local was = if before then before.health else 0
	warrior.strike(h, id, amount * warrior.factor(h, id), kind)
	local after = nox.object(id)
	return math.max(0, was - (if after then after.health else 0))
end

local function isBoss(id: number): boolean
	local m = monsters.info(id)
	return m ~= nil and m.tier == "boss"
end

-- ---- Berserker Charge

-- Trample: the impact where the charge stopped (x, y), along the warrior's facing.
local function trample(h: any, s: any, x: number, y: number, target: number?, why: string)
	local t = U.trample
	local fx, fy = nox.directionVector(nox.direction(h.unit))
	-- (the creature it hit, and the monsters about where it stopped)
	local struck = warrior.monstersNear(x, y, t.reach, nil, 32)
	if target and monsters.info(target) and nox.living(target) and not table.find(struck, target) then
		table.insert(struck, 1, target)
	end
	nox.effect(data.fx.trample, x, y)
	local base = share(h, t.pct)
	local n, dealt = 0, 0
	for _, id in struck do
		local ox, oy = nox.position(id)
		dealt += hit(h, id, base, t.kind)
		nox.effect(data.fx.trampleHit, ox, oy)
		n += 1
		-- (knocked aside: away from the charge's line; one on the line to its left)
		if nox.living(id) and not isBoss(id) then
			local rx, ry = ox - x, oy - y
			local along = rx * fx + ry * fy
			local px, py = rx - along * fx, ry - along * fy
			local d = math.sqrt(px * px + py * py)
			if d < 4 then
				px, py, d = -fy, fx, 1
			end
			nox.applyForce(id, ox - px / d * 10, oy - py / d * 10, t.force)
		end
	end
	count(s, "trample")
	count(s, "trampleHit", n)
	s.lastTrample = { x = x, y = y, hit = n, dealt = dealt, base = base, why = why }
end

-- Blood Rush: a kill by the hero within its seconds of the charge's end readies the charge.
local function bloodRush(h: any, s: any)
	local at = s.chargeEnd
	if not at or s.rushed or nox.frame() - at > U.bloodRush.seconds * FPS then
		return
	end
	if nox.abilityCooldown(h.unit, "BerserkerCharge") <= 0 then
		return -- (ready already: the game's own reset on the charge's kill)
	end
	s.rushed = true
	nox.setAbilityCooldown(h.unit, "BerserkerCharge", 0)
	local x, y = nox.position(h.unit)
	nox.effect(data.fx.bloodRush, x, y)
	count(s, "bloodRush")
end

-- ---- War Cry

local function rallyingCry(h: any, s: any, x: number, y: number)
	local r = U.rallyingCry
	local now = nox.frame()
	local till = now + r.seconds * FPS
	local n = 0
	for _, other in hero.all() do
		local unit = other.unit
		if other.ready and nox.living(unit) then
			local ox, oy = nox.position(unit)
			if (ox - x) * (ox - x) + (oy - y) * (oy - y) <= r.reach * r.reach then
				local was = other.rally
				other.rally = { till = if was and was.till > till then was.till else till, damage = r.damage, resist = r.resist }
				local o = nox.object(unit)
				if o and o.maxHealth > 0 then
					nox.heal(unit, math.max(1, math.floor(o.maxHealth * r.heal / 100 + 0.5)))
				end
				nox.effect(data.fx.rally, ox, oy)
				nox.tell(unit, string.format("Rallied: +%d%% damage, %d%% less taken for %d seconds", r.damage, r.resist, r.seconds))
				n += 1
			end
		end
	end
	count(s, "rally")
	count(s, "rallied", n)
end

local function terror(h: any, s: any, x: number, y: number)
	local t = U.terror
	nox.effect(data.fx.terror, x, y)
	local n = 0
	for _, id in warrior.monstersNear(x, y, t.reach, nil, 64) do
		local m = monsters.info(id)
		if m and affixes.frighten(id, m, t.seconds * FPS) then
			n += 1
		end
	end
	count(s, "terror")
	count(s, "frightened", n)
end

-- ---- Harpoon

local function hooked(h: any, s: any, target: number)
	s.hooked = target
	if has(h, "reelAndRend") then
		local r = U.reelAndRend
		warrior.bleed(h, target, share(h, r.pct), r.seconds)
		count(s, "rend")
	end
	if has(h, "chain") then
		local c = U.chain
		local tx, ty = nox.position(target)
		local other = warrior.monstersNear(tx, ty, c.reach, target, 1)[1]
		if other then
			local ox, oy = nox.position(other)
			local base = share(h, c.pct)
			local dealt = hit(h, other, base, c.kind)
			nox.effect(data.fx.trampleHit, ox, oy)
			if nox.living(other) and not isBoss(other) then
				local wx, wy = nox.position(h.unit)
				nox.applyForce(other, wx, wy, -c.force)
			end
			count(s, "chain")
			s.lastChain = { id = other, dealt = dealt, base = base }
		end
	end
end

-- The pull over: Reel and Rend's hold on what it held.
local function released(h: any, s: any, why: string)
	local target = s.hooked
	s.hooked = nil
	if not target or why == "death" or not has(h, "reelAndRend") or not nox.living(target) or not monsters.info(target) then
		return
	end
	local r = U.reelAndRend
	local frames = if isBoss(target) then r.bossHeld else r.held
	if nox.enchantTime(target, "ENCHANT_HELD") < frames then
		nox.enchant(target, "ENCHANT_HELD", frames, 1)
	end
	local x, y = nox.position(target)
	nox.effect(data.fx.reel, x, y)
	count(s, "reel")
end

-- ---- The hooks

-- A warrior's stock ability's news (mode.onAbility).
function upgrades.used(unit: number, ability: string, info: any)
	local h = hero.byUnit(unit)
	if not h or not h.ready or type(info) ~= "table" then
		return
	end
	local s = state(h)
	local event = info.event
	count(s, "heard " .. ability .. " " .. tostring(event))
	local x: number, y: number = info.x, info.y
	if type(x) ~= "number" or type(y) ~= "number" then
		x, y = nox.position(unit)
	end
	if ability == "BerserkerCharge" then
		if event == "end" and info.reason ~= "death" then
			s.chargeEnd = nox.frame()
			s.rushed = false
			if has(h, "trample") and (info.reason == "hit" or info.reason == "wall") then
				trample(h, s, x, y, info.target, info.reason)
			end
		end
	elseif ability == "WarCry" then
		if event == "start" then
			if has(h, "rallyingCry") then
				rallyingCry(h, s, x, y)
			end
			if has(h, "terror") then
				terror(h, s, x, y)
			end
		end
	elseif ability == "Harpoon" then
		if event == "hooked" and info.target and nox.living(info.target) and monsters.info(info.target) then
			hooked(h, s, info.target)
		elseif event == "end" then
			released(h, s, tostring(info.reason))
		end
	end
end

-- One of the mod's monsters killed (main.luau's onObjectKilled): Blood Rush for the hero whose
-- blow it was (its unit, or a thing it owns: a thrown weapon).
function upgrades.killed(e: any)
	local by = e.by
	if not by or by == 0 then
		return
	end
	local h = hero.byUnit(by)
	if not h then
		local o = nox.object(by)
		h = if o and o.owner ~= 0 then hero.byUnit(o.owner) else nil
	end
	if h and has(h, "bloodRush") then
		bloodRush(h, state(h))
	end
end

-- A new map: no harpoon holds anything, no charge just ended.
function upgrades.reset()
	for _, h in hero.all() do
		local s = h.upgradeState
		if s then
			s.hooked = nil
			s.chargeEnd = nil
		end
	end
end

-- "dd upgrades": the first hero's upgrades held, their counts, the charge's cooldown, its Rallying
-- Cry, the last impact and chain, what its harpoon holds; "clear": the counts emptied.
function upgrades.console(args: { string }, h: any): { string }
	if not h then
		return { "no player" }
	end
	local s = state(h)
	if args[2] == "clear" then
		table.clear(s.counts)
		s.lastTrample = nil
		s.lastChain = nil
		return { "cleared" }
	end
	local out = {}
	for _, key in KEYS do
		if has(h, key) then
			table.insert(out, "upgrade " .. key)
		end
	end
	local what = {}
	for k in s.counts do
		table.insert(what, k)
	end
	table.sort(what)
	for _, k in what do
		table.insert(out, "count " .. string.gsub(k, " ", "_") .. " " .. tostring(s.counts[k]))
	end
	if h.record.identity.class == "warrior" then
		table.insert(out, "charge cooldown " .. tostring(nox.abilityCooldown(h.unit, "BerserkerCharge")))
	end
	local r = h.rally
	local left = if r then r.till - nox.frame() else 0
	table.insert(out, if r and left > 0 then string.format("rally %d %d left %d", r.damage, r.resist, left) else "rally none")
	local t: any = s.lastTrample
	if t then
		table.insert(out, string.format("trample %d %d hit %d dealt %d base %d %s", math.floor(t.x :: number), math.floor(t.y :: number), t.hit :: number,
			t.dealt :: number, math.floor((t.base :: number) + 0.5), tostring(t.why)))
	end
	local c: any = s.lastChain
	if c then
		table.insert(out, string.format("chain %d dealt %d base %d", c.id :: number, c.dealt :: number, math.floor((c.base :: number) + 0.5)))
	end
	table.insert(out, "hooked " .. (if s.hooked then tostring(s.hooked) else "none"))
	return out
end

return upgrades
