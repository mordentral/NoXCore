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
-- biome's), weather (one of `weathers` below, by name: outdoor areas only; none indoors and none
-- in a lair).
--
-- An act's town: its name (as the healer's waters are told) and its look (the generator's
-- gen/towns.luau, by name: the town's floors, lights and scenery; the stations are the same in
-- every act). An act without one has `town` below.

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
		-- Up out of the catacombs: the open fields above them, grass and marsh under a wet sky.
		name = "The Barrow Fields",
		-- (a hamlet on the green, outdoors: the area's weather falls on it too)
		town = { name = "Low Barrow", look = "hamlet" },
		areas = {
			{ biome = "Grasslands", corridor = 3, waypoint = true, waypointAt = "start", weather = "drizzle" },
			{ biome = "Grasslands", corridor = 3, weather = "windy" },
			{ biome = "Swamp", corridor = 2, waypoint = true, weather = "rain" },
			{ biome = "Grasslands", corridor = 3, weather = "drizzle" },
			{ biome = "Swamp", corridor = 2, waypoint = true, weather = "rain" },
			{ biome = "Grasslands", corridor = 3, weather = "windy" },
			{ biome = "Grasslands", corridor = 3, lair = true, title = "Hillfort" },
		},
		-- (a placeholder boss: data/monsters.luau's)
		boss = "hillbreaker",
	},
}

-- The weathers an outdoor area may have (nox.weather's tables), drawn by NoX Core clients; they
-- change nothing in play. `fade`: frames to blend from the weather before.
local weathers = {
	drizzle = { kind = "rain", strength = 0.35, wind = { angle = 70, speed = 0.25 }, fade = 90 },
	rain = { kind = "rain", strength = 0.65, wind = { angle = 60, speed = 0.4 }, fade = 90 },
	windy = { kind = "none", wind = { angle = 80, speed = 0.55 }, fade = 90 },
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

local out: any = acts
out.weathers = weathers
out.gate = gate
-- (the town of an act that names none)
out.town = { name = "the Waystation", look = "waystation" }

return out
