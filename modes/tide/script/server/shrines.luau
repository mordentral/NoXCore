-- Undead Tide's revive shrines: one rises a while after someone falls for good, near a player
-- still standing; a player in play touching it brings every fallen one who's watching back beside
-- it. The director keeps the shrine's clock, finds the touch (within data.luau's reach) and asks
-- rise and bringBack as they happen.
local data = require("data")
local util = require("util")

local f32 = nox.f32

local shrines = {}

-- A shrine risen near one of the players in play (`units`, drawn on the director's stream), out of
-- the tide's thick: 260-400 away, on open ground (24 tries). Its id, or nil when none rose.
function shrines.rise(units: { number }): number?
    local near = nox.object(units[owner.roll() % #units + 1])
    for _ = 1, 24 do
        local a = f32((owner.roll() % 3600) * util.DEGREE_TENTH)
        local d = 260 + owner.roll() % 140
        local x = f32(near.x + f32(nox.cosf(a) * d))
        local y = f32(near.y + f32(nox.sinf(a) * d))
        if util.openGround(x, y) then
            local s = nox.create(data.shrine.type, x, y)
            if not s then
                return nil
            end
            nox.setCollide(s, "DefaultCollide")
            nox.sound(s, 0x3ED)
            nox.effect(0x82, x, y)
            nox.textToAll(data.text.shrineRises)
            return s
        end
    end
    return nil
end

-- The shrine touched: each fallen one ready to come back (`fallen`: {unit, name} each, its game
-- over answered, watching, not lying dead; the director's order) back in play beside it, no extra
-- life, whole and safe a moment; then the shrine spent (when anyone came back). Returns how many.
function shrines.bringBack(shrine: number, fallen: { any }): number
    local back = 0
    for _, f in fallen do
        owner.revive(f.unit) -- (out of the run no more, no lives)
        nox.backIntoPlay(f.unit)
        local x, y = nox.nearSoulGate(shrine)
        nox.setPosition(f.unit, x, y)
        nox.heal(f.unit, nox.object(f.unit).maxHealth)
        nox.enchant(f.unit, "ENCHANT_INVULNERABLE", 3 * 30, 1)
        nox.effect(0x81, x, y)
        nox.textToAll(util.fill(data.text.shrineBrings, { player = f.name }))
        back += 1
    end
    if back > 0 then
        nox.sound(shrine, 0x386)
        nox.delete(shrine)
    end
    return back
end

return shrines
