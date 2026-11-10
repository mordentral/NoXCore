--!strict
-- A room's monsters waking together, and a pack with them.
--
-- The engine's own look is what makes a monster notice a foe: every few frames a monster with no
-- foe looks round it (all about it: the mode's monstersAlert, so its facing never matters) for a
-- foe within its sight (its own, at least 250; outdoors the biome's 450) with nothing between them
-- (walls and closed doors block it). A room is often wider than that, so a hero could come into a
-- room, or shoot into it from its doorway, while most of the room's monsters stood unaware.
--
-- What this adds, by the engine's own sight (nox.sightRange) alone:
--   * a room comes awake as a hero comes into it: each of its monsters' sight then reaches the
--     room's far side (its distance from the room's middle plus the room's reach), so each one that
--     sees the hero (nothing between them) turns on it. The rooms are the generator's (spots.rooms:
--     the rooms that hold a pack, the end chamber's champion's too); a pack in a corridor has none
--     and keeps its biome's sight;
--   * a pack wakes together: one of it struck by a hero (or the hero's creature, or a missile of
--     theirs) raises each of its pack's sight to reach whoever struck. The engine's look then finds
--     the foe (still only in its sight).
-- A monster's sight is only ever raised, never past numbers.wake.most; one that never moves (the
-- plants: data/monsters.luau's powerKinds.still) keeps its own.
--
-- The cost: one timer for the map, every numbers.wake.every frames, one unitsNear for each room
-- still asleep (a room awake is left alone; the timer stops when none sleep). A hit is a table
-- lookup and a sight read for each of its pack. Nothing walks the world.

local numbers = require("data/numbers")
local monsterData = require("data/monsters")

local wake = {}

local CELL = 23

-- A room: its middle, its reach (world units, |dx| + |dy| from its middle: its radius and the
-- margin), where the route comes into it (or nil), its monsters, awake or not.
type Room = { index: number, x: number, y: number, radius: number, reach: number, door: any?, members: { number }, woken: boolean }
-- A pack: its monsters and its room (or nil: a corridor's, a spawned group's).
type Pack = { members: { number }, room: Room? }

local rooms: { Room } = {}
local asleep: { Room } = {}
local packOf: { [number]: Pack } = {}
local timer: number? = nil
-- Whether the rooms wake (the console's lever: "dd wake off" for a measure; on by default).
local on = true
-- What it has done this map: beats, rooms woken, packs woken by a hit, sights raised, and the
-- beats' time all told (seconds, os.clock).
local done = { beats = 0, rooms = 0, hits = 0, raised = 0, clock = 0 }

local still: { [string]: boolean } = monsterData.powerKinds.still :: any

-- One of a pack's monsters' sight raised to `to` (never past most, never lowered); whether it was.
-- (A zombie lying dead is still its pack's, and the engine stands it up as a foe comes into its
-- sight: so it rises as its room wakes, or as its pack fights.)
local function raise(id: number, to: number): boolean
	if not packOf[id] or not nox.present(id) then
		return false
	end
	local o = nox.object(id)
	if not o or still[o.type] then
		return false
	end
	local want = math.floor(math.min(to, numbers.wake.most) + 0.5)
	local ok, now = pcall(function(): number
		return nox.sightRange(id)
	end)
	if ok and now < want then
		nox.sightRange(id, want)
		done.raised += 1
		return true
	end
	return false
end

-- A room awake: each of its monsters' sight to its far side (its distance from the middle and
-- the room's reach, a diamond's farthest point from its middle).
local function wakeRoom(room: Room)
	room.woken = true
	done.rooms += 1
	for _, id in room.members do
		local o = nox.object(id)
		if o then
			raise(id, math.sqrt((o.x - room.x) ^ 2 + (o.y - room.y) ^ 2) + room.reach)
		end
	end
end

-- Whether a hero stands in a room (within its reach, |dx| + |dy| from its middle).
local function heroIn(room: Room): boolean
	for _, u in nox.unitsNear(room.x, room.y, room.reach, { players = true, alive = true }) do
		local x, y = nox.position(u)
		if math.abs(x - room.x) + math.abs(y - room.y) <= room.reach then
			return true
		end
	end
	return false
end

local function stopTimer()
	local t = timer
	if t then
		nox.cancelTimer(t)
		timer = nil
	end
end

-- The beat: each room still asleep looked at once.
local function beat()
	local t0 = os.clock()
	done.beats += 1
	for i = #asleep, 1, -1 do
		local room = asleep[i]
		if heroIn(room) then
			table.remove(asleep, i)
			wakeRoom(room)
		end
	end
	if #asleep == 0 then
		stopTimer()
	end
	done.clock += os.clock() - t0
end

local function startTimer()
	if on and not timer and #asleep > 0 then
		timer = nox.every(numbers.wake.every, beat)
	end
end

-- A new area's rooms (spots.rooms: { x, y, radius (cells), door }), before its packs are made.
function wake.rooms(list: { any }?)
	for i, r in list or {} do
		local radius = tonumber(r.radius) or 0
		local room: Room = {
			index = i,
			x = tonumber(r.x) or 0,
			y = tonumber(r.y) or 0,
			radius = radius,
			reach = (radius + numbers.wake.margin) * CELL + CELL / 2,
			door = r.door,
			members = {},
			woken = false,
		}
		table.insert(rooms, room)
		table.insert(asleep, room)
	end
end

-- A pack made: its monsters (ids), in its room (spots.rooms' index; nil: none).
function wake.pack(ids: { number }, roomIndex: number?)
	local room = if roomIndex then rooms[roomIndex] else nil
	local pack: Pack = { members = ids, room = room }
	for _, id in ids do
		packOf[id] = pack
		if room then
			table.insert(room.members, id)
		end
	end
end

-- The area's packs are in: the rooms' beat starts (a room with no monster sleeps no more).
function wake.start()
	for i = #asleep, 1, -1 do
		if #asleep[i].members == 0 then
			table.remove(asleep, i)
		end
	end
	startTimer()
end

-- One of the mod's monsters struck, a hero behind the blow (`foes`: the hero's unit and whatever
-- struck for it, a creature or a missile): its pack's sight reaches the farthest of them.
function wake.struck(target: number, foes: { number })
	local pack = packOf[target]
	if not on or not pack then
		return
	end
	local points = {}
	for _, f in foes do
		if nox.present(f) then
			local x, y = nox.position(f)
			table.insert(points, { x = x, y = y })
		end
	end
	if #points == 0 then
		return
	end
	local any = false
	for _, id in pack.members do
		local o = nox.object(id)
		if o then
			local far = 0
			for _, p in points do
				far = math.max(far, math.sqrt((o.x - p.x) ^ 2 + (o.y - p.y) ^ 2))
			end
			if raise(id, far + numbers.wake.hit) then
				any = true
			end
		end
	end
	if any then
		done.hits += 1
	end
end

-- A monster no longer the mod's (charmed away): out of its pack.
function wake.forget(id: number)
	packOf[id] = nil
end

-- A new map: its rooms and packs gone, the beat stopped.
function wake.reset()
	stopTimer()
	table.clear(rooms)
	table.clear(asleep)
	table.clear(packOf)
	done.beats, done.rooms, done.hits, done.raised, done.clock = 0, 0, 0, 0, 0
end

-- ---- For the console

-- "dd wake [on|off|all]": the lever ("off": no room wakes, no hit wakes a pack, the beat stopped;
-- "on" again; "all": every room awake now), then the lines: "wake on|off, rooms N, asleep A, beats
-- B, us U, raised R, hits H" (U: the beats' time all told, microseconds), then each room "room I X
-- Y radius R reach D door X Y|none monsters M woken 0|1 ids A,B,...|-" (M: its monsters alive now,
-- their ids).
function wake.command(op: string?): { string }
	if op == "off" then
		on = false
		stopTimer()
	elseif op == "on" then
		on = true
		startTimer()
	elseif op == "all" then
		for i = #asleep, 1, -1 do
			wakeRoom(table.remove(asleep, i) :: Room)
		end
		stopTimer()
	end
	local out = {
		string.format("wake %s, rooms %d, asleep %d, beats %d, us %d, raised %d, hits %d", if on then "on" else "off", #rooms, #asleep, done.beats,
			math.floor(done.clock * 1e6 + 0.5), done.raised, done.hits),
	}
	for _, r in rooms do
		local alive: { string } = {}
		for _, id in r.members do
			if packOf[id] and nox.living(id) then
				table.insert(alive, tostring(id))
			end
		end
		local door = if r.door then string.format("%.1f %.1f", tonumber(r.door.x) or 0, tonumber(r.door.y) or 0) else "none"
		table.insert(out, string.format("room %d %.1f %.1f radius %d reach %d door %s monsters %d woken %d ids %s", r.index, r.x, r.y, r.radius,
			math.floor(r.reach + 0.5), door, #alive, if r.woken then 1 else 0, if #alive > 0 then table.concat(alive, ",") else "-"))
	end
	return out
end

return wake
