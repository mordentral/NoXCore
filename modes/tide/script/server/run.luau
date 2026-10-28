-- Undead Tide's run as it goes on: the damage players take through the night, the guardian's fall
-- carrying the party on to the next stage, an elite's chest opened, and what a run's record holds.
-- The director's state machine (its lobby, arena and stages) still asks these where its C++ did.
local data = require("data")
local rules = require("rules")

local f32 = nox.f32
local float = rules.float

local FPS = 30 -- (frames a second)

-- A text line from the script's text table, its {fields} filled in one pass (fillText's).
local function fill(template: string, fields: { [string]: string }): string
    return (string.gsub(template, "{([%w_]+)}", function(name)
        return fields[name]
    end))
end

local run = {}

-- The damage players take, the run's own curve (the quest's multiplier, set each frame over the
-- quest's own): 15% at the start, the whole of it by dawn, the climb steepest late (gentle first
-- minutes; a network game's creatures hit hard), a fifth more each stage past the first; four
-- times at most.
local DAMAGE_START = float(0.15)
local DAMAGE_NIGHT = float(0.65)
local DAMAGE_LATE = float(0.2)
local DAMAGE_STAGE = float(0.2)
function run.stageDamage(seconds: number, winSeconds: number, stage: number): number
    local night = math.min(1, f32(seconds / math.max(60, winSeconds)))
    local damage = f32(f32(DAMAGE_START + f32(DAMAGE_NIGHT * night)) + f32(f32(DAMAGE_LATE * night) * night))
    damage = f32(damage + f32(DAMAGE_STAGE * math.max(0, stage - 1)))
    return math.min(4, damage)
end

-- The guardian slain (with dawn not winning): the next stage's load due `delay` seconds on (at
-- least one), told, the players in play (`units`) kept safe till the land changes and a moment
-- after. Returns the frame it's due.
function run.advance(frame: number, delay: number, stage: number, guardian: string, units: { number }): number
    delay = math.max(1, delay)
    nox.textToAll(fill(data.text.guardianFallsStage, { guardian = guardian, seconds = tostring(delay), next = tostring(stage + 1) }))
    for _, u in units do
        nox.enchant(u, "ENCHANT_INVULNERABLE", (delay + 3) * FPS, 1)
    end
    return frame + delay * FPS
end

-- An elite's chest opened at (x, y): a free upgrade (a level banked) to the player in play nearest
-- it then (`near`: each one's unit, in the director's order of its runners; the first of two as
-- near).
function run.chestOpened(x: number, y: number, near: { number })
    local opener = nil
    local best = 0
    for _, u in near do
        local o = nox.object(u)
        local dx = f32(o.x - x)
        local dy = f32(o.y - y)
        local d2 = f32(f32(dx * dx) + f32(dy * dy))
        if opener == nil or d2 < best then
            opener = u
            best = d2
        end
    end
    if opener then
        for _, name in owner.rewardChest(opener) do
            nox.sound(opener, 0x386)
            nox.textToAll(fill(data.text.chestOpened, { player = name }))
        end
    end
end

-- What a run's record holds: its outcome (2 the guardian slain, 1 dawn, 0 lost), its seconds
-- (every stage's), the stage it ended in, the night's length in minutes, the biome, the date, the
-- difficulty's name, and its party (each {name, class, multiclass, noxCore, level, kills}), the best
-- first: its level, then its kills, as they were among equals.
function run.record(won: boolean, guardianSlain: boolean, seconds: number, stage: number, winSeconds: number, biome: string,
    date: string, difficulty: number, party: { any })
    local sorted = {}
    for i, m in party do
        -- (An insertion: each after every one at least as good, so equals keep their order.)
        local at = i
        while at > 1 do
            local p = sorted[at - 1]
            local better = if m.level ~= p.level then m.level > p.level else m.kills > p.kills
            if not better then
                break
            end
            sorted[at] = p
            at -= 1
        end
        sorted[at] = m
    end
    return {
        outcome = if not won then 0 elseif guardianSlain then 2 else 1,
        seconds = seconds,
        stage = stage,
        minutes = winSeconds // 60,
        biome = biome,
        date = date,
        difficulty = (data.difficulties[difficulty + 1] or data.difficulties[1]).name,
        party = sorted,
    }
end

-- A run filed at `place` (from 0) on the board: the top ten told.
-- When the night's guardian comes, seconds into a stage: the host's own time, else at dawn (when
-- dawn doesn't win: the stage's last fight), else two minutes before dawn (three quarters of a
-- short night).
function run.guardianAt(guardian: number, dawnWins: number, dawn: number): number
    if guardian ~= 0 then
        return guardian
    end
    if dawnWins == 0 then
        return dawn
    end
    return if dawn >= 600 then dawn - 120 else dawn * 3 // 4
end

-- A player's unit made ready for the run (once a unit; the runner in hand): its book, abilities
-- and guides emptied (not revoked on its client: its bars are its .plr's), no extra life at a run's
-- start (one coming back its own again), its class's weapon to start (one coming back keeps its
-- loadout), and the first stage's word on how it plays.
local START = { [0] = 0, [1] = 1 } -- (a warrior's Swing, a wizard's Magic Missile; else Arrows)
function run.prepare(unit: number, class: number, stage: number)
    nox.clearBook(unit)
    local weapons = owner.kit()
    owner.setQuestLives(unit, if #weapons == 0 then 0 else owner.lives())
    if #weapons == 0 then
        owner.setWeapon(START[class] or 2, 1)
        weapons = owner.kit()
    end
    if stage == 1 then
        nox.tell(unit, fill(data.text.firstWeapon, { weapon = data.weapons[weapons[1].kind + 1].name }))
    end
end

-- A new stage: one out as the last ended back in it (its game over done with; one still lying dead
-- got up first, as its client's Continue: asked again the next frame). 1 when it's back.
function run.stageBack(unit: number, player: string): number
    -- (One that never answered its game over still has the window up: only its own buttons close
    -- it, and its Continue now does nothing but that.)
    if nox.gameOverAt(unit) ~= 0 then
        nox.tell(unit, data.text.backInFight)
    end
    nox.setGameOverAt(unit, 0)
    local o = nox.object(unit)
    if not o.alive then
        owner.respawn(unit)
        return 0
    end
    owner.backInRun(unit)
    if bit32.band(nox.status(unit), 1) ~= 0 then
        nox.backIntoPlay(unit)
    end
    nox.heal(unit, o.maxHealth)
    owner.announce(fill(data.text.risesAgain, { player = player }))
    return 1
end

function run.filed(place: number)
    if place >= 0 and place < 10 then
        nox.textToAll(fill(data.text.boardPlace, { place = tostring(place + 1) }))
    end
end

return run
