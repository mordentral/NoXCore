-- Undead Tide's chat commands: "?difficulty [name]" (told, or set in the lobby: anyone's),
-- "?tide" / "?me" (the player's level, experience and kit, told only to them) and a single digit
-- 1-9 (a pick from an open level-up offer). A line taken isn't relayed.
local data = require("data")
local rules = require("rules")
local util = require("util")

local fill = util.fill

local chat = {}

-- The difficulties' names lower-cased, for "?difficulty vet" (the first whose name starts so).
local lowerNames = {}
for i, d in data.difficulties do
    lowerNames[i] = string.lower(d.name)
end

local function difficultyName(i: number): string
    return (data.difficulties[i + 1] or data.difficulties[1]).name
end
local function difficultyAbout(i: number): string
    return (data.difficulties[i + 1] or data.difficulties[#data.difficulties]).about
end

-- A line said in chat by the player at `peer` (`inGame`: one of the game's players; `name` its
-- name; `unit` its unit, nil none). `lower`: the line ASCII lower-cased, anything else '?'; `ascii`:
-- the line all ASCII; `length`: its length in characters. `running`: a run going and the player's
-- runner in hand. Whether it was taken.
function chat.onChat(peer: number, inGame: boolean, name: string, unit: number?, lower: string, ascii: boolean, length: number,
    running: boolean): boolean
    if (lower == "?difficulty" or string.sub(lower, 1, 12) == "?difficulty ") and inGame then
        local word = if #lower > 12 then string.sub(lower, 13) else ""
        local to = nil
        for i, n in lowerNames do
            if word ~= "" and string.sub(n, 1, #word) == word then
                to = i - 1
                break
            end
        end
        local current = owner.difficulty()
        local now = difficultyName(current)
        local t = data.text
        if to == nil then
            if unit then
                nox.tell(unit, fill(t.difficultyIs, { difficulty = now, about = difficultyAbout(current) }))
            end
        elseif not owner.setDifficulty(to) then
            if unit then
                nox.tell(unit, fill(t.difficultyLocked, { difficulty = now }))
            end
        else
            nox.textToAll(fill(t.setsDifficulty, { player = name, difficulty = difficultyName(to), about = difficultyAbout(to) }))
            owner.refreshAsks()
        end
        return true
    end
    -- The rest only while a run is going: the player's own runner answers.
    if not running then
        return false
    end
    if ascii and (lower == "?tide" or lower == "?me") then
        if unit then
            local t = data.text
            local level, xp, kills = owner.me()
            local weapons, passives = owner.kit()
            local kit = ""
            for _, w in weapons do
                kit ..= (if kit == "" then "" else t.listSeparator) .. fill(t.kitItem, { name = data.weapons[w.kind + 1].name, level = tostring(w.level) })
            end
            for k, v in passives do
                if v ~= 0 then
                    kit ..= t.listSeparator .. fill(t.kitItem, { name = data.passives[k].name, level = tostring(v) })
                end
            end
            nox.tell(unit, fill(t.me, { level = tostring(level), xp = tostring(xp), need = tostring(rules.xpFor(level)), kills = tostring(kills), kit = kit }))
        end
        return true
    end
    -- Otherwise a single digit 1-9 picks from an open level-up offer.
    local open = owner.offerSize()
    if open == 0 or length ~= 1 or not string.match(lower, "^[1-9]$") then
        return false
    end
    local n = tonumber(lower) - 1
    if n >= open then
        return false
    end
    return owner.pickOffer(peer, n)
end

return chat
