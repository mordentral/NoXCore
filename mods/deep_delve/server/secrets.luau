--!strict
-- Secret rooms and chests (data/secrets.luau; the generator's side: gen/secrets.luau).
--
-- The generator writes them into an area's map (a thing that never moves is drawn from each
-- client's copy of the map): a secret room shut by a line of the game's secret wall with a floor
-- lever near it (the map's own script opens the wall when it's thrown), its treasure chest and its
-- guards' spots; and chests in some side rooms, some trapped. Never in town or in a lair.
--
-- Here, as the area is entered: each chest found at its spot (one walk of the map's empty chests),
-- the secret room's guards put in (the area's roster, at its level), and the chests watched (a
-- short list, looked at every few frames while any is shut). A chest is opened by the game's own
-- touch (a player's: it opens, its lid up, its sound); seen open, it gives the mod's loot (loot.drop:
-- rarity, labels, the ground's index; more and better than a monster's), and a trapped one's trap
-- goes off at it.
--
-- No respawns: what a chest gave is remembered for the play (an area keeps its map within a play,
-- so its chests stand shut again when it's entered again): one opened before gives nothing more,
-- springs no trap, and a looted secret room's guards aren't put back.
--
-- A trap is owned by nobody (the game's invisible caster, made at the chest as it goes off), and
-- hurts only players: each within its reach takes the engine's damage from that caster (combat.luau
-- then takes a player's resistances off; no player stands behind it, so the party's rule spares
-- nothing), and a burst of monsters is the area's own monsters.

local data = require("data/secrets")
local monsterData = require("data/monsters")
local loot = require("loot")
local monsters = require("monsters")

local secrets = {}

-- The area entered: { key (play and place), level, area (areas.info's), chests = { { index (0 the
-- secret room's), id, type, x, y, kind ("hoard", "chest", "trapped"), trap, opened } }, secret (the
-- generator's spot), guards = { ids } }; nil between areas.
local current: any = nil
-- What the play's chests gave: key -> { [index] = true }.
local looted: { [string]: { [number]: boolean } } = {}
local lootedPlay: number? = nil
-- The beat looking at the shut chests (nil: none shut).
local beat: number? = nil
-- What the traps did, for "dd secrets": the last few, newest last.
local sprung: { string } = {}

-- The generator's numbers for an area (areas.request's params.secrets): none in a lair.
function secrets.params(info: any): any?
	if info.lair then
		return nil
	end
	local traps = {}
	for _, kind in data.trapOrder do
		local t: any = (data.traps :: any)[kind]
		table.insert(traps, { kind, t.weight })
	end
	return {
		chance = data.chance.room,
		chests = data.chance.chests,
		trapped = data.chance.trapped,
		traps = traps,
		hoard = data.hoard,
		guards = data.guards.count,
	}
end

-- ---- The area's monsters for the guards and a trap's burst

-- A type of the area's roster (its own, its biome's, a run's), by weight; none that rises again.
local function rosterType(area: any): string?
	local roster: any = area.runRoster or (monsterData.rosters :: any)[area.roster or area.biome] or monsterData.rosters.Crypt
	local index: number = area.rosterAt or area.index or 1
	local list: { any }, total = {}, 0
	for _, e in roster do
		if ((e.at and e.at <= index) or (e.from and e.from <= area.level)) and not e.rises then
			table.insert(list, e)
			total += e.weight
		end
	end
	if #list == 0 then
		return nil
	end
	local roll = nox.random() * total
	for _, e in list do
		roll -= e.weight
		if roll < 0 then
			return e.type
		end
	end
	return list[#list].type
end

local function spawnAt(area: any, x: number, y: number, tier: string): number?
	local t = rosterType(area)
	if not t then
		return nil
	end
	return monsters.spawn(t, x, y, tier, area.level)
end

-- ---- Traps

local function note(line: string)
	table.insert(sprung, line)
	if #sprung > 8 then
		table.remove(sprung, 1)
	end
end

-- A trap's damage at the area level.
local function trapDamage(t: any, level: number): number
	return math.max(1, math.floor(t.damage * (1 + data.perLevel * (level - 1)) + 0.5))
end

-- Chest `c`'s trap going off (it was opened): what it did, as a line.
local function spring(c: any)
	local t: any = (data.traps :: any)[c.trap]
	if not t then
		return
	end
	local x: number, y: number = c.x, c.y
	nox.effect(t.effect, x, y)
	nox.soundAt(x, y, t.sound)
	local level = current.level
	if c.trap == "monsters" then
		local n = nox.random(t.count[1], t.count[2])
		local made = 0
		for i = 1, n do
			local a = i * 2 * math.pi / n
			local px, py = x + t.reach * math.cos(a), y + t.reach * math.sin(a)
			nox.effect(0x8A, px, py) -- (MSG_FX_SMOKE_BLAST)
			if spawnAt(current.area, px, py, "normal") then
				made += 1
			end
		end
		note(tostring(c.trap) .. " at " .. math.floor(x) .. " " .. math.floor(y) .. ": " .. made .. " monsters")
		return
	end
	-- The trap's own caster: the game's invisible one, owned by nobody, put away a moment on.
	local caster = nox.create(data.caster.type, x, y)
	local amount = trapDamage(t, level)
	local hit, took = 0, 0
	for _, u in nox.unitsNear(x, y, t.reach, { players = true, alive = true }) do
		local o = nox.object(u)
		if o and o.player then
			hit += 1
			if c.trap == "lightning" then
				nox.effect(t.effect, o.x, o.y)
			end
			-- (what it took: its health before and after, the engine's armour and the mod's
			-- resistances in it)
			nox.damage(u, caster, amount, t.kind)
			local after = nox.object(u)
			took += o.health - (if after then after.health else 0)
			if t.spell and caster then
				nox.cast(t.spell, caster, u, o.x, o.y, t.power)
			end
		end
	end
	if caster then
		nox.after(data.caster.keep, function()
			if nox.present(caster) then
				nox.delete(caster)
			end
		end)
	end
	note(tostring(c.trap) .. " at " .. math.floor(x) .. " " .. math.floor(y) .. ": " .. amount .. " each, " .. hit .. " players, " .. took .. " taken")
end

-- ---- Chests

local function stopBeat()
	if beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

-- The nearest live player's unit within reach of (x, y), or nil.
local function nearestPlayer(x: number, y: number, reach: number): number?
	local best, bestD = nil, math.huge
	for _, u in nox.unitsNear(x, y, reach, { players = true, alive = true }) do
		local o = nox.object(u)
		if o and o.player then
			local d = (o.x - x) * (o.x - x) + (o.y - y) * (o.y - y)
			if d < bestD then
				best, bestD = u, d
			end
		end
	end
	return best
end

-- Chest `c` seen open: remembered for the play; its loot (none when it gave before), its trap.
local function opened(c: any)
	c.opened = true
	local given = looted[current.key]
	if not given then
		given = {}
		looted[current.key] = given
	end
	if given[c.index] then
		return -- (opened in an earlier visit of this play: empty, its trap sprung then)
	end
	given[c.index] = true
	local l: any = (data.loot :: any)[c.kind]
	local made = loot.drop(c.x, c.y, current.level, loot.rollsFor(l.rolls), l.better)
	c.gave = #made
	local who = nearestPlayer(c.x, c.y, data.opener)
	if c.trap then
		if who then
			nox.tell(who, "It was trapped!")
		end
		spring(c)
	elseif c.kind == "hoard" and who then
		nox.tell(who, "A hidden hoard!")
	end
end

-- The beat: each shut chest looked at; open ones handled; none shut, the beat stops.
local function look()
	if not current then
		stopBeat()
		return
	end
	local shut = 0
	for _, c in current.chests do
		if not c.opened then
			local o = nox.object(c.id)
			if not o then
				c.opened = true -- (gone: nothing to give)
			elseif not o.alive then
				opened(c)
			else
				shut += 1
			end
		end
	end
	if shut == 0 then
		stopBeat()
	end
end

-- The map's empty chest standing at (x, y), from the list `ids` (nox.chests'), or nil.
local function chestAt(ids: { number }, x: number, y: number): number?
	for _, id in ids do
		local ox, oy = nox.position(id)
		if math.abs(ox - x) < 2 and math.abs(oy - y) < 2 then
			return id
		end
	end
	return nil
end

-- An area entered (its spots in, its things made): its chests found and watched, its secret
-- room's guards put in unless its chest gave already. `play`: which play of the game it is.
function secrets.enter(spots: any, area: any, play: number)
	secrets.reset()
	if lootedPlay ~= play then
		table.clear(looted)
		lootedPlay = play
	end
	local key = tostring(play) .. ":" .. tostring(area.place)
	current = { key = key, level = area.level, area = area, chests = {}, secret = spots.secret, guards = {} }
	local given = looted[key] or {}
	local wanted: { any } = {}
	local s = spots.secret
	if s and s.chest then
		table.insert(wanted, { index = 0, spot = s.chest, kind = "hoard" })
	end
	for i, c in spots.chests or {} do
		table.insert(wanted, { index = i, spot = c, kind = if c.trap then "trapped" else "chest", trap = c.trap })
	end
	if #wanted == 0 then
		return
	end
	-- (one walk of the map's empty chests, at set-up)
	local ids = nox.chests()
	for _, e in wanted do
		local id = chestAt(ids, e.spot.x, e.spot.y)
		if id then
			table.insert(current.chests, {
				index = e.index,
				id = id,
				type = e.spot.type,
				x = e.spot.x,
				y = e.spot.y,
				kind = e.kind,
				trap = e.trap,
				opened = given[e.index] == true,
			})
		else
			nox.log(string.format("deep_delve: no chest at %.1f, %.1f", e.spot.x, e.spot.y))
		end
	end
	-- The secret room's guards (its chest not given yet): the first a leader.
	if s and not given[0] then
		for i, g in s.guards or {} do
			local id = spawnAt(area, g.x, g.y, if i == 1 then data.guards.leader else data.guards.tier)
			if id then
				table.insert(current.guards, id)
			end
		end
	end
	stopBeat()
	beat = nox.every(data.every, look)
end

-- A new map: the old one's chests went with it.
function secrets.reset()
	stopBeat()
	current = nil
end

-- ---- The console ("dd secrets ...")

-- A chest's line: "chest I ID TYPE X Y kind KIND trap KIND|none shut|open[ gave N]".
local function chestLine(c: any): string
	return string.format("chest %d %d %s %.1f %.1f kind %s trap %s %s%s", c.index, c.id, tostring(c.type), c.x, c.y, c.kind, tostring(c.trap or "none"),
		if c.opened then "open" else "shut", if c.gave then " gave " .. c.gave else "")
end

-- A trial's spots summed up: "secret 1|0 lever 1|0|open walls N guards G chest 1|0 chests C
-- trapped T fire F lightning L gas G monsters M intown N" (the console puts "lair 1|0" first).
local function trialLine(s: any): string
	local sec = s.secret
	local kinds: { [string]: number } = { fire = 0, lightning = 0, gas = 0, monsters = 0 }
	local trapped = 0
	-- (anything of the secrets inside the town's walls: never)
	local t = s.town and s.town.centre
	local inTown = 0
	local function town(p: any)
		if t and math.abs(p.x - t.x) + math.abs(p.y - t.y) <= 16 * 23 then
			inTown += 1
		end
	end
	for _, c in s.chests or {} do
		town(c)
		if c.trap then
			trapped += 1
			kinds[c.trap] = (kinds[c.trap] or 0) + 1
		end
	end
	if sec then
		town(sec)
		if sec.chest then
			town(sec.chest)
		end
	end
	return string.format("secret %d lever %s walls %d guards %d chest %d chests %d trapped %d fire %d lightning %d gas %d monsters %d intown %d",
		if sec then 1 else 0, if not sec then "0" elseif sec.open then "open" elseif sec.lever then "1" else "0",
		if sec then #sec.cells else 0, if sec then #sec.guards else 0, if sec and sec.chest then 1 else 0, #(s.chests or {}), trapped,
		kinds.fire, kinds.lightning, kinds.gas, kinds.monsters, inTown)
end

-- The loot `n` chests of `kind` (hoard, chest, trapped; "monster": a normal monster's drop) give at
-- `level`, rolled and counted, nothing made: "rolls R gear G normal N magic M rare R gold G potion P".
local function odds(kind: string, n: number, level: number): string
	local l: any = (data.loot :: any)[kind] or { rolls = 1, better = 0 }
	local count: { [string]: number } = { gear = 0, normal = 0, magic = 0, rare = 0, gold = 0, potion = 0 }
	local rolls = 0
	for _ = 1, n do
		for _ = 1, l.rolls do
			rolls += 1
			local d = loot.rollOne(level, l.better)
			if d then
				if d.kind == "gear" then
					count.gear += 1
					-- (a unique or a set piece counted in gear alone)
					local r: string = d.record.rarity
					count[r] = (count[r] or 0) + 1
				else
					count[d.kind] += 1
				end
			end
		end
	end
	return string.format("rolls %d gear %d normal %d magic %d rare %d gold %d potion %d", rolls, count.gear, count.normal, count.magic, count.rare,
		count.gold, count.potion)
end

-- "dd secrets": the area's secret room and chests; "trial SEED N": an area's built and summed up;
-- "go lever|room|chest I": the hero `h` to it (against a chest: its touch opens it); "trap I KIND":
-- chest I's trap set (none: "none"); "odds KIND N [LEVEL]": a chest's loot counted; "sprung": the
-- last traps that went off. `areas`: the module (its trial).
function secrets.console(args: { string }, areas: any, h: any): { string }
	local what = args[2]
	if what == "trial" then
		local place = tonumber(args[4]) or 1
		local ok, result = areas.trial(place, tonumber(args[3]) or 1)
		if not ok then
			return { "refused: " .. tostring(result) }
		end
		return { "lair " .. (if areas.info(place).lair then "1 " else "0 ") .. trialLine(result) }
	elseif what == "odds" then
		return { odds(tostring(args[3]), tonumber(args[4]) or 100, tonumber(args[5]) or (if current then current.level else 1)) }
	elseif what == "sprung" then
		return if #sprung > 0 then table.clone(sprung) else { "none" }
	end
	if not current then
		return { "no area" }
	end
	if what == "go" then
		if not h then
			return { "no player" }
		end
		local s = current.secret
		local at: any = nil
		if args[3] == "lever" then
			at = s and s.lever
		elseif args[3] == "room" then
			at = s
		elseif args[3] == "chest" then
			local i = tonumber(args[4]) or 0
			for _, c in current.chests do
				if c.index == i then
					-- (against its side: its touch)
					at = { x = c.x + 14, y = c.y }
				end
			end
		end
		if not at then
			return { "no such place" }
		end
		nox.setPosition(h.unit, at.x, at.y)
		return { string.format("at %.1f %.1f", at.x, at.y) }
	elseif what == "trap" then
		local i = tonumber(args[3]) or -1
		local kind = args[4]
		if kind ~= "none" and not (data.traps :: any)[tostring(kind)] then
			return { "no trap " .. tostring(kind) }
		end
		for _, c in current.chests do
			if c.index == i then
				c.trap = if kind == "none" then nil else kind
				c.kind = if c.index == 0 then "hoard" elseif c.trap then "trapped" else "chest"
				return { chestLine(c) }
			end
		end
		return { "no chest " .. tostring(args[3]) }
	end
	local out = {}
	local s: any = current.secret
	if s then
		local lever: string = if s.lever then string.format("%.1f %.1f", s.lever.x :: number, s.lever.y :: number) elseif s.open then "open" else "none"
		local walls: number, guards: number, alive: number = #s.cells, #(s.guards or {}), #current.guards
		table.insert(out, string.format("secret %.1f %.1f lever %s walls %d guards %d alive %d", s.x :: number, s.y :: number, lever, walls, guards, alive))
		for _, c in s.cells do
			table.insert(out, string.format("wall %d %d", c.x, c.y))
		end
	else
		table.insert(out, "secret none")
	end
	for _, c in current.chests do
		table.insert(out, chestLine(c))
	end
	return out
end

-- The data's names (dd check): each chest type, the trap caster, each trap's spell and sound.
function secrets.check(bad: { string })
	for _, t in data.hoard do
		if not nox.thingType(t) then
			table.insert(bad, "secrets: no thing " .. t)
		end
	end
	if not nox.thingType(data.caster.type) then
		table.insert(bad, "secrets: no thing " .. data.caster.type)
	end
	for _, kind in data.trapOrder do
		local t: any = (data.traps :: any)[kind]
		if not t then
			table.insert(bad, "secrets: no trap " .. kind)
		elseif t.spell and not nox.spellId(t.spell) then
			table.insert(bad, "secrets: no spell " .. t.spell)
		end
	end
	for _, t in { "Lever1", "CryptChest1", "CryptChest2", "CryptChest3", "CryptChest4" } do
		if not nox.thingType(t) then
			table.insert(bad, "secrets: no thing " .. t)
		end
	end
end

return secrets
