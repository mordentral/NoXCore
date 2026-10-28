--!strict
-- The three classes: where their stock numbers come from, and what a new character starts with.
--
-- A Deep Delve character's health, mana and strength are its own (hero.luau works them out from
-- its level), as shares of the class's stock maxima: gamedata.bin's numbers below, read once
-- with nox.gameNumber. So a level-1 character is weaker than a stock one, and a high level one
-- stronger.

return {
	warrior = {
		title = "Warrior",
		numbers = { health = "WarriorMaxHealth", mana = "WarriorMaxMana", strength = "WarriorMaxStrength" },
		-- What a new character carries: each { type, worn } (no modifiers: plain gear).
		start = {
			{ type = "Sword", worn = true },
			{ type = "LeatherArmor", worn = true },
			{ type = "LeatherBoots", worn = true },
			{ type = "RedPotion" },
			{ type = "RedPotion" },
		},
	},
	wizard = {
		title = "Wizard",
		numbers = { health = "WizardMaxHealth", mana = "WizardMaxMana", strength = "WizardMaxStrength" },
		start = {
			{ type = "StaffWooden", worn = true },
			{ type = "WizardRobe", worn = true },
			{ type = "BluePotion" },
			{ type = "RedPotion" },
		},
	},
	conjurer = {
		title = "Conjurer",
		numbers = { health = "ConjurerMaxHealth", mana = "ConjurerMaxMana", strength = "ConjurerMaxStrength" },
		start = {
			{ type = "Bow", worn = true },
			{ type = "Quiver", worn = true },
			{ type = "LeatherBoots", worn = true },
			{ type = "RedPotion" },
			{ type = "BluePotion" },
		},
	},
}
