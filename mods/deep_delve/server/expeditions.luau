--!strict
-- Expeditions: the endgame's map runs (data/expeditions.luau), chosen at the map table in town.
--
--   * The map table: a scholar's desk in every act's town, a gold ring on the floor round it; touching it opens the Expeditions window on the toucher's UI (ui/main.luau): every
--     biome offered, the rank ladder, the ranks it has cleared in each. Walking away closes it
--     (windows.luau's station).
--   * Open to a character once it has cleared the last act (atlas.luau keeps it so). Rank 1 is
--     open at once; rank N + 1 once rank N is cleared in any biome. `dd atlas free on` opens every
--     rank to everyone in the game, for testing (never kept).
--   * A run: one generated map of the biome (the same generator as the acts' areas), at the
--     rank's area level, with its packs by the rank and its biome's champion in the end chamber.
--     Starting one changes the game's map, so it is the party's vote when anyone else is in the
--     game (votes.luau, as the exit and the waypoints are); alone, at once. Everyone arrives at
--     the map's start.
--   * Each run is a place of its own past the acts' areas (areas.luau numbers them on from its
--     last), drawn new from the play's seed each time one is started, so no two runs share a map.
--   * The champion's death clears it: its reward (rare gear above the run's level and loot rolls
--     with better odds, more by rank), the clear kept in every character's atlas in the game (the
--     next rank opened), the vendor's stock rolled again, and the end chamber's arch leading home.
--     Before that, the arch says so; the way home by the start and the town portal work as in any
--     area.
--   * Death respawns in town as everywhere (nothing lost); the run's map stays, and the town's gate
--     leads back to its start. A run ends when the game changes map (another run, a waypoint, or
--     an emptied server game starting over): nothing of it is kept but the atlas.
--
-- One small table of runs asked for (pruned as it grows); nothing walks the world.

local data = require("data/expeditions")
local acts = require("data/acts")
local monsterData = require("data/monsters")
local atlas = require("atlas")
local decals = require("decals")
local hero = require("hero")
local hud = require("hud")
local loot = require("loot")
local monsters = require("monsters")
local town = require("town")
local touch = require("touch")
local vendor = require("vendor")
local votes = require("votes")
local windows = require("windows")

local expeditions = {}

