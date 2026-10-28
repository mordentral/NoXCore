--!strict
-- Votes to change area. A Nox game holds one map for everybody, so going to another area (the
-- area's exit, or a waypoint in another area) moves the whole game. With anyone else in the game,
-- wherever they are (out in the field, in town, dead), that is the party's vote:
--
--   * Asked by stepping on the exit or picking another area's waypoint; the asker's yes is counted.
--   * Every player in the game when it's asked answers it (on the party strip, or by saying yes or
--     no); every yes moves the game. A no, the asker calling it off, the asker leaving, or no answer
--     within numbers.vote.seconds ends it, and the asker may ask again after numbers.vote.again.
--   * A player who leaves is no longer waited for (the rest's yes is enough); one who joins after
--     it was asked isn't asked and goes along.
--   * One vote at a time; a new map ends it.
--
-- A player alone in the game goes at once. A waypoint in the area being played is no vote at all:
-- it moves only its player (waypoints.luau). The host's "dd area" moves the game without one.
--
-- What the strip shows (ui/main.luau): each member's answer comes with the party's numbers
-- (party.luau reads votes.mark); the vote itself, its asker, where to and the time left, is a
-- value of its own sent to each player as it changes.

local hero = require("hero")
local hud = require("hud")
local town = require("town")
local numbers = require("data/numbers")

local votes = {}

-- areas.luau's own (it requires this module and hands itself in).
local areas: any = nil

-- The vote under way: { serial, by (peer), byName, index, arrive, how, title, ballots = peer ->
-- "y" | "n" | "?", names = peer -> name, ends (frame), beat }, or nil.
local current: any = nil
local serial = 0
-- How the last vote ended (shown on the strip for a little while).
local lastResult: string? = nil
-- When each player's last vote failed (peer -> frame).
local failedAt: { [number]: number } = {}
local seconds: number = numbers.vote.seconds

function votes.attach(areasApi: any)
	areas = areasApi
end

-- Whether anyone but `h` is in the game (wherever they are: the field, the town, dead or still
-- loading the map).
function votes.othersIn(h: any): boolean
	for _, other in hero.all() do
		if other ~= h then
			return true
		end
	end
	return false
end

-- The vote's value for one player's UI.
local function valueFor(peer: number): any
	local c = current
	if not c then
		return { kind = "vote", s = serial, active = false, done = lastResult }
	end
	local yes, of = 0, 0
	for _, b in c.ballots do
		of += 1
		if b == "y" then
			yes += 1
		end
	end
	return {
		kind = "vote",
		s = c.serial,
		active = true,
		by = c.byName,
		to = c.title,
		how = c.how,
		left = math.max(0, math.ceil((c.ends - nox.frame()) / 30)),
		yes = yes,
		of = of,
		mine = c.ballots[peer],
		asker = c.by == peer,
	}
end

local function tellUis()
	for _, h in hero.all() do
		hud.send(h.peer, "vote", valueFor(h.peer))
	end
end

-- A player's UI told the vote again (its client ready in a new map).
function votes.resend(h: any)
	hud.send(h.peer, "vote", valueFor(h.peer))
end

local function finish(passed: boolean, text: string)
	local c = current
	if not c then
		return
	end
	current = nil
	nox.cancelTimer(c.beat)
	lastResult = text
	if not passed then
		failedAt[c.by] = nox.frame()
	end
	nox.textToAll("Deep Delve: " .. text)
	tellUis()
	if passed and areas then
		local ok, why = areas.travel(c.index, c.arrive)
		if not ok then
			nox.textToAll("Deep Delve: the way to " .. c.title .. " isn't open (" .. tostring(why) .. ").")
		end
	end
end

-- The ballots read: every yes goes, a no ends it, and so does the time running out.
local function check()
	local c = current
	if not c then
		return
	end
	local all = true
	local waiting: { string } = {}
	for peer, b in c.ballots do
		if b == "n" then
			finish(false, tostring(c.names[peer]) .. " said no: the party stays in " .. hud.currentArea().title .. ".")
			return
		elseif b ~= "y" then
			all = false
			table.insert(waiting, tostring(c.names[peer]))
		end
	end
	if all then
		finish(true, "everyone agreed: on to " .. c.title .. ".")
		return
	end
	if nox.frame() >= c.ends then
		table.sort(waiting)
		finish(false, "no answer from " .. table.concat(waiting, ", ") .. ": the vote to go to " .. c.title .. " lapsed.")
	end
end

-- A hero asks for the game to go to area `index` of the act, arriving at `arrive` ("start",
-- "waypoint"); `how` is how it asked ("exit", "waypoint"). With anyone else in the game a vote
-- starts; alone, the game goes at once. Whether it went or a vote started ("vote"), else why.
function votes.ask(h: any, index: number, arrive: string, how: string): (boolean, string?)
	if not areas then
		return false, "not now"
	end
	if current then
		if current.by ~= h.peer then
			town.say(h.unit, "A vote to change area is under way: answer it first.")
		end
		return false, "a vote is under way"
	end
	if not votes.othersIn(h) then
		return areas.travel(index, arrive)
	end
	local last = failedAt[h.peer]
	if last and nox.frame() - last < numbers.vote.again then
		return false, "asked too soon"
	end
	serial += 1
	local name = tostring(h.record.identity.name)
	local c: any = {
		serial = serial,
		by = h.peer,
		byName = name,
		index = index,
		arrive = arrive,
		how = how,
		title = areas.info(index).title,
		ballots = {},
		names = {},
		ends = nox.frame() + seconds * 30,
	}
	for _, other in hero.all() do
		c.ballots[other.peer] = if other == h then "y" else "?"
		c.names[other.peer] = tostring(other.record.identity.name)
	end
	c.beat = nox.every(numbers.vote.beat, check)
	current = c
	lastResult = nil
	nox.textToAll(string.format("Deep Delve: %s asks to go on to %s%s. Answer on the party strip, or say yes or no (%d s).", name, tostring(c.title),
		if how == "waypoint" then " by a waypoint" else "", seconds))
	tellUis()
	return true, "vote"
end

-- A hero answers the vote; whether it counted, else why.
function votes.cast(h: any, yes: boolean): (boolean, string?)
	local c = current
	if not c or not h then
		return false, "no vote"
	end
	local b = c.ballots[h.peer]
	if b == nil then
		return false, "not asked"
	end
	if b ~= "?" then
		return false, "answered already"
	end
	c.ballots[h.peer] = if yes then "y" else "n"
	tellUis()
	check()
	return true, nil
end

-- The asker calls its vote off; whether it did, else why.
function votes.cancel(h: any): (boolean, string?)
	local c = current
	if not c or not h then
		return false, "no vote"
	end
	if c.by ~= h.peer then
		return false, "not yours"
	end
	finish(false, c.byName .. " called off the vote to go to " .. c.title .. ".")
	return true, nil
end

-- A player left: no longer waited for (its asker gone: the vote ends).
function votes.left(peer: number)
	failedAt[peer] = nil
	local c = current
	if not c or c.ballots[peer] == nil then
		return
	end
	if c.by == peer then
		finish(false, c.byName .. " left: the vote to go to " .. c.title .. " is off.")
		return
	end
	c.ballots[peer] = nil
	tellUis()
	check()
end

-- A player's answer to the vote under way ("y", "n", "?"), nil when none is or it wasn't asked.
function votes.mark(peer: number): string?
	return current and current.ballots[peer]
end

-- Whether a vote is under way.
function votes.active(): boolean
	return current ~= nil
end

-- A new map: a vote under way ends with it (the game moved without it).
function votes.reset()
	if current then
		nox.cancelTimer(current.beat)
		current = nil
	end
	lastResult = nil
end

-- The time a vote takes (for "dd vote seconds N"; numbers.vote.seconds by default).
function votes.setSeconds(n: number)
	seconds = math.clamp(math.floor(n), 1, 600)
end

-- The vote as lines (for "dd vote"): "none" (and how the last ended), or its asker, where to,
-- the time left and each answer.
function votes.lines(): { string }
	local c = current
	if not c then
		return { "none" .. (if lastResult then ": " .. lastResult else "") }
	end
	local out = { string.format("%s to %s (%s), %d s left", c.byName, c.title, c.how, math.max(0, math.ceil((c.ends - nox.frame()) / 30))) }
	local names: { string } = {}
	for peer, b in c.ballots do
		table.insert(names, tostring(c.names[peer]) .. " " .. b)
	end
	table.sort(names)
	for _, l in names do
		table.insert(out, l)
	end
	return out
end

return votes
