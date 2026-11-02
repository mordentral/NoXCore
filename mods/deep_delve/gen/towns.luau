-- The town's looks, one for each act (data/acts.luau names an act's `look`): what the town section
-- of every area map is dressed with. Only the look changes from act to act: the stations (the
-- healer's fountain, the stash's chest, the vendor's anvil, the gate's arch, the pentagrams and the
-- players' starts) are the same things at the same spots in every act (town.luau), so the server
-- works them alike everywhere.
--
-- A look's fields (names from thing.bin):
--   floor       the town's floor, wall to wall
--   square      { floor, radius }: a square of another floor round the fountain (none: none)
--   paths       a floor worn from the square to the stations and the gate (none: none)
--   flank       { type, light }: the pair standing either side of the way to the vendor, and the
--               light kind (lights.luau) written on each
--   wallPieces  { types, every, out, light }: things set against every `every`-th back wall the
--               view looks at, `out` of a cell off the wall's line, the types in turn; `light` a
--               light kind written on each (none: none)
--   props       { type, dx, dy } in order: each at the free cell nearest (dx, dy) cells from the
--               town's middle
--   grove       { types, at, clear, crown, near }: trees round the fountain (none: none), placed
--               after everything else (the map table too, so nothing moves for them): each of `at`
--               ({ dx, dy } cells from the middle) at the free cell nearest it, within `near`, that
--               is off the worn ways (town.ways: the paths between the stations and to the gate,
--               drawn or not), `clear` cells or more from the portal's and waypoints' pads, and
--               whose crown (`crown`: across, up) hides no station, pad or the gate; the types in
--               turn
--
-- The walls are the area's own (one wall set a map), so a town takes each area's walls.

local towns = {}

-- Act 1, the Sunken Halls: the Waystation, a cobbled crypt hall lit by braziers and wall torches.
towns.waystation = {
	floor = "CobblestoneGrayLight",
	flank = { type = "Brazier", light = "brazier" },
	wallPieces = { types = { "Torch" }, every = 3, out = 0.2, light = "torch" },
	props = {
		{ "Barrel", -7, 2 },
		{ "Barrel2", -7, -2 },
		{ "BarrelWithTools2", 2, -7 },
		{ "Barrel", -2, -7 },
	},
}

-- Act 2, the Barrow Fields: a hamlet in the open, its green round a cobbled square, dirt paths
-- worn to the stations and the gate, torch poles by the vendor's cart, a vegetable patch west of
-- the starts, bushes and flowers along its walls.
towns.hamlet = {
	floor = "GrassNorm",
	square = { floor = "CobbleStone", radius = 4 },
	paths = "DirtLight2",
	flank = { type = "TorchPole", light = "torch" },
	wallPieces = { types = { "Bush4", "FlowersYellowDense", "FlowersWhiteDense" }, every = 2, out = 0.5 },
	props = {
		{ "OutdoorTraderCart", 3, -7 },
		{ "TraderAppleCrate", -2, -7 },
		{ "OutdoorTraderArmorRack1", -3, -6 },
		{ "Barrel", -7, 2 },
		{ "BarrelWithTools1", -7, -2 },
		{ "WaterBarrel", -8, 0 },
		-- (the vegetable patch west of the starts, by the way in: the waypoints' pad is south of
		-- the starts, and nothing stands within five cells of it)
		{ "GardenCabbage", -5, 3 },
		{ "GardenCorn", -3, 3 },
		{ "GardenTomatos", -4, 2 },
		-- (the tree against the north-west back wall and the purple flowers by the way to the
		-- stash: no prop within five cells of the portal's and waypoints' pads, south-east and
		-- south of the fountain, so their rings on the floor stay clear)
		{ "TreeApple", -5, -5 },
		{ "Stump1", -6, -2 },
		{ "FlowersPurpleSparse", -3, -2 },
		{ "FlowersBlueSparse", -2, 2 },
	},
}

-- Act 3, the Greenwood: Fernhollow, a woodcutters' camp in a clearing: dense grass round a
-- trodden dirt yard, dirt paths to the stations and the gate, torch poles by the vendor's tent and
-- bow racks, the logs and stumps of the woodcutting, a cold fire ring west of the starts, ferns
-- and foliage along its walls.
towns.camp = {
	floor = "GrassDense",
	square = { floor = "DirtDark2", radius = 3 },
	paths = "DirtDark2",
	flank = { type = "TorchPole", light = "torch" },
	wallPieces = { types = { "FoliageDense1", "PlantFern2", "Bush6" }, every = 2, out = 0.5 },
	props = {
		{ "OutdoorTraderPupTent", 3, -7 },
		{ "TraderBowRack1", -2, -7 },
		{ "TraderBowRack2", -4, -6 },
		{ "ForestLog01", -7, 2 },
		{ "Stump4", -7, -2 },
		{ "ForestLog03", -8, 0 },
		-- (the cold fire ring and its stumps west of the starts: the waypoints' pad is south of
		-- them, and nothing stands within five cells of it)
		{ "CampFireUnused", -3, 3 },
		{ "Stump1", -4, 2 },
		{ "Stump2", -2, 2 },
		-- (as the hamlet's: nothing within five cells of the portal's and waypoints' pads, south-east
		-- and south of the fountain)
		{ "TreeForest02", -5, -5 },
		{ "Stump3", -5, 3 },
		{ "Mushroom4", -3, -2 },
		{ "PlantFern1", -6, -2 },
	},
}

-- Act 4, the Sunscorched Waste: the Last Well, a caravan stop at an oasis: sand wall to wall, a
-- green of dense grass round the fountain's waters, worn paths to the stations and the gate,
-- braziers by the vendor's way, the traders' cart, crates and helm poles behind the anvil, water
-- barrels by the stash, the old well west of the starts by the way in, a palm in the corner and a
-- broken column by the stash, palms and ferns west of the fountain, dry plants along its walls.
towns.oasis = {
	floor = "DirtSand",
	square = { floor = "GrassDense", radius = 3 },
	paths = "DirtLight2",
	flank = { type = "Brazier", light = "brazier" },
	wallPieces = { types = { "PlantBarren1", "PlantFern4", "PlantBarren2" }, every = 2, out = 0.5 },
	props = {
		{ "OutdoorTraderCart", 3, -7 },
		{ "TraderAppleCrate", -2, -7 },
		{ "OutdoorTraderHelmPoles", -4, -6 },
		{ "WaterBarrel", -7, 2 },
		{ "Barrel", -7, -2 },
		{ "WaterBarrel", -8, 0 },
		-- (the old well west of the starts, by the way in, and its ferns by the fountain: the
		-- waypoints' pad is south of the starts, and nothing stands within five cells of it)
		{ "Well", -5, 3 },
		{ "PlantFern3", -2, 2 },
		{ "PlantFern2", -2, -2 },
		-- (as the other looks': nothing within five cells of the portal's and waypoints' pads,
		-- south-east and south of the fountain; the corner's palm is the grove's now)
		{ "RuinsColumnOutdoorShort01", -6, -2 },
		{ "PlantBarren3", -3, -2 },
		{ "PlantFern4", -3, 2 },
	},
	-- The oasis's palms (by decision, 2026-11-02): two short Galava palms on the green's west edge,
	-- west and south-west of the fountain, a tall one behind the stash by the barrels and the old
	-- one in the north-west corner (a prop before). Only west: south-east and south of the fountain
	-- are the pads (nothing within five cells of them), and a palm's crown stands over what is north of its
	-- foot, so none south of the fountain or under the vendor and the braziers (seen in noxsdl).
	grove = {
		types = { "TreeGalava07", "TreeGalava07", "TreeGalava06", "TreeGalava07" },
		at = { { -3, 1 }, { -3, 3 }, { -6, -3 }, { -5, -5 } },
		clear = 5,
		-- (its crown: `across` cells either side of its foot, `up` cells north of it; nothing to
		-- be kept in view under it)
		crown = { across = 2.5, up = 4.5 },
		-- (no tree when no cell within this of its place will do)
		near = 2,
	},
}

-- The map table (the expeditions'), the same in every look (by decision, 2026-11-02: by the
-- portal, its back to the wall, the pads moved south so the way to it crosses none). It stands
-- against the back wall nearest `at` ({ dx, dy } cells from the town's middle: east of the
-- fountain on the north-east wall, south-east of the gate's arch and clear of it), on the free
-- floor cell against that wall, `out` of a cell from the wall's line, placed after everything
-- else. The game's desk faces by its type: `desk` names the one whose back is to a north-east
-- wall ("\") and to a north-west one ("/"), its front (books and candles) to the town.
towns.mapTable = {
	at = { 9, -1 },
	out = 0.8,
	desk = { ne = "Desk2", nw = "Desk1" },
}

-- The look named `name`, or nil.
function towns.find(name: string)
	local look = towns[name]
	return if type(look) == "table" then look else nil
end

return towns
