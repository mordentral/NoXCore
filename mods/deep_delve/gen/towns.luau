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
-- worn to the stations and the gate, torch poles by the vendor's cart, a vegetable patch south of
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
		{ "GardenCabbage", -2, 8 },
		{ "GardenCorn", 0, 8 },
		{ "GardenTomatos", 2, 8 },
		-- (the tree against the north-west back wall and the purple flowers by the way to the
		-- stash: no prop within five cells of the portal's and waypoints' pads, east of the
		-- fountain, so their rings on the floor stay clear)
		{ "TreeApple", -5, -5 },
		{ "Stump1", -5, 6 },
		{ "FlowersPurpleSparse", -3, -2 },
		{ "FlowersBlueSparse", -3, 2 },
	},
}

-- The look named `name`, or nil.
function towns.find(name: string)
	local look = towns[name]
	return if type(look) == "table" then look else nil
end

return towns
