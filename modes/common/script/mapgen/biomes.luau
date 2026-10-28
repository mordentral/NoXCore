-- The generator's biomes: what a generated room is made of and dressed with. Each is drawn from
-- the shipped maps that look that way (their floors, wall sets and the scenery they place most):
-- Forest (con03a, G_Forest), Beach (So_Beach), Shop (So_Dun's traders), Cave (con05c), Swamp
-- (G_Swamp, con09a), Ice (con09d), Crypt (G_Crypts), Castle (G_Castle), Mines (G_Mines, con03b),
-- Lava (G_Lava's volcanic rock and lava), Desert (the sand and ancient ruins of So_Beach and the
-- Ix outskirts), Grasslands (field-stone walls round open grass) and Dungeon.
--
-- Each biome's fields (a field left out takes the default at the end):
--   name, title (as players are told it: "the Frozen Wastes"); wallSet (thing.bin WALL); floor
--   (the base); patches (floors laid in patches on it); path (the floor of a path from the start
--   to the exit, "" none); blend (patches and path edged into the base: outdoor); edgeMask (the
--   EDGE entry for the blending); ambient (each channel); large (trees, pillars: sparse, kept
--   apart), medium (bushes, rocks, furniture), small (plants, bones, shells), wall (set along the
--   inside of the walls); largeEvery, mediumEvery, smallEvery (roughly how many cells each piece
--   of a size gets: fewer, denser), wallEvery (a wall piece every this many wall cells); weight
--   (how often an act draws it against the others: rare ones 1); climate (Mild, Freezing: slowed
--   while there, Scorching: burnt a little, steadily); style (how its rooms are cut: Built halls,
--   Natural caverns, Open clearings); chests (the chests its loot goes in: none with a CHEST_
--   subclass, which a quest opens only with a silver key; empty, the stone CryptChest1-4).
--
-- A harming floor (Lava, FLOR 6: it burns whatever stands on it) is kept off the route, the start
-- and the end, and nothing is placed on it: harmingFloors names them.

local biomes = {
	{
		name = "Dungeon",
		title = "the Dungeon",
		wallSet = "DungeonStone",
		floor = "DungeonStoneDark",
		patches = { "DungeonStoneMuddy" },
		ambient = 120,
		large = { "Column6" },
		medium = { "Barrel", "Barrel2", "CaveRocksMediumImmobile" },
		small = { "ArmBoneImmobile", "LegBoneImmobile", "SkullImmobile", "CaveRocksPebblesImmobile", "CaveRocksTinyImmobile" },
		wall = { "Torch", "Barrel2" },
		largeEvery = 140,
		style = "Built",
	},
	{
		name = "Crypt",
		title = "the Crypt",
		wallSet = "DungeonStone",
		floor = "GreenBrick",
		patches = { "CryptFloor" },
		ambient = 110,
		large = { "Coffin1", "Coffin2", "Coffin3", "Coffin4", "Column6" },
		medium = { "Monument1", "Tombstone1", "Tombstone5" },
		small = { "ArmBoneImmobile", "LegBoneImmobile", "SkullImmobile" },
		wall = { "Statue2a", "Statue2c", "Statue2e", "Statue2g", "Torch" },
		largeEvery = 110,
		style = "Built",
	},
	{
		name = "Castle",
		title = "the Castle",
		wallSet = "BrickBlue",
		floor = "TileDark",
		patches = { "RedBrick6" },
		ambient = 150,
		large = { "CathedralColumn3" },
		medium = { "Barrel", "Barrel2", "SquareTable1" },
		small = { "CaveRocksPebblesImmobile" },
		wall = { "Torch", "Bookcase1", "Bookcase2", "Barrel2" }, -- (a bookcase faces its wall's way)
		largeEvery = 150,
		smallEvery = 60,
		style = "Built",
	},
	{
		name = "Mines",
		title = "the Mines",
		wallSet = "ManaMineWall",
		floor = "ManaMineDirt",
		patches = { "DirtCrackedLight" },
		path = "DirtCrackedDark",
		blend = true,
		ambient = 120,
		large = { "MineBeam1", "MineBeam2" },
		medium = { "MinePost3", "MinePost4", "Barrel", "CaveRocksMediumImmobile" },
		small = { "CaveRocksPebblesImmobile", "CaveRocksTinyImmobile", "MiningShovelInGround" },
		wall = { "TorchPole", "Barrel", "BarrelWithTools2" },
		style = "Natural",
		chests = { "SackChestLarge1", "SackChestLarge2", "SackChestMedium1", "SackChestMedium2" },
	},
	{
		name = "Cave",
		title = "the Caves",
		wallSet = "CaveWall2",
		floor = "CaveHardBrown",
		patches = { "CaveHardTan", "CaveHardDark" },
		path = "DirtDark2",
		blend = true,
		ambient = 110,
		large = { "CaveRockPillarTall1", "CaveRockPillarTall2", "CaveBoulders" },
		medium = { "CaveRockPillarShort1", "CaveRockPillarShort2", "LargeStalagmite", "SmallStalagmite", "CaveRocksLargeImmobile" },
		small = { "CaveRocksPebblesImmobile", "CaveRocksSmallImmobile", "Mushroom1", "Mushroom3" },
		wall = { "TorchPole" },
		wallEvery = 7,
		style = "Natural",
		chests = { "SackChestLarge1", "SackChestLarge2", "SackChestMedium1", "SackChestMedium2" },
	},
	{
		name = "Forest",
		title = "the Forest",
		wallSet = "DecidiousWallGreen",
		floor = "GrassNorm",
		patches = { "GrassDense", "GrassSparse2" },
		path = "DirtDark2",
		blend = true,
		ambient = 170,
		large = { "TreeForest01", "TreeForest02", "TreeForest03", "TreeForest04", "TreeForest05" },
		medium = { "Bush2", "Bush3", "Bush6", "Bush12", "FoliageDense1", "CaveRocksSmallImmobile" },
		small = { "Plant4", "Plant5", "PlantForest1", "PlantFern1", "FlowersYellowSparse", "FlowersPurpleSparse", "Mushroom4", "Mushroom5" },
		wall = { "FoliageDense1", "Bush6" },
		largeEvery = 70,
		smallEvery = 12,
		style = "Open",
		chests = { "StumpChest1", "SackChestLarge1", "SackChestMedium2" },
	},
	{
		name = "Beach",
		title = "the Beach",
		wallSet = "CaveWall2",
		floor = "DirtSand",
		patches = { "DirtLighter" },
		path = "DirtLight2",
		blend = true,
		ambient = 190,
		large = { "TreeGalava06", "TreeGalava07", "TreeGalava10" },
		medium = { "CaveRockPillarShort1", "CaveRockPillarShort2", "PlantFern2", "PlantFern3" },
		small = { "BeachStarfish", "BeachShellSmall", "Plant1", "PlantBarren1" },
		wall = { "PlantFern4", "CaveRockPillarShort1" },
		wallEvery = 6,
		style = "Open",
		chests = { "SackChestLarge1", "SackChestLarge2", "SackChestMedium1", "SackChestMedium2" },
	},
	{
		name = "Swamp",
		title = "the Swamp",
		wallSet = "RootLight",
		floor = "SwampGrass",
		patches = { "WeedsSparse" },
		path = "DirtDark2",
		blend = true,
		ambient = 130,
		large = { "TreeSwampTrunk2" },
		medium = { "PlantFern1", "PlantForest1", "Mushroom3", "CaveRocksSmallImmobile" },
		small = { "Plant3", "GrassTuft1", "GrassTuft2", "Mushroom2" },
		wall = { "PlantFern1", "Plant3" },
		largeEvery = 120,
		smallEvery = 12,
		style = "Natural",
		chests = { "StumpChest1", "SackChestLarge1", "SackChestMedium2" },
	},
	{
		name = "Desert",
		title = "the Desert",
		wallSet = "Dirt",
		floor = "DirtSand",
		patches = { "DirtCrackedLight", "DirtLighter" },
		path = "DirtLight2",
		blend = true,
		ambient = 215,
		large = {
			"OldTree1", "OldTree2", "OldTree3", "OldTree4",
			"RuinsColumnOutdoor01", "RuinsColumnOutdoor02", "RuinsColumnOutdoor03", "RuinsColumnOutdoor04",
		},
		medium = { "StatueRuin1", "StatueRuin2", "StatueRuin3", "StatueRuin4", "CaveRockPillarShort1", "BoulderCaveImmobile" },
		small = { "SkullImmobile", "ArmBoneImmobile", "LegBoneImmobile", "CaveRocksPebblesImmobile", "PlantBarren1" },
		wall = { "RuinsColumnOutdoor05", "RuinsColumnOutdoor06", "StatueRuin5", "StatueRuin6" },
		largeEvery = 120,
		mediumEvery = 70,
		smallEvery = 30,
		wallEvery = 6,
		style = "Open",
		chests = { "SackChestLarge1", "SackChestLarge2", "SackChestMedium1", "SackChestMedium2" },
	},
	{
		name = "Grasslands",
		title = "the Grasslands",
		wallSet = "FieldStoneShort",
		floor = "Grass",
		patches = { "GrassDenseYellow", "GrassSparseYellow", "GrassDense" },
		path = "DirtLight2",
		blend = true,
		ambient = 205,
		large = { "TreeApple", "TreeGreen1", "TreeGreen2", "TreeGreen3" },
		medium = { "Bush1", "Bush2", "Bush4", "Bush5", "Stump1", "Stump2" },
		small = {
			"FlowersYellowSparse", "FlowersWhiteSparse", "FlowersBlueSparse", "FlowersPurpleSparse",
			"GrassTuft1", "GrassTuft2", "GrassTuft3", "Plant2Flowered",
		},
		wall = { "Bush4", "FlowersYellowDense", "FlowersWhiteDense" },
		largeEvery = 220,
		mediumEvery = 80,
		smallEvery = 10,
		wallEvery = 5,
		style = "Open",
		chests = { "StumpChest1", "SackChestLarge1", "SackChestMedium2" },
	},
	{
		name = "Ice",
		title = "the Frozen Wastes",
		wallSet = "IceWall",
		floor = "IceFloorRough",
		patches = { "IceFloorDeepBlue", "IceFloorDark" },
		blend = true,
		ambient = 170,
		large = { "TreeSnowCovered1", "TreeSnowCovered2", "TreeSnowCovered3", "TreeSnowCovered4", "TreeSnowCovered5" },
		medium = { "CaveRocksSmallImmobile", "CaveRocksLargeImmobile" },
		small = { "CaveRocksTinyImmobile", "CaveRocksPebblesImmobile" },
		wall = { "TreeSnowCovered6" },
		wallEvery = 7,
		weight = 1,
		climate = "Freezing",
		style = "Natural",
	},
	{
		name = "Lava",
		title = "the Lava Fields",
		wallSet = "Volcano",
		floor = "VolcanicCraggy",
		patches = { "Lava" },
		path = "", -- (its edges would glow as lava's do: a path that looked burning)
		blend = true,
		edgeMask = "LavaEdgeBlackDirt",
		ambient = 120,
		large = { "CaveRockPillarTall1", "CaveRockPillarTall2", "LavaHardened1", "LavaHardened2" },
		medium = { "LavaHardened3", "LavaHardened4", "CaveRocksLargeImmobile", "LavaFountain" },
		small = { "SkullImmobile", "CaveRocksPebblesImmobile", "CaveRocksSmallImmobile" },
		wall = { "TorchPole", "CaveRockPillarShort2" },
		largeEvery = 110,
		wallEvery = 7,
		weight = 1,
		climate = "Scorching",
		style = "Natural",
	},
	{
		name = "Shop",
		title = "the Shop",
		wallSet = "DunMirCathedral",
		floor = "OakWoodFloor",
		patches = { "RugRed" },
		path = "",
		blend = true,
		edgeMask = "RugRedEdge",
		ambient = 180,
		large = {},
		medium = { "RoundTableWithFood", "SquareTable1", "DarkWoodenChair1", "Barrel2" },
		small = {},
		wall = {
			"TraderShelves1", "TraderShelves2", "TraderClothesRack1", "TraderClothesRack2", "TraderHelmShelf1",
			"TraderBowRack1", "TraderBowRack2", "TraderArmorRack2", "TraderPoleArm3", "TraderPoleArm4", "TraderAppleCrate",
		},
		mediumEvery = 70,
		smallEvery = 1000,
		wallEvery = 3,
		style = "Built",
	},
}

-- The defaults of a field left out.
local defaults = {
	path = "",
	blend = false,
	edgeMask = "BlendEdge",
	ambient = 140,
	largeEvery = 90,
	mediumEvery = 45,
	smallEvery = 18,
	wallEvery = 4,
	weight = 3,
	climate = "Mild",
	style = "Natural",
}
local lists = { "patches", "large", "medium", "small", "wall", "chests" }
for _, b in biomes do
	for field, value in defaults do
		if b[field] == nil then
			b[field] = value
		end
	end
	for _, field in lists do
		if b[field] == nil then
			b[field] = {}
		end
	end
end

return {
	biomes = biomes,
	harmingFloors = { "Lava" },
}
