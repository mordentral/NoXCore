--!strict
-- A hero's casts (onSpellCast): a spell whose rise has a `size` (data/tree.luau) makes its
-- missiles larger at the levels it names. Fireball 4 and 5: the TitanFireball a cast makes gets a
-- wider burst (nox.setBurstScale), a hit circle (nox.setHitRadius) and is drawn larger
-- (nox.setDrawScale; NoX Core clients at client level 5 draw its burst to match).
--
--   * Only a hero's cast: a monster's (or anyone's not set up as a hero) is left as the game made
--     it.
--   * The level is the caster's book's (nox.spellLevel: the level the cast went off at, the one the
--     tree granted), never the missile's type: levels 3, 4 and 5 all make a TitanFireball.
--   * Only the made things of the size's missile type are sized (`made` also holds anything the
--     cast's kills made at once).
--   * All are set at once, in the cast's frame, before the missile first flies. A fireball starts
--     only twice its caster's radius ahead, inside the caster's shape grown by the circle; the
--     engine spares a sized missile's owner (meets it only as the stock missile would) until its
--     flight first comes clear of that grown shape, so the circle doesn't burst it in the hero's
--     face. After that the hero is a body like any other: a Fireball turned back still hits it.
--
-- The engine hands the cast's things over in the frame they are made, before they first fly.
-- Nothing here walks the world: the cast's own list and one book lookup.

local data = require("data/tree")
local hero = require("hero")

local casts = {}

-- A cast heard (main.luau's onSpellCast): `spell` its name (a number for one the game has no
-- name for), `made` the ids it made (nil: none).
function casts.cast(caster: number, spell: string | number, made: { number }?)
	if not made or type(spell) ~= "string" then
		return
	end
	local size = data.rise(spell).size
	if not size or not hero.byUnit(caster) then
		return
	end
	local at = size.byLevel[nox.spellLevel(caster, spell)]
	if not at then
		return
	end
	for _, id in made do
		local o = nox.object(id)
		if o and o.type == size.missile then
			nox.setBurstScale(id, at.burst)
			nox.setHitRadius(id, at.hit)
			if at.draw then
				-- (drawn larger by NoX Core clients at client level 5, its burst with it; others
				-- draw the stock missile and burst)
				nox.setDrawScale(id, at.draw)
			end
		end
	end
end

return casts
