--!strict
-- A character's atlas: the expeditions it has cleared (store.luau's version 6), kept in its record:
--
--   atlas.open     the map table is open to it (set once it has cleared the last act: kept, so an
--                  act added later doesn't shut it again)
--   atlas.best     the highest rank it has cleared, in any biome (0: none)
--   atlas.runs     how many expeditions it has cleared
--   atlas.cleared  biome name -> the ranks it has cleared there, sorted
--
-- The ladder (data/expeditions.luau): rank 1 is open to anyone the table is open to; rank N + 1
-- opens once rank N is cleared in any biome. A rank's area level counts on from the last act's
-- last area, so a new act moves the ladder with it.
--
-- Everything here reads or writes one record; nothing walks the world.

local acts = require("data/acts")
local data = require("data/expeditions")
local progress = require("progress")

local atlas = {}

-- The last act's last area level (the ladder stands on it: data/acts.luau's tierOffset).
function atlas.lastLevel(): number
	local offset: number? = (acts :: any).tierOffset
	if offset then
		return offset
	end
	local last = acts[#acts]
	return last.base + #last.areas - 1
end

-- How many ranks the ladder has.
function atlas.ranks(): number
	return data.ranks.count
end

-- Rank `rank`'s area level.
function atlas.level(rank: number): number
	return atlas.lastLevel() + data.ranks.first + (rank - 1) * data.ranks.step
end

-- A new record's atlas (and an older one's, by store.luau's step to version 6).
function atlas.fresh(): any
	return { open = false, best = 0, runs = 0, cleared = {} }
end

-- The record's atlas (made when it has none).
function atlas.of(rec: any): any
	local a = rec.atlas
	if type(a) ~= "table" then
		a = atlas.fresh()
		rec.atlas = a
	end
	if type(a.cleared) ~= "table" then
		a.cleared = {}
	end
	if type(a.best) ~= "number" then
		a.best = 0
	end
	if type(a.runs) ~= "number" then
		a.runs = 0
	end
	return a
end

-- The table opened for the character when it has cleared the last act (at its tier); whether
-- that changed anything.
function atlas.note(rec: any): boolean
	local a = atlas.of(rec)
	if a.open ~= true and progress.isCleared(rec, #acts) then
		a.open = true
		return true
	end
	return false
end

-- Whether the map table is open to the character.
function atlas.isOpen(rec: any): boolean
	local a = rec.atlas
	return (type(a) == "table" and a.open == true) or progress.isCleared(rec, #acts)
end

-- The highest rank open to the character (0: the table isn't open to it).
function atlas.top(rec: any): number
	if not atlas.isOpen(rec) then
		return 0
	end
	return math.min(data.ranks.count, atlas.of(rec).best + 1)
end

-- Whether rank `rank` is open to the character.
function atlas.rankOpen(rec: any, rank: number): boolean
	return rank >= 1 and rank <= atlas.top(rec)
end

-- Whether it has cleared `biome` at `rank`.
function atlas.hasCleared(rec: any, biome: string, rank: number): boolean
	local list = atlas.of(rec).cleared[biome]
	return type(list) == "table" and table.find(list, rank) ~= nil
end

-- An expedition cleared: kept in the atlas (the table opened with it); whether this biome and rank
-- is a first clear.
function atlas.cleared(rec: any, biome: string, rank: number): boolean
	local a = atlas.of(rec)
	a.open = true
	a.runs += 1
	if rank > a.best then
		a.best = rank
	end
	local list = a.cleared[biome]
	if type(list) ~= "table" then
		list = {}
		a.cleared[biome] = list
	end
	if table.find(list, rank) then
		return false
	end
	table.insert(list, rank)
	table.sort(list)
	return true
end

return atlas
