--!strict
-- The act boss, in its act's last area (the lair, its end chamber the arena):
--
--   * made at the arena's middle as the "boss" tier (data/monsters.luau: more health, its damage
--     and experience; its own health factor and draw scale when it has them) with its own
--     enchantment set, and its guards; never charmed;
--   * its fight (data/monsters.luau's `fight`), thought over every beat (one timer, one monster):
--       warlord  act 1's: adds called at shares of its health, an enrage below another, and a
--                slam at a player near it (a warning at that player's feet, the blow a moment
--                later where the warning was);
--       barrow   act 2's barrow-king (barrowking.luau): his dead from the graves round the arena,
--                his charge, the grave-mist, and enraged the quake and the sky's fist;
--   * its health shown on every player's UI (a bar under its name, in the boss's colour);
--   * its death: a guaranteed better drop (rare gear above the area's level, and more rolls with
--     better odds), the act marked cleared in every character's record (a passive point the first
--     time at a tier; the next act opened), the vendor's stock rolled again, and the gate to the
--     next act opened in the arena (gate.luau; none after the last act).

local acts = require("data/acts")
local data = require("data/monsters")
local numbers = require("data/numbers")
local barrowking = require("barrowking")
local gate = require("gate")
local hero = require("hero")
local hud = require("hud")
local loot = require("loot")
local monsters = require("monsters")
local progress = require("progress")
local tree = require("tree")
local vendor = require("vendor")

local boss = {}

