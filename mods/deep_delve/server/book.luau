--!strict
-- The book (design 4.5, by decision 2026-10-27): a character has only the spells its passive tree
-- gives (a warrior: only its abilities; a conjurer also the creatures it summons), never a network
-- game's whole book at level 3.
--
-- Whenever a hero is set up (its first spawn, after a death, in each new map) and whenever its
-- tree changes, its book is cleared (nox.clearBook: spells, abilities and field guide) and only
-- what the tree gives is granted: spells at the tree's level (nox.grantSpell), a warrior's
-- abilities (nox.setAbility), a conjurer's creatures in its field guide (nox.grantCreature) with
-- the summon spell that calls each (at level 1: a summon's level changes nothing). Its client is
-- told what goes (nox.revoke, sent 30 a frame by nox.sendRevokes) and what comes (the grants' own
-- messages). The record's `spells` keeps the result, by name.
--
-- The game's book is the mod's (main.luau's bookIsTheMod): the server starts a player with
-- nothing, lets it use only what is granted here, at the level granted, and tells a NoX Core
-- client of level 2 or above to list only that. A client below that (none joins Deep Delve today:
-- it takes no stock client) still starts with its class's whole book, so the sweep below stays.
--
-- What its client holds: a network game's client starts with its class's whole book, so the
-- first telling of a session, and each after a map change or a client's ready, takes away every
-- spell of the class's book (every ability, for a warrior; for a conjurer every guide creature
-- too) that the tree doesn't give. Between those, only what the client was told before and the
-- tree no longer gives is taken away. A spell, ability or creature a node has just given or raised
-- is told as a learned one (its line, the book open on it, a spell or ability flown to the
-- client's bar); every other grant is told quietly. A summon spell is told with its creature.
--
-- Linked spells: a general spell's revoke takes its linked spells off a client too (Summon
-- Creature's, every summon spell). Summon Creature is never granted and always swept: a client
-- that lists only what was granted summons the guide's creatures without it. Its revoke reaches
-- the client after the summon spells' grants, so the book is told again once it has gone (below).
--
-- A revoke waits in the toolbox's queue (a player between maps gets it once it's in), while a
-- grant goes at once; what the tree gives is never revoked in the same telling. Should the tree
-- give again something whose revoke from an earlier telling is still queued (or its general
-- spell's), the book is told again once the queue is empty.

local data = require("data/tree")
local tree = require("tree")

local book = {}

local REVOKE_SPELL = 0x11
local REVOKE_ABILITY = 0x12
local REVOKE_CREATURE = 0x13

-- A spell's flags (thing.bin's), read once a spell.
local flagCache: { [number]: number } = {}
local function flagsOf(id: number): number
	local f = flagCache[id]
	if f == nil then
		local info = nox.spellInfo(id)
		f = if info then info.flags else 0
		flagCache[id] = f
	end
	return f
end

-- The linked groups: a general spell's flag, and the flag its linked spells carry.
local LINKS = { { 0x1000, 0x2000 }, { 0x4000, 0x8000 }, { 0x10000, 0x20000 } }

-- Whether `member` is one of general spell `leader`'s linked spells.
local function linked(leader: number, member: number): boolean
	local lf, mf = flagsOf(leader), flagsOf(member)
	for _, l in LINKS do
		if bit32.band(lf, l[1]) ~= 0 and bit32.band(mf, l[2]) ~= 0 then
			return true
		end
	end
	return false
end

-- Each caster class's book on a network game's client: the spells whose class flags are its
-- own or any caster's; and the general spells among them (worked out once a class).
local classBooks: { [string]: { number } } = {}
local classLeaders: { [string]: { number } } = {}
local function classBook(class: string): { number }
	local list = classBooks[class]
	if list then
		return list
	end
	list = {}
	local leaders = {}
	local bits = (data.classFlags :: any)[class]
	if bits then
		for id = 1, 136 do
			local info = nox.spellInfo(id)
			if info and bit32.band(info.flags, bits + data.anyCaster) ~= 0 then
				table.insert(list, id)
				if bit32.band(info.flags, 0x1000 + 0x4000 + 0x10000) ~= 0 then
					table.insert(leaders, id)
				end
			end
		end
	end
	classBooks[class] = list
	classLeaders[class] = leaders
	return list
end
book.classBook = classBook

local function leadersOf(class: string): { number }
	classBook(class)
	return classLeaders[class]
end

-- ---- The revokes' queue

local pump: number? = nil
-- Heroes with revokes queued, and whether each must be told its book again once they're out.
local waiting: { [any]: boolean } = {}

local function flush()
	if nox.sendRevokes() > 0 then
		return
	end
	if pump then
		nox.cancelTimer(pump)
		pump = nil
	end
	local again = {}
	for h, a in waiting do
		h.revoking = nil
		if a then
			table.insert(again, h)
		end
	end
	table.clear(waiting)
	for _, h in again do
		-- (a hero between maps is told everything at its set-up there)
		if h.told and h.ready then
			book.apply(h)
		end
	end
end

local function revoke(h: any, name: string, kind: number, id: number)
	nox.revoke(name, kind, id)
	h.revoking = h.revoking or {}
	h.revoking[kind * 1000 + id] = true
	if waiting[h] == nil then
		waiting[h] = false
	end
	if not pump then
		pump = nox.every(1, flush)
	end
end

-- ---- Telling

-- The hero's book made the tree's, its client told. `fresh`: what its client holds isn't known
-- (a new session, a new map, its ready), so the class's whole book is swept.
function book.apply(h: any, fresh: boolean?)
	local rec = h.record
	local class = rec.identity.class or "warrior"
	local p = nox.player(h.peer)
	if not p or not nox.present(h.unit) then
		return
	end
	if fresh then
		h.told = nil
	end
	local want = tree.wanted(rec)
	-- The spells by number: the tree's, and its creatures' summon spells (told with the creature,
	-- never as a learned spell).
	local spells: { [number]: number } = {}
	local quiet: { [number]: boolean } = {}
	for name, level in want.spells do
		local id = tree.spellId(name)
		if id then
			spells[id] = level
		end
	end
	local creatures: { [number]: string } = {}
	for name in want.creatures do
		local n, id = tree.guideNumber(name), tree.summonSpell(name)
		if n and id then
			creatures[n] = name
			spells[id] = 1
			quiet[id] = true
		end
	end
	local abilities: { [number]: number } = want.abilities
	nox.clearBook(h.unit)
	-- What comes. What the tree has just given or raised (since the last telling in this map) is
	-- told as the game tells a learned one: the line, the book opened on it, a spell or ability
	-- flown to the bar.
	local told = h.told
	local queued = h.revoking or {}
	local leaders = leadersOf(class)
	local again = false
	for id, level in spells do
		local learned = not quiet[id] and told ~= nil and (told.spells[id] or 0) < level
		nox.grantSpell(h.unit, id, level, learned, learned)
		again = again or queued[REVOKE_SPELL * 1000 + id] == true
		for _, l in leaders do
			if queued[REVOKE_SPELL * 1000 + l] and linked(l, id) then
				again = true
			end
		end
	end
	for a, level in abilities do
		local learned = told ~= nil and (told.abilities[a] or 0) < level
		nox.setAbility(h.unit, a, level, true, learned)
		again = again or queued[REVOKE_ABILITY * 1000 + a] == true
	end
	for n, name in creatures do
		local learned = told ~= nil and not told.creatures[n]
		local ok, err = pcall(nox.grantCreature, h.unit, name, learned)
		if not ok then
			nox.log("deep_delve: the tree's creature " .. name .. " not given: " .. tostring(err))
		end
		again = again or queued[REVOKE_CREATURE * 1000 + n] == true
	end
	-- What the field guide holds now (a creature's group would come with it).
	local guide: { [number]: boolean } = {}
	for n, has in nox.fieldGuide(h.unit) do
		if has then
			guide[n] = true
		end
	end
	-- What goes.
	if class == "warrior" then
		if told then
			for a in told.abilities do
				if not abilities[a] then
					revoke(h, p.name, REVOKE_ABILITY, a)
				end
			end
		else
			for a = 1, 5 do
				if not abilities[a] then
					revoke(h, p.name, REVOKE_ABILITY, a)
				end
			end
		end
	else
		if told then
			for id in told.spells do
				if not spells[id] then
					revoke(h, p.name, REVOKE_SPELL, id)
				end
			end
		else
			for _, id in classBook(class) do
				if not spells[id] then
					revoke(h, p.name, REVOKE_SPELL, id)
					-- (a general spell's revoke sets its linked spells to 0 on the client when it
					-- arrives, after this telling's grants: those the tree gives are told again)
					if not again and table.find(leaders, id) then
						for given in spells do
							if linked(id, given) then
								again = true
								break
							end
						end
					end
				end
			end
		end
	end
	if told then
		for n in told.creatures do
			if not guide[n] then
				revoke(h, p.name, REVOKE_CREATURE, n)
			end
		end
	elseif class == "conjurer" then
		for n = 1, data.guideSize do
			if not guide[n] then
				revoke(h, p.name, REVOKE_CREATURE, n)
			end
		end
	end
	if again then
		waiting[h] = true
	end
	h.told = { spells = spells, abilities = abilities, creatures = guide }
	-- The record's: spells, abilities and creatures by name.
	local names: { [string]: number } = {}
	for name, level in want.spells do
		names[name] = level
	end
	local skills: { [string]: number } = {}
	for a, level in abilities do
		local def = tree.ability(a)
		skills[if def then def.key else tostring(a)] = level
	end
	local kept: { string } = {}
	for _, name in creatures do
		table.insert(kept, name)
	end
	table.sort(kept)
	rec.spells = { spells = names, abilities = skills, creatures = kept }
end

-- A hero gone: nothing more is told for it.
function book.forget(h: any)
	waiting[h] = nil
end

-- What the hero's unit's book holds against what the tree gives: "ok", or the differences (for
-- "dd book"). (The game keeps a warrior's abilities where a caster's spells 1-5 are, so a
-- warrior's book is its abilities and a caster's its spells.)
function book.check(h: any): { string }
	local want = tree.wanted(h.record)
	local out = {}
	if (h.record.identity.class or "warrior") == "warrior" then
		for a = 1, 5 do
			local have = nox.abilityLevel(h.unit, a)
			local should: number = want.abilities[a] or 0
			if have ~= should then
				table.insert(out, "ability " .. a .. ": " .. have .. ", the tree's " .. should)
			end
		end
		local spells = nox.spells(h.unit)
		for id = 6, 136 do
			if (spells[id] or 0) ~= 0 then
				table.insert(out, string.format("spell %d: %d, the tree's 0", id, spells[id]))
			end
		end
	else
		local spells = nox.spells(h.unit)
		local wantIds: { [number]: number } = {}
		for name, level in want.spells do
			wantIds[tree.spellId(name) or 0] = level
		end
		for name in want.creatures do
			wantIds[tree.summonSpell(name) or 0] = 1
		end
		for id = 1, 136 do
			local have = spells[id] or 0
			local should = wantIds[id] or 0
			if have ~= should then
				table.insert(out, string.format("spell %d: %d, the tree's %d", id, have, should))
			end
		end
	end
	local wantGuide: { [number]: boolean } = {}
	for name in want.creatures do
		wantGuide[tree.guideNumber(name) or 0] = true
	end
	for n, has in nox.fieldGuide(h.unit) do
		if has ~= (wantGuide[n] == true) then
			table.insert(out, string.format("creature %d: %s, the tree's %s", n, if has then "known" else "unknown",
				if wantGuide[n] then "known" else "unknown"))
		end
	end
	if #out == 0 then
		return { "ok" }
	end
	return out
end

return book