-- areas.luau's own (it requires this module and hands itself in).
local areas: any = nil
-- The runs asked for: place -> { biome, rank }. A run's place is the acts' last place plus its
-- serial.
local runs: { [number]: any } = {}
local serial = 0
-- How many runs are kept asked for (the one being played, the one a vote or a map change is on
-- its way to, and a few more).
local KEEP = 8
-- Every rank open to everyone (the console's `dd atlas free`, for testing; never kept).
local free = false
-- This map's run: { place, biome, rank, level, champion (its id), name (its name), cleared }, or
-- nil (an act's area).
local current: any = nil

-- The table's touches come every PAD_EVERY frames while a player stands against it; a gap longer
-- than PAD_GAP is a new touch (as the waypoints' pad's steps).
local PAD_EVERY = 5
local PAD_GAP = 15

function expeditions.attach(areasApi: any)
	areas = areasApi
end

-- ---- Runs

-- Whether `place` is a run's.
function expeditions.has(place: number): boolean
	return runs[place] ~= nil
end

-- A new run of `biome` at `rank`: its place.
function expeditions.newRun(biome: string, rank: number): number
	serial += 1
	local place = areas.count() + serial
	runs[place] = { biome = biome, rank = rank }
	-- (the oldest let go, but never the one being played)
	local playing = current and current.place
	for p in runs do
		if p <= place - KEEP and p ~= playing then
			runs[p] = nil
		end
	end
	return place
end

-- A run's packs at `rank`.
local function packsAt(rank: number): number
	local p = data.map.packs
	return math.min(p.most, p.base + math.floor(p.perRank * rank))
end

-- A run's facts, as areas.info gives an area's (nil: no such run). Its act is the one whose town
-- dresses its map (index 0: no act's area); `expedition` says it's a run.
function expeditions.info(place: number): any
	local r = runs[place]
	if not r then
		return nil
	end
	local b = data.biome(r.biome)
	if not b then
		return nil
	end
	local c = data.champion
	return {
		place = place,
		index = 0,
		act = b.town,
		level = atlas.level(r.rank),
		biome = b.name,
		corridor = b.corridor,
		title = b.name .. ", rank " .. tostring(r.rank),
		waypoint = false,
		lair = false,
		gate = false,
		-- (its weathers, rolled as its map is built: the acts' list for its biome, else its own)
		skies = (acts.skies :: any)[b.name] or b.skies,
		segments = data.map.segments,
		packs = packsAt(r.rank),
		endRadius = data.map.endRadius,
		opening = nil,
		expedition = { biome = b.name, rank = r.rank },
		runRoster = (monsterData.rosters :: any)[b.name] or (data.rosters :: any)[b.name],
		runChampion = (monsterData.champions :: any)[b.name] or (data.champions :: any)[b.name],
		guards = c.guards + r.rank // math.max(1, c.perRanks),
		-- (every roster entry counted by an act's index is in: the ladder is past the acts)
		rosterAt = 99,
	}
end

-- Whether the map table is open to a hero (or to everyone: `dd atlas free`).
local function tableOpen(h: any): boolean
	return free or atlas.isOpen(h.record)
end

-- Whether rank `rank` is open to a hero.
local function rankOpen(h: any, rank: number): boolean
	if free then
		return rank >= 1 and rank <= atlas.ranks()
	end
	return atlas.rankOpen(h.record, rank)
end

-- A hero asks for an expedition: `biome` at `rank`. The party's vote with anyone else in the game,
-- else at once. Whether it went or a vote started ("vote"), else why.
function expeditions.start(h: any, biome: string, rank: number): (boolean, string?)
	if not areas then
		return false, "not now"
	end
	if not tableOpen(h) then
		return false, "the map table isn't open to you"
	end
	if not data.biome(biome) then
		return false, "no such biome"
	end
	if not rankOpen(h, rank) then
		return false, "rank " .. tostring(rank) .. " isn't open to you"
	end
	if votes.active() then
		return false, "a vote is under way"
	end
	local place = expeditions.newRun(biome, rank)
	return votes.ask(h, place, "start", "expedition")
end

-- ---- The map table and its window

-- The window's value for a hero: { kind = "maps", open, why (when shut), free, top (the highest
-- rank open), best, runs, biomes = { { n = name, c = { ranks cleared } } }, ranks = { { r, l } },
-- here = { b, r, c } (the run being played, if one is) }. (About 600 bytes.)
local function valueFor(h: any): any
	local a = atlas.of(h.record)
	local list = {}
	for _, b in data.biomes do
		local cleared = a.cleared[b.name]
		table.insert(list, { n = b.name, c = if type(cleared) == "table" then table.clone(cleared) else {} })
	end
	local ranks = {}
	for r = 1, atlas.ranks() do
		table.insert(ranks, { r = r, l = atlas.level(r) })
	end
	local last = acts[#acts]
	local open = tableOpen(h)
	return {
		kind = "maps",
		open = open,
		why = if open then nil else string.format("Clear Act %d, %s, to use the map table.", #acts, tostring(last.name)),
		free = free or nil,
		top = if free then atlas.ranks() else atlas.top(h.record),
		best = a.best,
		runs = a.runs,
		biomes = list,
		ranks = ranks,
		here = if current then { b = current.biome, r = current.rank, c = current.cleared } else nil,
	}
end

function expeditions.view(h: any)
	hud.send(h.peer, "maps", valueFor(h))
end

-- A message from the window: { op = "go", biome, rank }. A start (or its vote) closes the window;
-- a refusal is told in it.
function expeditions.message(h: any, v: any)
	if v.op ~= "go" or not windows.isOpen(h, "maps") then
		return
	end
	local rank = tonumber(v.rank) or 0
	local why: string? = nil
	if not windows.near(h, "maps") then
		why = "Stand at the map table."
	else
		local ok, refused = expeditions.start(h, tostring(v.biome), rank)
		if ok then
			windows.close(h, "maps", true)
			return
		end
		why = if refused == "a vote is under way" then "A vote to change area is under way: answer it first."
			elseif refused == "asked too soon" then "Your last vote failed a moment ago: ask again in a few seconds."
			else "Not now (" .. tostring(refused) .. ")."
	end
	windows.note(h, "maps", why :: string)
end

-- The map table touched: the window opened on the toucher's UI, at a new touch (not again while it
-- stands against it after closing it).
local function padTouched(unit: number)
	local h = hero.byUnit(unit)
	if not h then
		return
	end
	local now = nox.frame()
	local last = h.mapsAt
	h.mapsAt = now
	if last and now - last <= PAD_GAP then
		return
	end
	windows.open(h, "maps")
end

windows.register("maps", { station = "maps", view = expeditions.view })

-- A map's town entered (`spots`: the generator's): the map table set to work, its ring on the
-- floor round it.
function expeditions.place(spots: any)
	local at = spots.town and spots.town.maps
	if not at then
		return
	end
	touch.adopt("Desk1", at.x, at.y, padTouched, PAD_EVERY)
	decals.put("maps", at.x, at.y, data.table.ring)
end

-- ---- A run being played

-- The end chamber's arch touched: home once the run is cleared; before, a word. Whether it went
-- home, else why.
function expeditions.exitTouched(unit: number): (boolean, string?)
	local c = current
	if not c then
		return false, "no run"
	end
	if c.cleared then
		town.sendHome(unit)
		return true, nil
	end
	town.say(unit, "The way out opens when " .. tostring(c.name or "the champion") .. " falls.")
	return false, "the champion stands"
end

-- A run's map entered (`info`: areas.info's, after its monsters are in): its champion marked, the
-- party told what waits.
function expeditions.enter(_spots: any, info: any)
	local e = info.expedition
	if not e then
		current = nil
		return
	end
	local g = monsters.guardians()
	local id = g and g.id
	local entry = id and monsters.info(id)
	current = { place = info.place, biome = e.biome, rank = e.rank, level = info.level, champion = id, name = entry and entry.name, cleared = false }
	if id and entry then
		entry.runEnd = true
		-- (never charmed: a charmed champion is no longer the mod's, and the run would never end)
		nox.setCharmable(id, false)
	else
		nox.log("deep_delve: expedition " .. info.title .. " has no champion")
	end
	nox.textToAll(string.format("Deep Delve: the %s expedition, rank %d (area level %d). %s waits at its end.", e.biome, e.rank, info.level,
		tostring(current.name or "Its champion")))
end

-- A monster the mod made died (`m`: its entry): the run cleared when it was the champion.
function expeditions.killed(m: any, x: number, y: number)
	local c = current
	if not c or c.cleared or not m.runEnd then
		return
	end
	c.cleared = true
	local r = data.reward
	loot.bossDrop(x, y, c.level, {
		rare = r.rare + c.rank // math.max(1, r.rarePerRanks),
		levelUp = r.levelUp,
		rolls = r.rolls + math.floor(r.rollsPerRank * c.rank),
		better = r.better,
	})
	nox.effect(0x85, x, y) -- (MSG_FX_EXPLOSION)
	for _, h in hero.all() do
		atlas.cleared(h.record, c.biome, c.rank)
		hero.save(h, "expedition cleared")
		hud.hero(h)
		if nox.present(h.unit) then
			nox.sound(h.unit, "LevelUp")
		end
	end
	local rank: number = tonumber(c.rank) or 1
	local line = string.format("Deep Delve: %s is slain: the %s expedition, rank %d, is cleared.", tostring(c.name or "the champion"), tostring(c.biome), rank)
	if rank < atlas.ranks() then
		line ..= string.format(" Rank %d is open.", rank + 1)
	end
	nox.textToAll(line)
	nox.textToAll("Deep Delve: the arch in the end chamber leads back to town.")
	vendor.restock(c.level + r.levelUp)
	hud.runCleared(hero.all())
end

-- The run being played, for "dd atlas run": { place, biome, rank, level, champion, name, cleared }
-- or nil.
function expeditions.current(): any
	return current
end

-- `dd atlas free on|off`: every rank open to everyone in the game (testing), or not.
function expeditions.setFree(on: boolean)
	free = on
end

function expeditions.isFree(): boolean
	return free
end

-- A new map: the old one's run went with it.
function expeditions.reset()
	current = nil
end

return expeditions
