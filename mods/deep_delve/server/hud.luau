--!strict
-- What each player's UI shows (ui/main.luau): its character's name, level and experience, the
-- area it's in, whether its town portal is open, whether it has cleared the act, its passive
-- points left and its options (store.OPTIONS: how ground loot's names show); in a lair, the boss's health; and what the bigger windows show (windows.luau and
-- the modules it names). Each is a small value (nox.send) sent when it changes, under a key: a
-- newer value under the same key replaces one not sent yet. A send refused (a UI that isn't ready
-- yet, a player too far behind or with too many unacknowledged) keeps its value owed, tried again
-- by the beat below each frame until it's taken; and a player is sent at most a few a frame.
--
-- Big values: nox.send takes a value up to 256 KB from a client of level 6 on (the mod's minimum),
-- packed and sent in pieces by the engine (about 14 KB a frame a player), whole in the UI's
-- onMessage, in order with the rest. So a list goes as one value (hud.parts: one part, its serial
-- kept, the UI takes it as it took parts), and only one past MOST below is cut into parts: carried,
-- stash, stock (the vendor's), treeData, labels and chars (character select). Single values (sheet,
-- waypoints, party) go whole as they are.

local progress = require("progress")
local store = require("store")
local tree = require("tree")

local hud = {}

-- Whether this game keeps server characters (roster.luau sets it): the Character window gets its
-- Characters button.
hud.serverCharacters = false

-- The area now: { title, level } (areas.luau sets it).
local area: any = { title = "", level = 0 }
-- The boss's value now (nil: no boss in this map).
local bossValue: any = nil
-- Each peer's values waiting to be sent: peer -> key -> value.
local owed: { [number]: { [string]: any } } = {}
-- How many parts each peer's last list of a kind had (parts beyond a shorter list's are dropped).
local partsSent: { [number]: { [string]: number } } = {}
local beat: number? = nil

-- The most values a player is sent in one frame: a long list (the labels after a map load) goes
-- over a few frames, never in one burst.
local PER_FRAME = 3

-- Each player's values owed, PER_FRAME at most each, until a send is refused (its UI isn't ready,
-- or it has too many unacknowledged): what's left stays owed, and the beat tries again next frame.
-- Nothing is dropped but a value replaced by a newer one under its key, or a player gone.
local sentIn: { [number]: { frame: number, n: number } } = {}
local function flush()
	local now = nox.frame()
	for peer, values in owed do
		if not nox.player(peer) then
			owed[peer] = nil
			sentIn[peer] = nil
		elseif nox.hasUi(peer) or now % 15 == 0 then
			-- (a player whose client runs no UI, or not yet, is tried twice a second)
			local s = sentIn[peer]
			if not s or s.frame ~= now then
				s = { frame = now, n = 0 }
				sentIn[peer] = s
			end
			for key, value in values do
				if s.n >= PER_FRAME or not nox.send(peer, value) then
					break
				end
				values[key] = nil
				s.n += 1
			end
			if next(values) == nil then
				owed[peer] = nil
			end
		end
	end
	if next(owed) == nil and beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

local function owe(peer: number, key: string, value: any)
	local values = owed[peer]
	if not values then
		values = {}
		owed[peer] = values
	end
	values[key] = value
	if not beat then
		beat = nox.every(1, flush)
	end
end

-- How many values a player is owed now (for "dd hud").
function hud.owed(peer: number): number
	local n = 0
	for _ in owed[peer] or {} do
		n += 1
	end
	return n
end

-- A value to a player's UI under `key` (sent now when it can be, else by the beat).
function hud.send(peer: number, key: string, value: any)
	owe(peer, key, value)
	flush()
end

-- A value's size as the UI channel encodes it (value_codec's bytes).
local function sizeOf(v: any): number
	local t = type(v)
	if t == "number" then
		return 9
	elseif t == "string" then
		return 3 + #v
	elseif t == "table" then
		local n = 3
		for k, x in v do
			n += sizeOf(k) + sizeOf(x)
		end
		return n
	end
	return 1
end
hud.sizeOf = sizeOf

-- The most a list's part may be: half the engine's 256 KB (sizeOf's count is near the codec's, not
-- exact).
local MOST = 128 * 1024

-- A list to a player's UI in parts: each part { kind, serial, part, parts, list, ...header },
-- as many of `list`'s entries in each as fit. The UI keeps the parts of the newest serial.
function hud.parts(peer: number, kind: string, serial: number, list: { any }, header: any?)
	local base: any = { kind = kind, serial = serial, part = 1, parts = 1, list = {} }
	for k, v in header or {} do
		base[k] = v
	end
	local room = MOST - sizeOf(base) - 40
	local chunks: { { any } } = {}
	local cur: { any } = {}
	local used = 0
	for _, e in list do
		local s = sizeOf(e) + 9 + 1 -- (its index key, a little slack)
		if used + s > room and #cur > 0 then
			table.insert(chunks, cur)
			cur, used = {}, 0
		end
		table.insert(cur, e)
		used += s
	end
	if #cur > 0 or #chunks == 0 then
		table.insert(chunks, cur)
	end
	local sent = partsSent[peer]
	if not sent then
		sent = {}
		partsSent[peer] = sent
	end
	local values = owed[peer]
	for i = #chunks + 1, sent[kind] or 0 do
		if values then
			values[kind .. i] = nil
		end
	end
	sent[kind] = #chunks
	for i, chunk in chunks do
		local v = table.clone(base)
		v.part, v.parts, v.list = i, #chunks, chunk
		owe(peer, kind .. i, v)
	end
	flush()
end

-- A player's character changed (`h`: hero.luau's).
function hud.hero(h: any)
	local lv = h.record.level
	local _, _, left = tree.points(h.record)
	owe(h.peer, "hero", {
		kind = "hero",
		name = h.record.identity.name,
		class = h.record.identity.class,
		level = lv.level,
		experience = lv.experience,
		need = h.need,
		area = area.title,
		areaLevel = area.level,
		portal = h.portal ~= nil,
		act = area.act,
		cleared = area.act ~= nil and progress.isCleared(h.record, area.act),
		points = left,
		lootNames = store.option(h.record, "lootNames"),
		server = hud.serverCharacters or nil,
	})
	if bossValue then
		owe(h.peer, "boss", bossValue)
	end
	flush()
end

-- The area changed: every player's value again (`all`: the heroes).
function hud.area(info: any, all: any)
	area = { title = info.title, level = info.level, act = info.act }
	for _, h in all do
		hud.hero(h)
	end
end

-- The area now ({ title, level, act }).
function hud.currentArea(): any
	return area
end

-- The boss's health to everyone: { kind = "boss", name, health, most } (most 0: it's gone), or
-- nil (a new map: none).
function hud.boss(value: any)
	bossValue = if value and value.most > 0 then value else nil
	local sent = value or { kind = "boss", name = "", health = 0, most = 0 }
	for _, p in nox.players() do
		owe(p.peer, "boss", sent)
	end
	flush()
end

-- A peer gone.
function hud.forget(peer: number)
	owed[peer] = nil
	partsSent[peer] = nil
	sentIn[peer] = nil
end

return hud
