-- Undead Tide's small helpers its rules share: a text line filled, a C++ float literal's value,
-- std::lround, a creature's name as read, a point the tide may stand at, a tenth of a degree.
local f32 = nox.f32
local floor = math.floor

local util = {}

-- A text line from the script's text table, its {fields} filled in one pass (fillText's).
function util.fill(template: string, fields: { [string]: string }): string
    return (string.gsub(template, "{([%w_]+)}", function(name)
        return fields[name]
    end))
end

-- The float a C++ float literal of `x` is: to nearest, a tie to the even one. Exact steps only
-- (frexp and ldexp scale by powers of two; the 24 bits' integer and its rest are exact), so the
-- server's rounding can't reach it. For the normal floats the tables hold.
function util.float(x: number): number
    if x == 0 then
        return x
    end
    local sign = 1
    if x < 0 then
        sign = -1
        x = -x
    end
    local m, e = math.frexp(x) -- (x = m * 2 to the e, m in [0.5, 1))
    local scaled = m * 16777216 -- (2 to the 24: the float's 24 bits above the point)
    local kept = floor(scaled)
    local rest = scaled - kept
    if rest > 0.5 or (rest == 0.5 and kept % 2 == 1) then
        kept += 1
    end
    return sign * math.ldexp(kept, e - 24)
end

-- std::lround of a float: to nearest, a half away from zero (exact: a float plus a half is exact
-- in a double).
function util.lround(v: number): number
    if v < 0 then
        return -floor(-v + 0.5)
    end
    return floor(v + 0.5)
end

-- A creature type's name as players read it: "OgreBrute" -> "Ogre Brute".
function util.spacedName(t: string): string
    return (string.gsub(t, "(%l)(%u)", "%1 %2"))
end

-- A point the tide (or a summoned creature) may come in at: floor, no wall in its cell or about
-- it. (Wall cells are 23 units square: the 3 x 3 cells about the point must hold no wall.)
local CELL = 23
local MAP = 5888 -- (256 cells: past it no floor)
function util.openGround(x: number, y: number): boolean
    if x < 46 or y < 46 or x >= MAP or y >= MAP or nox.floorAt(x, y) < 0 then
        return false
    end
    local cx = floor(f32(x / CELL))
    local cy = floor(f32(y / CELL))
    for dx = -1, 1 do
        for dy = -1, 1 do
            if nox.wallAt(cx + dx, cy + dy) then
                return false
            end
        end
    end
    return true
end

-- pi_v<float> / 1800.0f as the compiler folds it (a tenth of a degree), and 2 pi_v<float>.
util.DEGREE_TENTH = util.float(util.float(math.pi) / 1800)
util.TWO_PI = 2 * util.float(math.pi)

return util
