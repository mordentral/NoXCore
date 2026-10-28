-- The archways of an area map: the game's exit arches (GauntletExitA along a "/" wall,
-- GauntletExitB along a "\" one), set against a straight back wall as the generator's own exits
-- stand, half a cell out from the wall's line, the floor before each kept clear. Deep Delve uses
-- them for every way between places: the town's gate out, the way home from the area's start, the
-- area's exit on.
--
-- (Its own exits, not the game's: the server watches each one's touches and decides where it
-- leads.)

local util = require("util")
local floors = require("floors")

local arch = {}

local TYPES = { [0] = "GauntletExitA", [1] = "GauntletExitB" }

-- An arch of `shape` (0 "/", 1 "\") on the straight back wall nearest `near` (a cell), no further
-- than `reach` from it, none of `taken` (wall cells); written through `w`, the floor before it
-- marked in `used`. Returns its point { x, y } and its wall cell, or nil when there's no such wall.
function arch.place(w, site, shape: number, near: number, reach: number, taken, used)
	local wc = site.backs:straightBack(site.walls, shape, near, taken)
	if not wc or util.manhattan(wc, near) > reach then
		return nil, nil
	end
	local x, y = util.xy(wc)
	local at = { x = (if shape == 0 then x + 1 else x) * 23, y = (y + 1) * 23 }
	w:exit(TYPES[shape], at.x, at.y)
	local away, along = floors.outOf(shape), floors.alongOf(shape)
	for d = -2, 2 do
		for depth = 1, 2 do
			used:add(wc + depth * away + d * along)
		end
	end
	return at, wc
end

return arch
