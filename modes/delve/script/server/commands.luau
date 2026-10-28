-- Delving Mode's chat commands: what a player's line asks for, before it's said aloud, and the
-- replies to "?difficulty" and "?blessings". The director does what it says (tells the records,
-- sends the replies, takes an offer's number).
local data = require("data")
local words = require("words")

-- What a line asks for (the director's numbers for them).
local NONE, RECORDS, DIFFICULTY, BLESSINGS, NUMBER = 0, 1, 2, 3, 4

local commands = {}

-- The line `text` (UTF-8, each of the client's characters its own: none of them ASCII but the
-- ASCII ones): what it asks for, and its argument (a difficulty, from 0, -1 none asked or none
-- such: told it; an offer's number).
-- - "?record", "?records" or "?deepest" (any case): the deepest delves.
-- - "?difficulty", or "?difficulty <name>" (a name's start, any case; the last that fits).
-- - "?blessing" or "?blessings": the player's own.
-- - digits alone (a long number refused): an offer's number.
function commands.chat(text: string): (number, number)
    local lower = string.lower(text)
    if lower == "?record" or lower == "?records" or lower == "?deepest" then
        return RECORDS, 0
    end
    if lower == "?difficulty" or string.sub(lower, 1, 12) == "?difficulty " then
        -- What follows "?difficulty " (12 characters), less the spaces before it.
        local asked = string.sub(lower, 13)
        local at = 1
        while string.byte(asked, at) == 32 do
            at += 1
        end
        asked = string.sub(asked, at)
        local to = -1
        if asked ~= "" then
            for i, d in data.difficulties do
                local name = string.lower(d.name)
                if string.sub(name, 1, #asked) == asked then
                    to = i - 1
                end
            end
        end
        return DIFFICULTY, to
    end
    if lower == "?blessing" or lower == "?blessings" then
        return BLESSINGS, 0
    end
    -- A number of the offer: digits only; a long number is refused.
    local n = 0
    for i = 1, #text do
        local c = string.byte(text, i)
        if c < 48 or c > 57 or n > 1000 then
            return NONE, 0
        end
        n = n * 10 + (c - 48)
    end
    return NUMBER, n
end

-- Who a reply goes to: the one who asked, or everyone.
local ASKER, EVERYONE = 0, 1

local function difficultyName(d: number): string
    -- (One past the four: the first's name, as ever.)
    return (data.difficulties[d + 1] or data.difficulties[1]).name
end
local function difficultyAbout(d: number): string
    return data.difficulties[math.min(d + 1, #data.difficulties)].about
end

-- "?difficulty" from `player` (its name), `to` the difficulty asked for (from 0; -1 none asked, or
-- none such): told it, or set (in the lobby: the owner's) and told everyone. Who the reply goes
-- to, and the reply.
function commands.difficulty(player: string, to: number): (number, string)
    local text = data.text
    local now = owner.difficulty()
    if to < 0 then
        return ASKER, words.fill(text.difficultyIs, { difficulty = difficultyName(now), about = difficultyAbout(now) })
    end
    if not owner.setDifficulty(to) then
        return ASKER, words.fill(text.difficultyLocked, { difficulty = difficultyName(now) })
    end
    return EVERYONE, words.fill(text.setsDifficulty, { player = player, difficulty = difficultyName(to), about = difficultyAbout(to) })
end

-- "?blessings": the reply, from the player's blessings (`held`: how many of each, by their
-- numbers' order, from 1).
function commands.blessings(held: { number }): string
    local text = data.text
    local told = ""
    for i, count in held do
        if count ~= 0 then
            told ..= (if told == "" then "" else text.listComma)
                .. words.fill(if count > 1 then text.blessingHeldMany else text.blessingHeld, { name = data.blessings[i].name, count = tostring(count) })
        end
    end
    return if told == "" then text.blessingsNone else words.fill(text.blessingsYours, { list = told })
end

return commands
