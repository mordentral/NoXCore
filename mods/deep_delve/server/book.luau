--!strict
-- The book (design 4.5, by decision 2026-10-27): a character has only the spells its passive tree
-- gives (a warrior: only its abilities), never a network game's whole book at level 3.
--
-- Whenever a hero is set up (its first spawn, after a death, in each new map) and whenever its
-- tree changes, its book is cleared (nox.clearBook) and only what the tree gives is granted
-- (nox.grantSpell at the tree's level; a warrior's abilities with nox.setAbility). Its client is
-- told what goes (nox.revoke, sent 30 a frame by nox.sendRevokes) and what comes (the grants'
-- own messages). The record's `spells` keeps the result, by name.
--
-- What its client holds: a network game's client starts with its class's whole book, so the
-- first telling of a session, and each after a map change or a client's ready, takes away every
-- spell of the class's book (every ability, for a warrior) that the tree doesn't give. Between
-- those, only what the client was told before and the tree no longer gives is taken away. A
-- spell a node has just given or raised is told as a learned spell (its line, the book open on
-- it, the spell flown to the client's bar); every other grant is told quietly.
--
-- A revoke waits in the toolbox's queue (a player between maps gets it once it's in), while a
-- grant goes at once; a spell the tree gives is never revoked in the same telling. Should the
-- tree give again a spell whose revoke from an earlier telling is still queued, its grant is told
-- again once the queue is empty.

local data = require("data/tree")
local tree = require("tree")

local book = {}

local REVOKE_SPELL = 0x11
local REVOKE_ABILITY = 0x12

-- Each caster class's book on a network game's client: the spells whose class flags are its
-- own or any caster's (worked out once a class).
local classBooks: { [string]: { number } } = {}
local function classBook(class: string): { number }
	local list = classBooks[class]
	if list then
		return list
	end
	list = {}
	local bits = (data.classFlags :: any)[class]
	if bits then
		for id = 1, 136 do
			local info = nox.spellInfo(id)
			if info and bit32.band(info.flags, bits + data.anyCaster) ~= 0 then
				table.insert(list, id)
			end
		end
	end
	classBooks[class] = list
	return list
end
book.classBook = classBook

-- ---- The revokes' queue

local pump: number? = nil
-- Heroes with revokes queued, and whether each must be told its grants again once they're out.
local waiting: { [any]: boolean } = {}

local function tellGrants(h: any)
	local told = h.told
	if not told or not nox.present(h.unit) then
		return
	end
	for id, level in told.spells do
		nox.grantSpell(h.unit, id, level, false, false)
	end
	for a, level in told.abilities do
		nox.setAbility(h.unit, a, level, true)
	end
end

local function flush()
	if nox.sendRevokes() > 0 then
		return
	end
	if pump then
		nox.cancelTimer(pump)
		pump = nil
	end
	for h, again in waiting do
		h.revoking = nil
		if again then
			tellGrants(h)
		end
	end
	table.clear(waiting)
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
	local spells: { [number]: number } = {}
	for name, level in want.spells do
		local id = (data.spells :: any)[name]
		if id then
			spells[id] = level
		end
	end
	local abilities: { [number]: number } = want.abilities
	nox.clearBook(h.unit)
	-- What goes.
	local told = h.told
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
				end
			end
		end
	end
	-- What comes. A spell the tree has just given or raised (since the last telling in this map)
	-- is told as the game tells a learned spell: the line, the book opened on it, the spell flown
	-- to the bar.
	local again = false
	local queued = h.revoking or {}
	for id, level in spells do
		local learned = told ~= nil and (told.spells[id] or 0) < level
		nox.grantSpell(h.unit, id, level, learned, learned)
		again = again or queued[REVOKE_SPELL * 1000 + id] == true
	end
	for a, level in abilities do
		nox.setAbility(h.unit, a, level, true)
		again = again or queued[REVOKE_ABILITY * 1000 + a] == true
	end
	if again then
		waiting[h] = true
	end
	h.told = { spells = spells, abilities = abilities }
	-- The record's: spells and abilities by name.
	local names: { [string]: number } = {}
	for name, level in want.spells do
		names[name] = level
	end
	local skills: { [string]: number } = {}
	for a, level in abilities do
		local def = tree.ability(a)
		skills[if def then def.key else tostring(a)] = level
	end
	rec.spells = { spells = names, abilities = skills }
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
			wantIds[(data.spells :: any)[name] or 0] = level
		end
		for id = 1, 136 do
			local have = spells[id] or 0
			local should = wantIds[id] or 0
			if have ~= should then
				table.insert(out, string.format("spell %d: %d, the tree's %d", id, have, should))
			end
		end
	end
	if #out == 0 then
		return { "ok" }
	end
	return out
end

return book
