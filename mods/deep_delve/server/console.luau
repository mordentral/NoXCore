--!strict
-- The host's console commands ("dd ..."): what the game is doing, and a few levers for testing a
-- build (the tests drive the mod through these too). Each returns its reply lines.
--
--   dd status            the area (its act, its weather: rolled as its map was built), its map
--                        and monsters; each player's
--                        character
--   dd spots             where the area's and the town's places are
--   dd save              every character's record written now
--   dd xp N              N experience to every character
--   dd award N           N experience as a kill gives it (split between the characters in play)
--   dd loot RARITY [N] [LEVEL]   N pieces of gear of that rarity at the first player's feet (their
--                        records), rolled at the area's level or LEVEL
--   dd modifiers [all]   each modifier of a worth that does nothing (no effect the engine runs):
--                        "NAME worth W fits F drops N" (the gear types it fits by the game's rule,
--                        those it may drop on); "all": every modifier, "does" or "nothing" after
--                        it; "dd modifiers TYPE": what fits each slot of TYPE, "SLOT NAME worth W
--                        does|nothing"
--   dd monster ID [xp]   a monster's tier, name and health ("xp": "xp N", its death's experience for
--                        one player, "minion of ID" after it for a summoner's minion)
--   dd ground            the items on the ground that spark (id, effect)
--   dd go WHERE          the first player to: town, arrive, exit, guardian, boss, waypoint,
--                        wayArrive, portal, pad (the town's waypoints pentagram), wayIn, actGate
--                        (a lair's gate spot), maps (the map table), pack [N]
--   dd area N [HOW]      the game to area N, arriving at its start (HOW "waypoint": beside its
--                        waypoint). N: the area's number on through the acts (act 1's 1-7, act
--                        2's 8-16, act 3's 17-25, act 4's from 26), or "A-N": area N of act A
--   dd trial SEED N      area N (as dd area names it) built now from game seed SEED (into a map
--                        never played); "ok", or why it was refused or what it lacks (its act's
--                        town and every one of its stations too); "dd trial SEED run BIOME RANK":
--                        an expedition's map
--   dd seed              the play's seed and which play of the game it is (an emptied game
--                        starting over is a new play, with a new seed)
--   dd town              the act's town: its look and name
--   dd sight             how far the map's monsters look (its biome's, or the engine's own), and the
--                        mod's live monsters' reach
--   dd gate [take]       the lair's gate to the next act (its spot, open or shut, its swirl and
--                        light); "take": the first player steps into it (the game on, or a vote)
--   dd atlas [levels | free on|off | go BIOME RANK | run | slay]   the expeditions: the first
--                        player's atlas; each rank's level; every rank open to everyone (testing);
--                        an expedition asked for (as the map table's window does); the run being
--                        played; its champion killed by the first player
--   dd portal            the first player opens a town portal (as its UI's button does)
--   dd waypoint N        the first player to area N's waypoint of the act being played ("A-N":
--                        of act A; one it has found), as a click in the town's waypoints window
--                        sends it (wherever it stands); "dd waypoint wind": this area's waypoint's
--                        cycle now (idle, up, down)
--   dd progress          the first player's record: its version, its act progress, boss kills
--   dd boss              the boss of this map: id, type, health, name
--   dd fight [now]       the boss's fight (the barrow-king's: its phase, adds, charges, mists,
--                        fists, graves; the troll chieftain's: its phase, scorched or mending, what
--                        it mended, its kin, stones, roots, pulls, stomps; the ruin-warden's: its
--                        phase, plates, pounds and misses, reels, wardens, quakes, sand-devils,
--                        stones); "now": its next blows on the next beat
--   dd smite             the boss killed by the first player
--   dd tiers             the difficulty tiers' level offset, tier I's last level, tier II's first
--   dd number NAME [I]   one of the game's numbers (gamedata), by name and index
--   dd check             every name in the data tables checked against thing.bin, every spell
--                        against the game's spells (its number, its class, no linked group),
--                        every creature the tree gives (its name the field guide's, its summon
--                        spell the one that calls it), every spell, ability and summon node's
--                        icon name (its spell's, its ability's ABILITY_* name, its creature's
--                        summon spell), the draw scales within 0.5-3 (no player needed)
--   dd types             every creature type the mod makes (rosters, champions, bosses, their
--                        adds), sorted
--   dd gold [N]          the first player's gold (set to N)
--   dd carried [grid]    what it carries: "n id type rarity worn" (n: the stash's and vendor's N,
--                        the newest first; its secondary weapon's line ends "secondWeapon"); "grid": the stash's and vendor's carried grid, a cell
--                        a line, "k type rarity count ids" (the gear, then the stacked potions and
--                        supplies, each the oldest first)
--   dd stash [open | put N | take TAB SLOT | deposit | withdraw]   its stash (as its window
--                        does: by the chest); alone, its contents "tab slot type rarity mods"
--   dd vendor [open | buy I | sell N | reroll N | restock LEVEL | stock ITEM | repair ID |
--             repairall | repairs | gear]   the vendor (by the anvil); "stock": ITEM (as dd give
--                        names it) put at the end of its stock; "repair": the piece of that item
--                        id (worn or carried) repaired; "repairs": what needs one, "id type cost
--                        worn|carried"; "gear": its rolled gear, "index type rarity effectiveness
--                        material primary secondary affixes"; alone, its stock "index type rarity
--                        price"
--   dd roll TYPE RARITY N [LEVEL] [CLASS]   N records of TYPE rolled at RARITY as the loot rolls
--                        them (CLASS: rolled for that class, as a vendor's stock is), nothing made:
--                        "rarity effectiveness material primary secondary affixes" each (N 250 at
--                        most)
--   dd give ITEM         an item into its inventory: TYPE RARITY [LEVEL] [SLOT=MODIFIER]...
--                        [affix=ID:VALUE]... (SLOT effectiveness, material, primary, secondary);
--                        or "unique ID [LEVEL]", "set PIECE [LEVEL]": a unique or a set piece
--   dd uniques [list | check | odds N LEVEL TIER [BOSS] | drop unique|set ID [LEVEL] | worn | powers]
--                        uniques and sets (uniqueconsole.luau): each one; the data checked
--                        against the game; N deaths rolled; one put at the first player's feet;
--                        the first player's worn sets and powers; its worn powers, how often each went
--                        off, their state
--   dd affixes [TYPE N [LEVEL] [CLASS] | worn | allowed [TYPE]]   Deep Delve's affixes: each
--                        one's id, stat, kinds and numbers; N rare TYPEs rolled (for CLASS), each
--                        "rarity id:value,..."; the first player's worn totals and its weapon;
--                        "allowed": each gear type (or TYPE) "TYPE kind classes ids" (classes: who
--                        may use it, ids: the affixes that may roll on it)
--   dd ward              the first player's ward: the harmful enchantments it cut short, when each
--                        runs out ("NAME ends E full F", frames from now; "poison" for its step)
--   dd tip carried N | stash TAB SLOT | stock I   an item's tooltip lines as its window gets them
--   dd tips [on | off]   its held items' tooltips by net code (as a UI that tips the game's
--                        inventory asks); alone, what was last sent ("code id:worn:durability:charges")
--   dd wire              its lists as last sent to its UI: "kind bytes entries parts"
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
--   dd sheet [gear]      its character sheet, "key value" lines; "gear": the sheet's Gear page,
--                        its worn affixes' lines then "--" and its uniques' and sets' (tagged),
--                        then "bytes N" (the value's size on the wire)
--   dd ui WINDOW [NAME]  a window opened on its UI (stash, vendor, waypoints, maps: it's moved by
--                        the station); "sheet NAME": party member NAME's sheet, as its card's click
--   dd upgrades [clear]  its stock abilities' upgrades: each held ("upgrade KEY"), how often each
--                        went off ("count WHAT N"), the charge's cooldown, its Rallying Cry, the
--                        last Trample and Chain, what its harpoon holds; or the counts emptied
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
--   dd plates            the named monsters' name plates ("id code tier r,g,b plate": the plate as
--                        shown, its powers' words and its name) and the codes the first player's
--                        UI was last sent
--   dd spawn TIER [N] [TYPE] [POWERS]   N monsters (1-60) of that tier (normal, magic, rare,
--                        champion: the area's biome's champion, its name and set) round the first
--                        player, of TYPE or the biome roster's first (a champion: its own; "-": that
--                        one); POWERS: their powers, comma-separated ids, or "none" (else its tier's
--                        rolled); "id code tier name"
--   dd powers [clear]    each live monster with powers ("id type tier powers drops D xp X", a
--                        summoner's then "minions IDS", then "beat 1" while its beat runs, else
--                        "beat 0"), how many have a beat now ("beating N")
--                        and what the powers have done ("done: ..."); "clear": those counts back
--                        to none
--   dd slay ID           one of the mod's monsters killed by the first player
--   dd portals           the open portals, "owner x y" ("owner(left)": its owner left the game)
--   dd portal take       the first player through the town's portal pentagram (as its touch)
--   dd decals            the pads' rings on the map's floor, "kind x y" (waypoint, portal)
--   dd hud               how many values the first player's UI is owed (sends not taken yet)
--   dd option [NAME VALUE]   the first player's options ("name value"), or one set (as its
--                        Character window does): lootNames alt | always
--   dd secrets [...]     the area's secret room and chests (secrets.luau): alone, "secret X Y lever
--                        X Y|open|none walls N guards G alive A", its "wall X Y" cells, each "chest
--                        I ID TYPE X Y kind K trap T shut|open [gave N]" (I 0: the secret room's);
--                        a locked room's (lever "lock") "lock", "door", "gap", "way" and
--                        "keyguard" lines; "trial SEED N" area N built and summed up; "go lever|
--                        room|chest I|door|keyguard|key" the first player to it (against a chest:
--                        its touch opens it); "take [NAME]" the locked room's key picked up; "trap I KIND|
--                        none" chest I's trap set; "odds KIND N [LEVEL]" N chests' loot counted
--                        (KIND hoard, chest, trapped, monster); "sprung" the last traps gone off
--   dd chars [...]       server characters (roster.luau): alone, each player's seat; "clock
--                        SECONDS" the link codes' clock moved on; "lockat N|none" the game closed to
--                        newcomers from N players (its lock policy, numbers.roster.lockAt); "NAME list | play I | new |
--                        code | enter CODE | unlink I | pcs" as its character select does

