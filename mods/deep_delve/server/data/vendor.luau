--!strict
-- The town's vendor and its stash (design 4.4): what it sells, what it pays, and the stash's size.
-- Every type is thing.bin's ("dd check" proves them).

local vendor = {}

-- Potions, always in stock: { type, price } (gold).
vendor.potions = {
	{ type = "RedPotion", price = 12 },
	{ type = "BluePotion", price = 12 },
	{ type = "CurePoisonPotion", price = 8 },
}

-- The gear it sells: `count` pieces drawn from the loot's gear list at the area level (none
-- above it), each magic at `magic` odds and rare at `rare` (else normal). Rolled again as each
-- area is entered and when an act is cleared.
vendor.gear = { count = 8, magic = 0.35, rare = 0.05 }

-- A piece of gear's price: (base + perLevel x its level) x its rarity's factor, rounded down.
-- The vendor pays `sell` of that for a piece (a potion: `sell` of its own price), and a reroll of
-- a magic or rare piece's modifiers costs `reroll` of it.
vendor.prices = {
	base = 20,
	perLevel = 6,
	rarity = { normal = 1, magic = 2.5, rare = 6, unique = 12 },
	sell = 0.25,
	reroll = 0.6,
}

-- How near a station (the stash's chest, the vendor's anvil, the fountain) a player must stand to
-- use it, in world units; walking further closes its window.
vendor.reach = 140

-- The stash: per character (design 12.6), `tabs` tabs of `size` slots each.
vendor.stash = { tabs = 4, size = 40 }

return vendor
