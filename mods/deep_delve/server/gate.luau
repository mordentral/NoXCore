--!strict
-- The gate to the next act. An act's lair has no exit on; its boss's death opens a gate in the
-- arena (at the lair's gate spot, the arena's back) to the next act's first area. There is none
-- before the boss dies, so nobody runs past it.
--
--   * What it is: a blue swirl (data/acts.luau's gate.swirl, a thing made as it opens, which every
--     client draws), the gate's light written into the lair's map (switched off as the lair is
--     entered, on as the gate opens), and sparks every so often.
--   * Kept: entering a lair, the gate opens at once when anyone in the game has cleared its act
--     (at that character's tier), and so does a player joining the lair's game whose character
--     has; the boss stands again all the same. A party member who hasn't killed the boss can still
--     go through it with the party: the gate moves the whole game.
--   * Going through: a player stepping into it asks for the next act's first area, as the exit
--     does: alone, the game goes at once; with anyone else in the game it's the party's vote
--     (votes.luau). Everyone arrives at the area's start, beside the act's first waypoint.
--   * The last act's lair has no gate.
--
-- One timer while it's open (its sparks and who stands in it, every gate.every frames); nothing
-- walks the world but the one look for the gate's light as a lair is entered.

local acts = require("data/acts")
local hero = require("hero")
local progress = require("progress")

local gate = {}

local G: any = acts.gate

-- This lair's gate site: { act, x, y, light (its light's id, or nil) }; nil when this map isn't a
-- lair with an act after it.
local site: any = nil
-- The open gate: { act, x, y, swirl (its thing's id), beat, sparkAt, last = unit -> frame }.
local current: any = nil
-- areas.luau's own (handed in by enter: it requires this module).
local areas: any = nil

-- Whether any character in the game has cleared the act.
local function anyCleared(act: number): boolean
	for _, h in hero.all() do
		if progress.isCleared(h.record, act) then
			return true
		end
	end
	return false
end

-- Its sparks now and then; a hero standing in it asks to go through.
local function look()
	local c = current
	if not c then
		return
	end
	local now = nox.frame()
	if now >= c.sparkAt then
		nox.effect(G.sparks, c.x, c.y)
		c.sparkAt = now + G.sparksEvery
	end
	for _, unit in nox.unitsNear(c.x, c.y, G.reach, { players = true, alive = true }) do
		local h = hero.byUnit(unit)
		if h and h.ready then
			local last = c.last[unit]
			if not last or now - last >= G.again then
				c.last[unit] = now
				areas.gateTouched(h)
			end
		end
	end
end

-- The gate opened (the boss slain, or kept from an earlier kill); whether it opened now.
function gate.open(): boolean
	local s = site
	if current or not s then
		return false
	end
	local swirl = nox.create(G.swirl, s.x, s.y)
	if swirl then
		-- (nothing pushes it about, and it pushes nothing)
		nox.setNoCollide(swirl, true)
	end
	if s.light then
		nox.setOnOff(s.light, true)
	end
	nox.effect(0x89, s.x, s.y) -- (MSG_FX_TELEPORT)
	current = { act = s.act, x = s.x, y = s.y, swirl = swirl, sparkAt = 0, last = {} }
	current.beat = nox.every(G.every, look)
	if areas then
		areas.gateOpened(s.act)
	end
	return true
end

-- The gate's light in this map: the ColorLight written at its spot (one look, as the lair is
-- entered).
local function lightAt(x: number, y: number): number?
	for _, id in nox.objects("ColorLight") do
		local lx, ly = nox.position(id)
		if math.abs(lx - x) < 2 and math.abs(ly - y) < 2 then
			return id
		end
	end
	return nil
end

-- A map's area entered (`spots`: the generator's; `info`: areas.info's; `areasApi`: areas.luau):
-- in a lair with an act after it, the gate's site kept, its light switched off, and the gate
-- opened at once when anyone here has cleared the act.
function gate.enter(spots: any, info: any, areasApi: any)
	areas = areasApi
	gate.reset()
	if not info.lair or not acts[info.act + 1] or not spots.gate then
		return
	end
	local light = lightAt(spots.gate.x, spots.gate.y)
	if light then
		nox.setOnOff(light, false)
	end
	site = { act = info.act, x = spots.gate.x, y = spots.gate.y, light = light }
	if anyCleared(info.act) then
		gate.open()
	end
end

-- A player joined (its record read): the gate opened if its character has cleared this lair's
-- act.
function gate.joined(h: any)
	if site and not current and h and progress.isCleared(h.record, site.act) then
		gate.open()
	end
end

-- The gate now, for "dd gate": { act, x, y, open, swirl } or nil (no gate site in this map).
function gate.state(): any
	local s = site
	if not s then
		return nil
	end
	return { act = s.act, x = s.x, y = s.y, open = current ~= nil, swirl = current and current.swirl, light = s.light }
end

-- A new map: the old one's gate went with it.
function gate.reset()
	if current then
		nox.cancelTimer(current.beat)
		current = nil
	end
	site = nil
end

return gate
