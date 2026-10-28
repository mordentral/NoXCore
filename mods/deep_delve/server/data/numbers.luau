--!strict
-- The numbers Deep Delve is tuned by, in one place (design section 7: numbers live in data).

local numbers = {}

-- ---- The character

numbers.levels = {
	cap = 50,
	-- The experience to go from level L to L + 1: base * L ^ growth, rounded down.
	base = 100,
	growth = 1.5,
}

-- Health, mana and strength as shares of the class's stock maxima (classes.luau): `atOne` at
-- level 1, `perLevel` more for each level after it.
numbers.stats = {
	health = { atOne = 0.7, perLevel = 0.03 },
	mana = { atOne = 0.7, perLevel = 0.03 },
	strength = { atOne = 0.8, perLevel = 0.02 },
}

-- ---- Areas

numbers.areas = {
	-- Monsters' health, damage and experience grow with the area level (1 at level 1).
	healthPerLevel = 0.12,
	damagePerLevel = 0.06,
	experiencePerLevel = 0.10,
	-- A monster's experience is monster.bin's (the stock game's: a zombie 30000) times this, so
	-- a zombie at level 1 gives 15 of the 100 to level 2.
	experienceScale = 0.0005,
	-- Packs in an area: base + perLevel x area level, at most `most`.
	packs = { base = 12, perLevel = 1, most = 24 },
	-- A pack's followers beside its leader.
	followers = { fewest = 2, most = 4 },
	-- The route: diagonal runs (more: a longer area) and the end chamber's size.
	segments = { base = 7, perLevel = 0.5, most = 12 },
	endRadius = 9,
	-- An act's lair: a short way in (segments), a few packs on it, and the arena (its end
	-- chamber, at most endRadius).
	lair = { segments = 3, packs = 5, endRadius = 13 },
}

-- ---- Waypoints, the portal, the boss

numbers.waypoints = {
	-- A hero within `reach` of an area's waypoint finds it; looked for every `every` frames.
	reach = 120,
	every = 10,
}

numbers.portal = {
	-- An open portal's sparks where it stands (nox.effect: violet), every `every` frames.
	sparks = 0x84,
	every = 24,
}

numbers.boss = {
	-- Its thinking beat (frames): its phases, its slam, its health bar.
	beat = 15,
	-- Its adds come at these shares of its health, `adds` at a time.
	summonAt = { 0.66, 0.33 },
	adds = 3,
	-- Below this share it's enraged (its enrage enchantment).
	enrageAt = 0.3,
	-- The slam: at a player within `range`, every `every` frames; a warning at its feet, the blow
	-- `warn` frames later: `damage` (scaled as the boss's own hits are) within `reach`.
	slam = { range = 260, every = 30 * 6, warn = 36, reach = 70, damage = 14 },
	-- Its drop: `rare` pieces of rare gear `levelUp` levels above the area's, and `rolls` more loot
	-- rolls with `better` odds.
	drop = { rare = 2, levelUp = 2, rolls = 6, better = 1.5 },
}

-- ---- The game

numbers.timing = {
	-- The character's record written this often while playing (and on area change, level up,
	-- death and leaving).
	saveEvery = 30 * 60,
	-- A joined player's character set up this many frames after the join (its unit settled).
	setUpAfter = 10,
	-- A level up's glow over the player put away after this many frames.
	glow = 60,
	-- A rare's or champion's name over its head every this many frames while a player is within
	-- nameReach of it (A7's name plate's stand-in).
	nameEvery = 30 * 4,
	nameReach = 300,
	-- A ready client's hero looked at once more this many frames after its ready (in case the
	-- game sets it watching after the ready).
	readyRecheck = 10,
}

return numbers
