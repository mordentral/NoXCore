-- The generator's helpers over plain numbers, in the
-- C++'s own rules so a port line by line gives the same numbers:
--   - a cell is one number, x * 4096 + y + 2048 (y from -2048 to 2047): numeric order is the
--     C++'s std::set order (x, then y), and a step is a constant;
--   - C's integer division and remainder truncate (Luau's // floors);
--   - unsigned 32-bit sums and products wrap (gen.mul32 is the exact product).
-- No powers by the power operator anywhere in the generator: under some roundings the C library's
-- pow is inexact even for powers of two, so they're written as literals.

local util = {}

local ROW = 4096
local YOFF = 2048
local U32 = 4294967296

util.ROW = ROW

function util.cell(x: number, y: number): number
	return x * ROW + y + YOFF
end

function util.x(k: number): number
	return k // ROW
end

function util.y(k: number): number
	return k - (k // ROW) * ROW - YOFF
end

function util.xy(k: number): (number, number)
	local x = k // ROW
	return x, k - x * ROW - YOFF
end

-- A step (dx, dy) as a number to add to a cell.
function util.step(dx: number, dy: number): number
	return dx * ROW + dy
end

-- A cell's four sides' neighbours as steps, NE SE SW NW (the iso diamond's edges: the bit order of
-- the walls' shapes), and the four cells two away in a straight line, N E S W, which meet a cell
-- only at a corner.
util.SIDES = { ROW - 1, ROW + 1, -ROW + 1, -ROW - 1 }
util.CORNERS = { -2, 2 * ROW, 2, -2 * ROW }
-- Each side step's (dx, dy).
util.SIDE_DX = { [ROW - 1] = 1, [ROW + 1] = 1, [-ROW + 1] = -1, [-ROW - 1] = -1 }
util.SIDE_DY = { [ROW - 1] = -1, [ROW + 1] = 1, [-ROW + 1] = 1, [-ROW - 1] = -1 }

local abs, max = math.abs, math.max

function util.manhattan(a: number, b: number): number
	local ax = a // ROW
	local bx = b // ROW
	return abs(ax - bx) + abs((a - ax * ROW) - (b - bx * ROW))
end

function util.chebyshev(a: number, b: number): number
	local ax = a // ROW
	local bx = b // ROW
	return max(abs(ax - bx), abs((a - ax * ROW) - (b - bx * ROW)))
end

-- A chamber's centre as a cell.
function util.centre(room): number
	return room.x * ROW + room.y + YOFF
end

-- A cell's centre in world units (cell * 23 + 11.5): where every placed thing stands (exact as a
-- float: every cell's).
function util.worldX(k: number): number
	return (k // ROW) * 23 + 11.5
end

function util.worldY(k: number): number
	return (k - (k // ROW) * ROW - YOFF) * 23 + 11.5
end

-- Every cell of `list` in `set` (a table keyed by cells).
function util.allIn(list, set): boolean
	for _, c in list do
		if not set[c] then
			return false
		end
	end
	return true
end

function util.find(list, v): number?
	for i, x in list do
		if x == v then
			return i
		end
	end
	return nil
end

-- The cells within r of c on both axes (a square, both checkerboard colours) into `set`.
function util.insertSquare(set, c: number, r: number)
	for dx = -r, r do
		local k = c + dx * ROW
		for dy = -r, r do
			set[k + dy] = true
		end
	end
end

-- C's a / b for whole numbers (towards zero).
function util.idiv(a: number, b: number): number
	if (a >= 0) == (b > 0) then
		return math.abs(a) // math.abs(b)
	end
	return -(math.abs(a) // math.abs(b))
end

-- C's a % b for whole numbers (the sign of a).
function util.irem(a: number, b: number): number
	return a - util.idiv(a, b) * b
end

-- An unsigned 32-bit value of a whole number (one a double holds exactly): wrapped.
function util.u32(v: number): number
	return v % U32
end

-- a * b + c in unsigned 32 bits (the seeds' hashes).
function util.hash(a: number, b: number, c: number): number
	return (gen.mul32(a, b) + c) % U32
end

return util
