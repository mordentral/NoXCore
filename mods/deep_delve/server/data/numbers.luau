--!strict
-- The numbers Deep Delve is tuned by, in one place (design section 7: numbers live in data).

local numbers = {}

-- ---- The character

numbers.levels = {
	cap = 50,
	-- The experience to go from level L to L + 1: base * L ^ growth, rounded down; but from level
	-- 1 to 2 `first` (a quick first level: a handful of the first area's kills).
	base = 100,
	growth = 1.5,
	first = 60,
}

-- Health, mana and strength as shares of the class's stock maxima (classes.luau): `atOne` at
-- level 1, `perLevel` more for each level after it. (Mana from 0.8: a level-1 wizard's 120 is
-- four Fireballs.)
numbers.stats = {
	health = { atOne = 0.7, perLevel = 0.03 },
	mana = { atOne = 0.8, perLevel = 0.03 },
	strength = { atOne = 0.8, perLevel = 0.02 },
}

-- Passive regeneration (regen.luau), on top of the game's own refill (health and mana each from
-- empty in 300 s). Per second: each class's base, and what the passive tree's healthRegen and
-- manaRegen stats add. Given every `every` frames; health not within `afterHurt` frames of a hit
-- (as the game's own refill waits).
numbers.regen = {
	base = {
		warrior = { health = 1.5, mana = 0 },
		wizard = { health = 0.5, mana = 2.5 },
		conjurer = { health = 0.8, mana = 2.0 },
	},
	every = 10,
	afterHurt = 30,
}

-- ---- Areas

numbers.areas = {
	-- Monsters' health, damage and experience grow with the area level (1 at level 1).
	healthPerLevel = 0.12,
	damagePerLevel = 0.06,
	experiencePerLevel = 0.10,
	-- A monster's experience is monster.bin's (the stock game's: a zombie 30000) times this, so
	-- a zombie at level 1 gives 15 of the 60 to level 2.
	experienceScale = 0.0005,
	-- Packs in an area: base + perLevel x area level, at most `most`.
	packs = { base = 12, perLevel = 1, most = 24 },
	-- A pack's followers beside its leader.
	followers = { fewest = 2, most = 4 },
	-- The opening (by area level; a level not here takes the numbers above, as act 1's area 4 on
	-- does): fewer and smaller packs, ramping up over the first areas.
	--   packs      packs in the area (in place of the formula)
	--   followers  { fewest, most } beside each leader
	--   ramp       how the packs thin out toward the start: a route pack's place drawn with a
	--              density rising as (place along the route) ^ ramp (0: even)
	--   quiet      the share of the route from its start where packs are the smallest (fewest
	--              followers) and led by a normal monster
	--   light      the share of the route from its start where packs are only light monsters (the
	--              rosters' `light`; by decision 2026-10-29, lighter monsters lead the first rooms)
	--   health, damage  the monsters' health and the damage they deal, times this (champions
	--              too; the act boss's lair is past the opening)
	--   guards     the end chamber's champion's guards (in place of the champion's own count)
	opening = {
		[1] = { packs = 8, followers = { 1, 2 }, ramp = 1, quiet = 0.4, light = 0.4, health = 0.5, damage = 0.5, guards = 1 },
		[2] = { packs = 10, followers = { 1, 3 }, ramp = 0.5, quiet = 0.25, light = 0.15, health = 0.7, damage = 0.7, guards = 2 },
		[3] = { packs = 13, followers = { 2, 3 }, ramp = 0, quiet = 0, light = 0, health = 0.85, damage = 0.85, guards = 3 },
	},
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

-- The pads' rings on the floor (decals.luau): the asset, its size across (world units; a
-- pentagram's own ring is about 52 across, so its ring sits inside the picture's outer band), its
-- turn (degrees a second) and its alpha's pulse (amount 0-1, period in seconds). Drawn additive.
numbers.decals = {
	waypoint = { asset = "fx/waypoint", size = 80, spin = 12, pulse = 0.25, period = 3 },
	portal = { asset = "fx/portal", size = 80, spin = -40, pulse = 0.35, period = 1.6 },
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

-- Act 2's barrow-king (barrowking.luau; data/monsters.luau's barrowking). Frames at 30 a second.
--   beat      his thinking beat (his phases, the next blow chosen, his bar)
--   graves    every `every` frames the graves round his arena give up `count` of his dead (`count`
--             + `enraged` once he is), one grave each, `warn` frames after the grave's sparks; at
--             most `most` of them standing at once; of tier `tier`
--   charge    every `every` frames he charges a player `near`-`far` from him: `warn` frames of
--             sparks along the way, then a rush of `speed` units a frame for at most `frames`
--             frames (a wall stops it), each player within `reach` of him on the way struck once
--             for `damage` (crushing, scaled as his own hits are) and thrown back (`force`).
--             Enraged, every `enragedEvery`.
--   mist      below `at` of his health: every `every` frames the grave-mist (the game's Toxic
--             Cloud, his own) at `count` players' feet (`enraged` more once he is), `warn` frames
--             after the green warning there
--   enrage    below `at`: hasted for good, and the barrow quakes once (Earthquake at `quake`
--             power); then every `fistEvery` frames a Fist of Vengeance (`fistPower`) falls on a
--             player
numbers.barrowKing = {
	beat = 15,
	graves = { every = 30 * 10, count = 2, enraged = 1, warn = 30, most = 6, tier = "normal" },
	charge = { every = 30 * 8, enragedEvery = 30 * 5, near = 100, far = 420, warn = 27, speed = 24, frames = 18, reach = 42, damage = 16, force = 70 },
	mist = { at = 0.7, every = 30 * 9, count = 1, enraged = 1, warn = 24 },
	enrage = { at = 0.3, quake = 1, fistEvery = 30 * 6, fistPower = 1 },
}

-- ---- The party (design 7)

-- Party scaling, counted from the players in the game (each extra player beyond the first adds
-- this much; one player: all 1, as solo). `most` players are counted at most.
--   health      every monster's most health (live ones changed as players come and go)
--   damage      the damage monsters deal
--   experience  a kill's experience, before it's split between the players in play
--   packs       packs in an area (extra packs at the generator's spots, as the area is entered)
--   loot        a death's loot rolls (a part roll is a chance of one more)
--   ownSpared   the missiles whose hits on their own hero are cancelled (combat.luau): the
--               Fireball's, as a cast at levels 1, 2 and 3-5 makes it (a hero's other spells
--               hurt it as the game has them; no damage within the party is a rule of its own)
numbers.party = {
	health = 0.5,
	damage = 0.2,
	experience = 0.5,
	packs = 0.1,
	loot = 0.15,
	most = 6,
	ownSpared = { Fireball = true, StrongFireball = true, TitanFireball = true },
}

-- The party strip: each member's numbers looked at every `every` frames, sent when changed.
numbers.strip = { every = 8 }

-- A vote to change area (the whole game moves): `seconds` to answer; a player whose vote failed
-- may ask again after `again` frames; the vote's clock looked at every `beat` frames.
numbers.vote = { seconds = 30, again = 30 * 5, beat = 15 }

-- Ground loot's labels (shown on NoX Core clients while Alt is held): each player is sent those
-- within `reach` of its unit (at most `most`, nearest first), looked at every `every` frames.
numbers.labels = { reach = 450, most = 96, every = 15 }

-- A click on a piece of gear's label picks it up when the hero is within `reach` of it (the game's
-- own click-pickup reach, world units); further, the player is told "too far", at most once in
-- `toldEvery` frames.
numbers.pick = { reach = 75, toldEvery = 30 }

-- Named monsters' name plates (rares, champions, the act boss: their names over their heads in
-- their tier's colour, always shown on NoX Core clients): each player is sent every named monster
-- of the area while there are at most `most` of them, else the `most` nearest its unit (looked at
-- on the labels' beat). `offset`: how far over the top of the creature's picture the plate sits.
-- The plates' most and the loot's add up to the client's 128 labels at most.
numbers.plates = { most = 32, offset = 6 }

-- ---- The game

numbers.timing = {
	-- The character's record written this often while playing (and on area change, level up,
	-- death and leaving).
	saveEvery = 30 * 60,
	-- A joined player's character set up this many frames after the join (its unit settled).
	setUpAfter = 10,
	-- A level up's glow over the player put away after this many frames.
	glow = 60,
	-- A ready client's hero looked at once more this many frames after its ready (in case the
	-- game sets it watching after the ready).
	readyRecheck = 10,
	-- An empty game of server characters goes back to the first area this many frames after its
	-- last player left (roster.luau).
	homeAfter = 30 * 5,
}

-- ---- Server characters (roster.luau, accounts.luau)

numbers.roster = {
	-- The most characters an account keeps.
	most = 10,
	-- A server's game (the bot server's pool) closes to newcomers once this many players are in,
	-- and opens again below it: the pool then opens another game for the next to come (nil: never
	-- closed). The game's most (mode.json's players) for now; when a party's game should close is
	-- the owner's call.
	lockAt = 6,
}

numbers.links = {
	-- A link code is good this many minutes, once.
	minutes = 10,
	-- Wrong codes counted over this many seconds: a PC may try `tries` in it, the whole server
	-- `storeTries`; past either, none till the window's end.
	window = 600,
	tries = 5,
	storeTries = 50,
}

return numbers
