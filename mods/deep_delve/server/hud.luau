--!strict
-- What each player's UI shows (ui/main.luau): its character's name, level and experience, the
-- area it's in, whether its town portal is open, whether it has cleared the act and its passive
-- points left; in a lair, the boss's health; and what the bigger windows show (windows.luau and
-- the modules it names). Each is a small value (nox.send) sent when it changes, under a key: a
-- newer value under the same key replaces one not sent yet. A UI that isn't ready yet, or a
-- player too far behind, is sent its values again by the beat below until it takes them.
--
-- A value is at most 900 bytes (the UI channel's limit): a list too long for one goes in parts
-- (hud.parts), each part carrying the list's serial so the UI puts the parts of one list together.

local progress = require("progress")
local tree = require("tree")

local hud = {}

-- The area now: { title, level } (areas.luau sets it).
local area: any = { title = "", level = 0 }
-- The boss's value now (nil: no boss in this map).
local bossValue: any = nil
-- Each peer's values waiting to be sent: peer -> key -> value.
local owed: { [number]: { [string]: any } } = {}
-- How many parts each peer's last list of a kind had (parts beyond a shorter list's are dropped).
local partsSent: { [number]: { [string]: number } } = {}
local beat: number? = nil

local function flush()
	for peer, values in owed do
		if not nox.player(peer) then
			owed[peer] = nil
		else
			for key, value in values do
				if nox.send(peer, value) then
					values[key] = nil
				end
			end
			if next(values) == nil then
				owed[peer] = nil
			end
		end
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
		beat = nox.every(15, flush)
	end
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

-- The most a value may be (the channel's 900, less a little).
local MOST = 880

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
end

return hud
