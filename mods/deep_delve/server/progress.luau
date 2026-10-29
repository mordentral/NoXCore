--!strict
-- A character's progress through the acts, kept in its record's world (store.luau's versions 2
-- and 5):
--
--   world.progress["<tier>-<act>"] = { deepest, cleared, opened, waypoints = { area indices,
--                                     sorted } }
--   world.bosses["<act>"]          = how many times it has killed the act's boss
--   world.deepest                  = the deepest area index reached in any act
--   world.act                      = the furthest act it has reached or opened at its tier
--
-- An act is opened for a character when it clears the act before it (its boss's kill) at that
-- tier, or when it arrives in one of its areas (a party member who followed through the gate).
-- Act 1 is always open. The waypoints window lists the opened acts.
--
-- Everything here reads or writes one record; nothing walks the world.

local acts = require("data/acts")

local progress = {}

local function keyOf(rec: any, act: number): string
	return tostring(rec.identity.tier or 1) .. "-" .. tostring(act)
end

-- The record's progress in an act at its difficulty tier (made when it's new).
function progress.of(rec: any, act: number): any
	local w = rec.world
	local key = keyOf(rec, act)
	local p = w.progress[key]
	if not p then
		p = { deepest = 0, cleared = false, opened = act == 1, waypoints = {} }
		w.progress[key] = p
	end
	return p
end

-- Whether it has found the waypoint of area `index` of the act.
function progress.hasWaypoint(rec: any, act: number, index: number): boolean
	return table.find(progress.of(rec, act).waypoints, index) ~= nil
end

-- The waypoint of area `index` found; whether it's new.
function progress.addWaypoint(rec: any, act: number, index: number): boolean
	local list = progress.of(rec, act).waypoints
	if table.find(list, index) then
		return false
	end
	table.insert(list, index)
	table.sort(list)
	return true
end

-- The act's waypoints found, in area order.
function progress.waypoints(rec: any, act: number): { number }
	return progress.of(rec, act).waypoints
end

-- The act opened for the character at its tier (the act it has reached moved on with it).
function progress.open(rec: any, act: number)
	progress.of(rec, act).opened = true
	if act > (rec.world.act or 1) then
		rec.world.act = act
	end
end

-- Whether the act is open to the character at its tier (act 1 always; a later one once the act
-- before it is cleared, or once it has been in it). Reading makes no entry.
function progress.isOpen(rec: any, act: number): boolean
	if act == 1 then
		return true
	end
	local p = rec.world.progress[keyOf(rec, act)]
	return p ~= nil and p.opened == true
end

-- Area `index` of the act reached (the act opened with it).
function progress.reached(rec: any, act: number, index: number)
	local p = progress.of(rec, act)
	if index > p.deepest then
		p.deepest = index
	end
	if index > rec.world.deepest then
		rec.world.deepest = index
	end
	if not p.opened or act > (rec.world.act or 1) then
		progress.open(rec, act)
	end
end

-- The act's boss killed: the act cleared, the kill counted, the next act (if there is one) opened.
function progress.cleared(rec: any, act: number)
	progress.of(rec, act).cleared = true
	local key = tostring(act)
	rec.world.bosses[key] = (rec.world.bosses[key] or 0) + 1
	if acts[act + 1] then
		progress.open(rec, act + 1)
	end
end

function progress.isCleared(rec: any, act: number): boolean
	local p = rec.world.progress[keyOf(rec, act)]
	return p ~= nil and p.cleared == true
end

-- Every act after one the character has cleared at its tier opened (an act built after the
-- record's clear: done as each record is read, and by the step to version 5); whether anything
-- changed.
function progress.openAfterCleared(rec: any): boolean
	local changed = false
	for a = 1, #acts - 1 do
		if progress.isCleared(rec, a) and not progress.isOpen(rec, a + 1) then
			progress.open(rec, a + 1)
			changed = true
		end
	end
	for a = 1, #acts do
		if progress.isOpen(rec, a) and a > (rec.world.act or 1) then
			rec.world.act = a
			changed = true
		end
	end
	return changed
end

return progress
