--!strict
-- Unique items and item sets (by decision 2026-11-01): what each one is, what it carries, its
-- power or its set's bonuses, and how often they drop. Every type is thing.bin's and every
-- modifier modifier.bin's, each checked against the game's fit rule ("dd uniques check").
--
-- A unique: a named item on one of the game's item types, with fixed game modifiers (by slot),
-- Deep Delve affixes (`value` fixed, or `low`-`high` drawn as it drops), and one power no ordinary
-- item has (`power`: its kind, powers.luau's, and its numbers). `class` is the class it's meant
-- for (its type's own rule says who may use it); `from` the lowest area level it drops at.
--
-- A set: two to four named pieces (each fixed: its modifiers and affixes) on the game's types for
-- one class, and bonuses that count only the pieces worn: at `n` worn, its `stats` (each added to
-- the wearer's, as an affix of that stat is) and its `power` (none: stats alone). Its pieces drop
-- at area levels `from` to `to`.
--
-- A power's numbers: `seconds` where a time is meant (frames are worked out from it), `pct` and
-- `chance` in hundredths.

local u = {}

u.uniques = {
	-- ---- the warrior's
	{
		id = "gravecleaver", name = "Gravecleaver, the Hungering Axe", type = "BattleAxe", class = "warrior", from = 5,
		mods = { effectiveness = "WeaponPower3", material = "Material3" },
		affixes = { { id = "damage", low = 8, high = 12 }, { id = "health", value = 10 } },
		power = { kind = "leech", pct = 6 },
	},
	{
		id = "briarmail", name = "Briarmail of the Thorn Knight", type = "ChainTunic", class = "warrior", from = 7,
		mods = { effectiveness = "ArmorQuality3", material = "Material3" },
		affixes = { { id = "health", low = 15, high = 20 }, { id = "resist", value = 4 } },
		power = { kind = "thorns", pct = 60 },
	},
	{
		id = "stormbrand", name = "Stormbrand, the Thunder Sword", type = "Longsword", class = "warrior", from = 9,
		mods = { effectiveness = "WeaponPower3", primary = "Lightning2" },
		affixes = { { id = "damage", low = 8, high = 14 } },
		power = { kind = "chain", chance = 12, spell = "SPELL_CHAIN_LIGHTNING", level = 2 },
	},
	{
		id = "cinderstep", name = "Cinderstep Greaves", type = "PlateBoots", class = "warrior", from = 12,
		mods = { effectiveness = "ArmorQuality3", primary = "FireProtect2" },
		affixes = { { id = "health", value = 15 } },
		power = { kind = "trail", step = 36, seconds = 3, spell = "SPELL_BURN", level = 2 },
	},
	{
		id = "ruinmaker", name = "Ruinmaker, the Breaker's Maul", type = "WarHammer", class = "warrior", from = 16,
		mods = { effectiveness = "WeaponPower4", material = "Material4" },
		affixes = { { id = "damage", low = 12, high = 18 }, { id = "strength", value = 10 } },
		power = { kind = "explode", pct = 25, reach = 90, most = 3 },
	},
	{
		id = "unyielding", name = "The Unyielding Wall", type = "SteelShield", class = "warrior", from = 20,
		mods = { effectiveness = "ArmorQuality4", material = "Material4", primary = "LightningProtect2" },
		affixes = { { id = "health", low = 25, high = 35 }, { id = "resist", value = 6 } },
		power = { kind = "bulwark", pct = 20 },
	},
	{
		id = "ragebound", name = "Ragebound Plate", type = "Breastplate", class = "warrior", from = 24,
		mods = { effectiveness = "ArmorQuality5", material = "Material5", primary = "Regeneration2" },
		affixes = { { id = "health", low = 30, high = 40 }, { id = "damage", value = 10 } },
		power = { kind = "lastWind", below = 30 },
	},
	-- ---- the wizard's
	{
		id = "emberwake", name = "Emberwake, the Kindled Staff", type = "LesserFireballWand", class = "wizard", from = 6,
		affixes = { { id = "mana", low = 15, high = 20 }, { id = "manaRegen", value = 2 } },
		power = { kind = "split", extra = 2, spread = 16 },
	},
	{
		id = "stillmind", name = "Stillmind Mantle", type = "MedievalCloak", class = "wizard", from = 10,
		mods = { effectiveness = "ArmorQuality2" },
		affixes = { { id = "mana", low = 20, high = 26 }, { id = "manaRegen", value = 2 } },
		power = { kind = "manaShield", share = 40 },
	},
	{
		id = "echoing", name = "The Echoing Crown", type = "WizardHelm", class = "wizard", from = 14,
		mods = { effectiveness = "ArmorQuality3" },
		affixes = { { id = "mana", low = 18, high = 24 }, { id = "damage", value = 6 } },
		power = { kind = "echo", n = 4, spells = { "SPELL_FIREBALL", "SPELL_BURN", "SPELL_FORCE_OF_NATURE", "SPELL_METEOR", "SPELL_FIST", "SPELL_DEATH_RAY" } },
	},
	{
		id = "lastember", name = "Mantle of the Last Ember", type = "WizardRobe", class = "wizard", from = 18,
		mods = { effectiveness = "ArmorQuality3", primary = "FireProtect2" },
		affixes = { { id = "health", value = 20 }, { id = "mana", value = 20 } },
		power = { kind = "undying", seconds = 60, spell = "SPELL_CLEANSING_FLAME", level = 3 },
	},
	{
		id = "heartspill", name = "Heartspill, the Crimson Staff", type = "DeathRayWand", class = "wizard", from = 22,
		affixes = { { id = "damage", low = 15, high = 20 }, { id = "health", value = 30 } },
		power = { kind = "bloodMagic", pct = 75 },
	},
	{
		id = "stormcore", name = "Stormcore, the Living Staff", type = "StaffWooden", class = "wizard", from = 26,
		affixes = { { id = "mana", value = 30 }, { id = "manaRegen", value = 3 } },
		power = { kind = "nova", seconds = 3, reach = 120, damage = 30, perLevel = 2 },
	},
	-- ---- the conjurer's
	{
		id = "packmother", name = "Packmother's Hide", type = "LeatherArmor", class = "conjurer", from = 8,
		mods = { effectiveness = "ArmorQuality2" },
		affixes = { { id = "health", value = 12 }, { id = "summonHealth", value = 10 } },
		power = { kind = "companion", creature = "Wolf", seconds = 10 },
	},
	{
		id = "seekersong", name = "Seekersong, the Hunting Bow", type = "Bow", class = "conjurer", from = 11,
		affixes = { { id = "damage", low = 8, high = 12 }, { id = "grip", low = 6, high = 10 } },
		power = { kind = "seeker", chance = 50, reach = 250 },
	},
	{
		id = "bloodbond", name = "Bloodbond Circlet", type = "ConjurerHelm", class = "conjurer", from = 13,
		mods = { effectiveness = "ArmorQuality3" },
		affixes = { { id = "mana", value = 15 }, { id = "summonHealth", value = 20 } },
		power = { kind = "packVamp" },
	},
	{
		id = "rimebolt", name = "Rimebolt, the Winter Crossbow", type = "CrossBow", class = "conjurer", from = 19,
		affixes = { { id = "damage", low = 10, high = 16 }, { id = "frost", value = 10 } },
		power = { kind = "freeze", chance = 12, frames = 45 },
	},
	{
		id = "keenwind", name = "Quiver of the Keen Wind", type = "Quiver", class = "conjurer", from = 28,
		mods = { effectiveness = "WeaponPower5", primary = "Fire3" },
		affixes = { { id = "damage", low = 12, high = 18 } },
		power = { kind = "crit", chance = 15, times = 2 },
	},
	{
		id = "huntfever", name = "Huntfever Leggings", type = "LeatherLeggings", class = "conjurer", from = 30,
		mods = { effectiveness = "ArmorQuality4" },
		affixes = { { id = "health", value = 25 }, { id = "damage", value = 8 } },
		power = { kind = "frenzy", seconds = 3, step = 5, most = 40 },
	},
	{
		id = "reaper", name = "The Reaper's Bracers", type = "LeatherArmbands", class = "conjurer", from = 34,
		mods = { effectiveness = "ArmorQuality5", primary = "Regeneration3" },
		affixes = { { id = "health", value = 30 }, { id = "resist", value = 6 } },
		power = { kind = "healOnKill", pct = 5 },
	},
}

u.sets = {
	-- ---- the warrior's
	{
		id = "ironvow", name = "The Ironvow Oath", class = "warrior", from = 4, to = 14,
		pieces = {
			{ id = "ironvowHelm", name = "Ironvow Helm", type = "SteelHelm", mods = { effectiveness = "ArmorQuality2" }, affixes = { { id = "health", value = 10 } } },
			{ id = "ironvowHauberk", name = "Ironvow Hauberk", type = "ChainTunic", mods = { effectiveness = "ArmorQuality2" }, affixes = { { id = "health", value = 15 } } },
			{ id = "ironvowGreaves", name = "Ironvow Greaves", type = "ChainLeggings", mods = { effectiveness = "ArmorQuality2" }, affixes = { { id = "damage", value = 4 } } },
		},
		bonuses = { { n = 2, stats = { health = 25 } }, { n = 3, stats = { resist = 8 } } },
	},
	{
		id = "legion", name = "The Drowned Legion", class = "warrior", from = 12, to = 24,
		pieces = {
			{ id = "legionVisor", name = "Legion Visor", type = "OrnateHelm", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "health", value = 15 } } },
			{ id = "legionCuirass", name = "Legion Cuirass", type = "Breastplate", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "health", value = 20 } } },
			{ id = "legionVambraces", name = "Legion Vambraces", type = "PlateArms", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "damage", value = 5 } } },
			{ id = "legionGreaves", name = "Legion Greaves", type = "PlateLeggings", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "resist", value = 3 } } },
		},
		bonuses = { { n = 2, stats = { damage = 12 } }, { n = 4, power = { kind = "explode", pct = 10, reach = 70, most = 2 } } },
	},
	{
		id = "bloodsworn", name = "The Bloodsworn", class = "warrior", from = 22, to = 34,
		pieces = {
			{ id = "bloodswornBlade", name = "Bloodsworn Blade", type = "Longsword", mods = { effectiveness = "WeaponPower4" }, affixes = { { id = "damage", value = 10 } } },
			{ id = "bloodswornBulwark", name = "Bloodsworn Bulwark", type = "SteelShield", mods = { effectiveness = "ArmorQuality4" }, affixes = { { id = "health", value = 25 } } },
			{ id = "bloodswornSabatons", name = "Bloodsworn Sabatons", type = "PlateBoots", mods = { effectiveness = "ArmorQuality4" }, affixes = { { id = "health", value = 20 } } },
		},
		bonuses = { { n = 2, stats = { health = 40 } }, { n = 3, power = { kind = "leech", pct = 3 } } },
	},
	-- ---- the wizard's
	{
		id = "scholar", name = "The Quiet Scholar", class = "wizard", from = 2, to = 12,
		pieces = {
			{ id = "scholarRobe", name = "Scholar's Robe", type = "WizardRobe", affixes = { { id = "mana", value = 10 } } },
			{ id = "scholarHat", name = "Scholar's Hat", type = "WizardHelm", affixes = { { id = "mana", value = 8 } } },
			{ id = "scholarBreeches", name = "Scholar's Breeches", type = "MedievalPants", affixes = { { id = "health", value = 8 } } },
		},
		bonuses = { { n = 2, stats = { mana = 25 } }, { n = 3, stats = { manaRegen = 2 } } },
	},
	{
		id = "conclave", name = "The Ashen Conclave", class = "wizard", from = 10, to = 22,
		pieces = {
			{ id = "conclaveStaff", name = "Conclave Staff", type = "FireStormWand", affixes = { { id = "damage", value = 8 } } },
			{ id = "conclaveRobe", name = "Conclave Robe", type = "WizardRobe", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "mana", value = 15 } } },
			{ id = "conclaveCowl", name = "Conclave Cowl", type = "WizardHelm", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "mana", value = 12 } } },
			{ id = "conclaveShirt", name = "Conclave Shirt", type = "MedievalShirt", affixes = { { id = "health", value = 15 } } },
		},
		bonuses = { { n = 2, stats = { damage = 10 } }, { n = 4, stats = { manaOnKill = 8, mana = 20 } } },
	},
	{
		id = "stormwoven", name = "Stormwoven Garb", class = "wizard", from = 20, to = 34,
		pieces = {
			{ id = "stormwovenCloak", name = "Stormwoven Cloak", type = "MedievalCloak", mods = { effectiveness = "ArmorQuality4" }, affixes = { { id = "mana", value = 20 } } },
			{ id = "stormwovenShirt", name = "Stormwoven Shirt", type = "MedievalShirt", affixes = { { id = "health", value = 20 } } },
			{ id = "stormwovenBreeches", name = "Stormwoven Breeches", type = "MedievalPants", affixes = { { id = "manaRegen", value = 2 } } },
		},
		bonuses = { { n = 2, stats = { resist = 8 } }, { n = 3, power = { kind = "manaShield", share = 20 } } },
	},
	-- ---- the conjurer's
	{
		id = "huntsman", name = "The Huntsman's Leathers", class = "conjurer", from = 2, to = 12,
		pieces = {
			{ id = "huntsmanCap", name = "Huntsman's Cap", type = "LeatherHelm", mods = { effectiveness = "ArmorQuality2" }, affixes = { { id = "health", value = 8 } } },
			{ id = "huntsmanJerkin", name = "Huntsman's Jerkin", type = "LeatherArmor", mods = { effectiveness = "ArmorQuality2" }, affixes = { { id = "health", value = 12 } } },
			{ id = "huntsmanBoots", name = "Huntsman's Boots", type = "LeatherBoots", mods = { effectiveness = "ArmorQuality2" }, affixes = { { id = "mana", value = 8 } } },
		},
		bonuses = { { n = 2, stats = { health = 20 } }, { n = 3, stats = { damage = 6, mana = 10 } } },
	},
	{
		id = "beastcaller", name = "The Beastcaller", class = "conjurer", from = 10, to = 24,
		pieces = {
			{ id = "beastcallerCrown", name = "Beastcaller's Crown", type = "ConjurerHelm", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "mana", value = 12 } } },
			{ id = "beastcallerBracers", name = "Beastcaller's Bracers", type = "LeatherArmbands", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "health", value = 12 } } },
			{ id = "beastcallerLeggings", name = "Beastcaller's Leggings", type = "LeatherLeggings", mods = { effectiveness = "ArmorQuality3" }, affixes = { { id = "health", value = 12 } } },
			{ id = "beastcallerStaff", name = "Beastcaller's Staff", type = "StaffWooden", affixes = { { id = "mana", value = 15 } } },
		},
		bonuses = { { n = 2, stats = { summonHealth = 20 } }, { n = 4, power = { kind = "packVamp" } } },
	},
	{
		id = "greenwarden", name = "The Greenwarden", class = "conjurer", from = 20, to = 34,
		pieces = {
			{ id = "greenwardenBow", name = "Greenwarden's Bow", type = "Bow", affixes = { { id = "damage", value = 10 } } },
			{ id = "greenwardenQuiver", name = "Greenwarden's Quiver", type = "Quiver", mods = { effectiveness = "WeaponPower4" }, affixes = { { id = "damage", value = 6 } } },
			{ id = "greenwardenHood", name = "Greenwarden's Hood", type = "LeatherHelm", mods = { effectiveness = "ArmorQuality4" }, affixes = { { id = "health", value = 20 } } },
		},
		bonuses = { { n = 2, stats = { damage = 12 } }, { n = 3, power = { kind = "healOnKill", pct = 2 } } },
	},
}

