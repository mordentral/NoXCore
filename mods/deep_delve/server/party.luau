--!strict
-- The party (design 6 and 7): who is in the game, what the party strip at the top of each player's
-- screen shows of them, and the party's size for the monsters' scaling.
--
--   * The strip: one card a member (ui/main.luau draws it): name, class, level, health and mana
--     (as shares of their most), where they are (the area, or the town) and whether they're dead;
--     during a vote, each one's answer (votes.luau). Looked at every numbers.strip.every frames
--     (four times a second), one small value to every player, sent only when it changed.
--   * The count: every player in the game (joined, alive or not) counts; scale.luau's factors
--     follow it, and the mod's live monsters' health is changed with it as players come and go.

local hero = require("hero")
local hud = require("hud")
local monsters = require("monsters")
local scale = require("scale")
local town = require("town")
local votes = require("votes")
local numbers = require("data/numbers")

local party = {}

-- The strip's members now, and their signature.
local members: { any } = {}
local signature = ""

local function cut(s: string, most: number): string
	if #s <= most then
		return s
	end
	return string.sub(s, 1, most)
end

-- One member's card: { p = peer, n = name, c = class, l = level, h = health %, m = mana % (-1:
-- none), w = where ("Town", the area's title, or "" when not in play), d = dead, v = its answer to
-- a vote under way }.
local function card(h: any): any
	local r = h.record
	local c: any = {
		p = h.peer,
		n = cut(tostring(r.identity.name), 16),
		c = tostring(r.identity.class),
		l = r.level.level,
		h = 0,
		m = -1,
		w = "",
		d = h.fell == true,
		v = votes.mark(h.peer),
	}
	local o = nox.present(h.unit) and nox.object(h.unit) or nil
	if o then
		c.h = if o.maxHealth > 0 then math.clamp(math.floor(100 * o.health / o.maxHealth + 0.5), 0, 100) else 0
		local mana, most = nox.mana(h.unit)
		if most > 0 then
			c.m = math.clamp(math.floor(100 * mana / most + 0.5), 0, 100)
		end
		c.d = c.d or not o.alive
		if h.ready or c.d then
			c.w = if town.contains(o.x, o.y) then "Town" else cut(tostring(hud.currentArea().title), 20)
		end
	end
	return c
end

local function look()
	local list = {}
	for _, h in hero.all() do
		table.insert(list, card(h))
	end
	table.sort(list, function(a: any, b: any): boolean
		return a.p < b.p
	end)
	local parts: { string } = {}
	for _, c in list do
		table.insert(parts, table.concat({ tostring(c.p), tostring(c.n), tostring(c.c), tostring(c.l), tostring(c.h), tostring(c.m), tostring(c.w), tostring(c.d), tostring(c.v) }, "|"))
	end
	members = list
	signature = table.concat(parts, ";")
	local value = { kind = "party", list = list }
	for _, h in hero.all() do
		if h.partySig ~= signature then
			h.partySig = signature
			hud.send(h.peer, "party", value)
		end
	end
end

-- The beat (the game's start).
function party.start()
	nox.every(numbers.strip.every, look)
end

-- A player's client ready in the map: its strip sent again at the next look.
function party.resend(h: any)
	h.partySig = nil
end

-- A player came or went: the count, and the live monsters' health with it.
function party.changed()
	local n = 0
	for _ in hero.all() do
		n += 1
	end
	local was = scale.set(n)
	if scale.factor("health", was) ~= scale.factor("health") then
		monsters.rescale()
	end
end

-- The strip as lines, "name class level health% mana% where dead vote" (for "dd party"); looked
-- at now.
function party.lines(): { string }
	look()
	local out = {}
	for _, c in members do
		table.insert(out, table.concat({ tostring(c.n), tostring(c.c), tostring(c.l), tostring(c.h), tostring(c.m), if c.w ~= "" then tostring(c.w) else "-",
			if c.d then "dead" else "alive", tostring(c.v or "-") }, " "))
	end
	return out
end

return party
