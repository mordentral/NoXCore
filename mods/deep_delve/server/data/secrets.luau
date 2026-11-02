--!strict
-- Secret rooms and chests (secrets.luau, gen/secrets.luau): how often, what they hold, what their
-- traps do. Never in town or in a lair.

local secrets = {}

-- How often (chances 0-1, drawn by the generator from the area's seed):
--   room      an area's secret room (a side room shut by a secret wall, a lever nearby); some
--             areas have no side room the wall can shut alone, so a little fewer have one
--   chests    each other side room's chest
--   trapped   a side room's chest that is trapped
secrets.chance = { room = 0.4, chests = 0.35, trapped = 0.35 }

-- The secret room's chest types (the game's treasure chests), and its guards: { fewest, most },
-- the first of them `leader` tier, the rest `tier`, of the area's roster at the area level.
secrets.hoard = { "Chest1", "Chest2", "Chest3", "Chest4" }
secrets.guards = { count = { 2, 3 }, leader = "magic", tier = "normal" }

-- Locked secret rooms (by decision, 2026-11-02): in a biome with a door of its own (`doors`: the
-- biome's door type, as the generated levels' locked doors take theirs; the outdoor biomes have
-- none, so their rooms keep the secret wall and lever), `chance` of the secret rooms are shut by
-- that door, locked with a silver key's lock, instead. The key is carried by a guard on the way near
-- the room (`guard`: its tier and name, of the area's roster at its level; never charmed away) and
-- dropped at its death. The key is looked after every `every` frames while it's out: gone while
-- its holder stood within `used` of the door, the door took it (unlocked); gone anywhere else, a new
-- one lies by the door's outside.
secrets.locked = {
	chance = 0.5,
	doors = {
		Crypt = "CryptDoor",
		Dungeon = "CryptDoor",
		Castle = "WoodAndSteelDoor",
		Shop = "WoodAndSteelDoor",
		Mines = "WoodenDoor",
		Cave = "WoodenDoor",
	},
	key = "SilverKey",
	guard = { tier = "champion", name = "Keybearer" },
	every = 5,
	used = 100,
}

-- What a chest holds when it's opened (the mod's loot: loot.drop, by the party's size as a
-- death's): `rolls` loot rolls at the area level with `better` odds (as a tougher monster's
-- drops: more gear, magic and rare more often).
--   hoard     the secret room's; besides its rolls, `rare` sure pieces of rare gear at the area
--             level (by decision, 2026-11-02: one; an item that takes no modifier can't be rare,
--             so a few draws are tried, as an act boss's rare pieces are)
--   chest     a side room's
--   trapped   a side room's trapped one (the risk pays)
-- (A monster's drop at the same level: 1 + its tier's drops rolls, `better` a quarter of its
-- drops: a normal monster's 1 roll at 0.)
secrets.loot = {
	hoard = { rolls = 7, better = 1.5, rare = 1 },
	chest = { rolls = 2, better = 0.4 },
	trapped = { rolls = 3, better = 0.8 },
}

-- The traps, by kind: `weight` (how often the generator draws it). Every trap is owned by nobody
-- and goes off at the chest the frame it's seen open; it hurts only players (each within `reach`
-- of the chest, by the engine's damage from the trap's own caster, so a player's resistances
-- count and the party's rule spares nothing), never monsters.
--   fire       a burst of flame (the game's explosion and fireball's blast): `damage` flame
--   lightning  a crack of lightning on each one (sparks, the bolt's sound): `damage` electric
--   gas        poison gas (a smoke blast, the poison trap's sound): `damage` poison, and the
--              game's Poison cast on each at `power`
--   monsters   a burst of the area's monsters round the chest: `count` { fewest, most }, normal
-- `damage` grows with the area level as the monsters' does: times (1 + perLevel x (level - 1)).
secrets.traps = {
	fire = { weight = 3, damage = 22, reach = 85, kind = 1, effect = 0x85, sound = "FireballExplode" },
	lightning = { weight = 2, damage = 18, reach = 110, kind = 9, effect = 0x93, sound = "LightningBolt" },
	gas = { weight = 2, damage = 8, reach = 95, kind = 5, effect = 0x8A, sound = "PoisonTrapTriggered", spell = "SPELL_POISON", power = 2 },
	monsters = { weight = 2, count = { 2, 4 }, reach = 70, effect = 0x8A, sound = "SummonComplete" },
}
-- (the generator's order for its draw)
secrets.trapOrder = { "fire", "lightning", "gas", "monsters" }
secrets.perLevel = 0.06

-- The trap's caster (the game's invisible one, owned by nobody), made at the chest as it goes off
-- and put away `keep` frames later.
secrets.caster = { type = "ImaginaryCaster", keep = 90 }

-- The chests are looked at every `every` frames while an area has any shut (a short list).
secrets.every = 3
-- Who opened it: the nearest live player within `opener` of the chest.
secrets.opener = 90

return secrets
