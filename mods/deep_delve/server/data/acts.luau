--!strict
-- The acts and their areas. An act is a chain of areas played in order, each a generated map of
-- one of the built-in generator's biomes (by name); its last area is the act boss's lair. The
-- area level is the act's base level plus the area's index less one. Each act's town (its name
-- and look) is here too.
--
-- The acts go by biome, a journey upward and onward: act 1 the catacombs, act 2 up out of them
-- into the open fields. An act without a `base` carries on from the act before it (that act's
-- base plus its areas), so a later act's levels follow whatever the earlier acts are tuned to.
-- Killing an act's boss opens the gate in its lair to the next act's first area (`gate` below);
-- the last act's boss opens none.
--
-- An area's fields: biome, corridor (the layout's widest corridor half width), waypoint (true:
-- the area has a waypoint), waypointAt ("start": in its start chamber, else off the route's
-- middle), lair (true: the act boss's lair, the act's last area), title (its name, when not the
-- biome's), roster (data/monsters.luau's roster and champion it draws from, when not its biome's),
-- arena (a lair's look: the generator's gen/area.luau arenas, by name; none: "pyre").
--
-- The weather is no area's own: each time an outdoor area's map is built, it rolls one from its
-- biome's list in `skies` below (a biome without a list, and every lair, has none). Its town, in
-- the same map, has the same.
--
-- An act's town: its name (as the healer's waters are told) and its look (the generator's
-- gen/towns.luau, by name: the town's floors, lights and scenery; the stations are the same in
-- every act). An act without one has `town` below.
--
-- A record keeps an area by its index in its act (progress.luau), so areas put into an act before
-- others move the later ones' indices: store.luau's record version 6 moved act 2's on by its two
-- opening areas. Another such change needs a record version of its own.

local acts: { any } = {
	{
		name = "The Sunken Halls",
		base = 1,
		town = { name = "the Waystation", look = "waystation" },
		areas = {
			{ biome = "Crypt", corridor = 2 },
			{ biome = "Dungeon", corridor = 2, waypoint = true },
			{ biome = "Mines", corridor = 3 },
			{ biome = "Crypt", corridor = 2, waypoint = true },
			{ biome = "Dungeon", corridor = 2 },
			{ biome = "Mines", corridor = 3, waypoint = true },
			{ biome = "Crypt", corridor = 2, lair = true, title = "Lair" },
		},
		-- The act boss (data/monsters.luau's bosses).
		boss = "warlord",
	},
	{
		-- Up out of the catacombs: the old workings at their mouth and the caves above them, then
		-- the open fields, grass and marsh under a changing sky, to the barrow-king's mound.
		name = "The Barrow Fields",
		-- (a hamlet on the green; in an outdoor area the area's weather falls on it too)
		town = { name = "Low Barrow", look = "hamlet" },
		areas = {
			-- The climb out (indoors, no weather): its first waypoint by the start, found as the
			-- party comes through act 1's gate.
			{ biome = "Mines", corridor = 3, title = "Old Workings", roster = "Workings", waypoint = true, waypointAt = "start" },
			{ biome = "Cave", corridor = 3, title = "Cave Mouth" },
			-- The open sky (these were areas 1-6 before the climb was put in front of them: record
			-- version 6). The first field's waypoint stays by its start.
			{ biome = "Grasslands", corridor = 3, waypoint = true, waypointAt = "start" },
			{ biome = "Grasslands", corridor = 3 },
			{ biome = "Swamp", corridor = 2, waypoint = true },
			{ biome = "Grasslands", corridor = 3 },
			{ biome = "Swamp", corridor = 2, waypoint = true },
			{ biome = "Grasslands", corridor = 3 },
			-- The barrow-king's mound: a hillfort ring on the grass, graves round its arena.
			{ biome = "Grasslands", corridor = 3, lair = true, title = "Barrow Mound", arena = "barrow" },
		},
		boss = "barrowking",
	},
}

-- The weathers an outdoor area may have (nox.weather's tables), drawn by NoX Core clients; they
-- change nothing in play. `fade`: frames to blend from the weather before. "clear": a still, dry
-- sky (nothing drawn).
local weathers = {
	drizzle = { kind = "rain", strength = 0.35, wind = { angle = 70, speed = 0.25 }, fade = 90 },
	rain = { kind = "rain", strength = 0.65, wind = { angle = 60, speed = 0.4 }, fade = 90 },
	windy = { kind = "none", wind = { angle = 80, speed = 0.55 }, fade = 90 },
	clear = { kind = "none", fade = 90 },
}

-- Each outdoor biome's weathers, by weight: one is rolled each time an area of that biome is built
-- (each visit: a waypoint's travel, the exit on, a fresh play builds its map again), and kept while
-- that map is played. A biome not here (indoors) has none, and so has every lair.
local skies = {
	Grasslands = { { "drizzle", 3 }, { "windy", 3 }, { "clear", 2 }, { "rain", 1 } },
	Swamp = { { "rain", 4 }, { "drizzle", 3 }, { "windy", 1 }, { "clear", 1 } },
}

-- The gate a lair's boss opens to the next act (gate.luau), at the lair's gate spot (the arena's
-- back, away from its way in): how near a player comes to step through (`reach`), how often it's
-- looked at (`every` frames), how soon one player's step is heard again (`again` frames), its
-- sparks (nox.effect, every `sparksEvery` frames) and the swirl standing in it (a thing made when
-- it opens, which every client draws).
local gate = {
	reach = 34,
	every = 5,
	again = 30,
	sparks = 0x83, -- (MSG_FX_CYAN_SPARKS)
	sparksEvery = 20,
	swirl = "BlueSummons",
}

-- Each act's base level: its own, else the act before it's base plus its areas.
for a = 2, #acts do
	local act = acts[a]
	if act.base == nil then
		local before = acts[a - 1]
		act.base = before.base + #before.areas
	end
end

-- The difficulty tiers' level offset (design 7, by decision): a tier's areas are its tier I level
-- plus this for each tier above I, and it is the last act's last area's level, so tier II's act 1
-- starts above tier I's deepest area (worked out from the acts, so it follows as acts are added or
-- tuned). Tiers aren't played yet: this is the number they will use.
local last = acts[#acts]
local tierOffset: number = last.base + #last.areas - 1

local out: any = acts
out.weathers = weathers
out.skies = skies
out.tierOffset = tierOffset
out.gate = gate
-- (the town of an act that names none)
out.town = { name = "the Waystation", look = "waystation" }

return out
