--!strict
-- The server characters' records for the host (nox.records: there only on a host that keeps the
-- mode's records; nil everywhere else, and then nothing here does anything). A host may show them,
-- best first, wherever it likes.
--
-- Only server characters (the mode option characters=server, their keys "srv/..."): a character
-- kept anywhere else is its own player's word, so it is never put.
--
-- A row holds what may be shown of a character and nothing more: its name and class, its level and
-- experience, the deepest place it has reached (its difficulty tier, act, the area's number in that
-- act, and `place`, the area counted on through the acts, for ordering), and `bosses`, how many
-- acts' bosses it has killed. The host stamps it with the time it was put. No account, key or PC is
-- in a row (the row's key, the character's own, stays with the host).
--
-- A row is looked at as the character's record is written (hero.save: a level up, an act boss
-- killed...) and as the party enters an area (hero.reached), and put only when what it shows has
-- moved on since this game last put it: a level up, a new deepest area, an act boss killed (and once
-- in each game it plays, the first time). Its experience goes with it as it stood then. One record
-- read each time; nothing walks the records.

local acts = require("data/acts")

local records = {}

-- Whether this game puts rows (a host that keeps records, server characters).
local function active(): boolean
	return nox.records ~= nil and nox.option("characters") == "server"
end

-- The deepest place a record has reached: tier, act, area (in the act), place (on through the
-- acts); all 0 but tier and act 1 when it has reached none.
function records.deepest(rec: any): (number, number, number, number)
	local tier, act, area = 1, 1, 0
	local w = rec.world
	if type(w) == "table" and type(w.progress) == "table" then
		for key, p in w.progress do
			local t, a = string.match(tostring(key), "^(%d+)-(%d+)$")
			local tn, an = tonumber(t), tonumber(a)
			local d = if type(p) == "table" and type(p.deepest) == "number" then p.deepest else 0
			if tn and an and d > 0 then
				if area == 0 or tn > tier or (tn == tier and (an > act or (an == act and d > area))) then
					tier, act, area = tn, an, d
				end
			end
		end
	end
	local place = area
	if area > 0 then
		for a = 1, act - 1 do
			local def = (acts :: any)[a]
			place += if def then #def.areas else 0
		end
	end
	return tier, act, area, place
end

-- How many acts' bosses a record has killed (each act once, however often).
function records.bosses(rec: any): number
	local n = 0
	local w = rec.world
	if type(w) == "table" and type(w.bosses) == "table" then
		for _, kills in w.bosses do
			if type(kills) == "number" and kills > 0 then
				n += 1
			end
		end
	end
	return n
end

-- A character's row.
function records.row(rec: any): any
	local tier, act, area, place = records.deepest(rec)
	return {
		name = string.sub(tostring(rec.identity.name), 1, 64),
		class = tostring(rec.identity.class),
		level = rec.level.level,
		experience = rec.level.experience,
		tier = tier,
		act = act,
		area = area,
		place = place,
		bosses = records.bosses(rec),
	}
end

-- A hero's record was written, or it reached an area: its row put when what it shows has moved on.
function records.note(h: any)
	local key = h.key
	if not key or string.sub(key, 1, 4) ~= "srv/" or not active() then
		return
	end
	local row = records.row(h.record)
	local sig = string.format("%d|%d|%d|%d|%d", row.level, row.tier, row.act, row.area, row.bosses)
	if h.boardSig == sig then
		return
	end
	local r = nox.records
	assert(r, "no records")
	local ok, err = r.put(key, row)
	if ok then
		h.boardSig = sig
	elseif h.boardSig ~= "refused" then
		h.boardSig = "refused" -- (told once a game; tried again as it moves on)
		nox.log("deep_delve: " .. row.name .. "'s record row not put: " .. tostring(err))
	end
end

return records