-- What each power's line says: a format, and the keys of its numbers in order (a `seconds` read as
-- seconds; `creature` a word). The line is wrapped to the tooltip's width.
u.powerText = {
	leech = { "Your hits give back %d%% of their damage as health", "pct" },
	thorns = { "A monster's blow on you: it takes %d%% of it back", "pct" },
	chain = { "Your weapon's hits: a %d%% chance to call Chain Lightning on the foe", "chance" },
	trail = { "The ground burns where you walk (the game's Burn)" },
	explode = { "A monster you kill bursts for %d%% of its health round it", "pct" },
	bulwark = { "No single hit takes more than %d%% of your most health", "pct" },
	lastWind = { "Below %d%% health you are hasted", "below" },
	split = { "Your Fireballs split: %d more fly beside each one", "extra" },
	manaShield = { "%d%% of the damage you take is paid from your mana", "share" },
	echo = { "Every %dth spell you cast goes off twice", "n" },
	undying = { "A killing blow leaves you at 1 health in a ring of fire (once in %d s)", "seconds" },
	bloodMagic = { "Your spells cost health instead of mana (%d%% of the price)", "pct" },
	nova = { "Every %d s a shock nova strikes the monsters near you", "seconds" },
	companion = { "A %s runs at your side; one comes back %d s after it falls", "creature", "seconds" },
	seeker = { "An arrow that strikes: a %d%% chance to loose another at the next foe", "chance" },
	packVamp = { "Your creatures drain life as they bite (vampirism)" },
	freeze = { "Your weapon's hits: a %d%% chance to freeze a monster where it stands", "chance" },
	crit = { "Your weapon's hits: a %d%% chance to deal double damage", "chance" },
	frenzy = { "Each kill within %d s of the last: +%d%% damage (to %d%%)", "seconds", "step", "most" },
	healOnKill = { "Each kill heals you %d%% of your most health", "pct" },
}

