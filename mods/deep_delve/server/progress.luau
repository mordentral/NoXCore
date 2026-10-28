--!strict
-- A character's progress through the acts, kept in its record's world (store.luau's version 2):
--
--   world.progress["<tier>-<act>"] = { deepest, cleared, waypoints = { area indices, sorted } }
--   world.bosses["<act>"]          = how many times it has killed the act's boss
--   world.deepest                  = the deepest area index reached in any act
--
-- Everything here reads or writes one record; nothing walks the world.

local progress = {}

-- The record's progress in an act at its difficulty tier (made when it's new).
function progress.of(rec: any, act: number): any
	local w = rec.world
	local key = tostring(rec.identity.tier or 1) .. "-" .. tostring(act)
	local p = w.progress[key]
	if not p then
		p = { deepest = 0, cleared = false, waypoints = {} }
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

-- Area `index` of the act reached.
function progress.reached(rec: any, act: number, index: number)
	local p = progress.of(rec, act)
	if index > p.deepest then
		p.deepest = index
	end
	if index > rec.world.deepest then
		rec.world.deepest = index
	end
end

-- The act's boss killed: the act cleared, the kill counted.
function progress.cleared(rec: any, act: number)
	progress.of(rec, act).cleared = true
	local key = tostring(act)
	rec.world.bosses[key] = (rec.world.bosses[key] or 0) + 1
end

function progress.isCleared(rec: any, act: number): boolean
	return progress.of(rec, act).cleared == true
end

return progress
