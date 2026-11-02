--!strict
-- How items read in their tooltips (tips.luau). The game's own words come first: a type's name
-- (nox.itemStats), a modifier's identify line ("Level 2 Fire") or, without one, its word ("Mighty",
-- "Bronze"), and its effects' numbers (nox.modifier's effects). What's here fills in around them:
-- each type's kind, how an effect's number reads, and names for a host without the game's string
-- table (where the game's words are missing).

local tips = {}

-- Item types: { kind (the word after its rarity: "Rare sword"), ammo (a bow's: the missile whose
-- record its damage is), does (what it's for: potions), name (a host without the string table: the
-- type's name split into words otherwise, a "Wand" called a staff) }. A type not here is an
-- "item".
tips.items = {
	-- weapons
	Sword = { kind = "sword" },
	Longsword = { kind = "sword" },
	MorningStar = { kind = "mace" },
	BattleAxe = { kind = "axe" },
	WarHammer = { kind = "hammer" },
	GreatSword = { kind = "sword" },
	Bow = { kind = "bow", ammo = "ArcherArrow" },
	CrossBow = { name = "Crossbow", kind = "crossbow", ammo = "ArcherBolt" },
	Quiver = { kind = "quiver" },
	StaffWooden = { name = "Wooden Staff", kind = "staff" },
	-- (thrown; none drops in Deep Delve yet: the kind names the warrior's chakram mastery)
	RoundChakram = { kind = "chakram" },
	FanChakram = { kind = "chakram" },
	LesserFireballWand = { kind = "spell staff" },
	SulphorousFlareWand = { kind = "spell staff" },
	ForceWand = { kind = "spell staff" },
	SulphorousShowerWand = { kind = "spell staff" },
	FireStormWand = { kind = "spell staff" },
	DeathRayWand = { kind = "spell staff" },
	InfinitePainWand = { kind = "spell staff" },
	-- armour
	LeatherBoots = { kind = "boots" },
	LeatherHelm = { kind = "helm" },
	LeatherArmor = { kind = "body armour" },
	LeatherLeggings = { kind = "leggings" },
	LeatherArmbands = { kind = "armbands" },
	WizardRobe = { kind = "robe" },
	MedievalShirt = { kind = "shirt" },
	MedievalPants = { kind = "pants" },
	WizardHelm = { kind = "helm" },
	ConjurerHelm = { kind = "helm" },
	MedievalCloak = { kind = "cloak" },
	WoodenShield = { kind = "shield" },
	ChainCoif = { kind = "helm" },
	ChainTunic = { kind = "body armour" },
	ChainLeggings = { kind = "leggings" },
	SteelHelm = { kind = "helm" },
	SteelShield = { kind = "shield" },
	PlateBoots = { kind = "boots" },
	Breastplate = { kind = "body armour" },
	-- potions
	RedPotion = { kind = "potion", does = "Restores health" },
	BluePotion = { kind = "potion", does = "Restores mana" },
	CurePoisonPotion = { kind = "potion", does = "Cures poison" },
}

-- How an effect's number reads (nox.modifier's effects, by function): "mul" a multiplier as a
-- percentage up or down, "add" a number added, "part" a fraction as a percentage, "wear" a
-- material's: the engine wears the item by 2 less its number for each point it would lose (1.04:
-- "4% less wear"). An effect not here gives no number (its identify line says what it is).
tips.effects = {
	DamageMultiplierEffect = { how = "mul", words = "weapon damage" },
	ArmorMultiplierEffect = { how = "mul", words = "armour" },
	DurabilityMultiplierEffect = { how = "wear", words = "wear" },
	FireEffect = { how = "add", words = "fire damage" },
	LightningEffect = { how = "add", words = "shock damage" },
	VampirismEffect = { how = "part", words = "of the damage back as health" },
	SpeedEngage = { how = "part", words = "speed" },
	ProjectileSpeedEffect = { how = "mul", words = "missile speed" },
}

-- Modifier families (a modifier's name less its number: "Fire2" is Fire's, at 2), for a modifier
-- with neither an identify line nor a word (a host without the string table, and a few the game
-- names neither way): { name, of (its steps: "Frost 2 of 4"), does (nil: the name alone) }. One
-- that does nothing (no effect the engine runs: loot.does) says so by itself. A family not here:
-- its split name.
tips.modifiers = {
	WeaponPower = { name = "Power", of = 6 },
	ArmorQuality = { name = "Quality", of = 6 },
	Material = { name = "Material", of = 7 },
	Stun = { name = "Stun", of = 4, does = "stuns on a hit" },
	Fire = { name = "Fire", of = 4 },
	FireRing = { name = "Ring of Fire", of = 4, does = "a ring of fire on a hit" },
	BlueFireRing = { name = "Ring of Mana Fire", of = 4, does = "a ring of mana fire on a hit" },
	Frost = { name = "Frost", of = 4 },
	Impact = { name = "Impact", of = 4, does = "knocks back on a hit" },
	Grip = { name = "Grip", of = 4 },
	Confuse = { name = "Confusion", of = 4, does = "confuses on a hit" },
	Lightning = { name = "Shock", of = 4 },
	ManaSteal = { name = "Drain Mana", of = 4, does = "drains mana on a hit" },
	Resilience = { name = "Resilience", of = 4 },
	Vampirism = { name = "Vampirism", of = 4 },
	Venom = { name = "Poison", of = 4, does = "poisons on a hit" },
	Brilliance = { name = "Light", of = 4, does = "gives light" },
	Dread = { name = "Dread", of = 4 },
	FireProtect = { name = "Fire Protection", of = 4, does = "resists fire" },
	LightningProtect = { name = "Shock Protection", of = 4, does = "resists shock" },
	PoisonProtect = { name = "Poison Protection", of = 4, does = "resists poison" },
	EnchantmentProtect = { name = "Spell Protection", of = 4 },
	MagicReflection = { name = "Reflection", of = 4 },
	Regeneration = { name = "Regeneration", of = 4, does = "heals over time" },
	Speed = { name = "Haste", of = 4 },
	Readiness = { name = "Readiness", of = 4, does = "shoots sooner" },
	ProjectileSpeed = { name = "Bolt Speed", of = 4 },
	Replenishment = { name = "Recharge", of = 4 },
	ContinualReplenishment = { name = "Continual Recharge", of = 4 },
}

-- Each rarity's word before the item's kind ("Rare sword").
tips.rarity = { normal = "Normal", magic = "Magic", rare = "Rare", unique = "Unique", set = "Set" }

-- The most lines an item's tooltip takes from the server (the window adds its price line).
tips.most = 11

return tips