-- The boss of this map: { id, def, info, called, enraged, slamAt, beat, shown, k (the barrow
-- fight's) }, or nil.
local current: any = nil

local NEAR_PLAYERS: any = { players = true, alive = true }

-- Its bar's colour: the boss tier's, as its name plate has it.
local COLOUR: any = (data.tiers :: any).boss.colour

local function show(c: any)
	local o = nox.object(c.id)
	local health, most = 0, 0
	if o then
		health, most = o.health, o.maxHealth
	end
	if c.shown == health then
		return
	end
	c.shown = health
	hud.boss({ kind = "boss", name = c.def.name, health = health, most = most, c = COLOUR })
end

-- Adds at the boss: `n` of its list, magic, as its roster's monsters are.
local function call(c: any, n: number)
	local x, y = nox.position(c.id)
	for _ = 1, n do
		local t = c.def.adds[nox.random(#c.def.adds)]
		local id = monsters.spawn(t, x + nox.random(-60, 60), y + nox.random(-60, 60), "magic", c.info.level)
		if id then
			local ax, ay = nox.position(id)
			nox.effect(0x89, ax, ay) -- (MSG_FX_TELEPORT)
		end
	end
end

-- Act 1's fight (the warlord's).
local function warlord(c: any)
	local b = numbers.boss
	local o = nox.object(c.id)
	if not o then
		return
	end
	local share = o.health / math.max(1, o.maxHealth)
	-- Its adds, once at each share passed.
	while c.called < #b.summonAt and share <= b.summonAt[c.called + 1] do
		c.called += 1
		nox.unitSay(c.id, c.def.lines.summon, 90)
		call(c, b.adds)
	end
	-- Enraged, once.
	if not c.enraged and share <= b.enrageAt then
		c.enraged = true
		nox.enchant(c.id, c.def.enrage, 0)
		nox.unitSay(c.id, c.def.lines.enrage, 90)
	end
	-- The slam: a warning at a player near it, the blow where the warning was.
	local now = nox.frame()
	if now >= c.slamAt then
		local near = nox.unitsNear(o.x, o.y, b.slam.range, NEAR_PLAYERS)
		if #near > 0 then
			c.slamAt = now + b.slam.every
			local tx, ty = nox.position(near[nox.random(#near)])
			nox.effect(0x82, tx, ty) -- (MSG_FX_YELLOW_SPARKS)
			nox.after(b.slam.warn // 2, function()
				if current == c then
					nox.effect(0x82, tx, ty)
				end
			end)
			nox.after(b.slam.warn, function()
				if current == c and nox.living(c.id) then
					nox.effect(0x86, tx, ty) -- (MSG_FX_LESSER_EXPLOSION)
					nox.damageAt(tx, ty, b.slam.reach, b.slam.reach // 2, b.slam.damage, 2, c.id, nil, true)
				end
			end)
		end
	end
end

local function think()
	local c = current
	if not c or not nox.living(c.id) then
		return
	end
	if c.def.fight == "barrow" then
		barrowking.think(c)
	else
		warlord(c)
	end
	show(c)
end

-- The lair entered: its boss made at the arena's middle, its guards round it.
function boss.enter(spots: any, info: any)
	boss.reset()
	local def = (data.bosses :: any)[acts[info.act].boss]
	if not def or not spots.boss then
		return
	end
	local id = monsters.spawn(def.type, spots.boss.x, spots.boss.y, "boss", info.level, def)
	if not id then
		return
	end
	local entry = monsters.info(id)
	entry.boss = true
	entry.spells = def.spells
	-- (never charmed: a charmed boss can't be hurt by the party, and the act would stall)
	nox.setCharmable(id, false)
	if def.draw then
		nox.setDrawScale(id, def.draw)
	end
	for _ = 1, def.guards do
		monsters.spawn(def.adds[nox.random(#def.adds)], spots.boss.x, spots.boss.y, "magic", info.level)
	end
	current = {
		id = id,
		def = def,
		info = info,
		called = 0,
		enraged = false,
		slamAt = nox.frame() + numbers.boss.slam.every,
		shown = -1,
	}
	if def.fight == "barrow" then
		barrowking.start(current, spots)
	end
	local beat = if def.fight == "barrow" then numbers.barrowKing.beat else numbers.boss.beat
	current.beat = nox.every(beat, think)
	show(current)
end

-- The boss died (at x, y): its drop, the act cleared for every character.
function boss.killed(id: number, x: number, y: number)
	local c = current
	if not c or c.id ~= id then
		return
	end
	nox.cancelTimer(c.beat)
	barrowking.stop(c)
	current = nil
	local d = numbers.boss.drop
	loot.bossDrop(x, y, c.info.level, d)
	nox.effect(0x85, x, y) -- (MSG_FX_EXPLOSION)
	local act = acts[c.info.act]
	for _, h in hero.all() do
		-- (a passive point the first time the act is cleared at the character's tier)
		if not progress.isCleared(h.record, c.info.act) then
			tree.bossPoint(h.record)
		end
		-- (the next act opened with it, when there is one)
		progress.cleared(h.record, c.info.act)
		hero.save(h, "act cleared")
		hud.hero(h)
		if nox.present(h.unit) then
			nox.sound(h.unit, "LevelUp")
		end
	end
	local line: string = "Deep Delve: " .. tostring(c.def.name) .. " is slain. Act " .. tostring(c.info.act) .. ", " .. tostring(act.name) .. ", is cleared."
	nox.textToAll(line)
	-- (the vendor's stock rolled again, better)
	vendor.restock(c.info.level + numbers.boss.drop.levelUp)
	hud.boss({ kind = "boss", name = c.def.name, health = 0, most = 0 })
	-- The gate to the next act, in the arena.
	local nextAct: any = acts[c.info.act + 1]
	if nextAct and gate.open() then
		nox.textToAll("Deep Delve: a gate opens in the arena: the way to Act " .. tostring(c.info.act + 1) .. ", " .. tostring(nextAct.name) .. ".")
	end
end

-- The boss now, for "dd boss": { id, name, type }, or nil.
function boss.current(): any
	local c = current
	if not c then
		return nil
	end
	return { id = c.id, name = c.def.name, type = c.def.type }
end

-- "dd fight": its fight's state (the barrow-king's), or nil; `hurry`: its next blows brought to
-- the next beat.
function boss.fight(hurry: boolean?): string?
	local c = current
	if not c or c.def.fight ~= "barrow" then
		return nil
	end
	if hurry then
		barrowking.hurry(c)
	end
	return barrowking.line(c)
end

-- A new map: the old one's boss went with it.
function boss.reset()
	if current then
		nox.cancelTimer(current.beat)
		barrowking.stop(current)
		current = nil
		hud.boss(nil)
	end
end

return boss