-- Stats a unique's or a set's affix or bonus may carry beyond the loot's own affixes (never rolled
-- on ordinary loot): { id, text (%d its value), stat (the hero's bonus it adds to) }.
u.stats = {
	{ id = "resist", text = "+%d%% damage resistance", stat = "resist" },
	{ id = "strength", text = "+%d strength", stat = "strength" },
	{ id = "healthRegen", text = "+%d health a second", stat = "healthRegen" },
	{ id = "summonHealth", text = "+%d%% health for your creatures", stat = "summonHealth" },
	{ id = "manaOnKill", text = "+%d mana a kill", stat = "manaOnKill" },
}

-- How often they drop (chances 0-1):
--   gear       on each loot roll that gives gear: a unique (any whose `from` the area level has
--              reached) in place of the piece, else a set piece (of a set whose levels hold the area
--              level), each times (1 + boost x the monster's betterness: magic 0.25, rare 0.5,
--              champion 1, a boss's rolls 1.5);
--   champion   one more roll for a unique, and one for a set piece, at a champion's death;
--   boss       the same at an act boss's death (at its drop's level); `favoured`: the share of the
--              boss's unique rolls that take one of its favoured uniques (any of them the level
--              allows), by the boss's id (data/monsters.luau's bosses).
u.drops = {
	unique = { gear = 0.004, champion = 0.06, boss = 0.35 },
	set = { gear = 0.006, champion = 0.08, boss = 0.3 },
	boost = 2,
	favoured = 0.5,
}

-- Each act boss's favoured uniques.
u.favoured = {
	warlord = { "gravecleaver", "emberwake", "briarmail" },
	barrowking = { "stormbrand", "seekersong", "echoing" },
	mosschief = { "ruinmaker", "lastember", "rimebolt" },
	-- (act 4's ruin-warden: the late uniques)
	warden = { "stormcore", "keenwind", "reaper" },
}

-- The powers' timer: one per hero wearing a power that runs on time (trail, nova, lastWind,
-- companion), every `every` frames.
u.beat = { every = 6 }

-- The tooltip: the most lines a unique's or a set piece's takes, and how wide its power and bonus
-- lines may be before they're wrapped (letters).
u.tip = { most = 22, wrap = 56 }

-- The party's line for a find off the ground ("X found Y."), in the item's colour as its label
-- and tooltip draw it (by rarity: a unique's orange, a set piece's green; NoX Core clients of
-- level 8 draw it so, others plain).
u.foundColour = { unique = { 245, 140, 40 }, set = { 80, 225, 80 } }

return u
