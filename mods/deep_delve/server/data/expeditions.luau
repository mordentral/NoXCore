--!strict
-- Expeditions: the endgame's map runs, chosen at the map table in town (expeditions.luau). A run
-- is one generated map of a chosen biome at a chosen rank, its champion waiting in the end
-- chamber; killing it clears the run, drops its reward and opens the next rank.
--
-- The ladder starts above the last act: rank 1's area level is the last act's last area level
-- plus `ranks.first`, and each rank `ranks.step` more. So a new act (or a retuned one) moves the
-- whole ladder with it; nothing here names an act's numbers.
--
-- A biome's fields: name (the built-in generator's biome), corridor (the layout's widest corridor
-- half width), town (the act whose town look dresses the run's town section), skies (its weathers
-- by weight, as data/acts.luau's `skies`: for a biome no act has a list for; outdoor biomes only).
--
-- A biome with a roster in data/monsters.luau uses it (and its champion); one without uses the
-- roster and champion here. Every type is thing.bin's ("dd check" tests them).

local expeditions = {}

-- The biomes the map table offers, in the order it lists them.
expeditions.biomes = {
	{ name = "Crypt", corridor = 2, town = 1 },
	{ name = "Dungeon", corridor = 2, town = 1 },
	{ name = "Mines", corridor = 3, town = 1 },
	{ name = "Cave", corridor = 3, town = 1 },
	{ name = "Grasslands", corridor = 3, town = 2 },
	{ name = "Swamp", corridor = 2, town = 2 },
	{ name = "Forest", corridor = 3, town = 3 },
	{ name = "Desert", corridor = 3, town = 4 },
	{ name = "Beach", corridor = 3, town = 4 },
}

-- The rank ladder: how many ranks, rank 1's level above the last act's last area level, and the
-- levels between ranks.
expeditions.ranks = { count = 10, first = 1, step = 2 }

-- A run's map: its route's diagonal runs (a little shorter than an act's longest areas), its end
-- chamber's size, and its packs (base + perRank x rank, at most `most`). The monsters' numbers
-- come from the area level by the acts' own curve (numbers.areas).
expeditions.map = {
	segments = 10,
	endRadius = 9,
	packs = { base = 20, perRank = 1, most = 30 },
}

-- The champion at the run's end: its guards (base + one per `perRanks` ranks).
expeditions.champion = { guards = 3, perRanks = 3 }

-- The reward when the champion dies: `rare` pieces of rare gear (plus one per `rarePerRanks`
-- ranks) `levelUp` levels above the run's, then `rolls` loot rolls (plus one per `rollsPerRank`)
-- with `better` odds (loot.bossDrop's).
expeditions.reward = { rare = 1, rarePerRanks = 4, levelUp = 1, rolls = 4, rollsPerRank = 0.5, better = 1.5 }

-- The map table in town: how near a player must stand to use it (its window closes past this),
-- and its ring on the floor (decals.luau: the waypoints' picture in gold).
expeditions.table = {
	ring = { asset = "fx/waypoint", size = 80, spin = 8, pulse = 0.2, period = 4, colour = { 255, 200, 90, 255 } },
}

-- Rosters for the biomes no act has yet (data/monsters.luau's fields; `from` 1: every entry at
-- every rank, since the ladder starts above the acts): none now. The Forest's are act 3's (its
-- roster and its champion, Old Bramblehide), the Desert's and the Beach's act 4's (data/monsters.luau).
expeditions.rosters = {}

expeditions.champions = {}

-- The biome named `name` (its entry above), or nil.
function expeditions.biome(name: string): any
	for _, b in expeditions.biomes do
		if b.name == name then
			return b
		end
	end
	return nil
end

return expeditions
