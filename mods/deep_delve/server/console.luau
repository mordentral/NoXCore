--!strict
-- The host's console commands ("dd ..."): what the game is doing, and a few levers for testing a
-- build (the tests drive the mod through these too). Each returns its reply lines.
--
--   dd status            the area (its act, its weather), its map and monsters; each player's
--                        character
--   dd spots             where the area's and the town's places are
--   dd save              every character's record written now
--   dd xp N              N experience to every character
--   dd award N           N experience as a kill gives it (split between the characters in play)
--   dd loot RARITY [N]   N pieces of gear of that rarity at the first player's feet (their records)
--   dd monster ID        a monster's tier, name and health
--   dd ground            the items on the ground that spark (id, effect)
--   dd go WHERE          the first player to: town, arrive, exit, guardian, boss, waypoint,
--                        wayArrive, portal, pad (the town's waypoints pentagram), wayIn, actGate
--                        (a lair's gate spot), pack [N]
--   dd area N [HOW]      the game to area N, arriving at its start (HOW "waypoint": beside its
--                        waypoint). N: the area's number on through the acts (act 1's 1-7, act
--                        2's from 8), or "A-N": area N of act A
--   dd trial SEED N      area N (as dd area names it) built now from game seed SEED (into a map
--                        never played); "ok", or why it was refused or what it lacks (its act's
--                        town and every one of its stations too)
--   dd seed              the play's seed and which play of the game it is (an emptied game
--                        starting over is a new play, with a new seed)
--   dd town              the act's town: its look and name
--   dd gate [take]       the lair's gate to the next act (its spot, open or shut, its swirl and
--                        light); "take": the first player steps into it (the game on, or a vote)
--   dd portal            the first player opens a town portal (as its UI's button does)
--   dd waypoint N        the first player to area N's waypoint of the act being played ("A-N":
--                        of act A; one it has found), as a click in the town's waypoints window
--                        sends it (wherever it stands)
--   dd progress          the first player's record: its version, its act progress, boss kills
--   dd boss              the boss of this map: id, type, health, name
--   dd smite             the boss killed by the first player
--   dd check             every name in the data tables checked against thing.bin, every spell
--                        against the game's spells (its number, its class, no linked group),
--                        every creature the tree gives (its name the field guide's, its summon
--                        spell the one that calls it), every spell, ability and summon node's
--                        icon name (its spell's, its ability's ABILITY_* name, its creature's
--                        summon spell), the draw scales within 0.5-3 (no player needed)
--   dd gold [N]          the first player's gold (set to N)
--   dd carried           what it carries: "n id type rarity worn" (n: the stash's and vendor's N)
--   dd stash [open | put N | take TAB SLOT | deposit | withdraw]   its stash (as its window
--                        does: by the chest); alone, its contents "tab slot type rarity mods"
--   dd vendor [open | buy I | sell N | reroll N | restock LEVEL]   the vendor (by the anvil);
--                        alone, its stock "index type rarity price"
--   dd tree [take ID... | respec | nodes [REGION]]   its passive tree; alone, its points and
--                        the nodes taken
--   dd tree refund [ID]  node ID given back alone (anywhere, for gold); alone, its price
--                        and the nodes taken that may be given back ("leaves ...")
--   dd tree describe ID [CLASS]   a node's name and what it gives, as the tree window shows
--                        them (no player needed); with a class, the tooltip's "For you" line
--   dd tree costs        the game's mana prices of the tree's spells (the cast-cost keystones' base)
--   dd icons             each tree node's picture, "id kind icon": the mod's picture, or the
--                        game's icon's name for a spell, ability or summon (no player needed)
--   dd book             the spells, abilities and creatures its tree gives, and whether its
--                        unit's book and field guide are exactly that ("server ok")
--   dd sheet             its character sheet, "key value" lines
--   dd ui WINDOW [NAME]  a window opened on its UI (stash, vendor, waypoints: it's moved by the
--                        station); "sheet NAME": party member NAME's sheet, as its card's click
--   dd hits [clear]      the last hits cancelled (no damage within the party, no splash from a
--                        hero's own Fireball): "why: target by source, weapon W, kind K, N
--                        cancelled"; or the list emptied
--   dd summons           the heroes' summoned creatures known (watched): "id type owner"
--   dd team              the party's team ("team ID Party colour C score S", or "no team") and
--                        each player on it or not ("NAME on", "NAME off")
--   dd as NAME COMMAND   any command above as the player NAME instead of the first
--   dd party             the party strip: "name class level health% mana% where alive|dead vote"
--   dd scale             the party's size and its factors, and one monster's health (the lowest id)
--   dd regen             its passive regeneration: "rates H M" (a second, the mod's), then its state:
--                        running, waiting (health held after a hit), paused (dead or between
--                        maps)
--   dd packs             the packs the area was given: "packs N, monsters M, experience X" (and
--                        the champion's group's), then each "index along size tier leader
--                        experience rising" (along: how far along the route, 0-1; rising: its
--                        monsters that rise again after death)
--   dd exit              the first player steps on the area's exit (the game on, or a vote)
--   dd vote [yes | no | cancel | seconds N]   answer the vote (as the first player), call it off,
--                        or set its time; alone, the vote: its asker, where to, time, each answer
--   dd labels            the ground's labels ("id code rarity text") and the codes the first
--                        player's UI was last sent
--   dd plates            the named monsters' name plates ("id code tier r,g,b name") and the
--                        codes the first player's UI was last sent
--   dd spawn TIER [N] [TYPE]   N monsters (1-60) of that tier (normal, magic, rare, champion: the
--                        area's biome's champion, its name and set) round the first player, of
--                        TYPE or the biome roster's first (a champion: its own); "id code tier name"
--   dd slay ID           one of the mod's monsters killed by the first player
--   dd portals           the open portals, "owner x y" ("owner(left)": its owner left the game)
--   dd portal take       the first player through the town's portal pentagram (as its touch)
--   dd decals            the pads' rings on the map's floor, "kind x y" (waypoint, portal)
--   dd hud               how many values the first player's UI is owed (sends not taken yet)
--   dd option [NAME VALUE]   the first player's options ("name value"), or one set (as its
--                        Character window does): lootNames alt | always
--   dd chars [...]       server characters (roster.luau): alone, each player's seat; "clock
--                        SECONDS" the link codes' clock moved on; "lockat N|none" the game closed to
--                        newcomers from N players (its lock policy, numbers.roster.lockAt); "NAME list | play I | new |
--                        code | enter CODE | unlink I | pcs" as its character select does

local areas = require("areas")
local boss = require("boss")
local combat = require("combat")
local decals = require("decals")
local gate = require("gate")
local hero = require("hero")
local hud = require("hud")
local labels = require("labels")
local loot = require("loot")
local monsters = require("monsters")
local party = require("party")
local portal = require("portal")
local scale = require("scale")
local store = require("store")
local votes = require("votes")
local progress = require("progress")
local regen = require("regen")
local characters = require("roster")
local town = require("town")
local waypoints = require("waypoints")
local book = require("book")
local sheet = require("sheet")
local stash = require("stash")
local summons = require("summons")
local team = require("team")
local tree = require("tree")
local treeview = require("treeview")
local vendor = require("vendor")
local windows = require("windows")
local acts = require("data/acts")
local monsterData = require("data/monsters")
local lootData = require("data/loot")
local classes = require("data/classes")
local treeData = require("data/tree")
local vendorData = require("data/vendor")
local numbers = require("data/numbers")

local console = {}

-- "dd as NAME ...": the hero the command acts as, in place of the first.
local acting: any = nil

local function firstHero(): any
	if acting then
		return acting
	end
	local best = nil
	for peer, h in hero.all() do
		if not best or peer < best.peer then
			best = h
		end
	end
	return best
end

local function words(line: string): { string }
	local out = {}
	for w in string.gmatch(line, "%S+") do
		table.insert(out, w)
	end
	return out
end

local function reply(ok: boolean, why: string?): { string }
	return { if ok then "ok" else "not: " .. tostring(why) }
end

local commands: { [string]: (args: { string }) -> { string } } = {}

-- An area named on the console: "N" (its place, numbered on through the acts: act 1's 1-7, act
-- 2's from 8) or "A-N" (area N of act A); its place, or nil.
local function placeNamed(text: string?): number?
	if not text then
		return nil
	end
	local a, n = string.match(text, "^(%d+)%-(%d+)$")
	if a and n then
		return areas.placeOf(tonumber(a) :: number, tonumber(n) :: number)
	end
	return tonumber(text)
end

function commands.status(_args: { string }): { string }
	local s = areas.status()
	local out = {
		string.format("area %d (%s), level %d, map %s, %s; next: %s; act %d, area %d of it, weather %s", s.place, s.title, s.level, s.map,
			if s.entered then "entered" else "not entered", s.nextState, s.act, s.index, tostring(s.weather or "none")),
		string.format("monsters alive: %d", monsters.count()),
	}
	for peer, h in hero.all() do
		local r = h.record
		table.insert(out, string.format("peer %s: %s (%s) level %s, %s/%s xp, unit %s, %s, key %s", tostring(peer), tostring(r.identity.name),
			tostring(r.identity.class), tostring(r.level.level), tostring(r.level.experience), tostring(h.need), tostring(h.unit),
			if h.ready then "ready" else "not ready", tostring(h.key)))
	end
	return out
end

function commands.save(_args: { string }): { string }
	return { "saved " .. hero.saveAll("console") }
end

function commands.xp(args: { string }): { string }
	local n = tonumber(args[2]) or 0
	for _, h in hero.all() do
		hero.gain(h, n)
	end
	return { "gave " .. n }
end

function commands.award(args: { string }): { string }
	local n = tonumber(args[2]) or 0
	hero.award(n, nil)
	return { "awarded " .. n }
end

function commands.loot(args: { string }): { string }
	local rarity = args[2] or "magic"
	local n = tonumber(args[3]) or 1
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local o = nox.object(h.unit)
	if not o then
		return { "no unit" }
	end
	local level = areas.status().level
	local all = lootData.gear
	local out = {}
	for i = 1, n do
		local g = all[(i - 1) % #all + 1]
		local rec = loot.makeGear(g.type, rarity, level)
		-- (a widening ring round the player, so each can be told apart)
		local r = 34 + 5 * i
		local x, y = nox.clearPointNear(o.x + r * math.cos(i * 1.3), o.y + r * math.sin(i * 1.3), 30)
		local id = loot.place(rec, x, y)
		table.insert(out, string.format("%s %s %s %s %s %s %s %s", tostring(id), rec.type, rec.rarity, tostring(rec.effectiveness),
			tostring(rec.material), tostring(rec.primary), tostring(rec.secondary), tostring(rec.affixes and #rec.affixes or 0)))
	end
	return out
end

function commands.ground(_args: { string }): { string }
	return loot.sparking()
end

function commands.monster(args: { string }): { string }
	local id = tonumber(args[2]) or 0
	local m = monsters.info(id)
	local o = nox.object(id)
	if not m or not o then
		return { "not one of ours" }
	end
	return { string.format("%s %s %s %d/%d", tostring(m.type), tostring(m.tier), tostring(m.name or "-"), o.health, o.maxHealth) }
end

-- The places "dd spots" and "dd go" know, by name, from the area's spots.
local function place(s: any, name: string): any
	local t = s.town
	local byName: any = {
		arrive = s.arrive,
		back = s.back,
		exit = s.exit,
		guardian = s.guardian,
		boss = s.boss,
		waypoint = s.waypoint,
		wayArrive = s.wayArrive,
		gate = t.gate,
		healer = t.healer,
		stash = t.stash,
		vendor = t.vendor,
		portal = t.portal,
		pad = t.waypoints,
		wayIn = t.wayIn,
		-- (a lair's gate to the next act: its spot, whether or not it's open)
		actGate = s.gate,
	}
	return byName[name]
end

function commands.go(args: { string }): { string }
	local h = firstHero()
	local s = areas.status().spots
	if not h or not s then
		return { "no player or no area" }
	end
	local where = args[2] or "town"
	local at = nil
	if where == "town" then
		town.sendHome(h.unit)
		return { "home" }
	elseif where == "pack" then
		at = s.packs[tonumber(args[3]) or 1]
	else
		at = place(s, where)
	end
	if not at then
		return { "no such place" }
	end
	nox.setPosition(h.unit, at.x, at.y)
	return { string.format("at %.1f %.1f", at.x, at.y) }
end

function commands.spots(_args: { string }): { string }
	local s = areas.status().spots
	if not s then
		return { "no area" }
	end
	local out = {}
	-- (one line a place: name, x, y, its thing's type or "-"; "none" for a place this area lacks)
	local types: any = { portal = "InvisibleTeleportPentagram", pad = "InvisibleTeleportPentagram", wayIn = "InvisibleTeleportPentagram", waypoint = "TeleportPentagram" }
	for _, name in { "arrive", "back", "exit", "guardian", "boss", "waypoint", "wayArrive", "gate", "healer", "stash", "vendor", "portal", "pad", "wayIn", "actGate" } do
		local p = place(s, name)
		if p then
			table.insert(out, string.format("%s %.1f %.1f %s", name, tonumber(p.x) or 0, tonumber(p.y) or 0, tostring(p.type or types[name] or "-")))
		else
			table.insert(out, name .. " none")
		end
	end
	table.insert(out, string.format("packs %d", #s.packs))
	return out
end

-- "dd seed": the play's seed and which play of the game it is ("seed S, play P"); "dd town": the
-- act's town ("town LOOK, NAME").
function commands.seed(_args: { string }): { string }
	local s = areas.status()
	return { string.format("seed %d, play %d", s.seed, s.play) }
end

function commands.town(_args: { string }): { string }
	local t = areas.status().town
	return { string.format("town %s, %s", tostring(t.look), tostring(t.name)) }
end

function commands.area(args: { string }): { string }
	local n = placeNamed(args[2]) or 0
	local ok, why = areas.travel(n, if args[3] == "waypoint" then "waypoint" else "start")
	return { if ok then "going to area " .. n else "not going: " .. tostring(why) }
end

-- "dd trial SEED N": the area built; "ok" when it built with every spot its kind needs (a waypoint
-- area's waypoint, by the start chamber when its data says so; a lair's boss and, with an act
-- after it, its gate spot; an area with a way on, its exit), else why.
function commands.trial(args: { string }): { string }
	local seed, place = tonumber(args[2]) or 1, placeNamed(args[3]) or 1
	local ok, result = areas.trial(place, seed)
	if not ok then
		return { string.format("seed %d area %d: %s", seed, place, tostring(result)) }
	end
	local info = areas.info(place)
	local s: any = result
	local missing = nil
	if info.waypoint and not s.waypoint then
		missing = "waypoint"
	elseif info.waypointAt == "start" and s.waypoint
		and math.abs(s.waypoint.x - s.arrive.x) + math.abs(s.waypoint.y - s.arrive.y) > 8 * 23 then
		missing = "waypoint by the start"
	elseif info.lair and not s.boss then
		missing = "boss"
	elseif info.gate and not s.gate then
		missing = "gate"
	elseif not info.lair and not s.exit then
		missing = "exit"
	elseif not s.town or s.town.look ~= areas.townOf(info.act).look then
		missing = "town of act " .. info.act
	else
		-- (the town's stations, every one, in every act's look)
		for _, name in { "centre", "healer", "gate", "stash", "vendor", "portal", "waypoints", "wayIn" } do
			if not missing and not s.town[name] then
				missing = "town " .. name
			end
		end
	end
	if missing then
		return { string.format("seed %d area %d: no %s", seed, place, missing) }
	end
	return { "ok" }
end

-- "dd gate": the lair's gate to the next act: "gate act A at X Y open|shut swirl ID light ID", or
-- "no gate" (not a lair with an act after it). "dd gate take": the first player steps into it (as
-- standing in it does: the game on, or a vote).
function commands.gate(args: { string }): { string }
	local g = gate.state()
	if not g then
		return { "no gate" }
	end
	if args[2] == "take" then
		local h = firstHero()
		if not h then
			return { "no player" }
		end
		if not g.open then
			return { "not: shut" }
		end
		local ok, why = areas.gateTouched(h)
		return { if ok then (if why == "vote" then "vote" else "going") else "not: " .. tostring(why) }
	end
	local open: string = if g.open then "open" else "shut"
	local act: number, x: number, y: number = g.act, g.x, g.y
	return { string.format("gate act %d at %.1f %.1f %s swirl %s light %s", act, x, y, open, tostring(g.swirl or "-"), tostring(g.light or "-")) }
end

function commands.portal(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	if args[2] == "take" then
		local list = portal.openFor(h)
		portal.touched(h.unit)
		return { if #list == 0 then "none open" elseif list[1].owner == h or #list == 1 then "through" else "offered" }
	end
	return { portal.open(h) }
end

function commands.portals(_args: { string }): { string }
	local out = portal.lines()
	return if #out > 0 then out else { "none" }
end

function commands.decals(_args: { string }): { string }
	local out = decals.lines()
	return if #out > 0 then out else { "none" }
end

-- ---- The party (Phase 4)

function commands.party(_args: { string }): { string }
	return party.lines()
end

function commands.scale(_args: { string }): { string }
	local n = scale.players()
	local out = {
		string.format("players %d: health x%.2f, damage x%.2f, experience x%.2f, packs x%.2f, loot x%.2f", n, scale.factor("health"),
			scale.factor("damage"), scale.factor("experience"), scale.factor("packs"), scale.factor("loot")),
	}
	local s = monsters.sample()
	if s then
		table.insert(out, table.concat({ "sample", tostring(s.id), tostring(s.health) .. "/" .. tostring(s.most), "base", tostring(s.base), "damage",
			string.format("%.4f", tonumber(s.damage) or 0), "xp", tostring(s.experience) }, " "))
	else
		table.insert(out, "sample none")
	end
	table.insert(out, "packs " .. tostring(monsters.packsMade()))
	return out
end

function commands.regen(_args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	return regen.lines(h)
end

function commands.packs(_args: { string }): { string }
	local list: { any } = monsters.packs()
	local total, xp = 0, 0
	local out: { string } = {}
	for i = 1, #list do
		local p = list[i]
		total += p.size
		xp += p.experience
		table.insert(out, string.format("%d %.3f %d %s %s %d %d", i, p.along, p.size, tostring(p.tier), tostring(p.leader), p.experience, p.rising))
	end
	local g = monsters.guardians()
	local head = string.format("packs %d, monsters %d, experience %d", #list, total, xp)
	if g then
		head ..= string.format("; champion's group %d, experience %d", g.size, g.experience)
	end
	table.insert(out, 1, head)
	return out
end

function commands.exit(_args: { string }): { string }
	local h = firstHero()
	if not h or not nox.present(h.unit) then
		return { "no player" }
	end
	local ok, why = areas.exitTouched(h.unit)
	return { if ok then (if why == "vote" then "vote" else "going") else "not: " .. tostring(why) }
end

function commands.vote(args: { string }): { string }
	local op = args[2]
	if op == "seconds" then
		votes.setSeconds(tonumber(args[3]) or 30)
		return { "ok" }
	end
	if not op then
		return votes.lines()
	end
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	if op == "cancel" then
		return reply(votes.cancel(h))
	end
	return reply(votes.cast(h, op == "yes"))
end

function commands.hud(_args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	return { "owed " .. tostring(hud.owed(h.peer)) }
end

function commands.option(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	if args[2] then
		return reply(hero.setOption(h, args[2], tostring(args[3])))
	end
	local out = {}
	for name in store.OPTIONS :: any do
		table.insert(out, tostring(name) .. " " .. store.option(h.record, name))
	end
	table.sort(out)
	return out
end

function commands.labels(_args: { string }): { string }
	return labels.lines(firstHero())
end

function commands.plates(_args: { string }): { string }
	return labels.plateLines(firstHero())
end

function commands.spawn(args: { string }): { string }
	local tier = args[2] or "rare"
	if tier == "boss" or not (monsterData.tiers :: any)[tier] then
		return { "tiers: normal, magic, rare, champion" }
	end
	local h = firstHero()
	local o = h and nox.object(h.unit)
	if not o then
		return { "no player" }
	end
	local s = areas.status()
	local fixed: any = nil
	local thingType = args[4]
	if tier == "champion" then
		fixed = (monsterData.champions :: any)[s.biome] or monsterData.champions.Crypt
		thingType = thingType or fixed.type
	end
	local roster: any = (monsterData.rosters :: any)[s.biome] or monsterData.rosters.Crypt
	local out = {}
	for i = 1, math.clamp(tonumber(args[3]) or 1, 1, 60) do
		-- (a widening ring round the player)
		local r = 60 + 6 * i
		local id = monsters.spawn(thingType or roster[1].type, o.x + r * math.cos(i * 1.3), o.y + r * math.sin(i * 1.3), tier, s.level, fixed)
		local m: any = if id then monsters.info(id) else nil
		if id and m then
			table.insert(out, table.concat({ tostring(id), tostring(m.code), tier, tostring(m.name or "-") }, " "))
		end
	end
	return if #out > 0 then out else { "not made" }
end

-- "dd slay ID": one of the mod's monsters killed by the first player (as a kill: its experience,
-- its loot, its plate gone).
function commands.slay(args: { string }): { string }
	local id = tonumber(args[2]) or 0
	local h = firstHero()
	if not h or not monsters.info(id) then
		return { "not one of ours" }
	end
	for _, e in { "ENCHANT_SHIELD", "ENCHANT_PROTECT_FROM_MAGIC", "ENCHANT_VAMPIRISM" } do
		nox.enchant(id, e, -1)
	end
	for _ = 1, 40 do
		local o = nox.object(id)
		if not o or not nox.living(id) then
			break
		end
		nox.damage(id, h.unit, o.health + 10, 0)
	end
	return { if nox.living(id) then "still standing" else "slain" }
end

function commands.waypoint(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	-- ("N": area N of the act being played; "A-N": area N of act A)
	local act, n = areas.current().act, tonumber(args[2]) or 0
	local a, i = string.match(args[2] or "", "^(%d+)%-(%d+)$")
	if a and i then
		act, n = tonumber(a) :: number, tonumber(i) :: number
	end
	if not progress.hasWaypoint(h.record, act, n) then
		return { "not found yet" }
	end
	local ok, why = waypoints.go(h, n, act)
	return { if ok then "going" else "not going: " .. tostring(why) }
end

function commands.progress(_args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local r = h.record
	local out = { string.format("version %d, act %d, deepest %d", r.version, r.world.act, r.world.deepest) }
	local keys: { string } = {}
	for key in r.world.progress do
		table.insert(keys, tostring(key))
	end
	table.sort(keys)
	for _, key in keys do
		local p = r.world.progress[key]
		local list: { string } = {}
		for _, i in p.waypoints do
			table.insert(list, tostring(i))
		end
		local state: string = if p.cleared then "cleared" else "not cleared"
		local found: string = if #list > 0 then table.concat(list, " ") else "none"
		table.insert(out, key .. ": deepest " .. tostring(p.deepest) .. ", " .. state .. ", waypoints " .. found .. (if p.opened then ", open" else ", shut"))
	end
	local kills: { string } = {}
	for act, n in r.world.bosses do
		table.insert(kills, tostring(act) .. ":" .. tostring(n))
	end
	table.sort(kills)
	table.insert(out, "bosses " .. (if #kills > 0 then table.concat(kills, " ") else "none"))
	return out
end

function commands.boss(_args: { string }): { string }
	local b = boss.current()
	local o = b and nox.object(b.id)
	if not b or not o then
		return { "no boss" }
	end
	local m = monsters.info(b.id)
	return { string.format("%d %s %d/%d %dxp %s", b.id, b.type, o.health, o.maxHealth, if m then m.experience else 0, b.name) }
end

function commands.smite(_args: { string }): { string }
	local b = boss.current()
	local h = firstHero()
	if not b or not h then
		return { "no boss or no player" }
	end
	-- (its shield and protection off first; then blows from the player until it falls)
	for _, e in { "ENCHANT_SHIELD", "ENCHANT_PROTECT_FROM_MAGIC", "ENCHANT_VAMPIRISM" } do
		nox.enchant(b.id, e, -1)
	end
	local took = {}
	for _ = 1, 40 do
		local o = nox.object(b.id)
		if not o or not nox.living(b.id) then
			break
		end
		table.insert(took, tostring(nox.damage(b.id, h.unit, o.health + 10, 0)))
	end
	if nox.living(b.id) then
		return { "still standing (" .. table.concat(took, " ") .. ")" }
	end
	return { "slain" }
end

function commands.check(_args: { string }): { string }
	local bad = {}
	local function thing(name: string, where: string)
		if not nox.thingType(name) then
			table.insert(bad, where .. ": no thing " .. name)
		end
	end
	for biome, roster in (monsterData.rosters :: any) do
		for _, e in roster do
			thing(e.type, "roster " .. biome)
		end
	end
	for biome, c in (monsterData.champions :: any) do
		thing(c.type, "champion " .. biome)
	end
	for name, b in (monsterData.bosses :: any) do
		thing(b.type, "boss " .. name)
		for _, t in b.adds do
			thing(t, "boss " .. name .. " adds")
		end
	end
	for _, g in lootData.gear do
		thing(g.type, "gear")
	end
	for _, p in lootData.potions do
		thing(p.type, "potions")
	end
	for class, c in (classes :: any) do
		for _, g in c.start do
			thing(g.type, "start " .. class)
		end
	end
	for _, t in { "GauntletExitA", "GauntletExitB", "Fountain", "Chest1", "Anvil1", "Gold", "LevelUp" } do
		thing(t, "stations")
	end
	-- The generator's own pieces (gen/*.luau): it skips a name that isn't a plain thing without a
	-- word, so a wrong one would only be missing from the map.
	for _, t in { "PlayerStart", "Torch", "Barrel", "Barrel2", "BarrelWithTools2", "Bookcase1", "Bookcase2", "Brazier", "ColorLight",
		"TeleportPentagram", "InvisibleTeleportPentagram" } do
		thing(t, "generator")
	end
	for _, p in vendorData.potions do
		thing(p.type, "vendor")
	end
	-- The acts: each act's boss in the bosses, each area's biome with a roster and its weather one
	-- of the weathers (a known kind); the gate's swirl a thing.
	local kinds = { rain = true, snow = true, storm = true, none = true }
	for a, act in ipairs(acts) do
		if not (monsterData.bosses :: any)[act.boss] then
			table.insert(bad, "act " .. a .. ": no boss " .. tostring(act.boss))
		end
		for i, spec in act.areas do
			local s: any = spec
			local where = "act " .. tostring(a) .. " area " .. tostring(i)
			if not (monsterData.rosters :: any)[s.biome] then
				table.insert(bad, where .. ": no roster for " .. tostring(s.biome))
			end
			if s.weather ~= nil then
				local w = (acts.weathers :: any)[s.weather]
				if not w or not kinds[w.kind] then
					table.insert(bad, where .. ": no weather " .. tostring(s.weather))
				end
			end
		end
	end
	thing(acts.gate.swirl, "gate")
	-- The missiles the tree's spells size (data/tree.luau's size).
	for name in (treeData.rises :: any) do
		local size = treeData.rise(name).size
		if size then
			thing(size.missile, "tree size " .. name)
		end
	end
	-- The missiles whose hits on their own hero are cancelled (the Fireball's).
	for name in (numbers.party.ownSpared :: any) do
		thing(name, "own spared")
	end
	-- The tree's spells: each name the game's (nox.spellId; its record and sounds the same by name
	-- and by that number), a spell of its region's class, in no linked group.
	local regionOf: { [string]: string } = {}
	local creatureOf: { [string]: string } = {}
	for _, n in treeData.nodes do
		if n.kind == "spell" then
			regionOf[n.spell] = n.region
		elseif n.kind == "ability" and not tree.ability(n.ability) then
			table.insert(bad, "tree: no ability " .. tostring(n.ability))
		elseif n.kind == "summon" then
			creatureOf[n.creature] = n.region
		end
	end
	-- A spell by name: its number, and whether its record and sounds are that number's.
	local function spellOf(name: string): (number?, any)
		local id = nox.spellId(name)
		local byName = nox.spellInfo(name)
		if not id or not byName then
			table.insert(bad, "tree: no spell " .. name)
			return nil, nil
		end
		local byId = nox.spellInfo(id)
		if not byId or byId.flags ~= byName.flags or nox.spellSound(name, 0) ~= nox.spellSound(id, 0) or nox.spellSound(name, 1) ~= nox.spellSound(id, 1) then
			table.insert(bad, "tree: " .. name .. " isn't spell " .. tostring(id))
			return nil, nil
		end
		if tree.spellId(name) ~= id then
			table.insert(bad, "tree: " .. name .. " isn't in the tree's numbers")
		end
		return id, byName
	end
	local function ofClass(name: string, flags: number, class: string?)
		local bits = class and (treeData.classFlags :: any)[class]
		if not class then
			table.insert(bad, "tree: " .. name .. " is on no node")
		elseif not bits or bit32.band(flags, bits + treeData.anyCaster) == 0 then
			table.insert(bad, "tree: " .. name .. " isn't a " .. class .. "'s spell")
		end
	end
	for name in (treeData.spellTitles :: any) do
		local id, info = spellOf(name)
		if id and info then
			ofClass(name, info.flags, regionOf[name])
			if bit32.band(info.flags, treeData.groupFlags) ~= 0 then
				table.insert(bad, "tree: " .. name .. " is in a linked group")
			end
		end
	end
	for name in regionOf do
		if not (treeData.spellTitles :: any)[name] then
			table.insert(bad, "tree: " .. name .. " has no title")
		end
	end
	for _, a in treeData.abilities do
		if not nox.abilitySound(a.id, 0) then
			table.insert(bad, "tree: ability " .. a.id .. " has no sound (no such ability?)")
		end
	end
	-- The tree's creatures, with no player needed: each a thing, its name the field guide's
	-- (nox.guideNumber, 1-40), its summon spell the game's and a conjurer's, one of the linked
	-- summons (its grant moves no other spell), and the one that calls that guide entry (the
	-- guide's creature n by the summon n - 1 after the first).
	local first = spellOf(treeData.firstSummon)
	for name, region in creatureOf do
		local c = (treeData.creatures :: any)[name]
		thing(name, "tree's creatures")
		if not c then
			table.insert(bad, "tree: creature " .. name .. " has no summon spell")
			continue
		end
		local n = nox.guideNumber(name)
		if not n or n < 1 or n > treeData.guideSize then
			table.insert(bad, "tree: creature " .. name .. " isn't in the field guide")
		elseif tree.guideNumber(name) ~= n then
			table.insert(bad, "tree: creature " .. name .. " isn't in the tree's guide numbers")
		end
		local id, info = spellOf(c.spell)
		if id and info then
			ofClass(c.spell, info.flags, region)
			if bit32.band(info.flags, 0x2000) == 0 or bit32.band(info.flags, 0x1000 + 0x4000 + 0x10000) ~= 0 then
				table.insert(bad, "tree: " .. c.spell .. " isn't a summon")
			end
			local want: number = tonumber(n) or 0
			local start: number = tonumber(first) or 0
			if n and first and id ~= start + want - 1 then
				table.insert(bad, string.format("tree: %s calls guide %d, not the %s's %d", tostring(c.spell), id - start + 1, tostring(name), want))
			end
		end
	end
	for name in (treeData.creatures :: any) do
		if not creatureOf[name] then
			table.insert(bad, "tree: creature " .. name .. " is on no node")
		end
	end
	-- Each spell, ability and summon node's icon (nox.spellIcon's name): a spell node's its spell,
	-- a summon's its creature's summon spell (each a SPELL_* name the game knows), an ability's its
	-- ABILITY_* name, the five each its own.
	local abilityIcons: { [string]: number } = {}
	for _, a in treeData.abilities do
		local icon = (a :: any).icon
		if type(icon) ~= "string" or string.sub(icon, 1, 8) ~= "ABILITY_" then
			table.insert(bad, "tree: ability " .. a.id .. " has no ABILITY_ icon name")
		elseif abilityIcons[icon] then
			table.insert(bad, "tree: abilities " .. abilityIcons[icon] .. " and " .. a.id .. " share the icon " .. icon)
		else
			abilityIcons[icon] = a.id
		end
	end
	for _, n in treeData.nodes do
		local icon = n.spellIcon
		if n.kind == "spell" or n.kind == "summon" then
			local want = if n.kind == "spell" then n.spell else ((treeData.creatures :: any)[n.creature] or {}).spell
			if icon ~= want or type(icon) ~= "string" or string.sub(icon, 1, 6) ~= "SPELL_" or not nox.spellId(icon) then
				table.insert(bad, "tree: node " .. n.id .. "'s icon " .. tostring(icon) .. " isn't its spell's")
			end
		elseif n.kind == "ability" then
			if type(icon) ~= "string" or abilityIcons[icon] ~= n.ability then
				table.insert(bad, "tree: node " .. n.id .. "'s icon " .. tostring(icon) .. " isn't its ability's")
			end
		elseif icon ~= nil then
			table.insert(bad, "tree: node " .. n.id .. " (" .. tostring(n.kind) .. ") has a game icon")
		end
	end
	-- The draw scales (nox.setDrawScale's 0.5-3).
	for name in (treeData.rises :: any) do
		local size = treeData.rise(name).size
		for level, at in (size and size.byLevel or {}) :: any do
			if at.draw ~= nil and (type(at.draw) ~= "number" or at.draw < 0.5 or at.draw > 3) then
				table.insert(bad, "tree: " .. name .. " " .. tostring(level) .. "'s draw scale " .. tostring(at.draw) .. " isn't 0.5-3")
			end
		end
	end
	if #bad == 0 then
		return { "ok" }
	end
	return bad
end

-- ---- The town (Phase 3)

function commands.gold(args: { string }): { string }
	local h = firstHero()
	if not h or not nox.present(h.unit) then
		return { "no player" }
	end
	local n = tonumber(args[2])
	if n then
		nox.setGold(h.unit, math.max(0, math.floor(n)), true)
	end
	return { "gold " .. nox.gold(h.unit) .. ", stashed " .. tostring(h.record.gold.stashed or 0) }
end

function commands.carried(_args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local out = {}
	for n, c in windows.carried(h) do
		local r = c.rec
		local words = { tostring(n), tostring(c.id), tostring(r.type), tostring(r.rarity), tostring(r.effectiveness), tostring(r.material),
			tostring(r.primary), tostring(r.secondary) }
		table.insert(out, table.concat(words, " "))
	end
	return out
end

-- The first hero's Nth carried item's id.
local function carriedId(h: any, n: string?): number?
	local c = windows.carried(h)[tonumber(n) or 0]
	return c and c.id
end

function commands.stash(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local op = args[2]
	if op == "open" then
		return reply(windows.open(h, "stash"), "too far from the stash")
	elseif op == "put" then
		local id = carriedId(h, args[3])
		if not id then
			return { "not: no such carried item" }
		end
		return reply(stash.put(h, id, tonumber(args[4])))
	elseif op == "take" then
		return reply(stash.take(h, tonumber(args[3]) or 0, tonumber(args[4]) or 0))
	elseif op == "deposit" or op == "withdraw" then
		return reply(stash.gold(h, op == "withdraw"))
	end
	return stash.lines(h.record)
end

function commands.vendor(args: { string }): { string }
	local h = firstHero()
	local op = args[2]
	if op == "restock" then
		vendor.restock(tonumber(args[3]) or 1)
		return { "restocked" }
	end
	if not h then
		return { "no player" }
	end
	if op == "open" then
		return reply(windows.open(h, "vendor"), "too far from the vendor")
	elseif op == "buy" then
		return reply(vendor.buy(h, tonumber(args[3]) or 0))
	elseif op == "sell" or op == "reroll" then
		local id = carriedId(h, args[3])
		if not id then
			return { "not: no such carried item" }
		end
		if op == "sell" then
			return reply(vendor.sell(h, id))
		end
		return reply(vendor.reroll(h, id))
	end
	return vendor.lines(h)
end

function commands.tree(args: { string }): { string }
	if args[2] == "describe" then
		local n = tree.node(tonumber(args[3]) or 0)
		if not n then
			return { "no such node" }
		end
		local name, what = tree.describe(n)
		-- (with a class: the tooltip's line for a hero of that class, when the node has one)
		local forYou = if args[4] then tree.forClass(n, args[4]) else nil
		return if forYou then { name, what, forYou } else { name, what }
	elseif args[2] == "costs" then
		-- The game's mana prices of the tree's spells and summonings (nox.spellInfo), "name price",
		-- the ones the cast-cost keystones change.
		local seen: { [string]: boolean } = {}
		for name in (treeData.spellTitles :: any) do
			seen[name] = true
		end
		for _, c in (treeData.creatures :: any) do
			seen[c.spell] = true
		end
		local out = {}
		for name in seen do
			local info = nox.spellInfo(name)
			table.insert(out, name .. " " .. (if info then tostring(info.manaCost) else "?"))
		end
		table.sort(out)
		return out
	end
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local op = args[2]
	if op == "take" then
		local out = {}
		for i = 3, #args do
			local ok, why = treeview.take(h, tonumber(args[i]) or 0)
			table.insert(out, if ok then "ok " .. args[i] else "not " .. args[i] .. ": " .. tostring(why))
		end
		return out
	elseif op == "respec" then
		return reply(treeview.respec(h))
	elseif op == "refund" then
		if not args[3] then
			-- (its price now, and each node taken that may be given back alone)
			local ids = {}
			for _, id in tree.of(h.record).nodes do
				if tree.canRefund(h.record, id) then
					table.insert(ids, tostring(id))
				end
			end
			table.sort(ids, function(a, b)
				return (tonumber(a) or 0) < (tonumber(b) or 0)
			end)
			return { "refund " .. tree.refundCost(h.record) .. " gold", "leaves " .. (if #ids > 0 then table.concat(ids, " ") else "none") }
		end
		return reply(treeview.refund(h, tonumber(args[3]) or 0))
	elseif op == "nodes" then
		local out = {}
		for id = 1, tree.count() do
			local n = tree.node(id)
			if not args[3] or n.region == args[3] then
				local links = {}
				for _, other in tree.neighbours(id) do
					table.insert(links, tostring(other))
				end
				table.insert(out, string.format("%d %d %d %s %s %s %s %s %s", id, n.x, n.y, n.kind, n.region, tostring(n.branch or "-"),
					tostring(n.spell or n.ability or n.creature or "-"), tostring(n.level or "-"), table.concat(links, ",")))
			end
		end
		return out
	end
	local total, spent, left = tree.points(h.record)
	local taken = {}
	for _, id in tree.of(h.record).nodes do
		table.insert(taken, tostring(id))
	end
	local t = tree.of(h.record)
	return {
		string.format("points %d, spent %d, left %d, boss %d, respec %d gold, respecs %d", total, spent, left, t.bossPoints, tree.respecCost(h.record),
			t.respecs),
		"taken " .. (if #taken > 0 then table.concat(taken, " ") else "none"),
	}
end

-- Each tree node's picture as the UI is sent it: "id kind icon", the icon the mod's picture's
-- name for a root, small, notable or keystone, the game's icon's (nox.spellIcon's) for a spell,
-- ability or summon ("-": none).
function commands.icons(_args: { string }): { string }
	local out = {}
	for id = 1, tree.count() do
		local n = tree.node(id)
		table.insert(out, string.format("%d %s %s", id, tostring(n.kind), tostring(n.icon or n.spellIcon or "-")))
	end
	return out
end

function commands.book(_args: { string }): { string }
	local h = firstHero()
	if not h or not nox.present(h.unit) then
		return { "no player" }
	end
	local want = tree.wanted(h.record)
	local out = {}
	local names: { string } = {}
	for name in want.spells do
		table.insert(names, name)
	end
	table.sort(names)
	for _, name in names do
		local id: number = tree.spellId(name) or 0
		local level: number = want.spells[name]
		table.insert(out, string.format("spell %s %d %d", name, id, level))
	end
	-- (a creature: its guide number and its summon spell's)
	local creatures: { string } = {}
	for name in want.creatures do
		table.insert(creatures, name)
	end
	table.sort(creatures)
	for _, name in creatures do
		local n: number = tree.guideNumber(name) or 0
		local spell: number = tree.summonSpell(name) or 0
		table.insert(out, string.format("creature %s %d %d", name, n, spell))
	end
	for a = 1, 5 do
		if want.abilities[a] then
			table.insert(out, string.format("ability %d %d", a, want.abilities[a]))
		end
	end
	local check = book.check(h)
	if #check == 1 and check[1] == "ok" then
		table.insert(out, "server ok")
	else
		for _, l in check do
			table.insert(out, "server " .. l)
		end
	end
	return out
end

function commands.sheet(_args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	return sheet.lines(h)
end

function commands.ui(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local name = args[2] or "sheet"
	if name == "sheet" and args[3] then
		-- (a party member's, as a click on its card asks for it)
		for _, other in hero.all() do
			if other.record.identity.name == args[3] then
				return reply(sheet.show(h, other.peer), "not now")
			end
		end
		return { "no player " .. args[3] }
	end
	local spot = if name == "stash" or name == "vendor" or name == "waypoints" then name else nil
	if spot then
		local at = town.spot(spot)
		if at and nox.present(h.unit) then
			-- (beside the station, a little south of it)
			local x, y = nox.clearPointNear(at.x, at.y + 40, 30)
			nox.setPosition(h.unit, x, y)
		end
	end
	if name == "sheet" then
		return reply(sheet.show(h, nil), "not now")
	end
	return reply(windows.open(h, name), "no such window, or not now")
end

function commands.hits(args: { string }): { string }
	if args[2] == "clear" then
		combat.clear()
		return { "cleared" }
	end
	local out = combat.lines()
	return if #out > 0 then out else { "none" }
end

function commands.summons(_args: { string }): { string }
	local out = summons.lines()
	return if #out > 0 then out else { "none" }
end

function commands.team(_args: { string }): { string }
	return team.lines(hero.all())
end

function commands.chars(args: { string }): { string }
	return characters.console(args)
end

function commands.help(_args: { string }): { string }
	return {
		"dd status | spots | save | xp N | award N | loot RARITY [N] | ground | monster ID | check",
		"dd go town|arrive|exit|guardian|boss|waypoint|wayArrive|portal|pad|wayIn|actGate|pack [N] | area N|A-N [waypoint] | portal | waypoint N|A-N",
		"dd progress | boss | smite | trial SEED N | gate [take] | seed | town",
		"dd gold [N] | carried | stash [open|put N|take T S|deposit|withdraw] | vendor [open|buy I|sell N|reroll N|restock L]",
		"dd tree [take ID...|respec|refund [ID]|nodes [REGION]|describe ID [CLASS]|costs] | icons | book | sheet | ui stash|vendor|waypoints|tree|sheet [NAME]",
		"dd as NAME COMMAND | party | scale | packs | regen | exit | vote [yes|no|cancel|seconds N] | labels | plates | spawn TIER [N] [TYPE] | slay ID | portals | portal take | hud",
		"dd option [lootNames alt|always] | hits [clear] | summons | team",
		"dd chars [clock S | lockat N|none | NAME list|play I|new|code|enter CODE|unlink I|pcs]",
	}
end

-- A console line: its replies when it's one of ours ("dd ..."), else nil (not taken).
function console.run(line: string): { string }?
	local w = words(line)
	if w[1] ~= "dd" then
		return nil
	end
	table.remove(w, 1)
	if w[1] == "as" then
		-- (the player by name, then the rest of the line as it)
		local name = w[2]
		local who = nil
		for _, h in hero.all() do
			if h.record.identity.name == name then
				who = h
			end
		end
		if not who then
			return { "no player " .. tostring(name) }
		end
		table.remove(w, 1)
		table.remove(w, 1)
		local fn = commands[w[1] or "help"] or commands.help
		acting = who
		local ok, out = pcall(fn, w)
		acting = nil
		if not ok then
			error(out, 0)
		end
		return out
	end
	local fn = commands[w[1] or "help"]
	if not fn then
		return commands.help(w)
	end
	return fn(w)
end

return console
