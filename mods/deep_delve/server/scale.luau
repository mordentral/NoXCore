--!strict
-- Party scaling (design 7): how much more the monsters have, deal and give with more players in
-- the game. One player: every factor is 1, so a single player's game is the same as ever.
--
-- The count is set by party.luau as players come and go; the factors are data/numbers.luau's
-- `party`, each per extra player.

local numbers = require("data/numbers")

local scale = {}

local players = 1

-- The players counted now (at least 1, at most numbers.party.most).
function scale.players(): number
	return players
end

-- Sets the count (party.luau); the count it was.
function scale.set(n: number): number
	local was = players
	players = math.clamp(math.floor(n), 1, numbers.party.most)
	return was
end

-- A factor for `n` players (default: now): "health", "damage", "experience", "packs", "loot".
function scale.factor(kind: string, n: number?): number
	local count = math.clamp(math.floor(n or players), 1, numbers.party.most)
	local per = (numbers.party :: any)[kind] or 0
	return 1 + per * (count - 1)
end

return scale