local areas = require("areas")
local atlas = require("atlas")
local boss = require("boss")
local expeditions = require("expeditions")
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
local powers = require("powers")
local scale = require("scale")
local secrets = require("secrets")
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
local items = require("items")
local affixes = require("affixes")
local uniques = require("uniques")
local uniquePowers = require("uniquepowers")
local uniqueConsole = require("uniqueconsole")
local warrior = require("warrior")
local actives = require("actives")
local upgrades = require("upgrades")
local tips = require("tips")
local tree = require("tree")
local treeview = require("treeview")
local vendor = require("vendor")
local windows = require("windows")
local acts = require("data/acts")
local expeditionData = require("data/expeditions")
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
	local level = tonumber(args[4]) or areas.status().level
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

-- N records of a type rolled at a rarity as the loot rolls them (for a class, as a vendor's stock
-- is), nothing made: "rarity effectiveness material primary secondary affixes" each (dd roll).
function commands.roll(args: { string }): { string }
	local t, rarity = args[2], args[3]
	if not t or not nox.thingType(t) then
		return { "no such type " .. tostring(t) }
	end
	if rarity ~= "normal" and rarity ~= "magic" and rarity ~= "rare" then
		return { "rarity normal, magic or rare" }
	end
	local level = tonumber(args[5]) or areas.status().level
	local out = {}
	for _ = 1, math.clamp(tonumber(args[4]) or 1, 1, 250) do
		local rec = loot.makeGear(t, rarity, level, args[6])
		local list = {}
		for _, a in rec.affixes or {} do
			table.insert(list, a.id .. ":" .. tostring(a.value))
		end
		table.insert(out, string.format("%s %s %s %s %s %s", rec.rarity, tostring(rec.effectiveness), tostring(rec.material), tostring(rec.primary),
			tostring(rec.secondary), if #list > 0 then table.concat(list, ",") else "-"))
	end
	return out
end

function commands.modifiers(args: { string }): { string }
	if args[2] and args[2] ~= "all" then
		if not nox.thingType(args[2]) then
			return { "no such type " .. args[2] }
		end
		local fits = loot.fitLines(args[2])
		return if #fits > 0 then fits else { "none" }
	end
	local out = loot.modifierLines(args[2] == "all")
	return if #out > 0 then out else { "none" }
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
	-- ("dd monster ID xp": its death's experience for one player, and whether it's a minion)
	if args[3] == "xp" then
		return { "xp " .. tostring(m.experience) .. (if m.minion then " minion of " .. tostring(m.master) else "") }
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
		-- (the map table)
		maps = t.maps,
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
	-- (the map table's spot names its own desk)
	local types: any = { portal = "InvisibleTeleportPentagram", pad = "InvisibleTeleportPentagram", wayIn = "InvisibleTeleportPentagram", waypoint = "TeleportPentagram" }
	for _, name in { "arrive", "back", "exit", "guardian", "boss", "waypoint", "wayArrive", "gate", "healer", "stash", "vendor", "portal", "pad", "wayIn", "actGate",
		"maps" } do
		local p = place(s, name)
		if p then
			table.insert(out, string.format("%s %.1f %.1f %s", name, tonumber(p.x) or 0, tonumber(p.y) or 0, tostring(p.type or types[name] or "-")))
		else
			table.insert(out, name .. " none")
		end
	end
	table.insert(out, string.format("packs %d", #s.packs))
	-- (a barrow arena's graves: "grave X Y" each; a glade's ring pieces, fire pits and kin points:
	-- "post X Y", "fire X Y", "kin X Y")
	local kinds: { { any } } = { { "grave", s.graves }, { "post", s.ring }, { "fire", s.fires }, { "kin", s.kin } }
	for _, list in kinds do
		local name: string = tostring(list[1])
		for _, g in list[2] or {} do
			table.insert(out, string.format("%s %.1f %.1f", name, tonumber(g.x) or 0, tonumber(g.y) or 0))
		end
	end
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

-- "dd sight": how far the map's monsters look ("sight 450", or "sight engine": the engine's own),
-- then the mod's live monsters' reach: "monsters N, reach LOW-HIGH" (each one's nox.sightRange).
function commands.sight(_args: { string }): { string }
	local s = monsters.sight()
	local n, low, high = 0, math.huge, 0
	for id in monsters.all() do
		local ok, r = pcall(function(): number
			return nox.sightRange(id)
		end)
		if ok and nox.living(id) then
			n += 1
			low = math.min(low, r)
			high = math.max(high, r)
		end
	end
	return {
		"sight " .. (if s then tostring(s) else "engine"),
		string.format("monsters %d, reach %d-%d", n, if n > 0 then math.floor(low + 0.5) else 0, math.floor(high + 0.5)),
	}
end

function commands.area(args: { string }): { string }
	local n = placeNamed(args[2]) or 0
	local ok, why = areas.travel(n, if args[3] == "waypoint" then "waypoint" else "start")
	return { if ok then "going to area " .. n else "not going: " .. tostring(why) }
end

-- "dd trial SEED N": the area built; "ok" when it built with every spot its kind needs (a waypoint
-- area's waypoint, by the start chamber when its data says so; a lair's boss and, with an act
-- after it, its gate spot; an area with a way on, its exit; a run's champion's spot), else why.
-- "dd trial SEED run BIOME RANK": an expedition's map, the same way.
function commands.trial(args: { string }): { string }
	local seed, place = tonumber(args[2]) or 1, placeNamed(args[3]) or 1
	if args[3] == "run" then
		place = expeditions.newRun(tostring(args[4]), tonumber(args[5]) or 1)
		if not areas.info(place).expedition then
			return { "no such biome " .. tostring(args[4]) }
		end
	end
	local ok, result = areas.trial(place, seed)
	if not ok then
		return { string.format("seed %d area %d: %s", seed, place, tostring(result)) }
	end
	local info = areas.info(place)
	local s: any = result
	local missing: string? = nil
	if info.waypoint and not s.waypoint then
		missing = "waypoint"
	elseif info.waypointAt == "start" and s.waypoint
		and math.abs(s.waypoint.x - s.arrive.x) + math.abs(s.waypoint.y - s.arrive.y) > 8 * 23 then
		missing = "waypoint by the start"
	elseif info.lair and not s.boss then
		missing = "boss"
	elseif info.arena == "barrow" and (type(s.graves) ~= "table" or #s.graves < 3) then
		missing = "graves"
	elseif info.arena == "glade" and (type(s.ring) ~= "table" or #s.ring < 3) then
		missing = "ring"
	elseif info.arena == "glade" and (type(s.fires) ~= "table" or #s.fires < 2) then
		missing = "fire pits"
	elseif info.arena == "glade" and (type(s.kin) ~= "table" or #s.kin < 1) then
		missing = "way in"
	elseif info.arena == "court" and (type(s.ring) ~= "table" or #s.ring < 3) then
		missing = "statues"
	elseif info.gate and not s.gate then
		missing = "gate"
	elseif not info.lair and not s.exit then
		missing = "exit"
	elseif info.expedition and not s.guardian then
		missing = "champion's spot"
	elseif not s.town or s.town.look ~= areas.townOf(info.act).look then
		missing = "town of act " .. info.act
	else
		-- (the town's stations, every one, in every act's look)
		for _, name in { "centre", "healer", "gate", "stash", "vendor", "portal", "waypoints", "wayIn", "maps" } do
			if not missing and not s.town[name] then
				missing = "town " .. name
			end
		end
	end
	if missing then
		return { string.format("seed %d area %d: no %s", seed, place, tostring(missing)) }
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

-- "dd atlas": the first player's atlas ("atlas open|shut, best B, top T, runs R, free on|off", then
-- "BIOME r r..." for each biome with a clear). "dd atlas levels": the last act ("last act A, lair
-- A-N, level L") and each rank's area level ("rank R level L"). "dd atlas
-- free on|off": every rank open to everyone (testing; never kept). "dd atlas go BIOME RANK": the
-- first player asks for an expedition, as the map table's window does (wherever it stands). "dd
-- atlas run": the run being played. "dd atlas slay": its champion killed by the first player.
function commands.atlas(args: { string }): { string }
	local op = args[2]
	if op == "levels" then
		-- (first, the last act: its lair, as "dd area" names it, and the level the ladder stands on)
		local out = { string.format("last act %d, lair %d-%d, level %d", #acts, #acts, #acts[#acts].areas, atlas.lastLevel()) }
		for r = 1, atlas.ranks() do
			table.insert(out, string.format("rank %d level %d", r, atlas.level(r)))
		end
		return out
	elseif op == "free" then
		expeditions.setFree(args[3] == "on")
		return { "ok" }
	elseif op == "run" or op == "slay" then
		local c = expeditions.current()
		if not c then
			return { "no run" }
		end
		if op == "slay" then
			local h = firstHero()
			local id = c.champion
			if not h or not id or not nox.living(id) then
				return { "no champion standing" }
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
		local alive: string = if c.champion and nox.living(c.champion) then "alive" else "dead"
		local done: string = if c.cleared then "cleared" else "not cleared"
		local rank: number, level: number = tonumber(c.rank) or 0, tonumber(c.level) or 0
		return { string.format("run %s rank %d level %d champion %s %s, %s", tostring(c.biome), rank, level, tostring(c.champion or "-"), alive, done) }
	end
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	if op == "go" then
		local ok, why = expeditions.start(h, tostring(args[3]), tonumber(args[4]) or 0)
		return { if ok then (if why == "vote" then "vote" else "going") else "not: " .. tostring(why) }
	end
	local a = atlas.of(h.record)
	local out = {
		string.format("atlas %s, best %d, top %d, runs %d, free %s", if atlas.isOpen(h.record) then "open" else "shut", a.best, atlas.top(h.record), a.runs,
			if expeditions.isFree() then "on" else "off"),
	}
	local names: { string } = {}
	for name in a.cleared do
		table.insert(names, tostring(name))
	end
	table.sort(names)
	for _, name in names do
		local ranks: { string } = {}
		for _, r in a.cleared[name] do
			table.insert(ranks, tostring(r))
		end
		table.insert(out, name .. " " .. table.concat(ranks, " "))
	end
	return out
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
	local thingType = if args[4] == "-" then nil else args[4]
	if tier == "champion" then
		fixed = (monsterData.champions :: any)[s.roster] or monsterData.champions.Crypt
		thingType = thingType or fixed.type
	end
	-- (its powers named: each must be one)
	local want: { string }? = nil
	if args[5] then
		want = {}
		if args[5] ~= "none" then
			for p in string.gmatch(args[5], "[^,]+") do
				if not powers.def(p) then
					return { "powers: " .. table.concat(powers.ids(), ", ") }
				end
				table.insert(want :: { string }, p)
			end
		end
	end
	local roster: any = (monsterData.rosters :: any)[s.roster] or monsterData.rosters.Crypt
	local out = {}
	for i = 1, math.clamp(tonumber(args[3]) or 1, 1, 60) do
		-- (a widening ring round the player)
		local r = 60 + 6 * i
		local id = monsters.spawn(thingType or roster[1].type, o.x + r * math.cos(i * 1.3), o.y + r * math.sin(i * 1.3), tier, s.level, fixed, want)
		local m: any = if id then monsters.info(id) else nil
		if id and m then
			table.insert(out, table.concat({ tostring(id), tostring(m.code), tier, tostring(m.name or "-") }, " "))
		end
	end
	return if #out > 0 then out else { "not made" }
end

-- "dd powers [clear]": the live monsters with powers and what the powers have done.
function commands.powers(args: { string }): { string }
	if args[2] == "clear" then
		powers.clear()
		return { "cleared" }
	end
	return powers.lines()
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
	if args[2] == "wind" then
		return { waypoints.wind() }
	end
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

-- "dd fight [now]": the boss's fight ("phase P awake|asleep, adds standing/raised, charges N (hits
-- H), mists M, fists F, graves G": the barrow-king's; "phase P awake|asleep, scorched|mending,
-- mended M, scorches S, adds standing/called, stones N, roots R, pulls P, stomps S, fires F": the
-- troll chieftain's; "phase P awake|asleep, plates N, plated|open|bare, pounds P (misses M, hits
-- H), reels R, adds standing/called, quakes Q, devils D (scoured S), stones N, ring R": the
-- ruin-warden's), "no fight" for one without such a state; "now": its next blows on the next beat.
function commands.fight(args: { string }): { string }
	return { boss.fight(args[2] == "now") or "no fight" }
end

-- "dd tiers": the difficulty tiers' level offset (data/acts.luau's tierOffset) and what it makes
-- of tier I's last area and tier II's first: "offset O; tier I to level L; tier II from level F".
function commands.tiers(_args: { string }): { string }
	local last = areas.info(areas.count())
	local first = areas.info(1)
	return { string.format("offset %d; tier I to level %d; tier II from level %d", acts.tierOffset, last.level, first.level + acts.tierOffset) }
end

-- "dd number NAME [I]": one of the game's numbers (gamedata), for reading the spells' sizes.
function commands.number(args: { string }): { string }
	local v = nox.gameNumber(args[2] or "", tonumber(args[3]))
	return { tostring(v) }
end

-- "dd types": every creature type the mod makes (each roster's, champion's and boss's, a boss's
-- adds, the expeditions' own), sorted, one a line (for the casters' check: a test reads each one's
-- spells from the engine).
function commands.types(_args: { string }): { string }
	local seen: { [string]: boolean } = {}
	local function add(t: string)
		seen[t] = true
	end
	for _, roster in (monsterData.rosters :: any) do
		for _, e in roster do
			add(e.type)
		end
	end
	for _, c in (monsterData.champions :: any) do
		add(c.type)
	end
	for _, b in (monsterData.bosses :: any) do
		add(b.type)
		for _, t in b.adds do
			add(t)
		end
		for _, t in b.laterAdds or {} do
			add(t)
		end
	end
	for _, roster in (expeditionData.rosters :: any) do
		for _, e in roster do
			add(e.type)
		end
	end
	for _, c in (expeditionData.champions :: any) do
		add(c.type)
	end
	local out: { string } = {}
	for t in seen do
		table.insert(out, t)
	end
	table.sort(out)
	return out
end

function commands.check(_args: { string }): { string }
	local bad = {}
	local function thing(name: string, where: string)
		if not nox.thingType(name) then
			table.insert(bad, where .. ": no thing " .. name)
		end
	end
	-- (a type's own health factor: a thing, a factor above 0)
	for t, f in (monsterData.typeHealth :: any) do
		thing(t, "typeHealth")
		if type(f) ~= "number" or f <= 0 then
			table.insert(bad, "typeHealth " .. tostring(t) .. ": no factor")
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
		for _, t in b.laterAdds or {} do
			thing(t, "boss " .. name .. " later adds")
		end
		if b.fight ~= "warlord" and b.fight ~= "barrow" and b.fight ~= "chief" and b.fight ~= "warden" then
			table.insert(bad, "boss " .. name .. ": no fight " .. tostring(b.fight))
		end
	end
	-- (the barrow-king's spells and the troll chieftain's: the game's)
	for _, s in { "SPELL_TOXIC_CLOUD", "SPELL_FIST", "SPELL_EARTHQUAKE", "SPELL_FORCE_OF_NATURE" } do
		if not nox.spellId(s) then
			table.insert(bad, "boss: no spell " .. s)
		end
	end
	-- (the troll chieftain's stone, the arena looks' pieces and the act 3 town's: the generator
	-- writes a plain one bare and any other with its own data, so each must be a thing)
	for _, t in { "ThrowingStone", "OgreFirePit", "OgrePostSkull1", "OgrePostSkull2", "ForestLog01", "ForestLog03", "Stump1", "Stump2", "Stump3", "Stump4",
		"OgreHutMeat", "OutdoorTraderPupTent", "TraderBowRack1", "TraderBowRack2", "CampFireUnused", "TreeForest02", "Mushroom4", "PlantFern1",
		"PlantFern2", "FoliageDense1", "Bush6", "TorchPole", "Tombstone1", "Monument1", "Tombstone5" } do
		thing(t, "arena or town")
	end
	-- (act 4's court and the Last Well's)
	for _, t in { "StatueRuin1", "StatueRuin2", "StatueRuin3", "StatueRuin4", "RuinsColumnOutdoor01", "RuinsColumnOutdoor02", "RuinsColumnOutdoor03",
		"RuinsColumnOutdoor04", "RuinsColumnOutdoorRubble01", "RuinsColumnOutdoorRubble02", "RuinsColumnOutdoorRubble03", "RuinsColumnOutdoorRubble04",
		"OutdoorTraderCart", "TraderAppleCrate", "OutdoorTraderHelmPoles", "WaterBarrel", "Well", "PlantFern3", "PlantFern4", "TreeGalava07",
		"RuinsColumnOutdoorShort01", "PlantBarren1", "PlantBarren2", "PlantBarren3", "TreeGalava06" } do
		thing(t, "arena or town")
	end
	for _, g in lootData.gear do
		thing(g.type, "gear")
	end
	-- (the warrior's weapon families: each kind some item type's, each thrown missile a thing)
	warrior.check(bad, thing)
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
	for _, p in vendorData.supplies do
		thing(p.type, "vendor")
	end
	-- The acts: each act's boss in the bosses, each area's roster (its own or its biome's) with its
	-- champion; each sky's weathers among the weathers (a known kind), each with a weight; the
	-- gate's swirl a thing.
	local kinds = { rain = true, snow = true, storm = true, none = true }
	for a, act in ipairs(acts) do
		if not (monsterData.bosses :: any)[act.boss] then
			table.insert(bad, "act " .. a .. ": no boss " .. tostring(act.boss))
		end
		for i, spec in act.areas do
			local s: any = spec
			local where = "act " .. tostring(a) .. " area " .. tostring(i)
			local roster = s.roster or s.biome
			if not (monsterData.rosters :: any)[roster] then
				table.insert(bad, where .. ": no roster " .. tostring(roster))
			elseif not s.lair and not (monsterData.champions :: any)[roster] then
				table.insert(bad, where .. ": no champion for " .. tostring(roster))
			end
			if s.sky ~= nil and not (acts.skies :: any)[s.sky] then
				table.insert(bad, where .. ": no skies " .. tostring(s.sky))
			end
		end
	end
	for biome, list in (acts.skies :: any) do
		for _, e in list do
			local w = (acts.weathers :: any)[e[1]]
			if not w or not kinds[w.kind] or type(e[2]) ~= "number" or e[2] <= 0 then
				table.insert(bad, "skies " .. tostring(biome) .. ": no weather " .. tostring(e[1]))
			end
		end
	end
	-- (the ruin-warden's storm: one of the weathers)
	if not (acts.weathers :: any)[numbers.ruinWarden.storm.weather] then
		table.insert(bad, "ruinWarden storm: no weather " .. tostring(numbers.ruinWarden.storm.weather))
	end
	-- (each biome's monsters' look: a reach past the engine's own 250)
	for biome, r in (acts.sights :: any) do
		if type(r) ~= "number" or r <= 250 then
			table.insert(bad, "sights " .. tostring(biome) .. ": no reach past 250")
		end
	end
	thing(acts.gate.swirl, "gate")
	-- The secrets' chests, trap caster and trap spells.
	secrets.check(bad)
	-- The expeditions: the map table's pieces; each biome offered with a roster and a champion (the
	-- acts' or its own), its skies' weathers known; the expeditions' own rosters' types.
	local loaded = areas.status().spots
	local maps = loaded and loaded.town and loaded.town.maps
	if maps then
		-- (the desk the generator chose, by the wall it stands against: towns.mapTable)
		thing(tostring(maps.type), "map table")
	end
	for biome, roster in (expeditionData.rosters :: any) do
		for _, e in roster do
			thing(e.type, "expedition roster " .. biome)
		end
	end
	for biome, c in (expeditionData.champions :: any) do
		thing(c.type, "expedition champion " .. biome)
	end
	for _, entry in expeditionData.biomes do
		local b: any = entry
		local where = "expedition " .. tostring(b.name)
		if not ((monsterData.rosters :: any)[b.name] or (expeditionData.rosters :: any)[b.name]) then
			table.insert(bad, where .. ": no roster")
		end
		if not ((monsterData.champions :: any)[b.name] or (expeditionData.champions :: any)[b.name]) then
			table.insert(bad, where .. ": no champion")
		end
		if not acts[b.town] then
			table.insert(bad, where .. ": no act " .. tostring(b.town) .. " for its town")
		end
		for _, e in b.skies or {} do
			local w = (acts.weathers :: any)[e[1]]
			if not w or not kinds[w.kind] or type(e[2]) ~= "number" or e[2] <= 0 then
				table.insert(bad, where .. ": no weather " .. tostring(e[1]))
			end
		end
	end
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
	-- The uniques and sets (their types, modifiers by the fit rule, classes, powers, spells).
	local u = uniques.check(uniquePowers.KINDS)
	if u[1] ~= "ok" then
		for _, l in u do
			table.insert(bad, "uniques: " .. l)
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

function commands.carried(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local out = {}
	if args[2] == "grid" then
		-- (the stash's and vendor's carried grid, a cell a line: "k type rarity count ids")
		for k, c in windows.grid(h) do
			local ids = {}
			for _, id in c.ids do
				table.insert(ids, tostring(id))
			end
			table.insert(out, string.format("%d %s %s %d %s", k, tostring(c.rec.type), tostring(c.rec.rarity), #c.ids, table.concat(ids, ",")))
		end
		return if #out > 0 then out else { "none" }
	end
	local second = if nox.present(h.unit) then nox.secondaryWeapon(h.unit) else nil
	for n, c in windows.carried(h) do
		local r = c.rec
		local words = { tostring(n), tostring(c.id), tostring(r.type), tostring(r.rarity), tostring(r.effectiveness), tostring(r.material),
			tostring(r.primary), tostring(r.secondary) }
		if c.id == second then
			table.insert(words, "secondWeapon")
		end
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

-- An item record from the console's words, from args[from]: TYPE RARITY [LEVEL] [SLOT=MODIFIER]...
-- [affix=ID:VALUE]... (SLOT: effectiveness, material, primary, secondary); nil and why when a
-- word is wrong.
local function recordFrom(args: { string }, from: number): (any, string?)
	local t, rarity = args[from], args[from + 1]
	-- ("unique ID [LEVEL]" or "set PIECE [LEVEL]": a unique's or a set piece's record)
	if t == "unique" or t == "set" then
		local level = tonumber(args[from + 2])
		local rec = if t == "unique" then uniques.make(rarity or "", level) else uniques.makePiece(rarity or "", level)
		return rec, if rec then nil else "no such " .. (if t == "unique" then "unique " else "set piece ") .. tostring(rarity)
	end
	if not t or not nox.thingType(t) then
		return nil, "no such type " .. tostring(t)
	end
	if rarity ~= "normal" and rarity ~= "magic" and rarity ~= "rare" then
		return nil, "rarity normal, magic or rare"
	end
	local rec: any = { type = t, rarity = rarity }
	for i = from + 2, #args do
		local key, value = string.match(args[i], "^(%a+)=(.+)$")
		if tonumber(args[i]) then
			rec.level = tonumber(args[i])
		elseif key == "affix" and value then
			local id, n = string.match(value, "^(%a+):(%d+)$")
			if not id then
				return nil, "affix=ID:VALUE"
			end
			-- (only one its kind may carry, as the loot rolls them)
			local fits = false
			for _, def in affixes.allowed(t) do
				fits = fits or def.id == id
			end
			if not fits then
				return nil, (if affixes.def(id) then id .. " doesn't go on a " .. t else "no such affix " .. id)
			end
			rec.affixes = rec.affixes or {}
			table.insert(rec.affixes, { id = id, value = tonumber(n) })
		elseif key and value and table.find(items.SLOTS, key) then
			if not nox.modifier(value) or not nox.modifierFits(t, key, value) then
				return nil, value .. " doesn't fit a " .. t .. "'s " .. key
			end
			rec[key] = value
		else
			return nil, "what is " .. args[i]
		end
	end
	return rec, nil
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
	elseif op == "repair" then
		-- (by the item's id: a worn piece is in no carried list)
		return reply(vendor.repair(h, tonumber(args[3]) or 0))
	elseif op == "repairall" then
		return reply(vendor.repairAll(h))
	elseif op == "repairs" then
		local out = {}
		for _, dmg in vendor.damaged(h) do
			local where: string = if dmg.facts.worn then "worn" else "carried"
			table.insert(out, tostring(dmg.id) .. " " .. tostring(dmg.rec.type) .. " " .. tostring(dmg.cost) .. " " .. where)
		end
		return if #out > 0 then out else { "none" }
	elseif op == "stock" then
		local rec, why = recordFrom(args, 3)
		if not rec then
			return { "not: " .. tostring(why) }
		end
		return { "ok " .. vendor.addStock(h, rec) }
	elseif op == "gear" then
		return vendor.gearLines(h)
	end
	return vendor.lines(h)
end

-- An item made from the console's words into the first player's inventory (dd give).
function commands.give(args: { string }): { string }
	local h = firstHero()
	if not h or not nox.present(h.unit) then
		return { "no player" }
	end
	local rec, why = recordFrom(args, 2)
	if not rec then
		return { "not: " .. tostring(why) }
	end
	-- (as the stash hands a piece back: one its class may not use by the pickup)
	local id, why = items.hand(h.unit, rec)
	return if id then { "ok " .. tostring(id) } else { "not: " .. tostring(why) }
end

-- An item's tooltip lines as its window's entry carries them: "carried N", "stash TAB SLOT",
-- "stock I" (dd tip).
function commands.tip(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local where = args[2]
	if where == "carried" then
		local c = windows.carried(h)[tonumber(args[3]) or 0]
		return if c then tips.lines(c.rec, c.facts, h) else { "nothing there" }
	elseif where == "stash" then
		local tab = stash.of(h.record).tabs[tonumber(args[3]) or 0]
		local rec = tab and tab[tonumber(args[4]) or 0]
		return if rec then tips.lines(rec, nil, h) else { "nothing there" }
	elseif where == "stock" then
		local s = vendor.stockOf(h)[tonumber(args[3]) or 0]
		if not s then
			return { "nothing there" }
		end
		return tips.lines(if s.potion or s.supply then { type = s.type, rarity = "normal" } else s.rec, nil, h)
	end
	return { "dd tip carried N | stash TAB SLOT | stock I" }
end

-- Deep Delve's affixes (dd affixes): alone, each one "id stat kinds low high perLevel most"
-- (kinds "any" or the gear kinds, most "-" for none); "TYPE N [LEVEL]": N rare pieces of TYPE
-- rolled (records only, nothing made), each "rarity id:value,id:value" ("-" for none);
-- "worn": the first hero's worn affixes' totals, "stat value" each, and its weapon's id.
function commands.affixes(args: { string }): { string }
	local out = {}
	if args[2] == "worn" then
		local h = firstHero()
		if not h then
			return { "no player" }
		end
		for _, def in lootData.affixes :: { any } do
			table.insert(out, def.stat .. " " .. tostring(h.bonus[def.stat] or 0))
		end
		table.insert(out, "weapon " .. tostring(h.bonus.weapon or "-") .. " " .. tostring(h.bonus.ammo or "-"))
		return out
	elseif args[2] == "allowed" then
		local types: { string } = {}
		if args[3] then
			table.insert(types, args[3])
		else
			for _, g in lootData.gear do
				table.insert(types, g.type)
			end
		end
		for _, t in types do
			local st = nox.itemStats(t)
			local cls = {}
			for c, yes in (if st and st.classes then st.classes else {}) :: any do
				if yes then
					table.insert(cls, c)
				end
			end
			table.sort(cls)
			local ids = {}
			for _, def in affixes.allowed(t) do
				table.insert(ids, def.id)
			end
			table.insert(out, string.format("%s %s %s %s", t, tostring(affixes.kindOf(t)), if #cls > 0 then table.concat(cls, ",") else "-",
				if #ids > 0 then table.concat(ids, ",") else "-"))
		end
		return out
	elseif args[2] then
		if not nox.thingType(args[2]) then
			return { "no such type " .. args[2] }
		end
		local level = tonumber(args[4]) or areas.status().level
		for _ = 1, math.clamp(tonumber(args[3]) or 1, 1, 2000) do
			local rec = loot.makeGear(args[2], "rare", level, args[5])
			local ids = {}
			for _, a in rec.affixes or {} do
				table.insert(ids, a.id .. ":" .. tostring(a.value))
			end
			table.insert(out, rec.rarity .. " " .. (if #ids > 0 then table.concat(ids, ",") else "-"))
		end
		return out
	end
	for _, def in lootData.affixes :: { any } do
		local kinds = {}
		for k in (def.kinds or {}) :: any do
			table.insert(kinds, k)
		end
		table.sort(kinds)
		table.insert(out, string.format("%s %s %s %s %s %s %s", tostring(def.id), tostring(def.stat), if #kinds > 0 then table.concat(kinds, ",") else "any",
			tostring(def.low), tostring(def.high), tostring(def.perLevel), tostring(def.most or "-")))
	end
	return out
end

-- The first hero's ward: the harmful enchantments it cut short and when each runs out (dd ward).
function commands.ward(_args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	return affixes.wardLines(h)
end

-- The held items' tooltips by net code, as the UI asks for them (its first item to tip):
-- "on", "off", or alone what its UI was last sent ("code id:worn:durability:charges" each).
function commands.tips(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	if args[2] == "on" or args[2] == "off" then
		tips.want(h, args[2] == "on")
		return { "ok" }
	end
	return tips.shownLines(h)
end

-- The first player's lists' sizes as last sent: "kind bytes entries parts" each (dd wire).
function commands.wire(_args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	local out = hud.listSizes(h.peer)
	return if #out > 0 then out else { "none" }
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
	elseif op == "find" then
		-- (each name's node id, the name's words joined by "_": "dd tree find Shield_Wall")
		local out = {}
		for i = 3, #args do
			local want = string.gsub(args[i], "_", " ")
			local found = "none"
			for id = 1, tree.count() do
				if tree.node(id).name == want then
					found = tostring(id)
					break
				end
			end
			table.insert(out, found)
		end
		return out
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

function commands.sheet(args: { string }): { string }
	local h = firstHero()
	if not h then
		return { "no player" }
	end
	if args[2] == "gear" then
		local g = sheet.gear(h)
		local out = table.clone(g.a)
		table.insert(out, "--")
		for _, l in g.s do
			table.insert(out, l)
		end
		-- (the value's size as the UI's channel counts it)
		table.insert(out, "bytes " .. tostring(hud.sizeOf({ kind = "sheetGear", a = g.a, s = g.s })))
		return out
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
	local spot = if name == "stash" or name == "vendor" or name == "waypoints" or name == "maps" then name else nil
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

-- Uniques and sets, and the first hero's worn powers (dd uniques ...): uniqueconsole.luau.
function commands.uniques(args: { string }): { string }
	return uniqueConsole.uniques(args, firstHero())
end

function commands.hits(args: { string }): { string }
	if args[2] == "clear" then
		combat.clear()
		return { "cleared" }
	end
	local out = combat.lines()
	return if #out > 0 then out else { "none" }
end

-- The first hero's warrior skills (dd warrior [rage N | clear]): warrior.luau.
function commands.warrior(args: { string }): { string }
	return warrior.console(args, firstHero())
end

-- The first hero's active abilities (dd actives [clear | ready]): actives.luau.
function commands.actives(args: { string }): { string }
	return actives.console(args, firstHero())
end

-- The first hero's stock abilities' upgrades (dd upgrades [clear]): upgrades.luau.
function commands.upgrades(args: { string }): { string }
	return upgrades.console(args, firstHero())
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

function commands.secrets(args: { string }): { string }
	return secrets.console(args, areas, firstHero())
end

function commands.help(_args: { string }): { string }
	return {
		"dd status | spots | save | xp N | award N | loot RARITY [N] [LEVEL] | modifiers [all | TYPE] | ground | monster ID | check | types",
		"dd go town|arrive|exit|guardian|boss|waypoint|wayArrive|portal|pad|wayIn|actGate|pack [N] | area N|A-N [waypoint] | portal | waypoint N|A-N",
		"dd progress | boss | fight [now] | smite | trial SEED N | gate [take] | seed | town | tiers | number NAME [I]",
		"dd gold [N] | carried [grid] | stash [open|put N|take T S|deposit|withdraw] | vendor [open|buy I|sell N|reroll N|restock L|stock ITEM|repair ID|repairall|repairs|gear] | roll TYPE RARITY N [L] [CLASS]",
		"dd give TYPE RARITY [LEVEL] [SLOT=MOD]... [affix=ID:N]... | tip carried N|stash T S|stock I | tips [on|off] | wire | affixes [TYPE N [L] [CLASS]|worn|allowed [TYPE]] | ward",
		"dd tree [take ID...|respec|refund [ID]|nodes [REGION]|describe ID [CLASS]|costs|find NAME...] | icons | book | sheet [gear] | ui stash|vendor|waypoints|tree|sheet [NAME]",
		"dd warrior [rage N | clear] | actives [clear | ready] | upgrades [clear]",
		"dd as NAME COMMAND | party | scale | packs | regen | exit | vote [yes|no|cancel|seconds N] | labels | plates | spawn TIER [N] [TYPE|-] [POWERS|none] | powers [clear] | slay ID | portals | portal take | hud",
		"dd option [lootNames alt|always] | hits [clear] | summons | team",
		"dd give unique ID [L] | give set PIECE [L] | uniques [list|check|odds N LEVEL TIER [BOSS]|drop unique|set ID [L]|worn|powers]",
		"dd secrets [trial SEED N | go lever|room|chest I|door|keyguard|key | take [NAME] | trap I KIND|none | odds KIND N [LEVEL] | sprung]",
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
