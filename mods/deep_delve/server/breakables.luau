--!strict
-- Breakable things (by decision 2026-11-11: every breakable in an area has a drop table): the
-- barrels, coffins and the rest of the game's kinds that break into pieces (data/loot.luau's
-- breakables.kinds) standing in an area are found once as it's entered (one walk of each kind, at
-- set-up), and those outside the town's walls watched for their death (nox.events.death). One
-- broken gives what its kind's drop table says (loot.dropBreakable: a chance of one small find,
-- gold or a potion, at the area level) where it stood, rolled more times for a party as a death's
-- loot is (by decision 2026-11-12: loot.rollsFor). The town's give nothing: they are never
-- watched.

local data = require("data/loot")
local loot = require("loot")
local town = require("town")

local breakables = {}

-- The watched ones of this map: id -> its kind; the area's level; what was found and given (for
-- "dd breakables"), by kind: { watched, town, broken, gave }.
local watched: { [number]: string } = {}
local level = 1
local counts: { [string]: any } = {}

local function countOf(kind: string): any
	local c = counts[kind]
	if not c then
		c = { watched = 0, town = 0, broken = 0, gave = 0 }
		counts[kind] = c
	end
	return c
end

-- A new map: the old one's went with it.
function breakables.reset()
	table.clear(watched)
	table.clear(counts)
end

-- An area entered (its town placed, so the town's walls are known; `area` areas.info's): its
-- breakables found and those outside the town watched.
function breakables.enter(area: any)
	breakables.reset()
	level = math.max(1, area.level or 1)
	for _, kind in data.breakables.kinds do
		for _, id in nox.objects(kind) do
			local o = nox.object(id)
			if o and nox.living(id) then
				if town.contains(o.x, o.y) then
					countOf(kind).town += 1
				else
					watched[id] = kind
					countOf(kind).watched += 1
					nox.watchEvents(id, nox.events.death)
				end
			end
		end
	end
end

-- A watched thing died (main.luau's onObjectKilled): one of ours, what its kind gives put where it
-- stood. Whether it was one.
function breakables.killed(e: any): boolean
	local kind = watched[e.id]
	if not kind then
		return false
	end
	watched[e.id] = nil
	local c = countOf(kind)
	c.broken += 1
	c.gave += #loot.dropBreakable(kind, e.x, e.y, level)
	return true
end

-- "dd breakables": "level L", then a line a kind found, "KIND watched W town T broken B gave G"
-- (sorted).
function breakables.lines(): { string }
	local out: { string } = {}
	for kind: string, c: any in counts do
		local line: string = string.format("%s watched %d town %d broken %d gave %d", kind, c.watched, c.town, c.broken, c.gave)
		table.insert(out, line)
	end
	table.sort(out)
	table.insert(out, 1, "level " .. tostring(level))
	return out
end

-- "dd breakables odds KIND N [LEVEL]": N breaks of KIND rolled as they'd break now, for the party
-- in the game (records only, nothing made), as "rolls N gold G potion P nothing X": the finds of
-- each kind in all (a party's break may give more than one), and the breaks that gave nothing.
function breakables.odds(kind: string, n: number, at: number): string
	local gold, potion, nothing = 0, 0, 0
	for _ = 1, n do
		local finds = loot.rollBreakables(kind, at)
		if #finds == 0 then
			nothing += 1
		end
		for _, d in finds do
			if d.kind == "gold" then
				gold += 1
			else
				potion += 1
			end
		end
	end
	return string.format("rolls %d gold %d potion %d nothing %d", n, gold, potion, nothing)
end

return breakables
