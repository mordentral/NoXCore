--!strict
-- The acts and their areas. An act is a chain of areas played in order, each a generated map of
-- one of the built-in generator's biomes (by name); its last area is the act boss's lair. The
-- area level is the act's base level plus the area's index less one. The town's own floor is here
-- too.
--
-- An area's fields: biome, corridor (the layout's widest corridor half width), waypoint (true:
-- the area has a waypoint), lair (true: the act boss's lair, the act's last area), title (its
-- name, when not the biome's).

return {
	{
		name = "The Sunken Halls",
		base = 1,
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
	town = {
		name = "the Waystation",
		floor = "CobblestoneGrayLight",
	},
}
