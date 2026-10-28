-- Delving Mode's server script: the run's rules, called by the native director, which owns this
-- script. Each function is a rule the director asks (delve.cpp's DelveRules, each looked up at
-- set-up).
local rules = require("rules")
local acts = require("acts")
local levels = require("levels")
local draws = require("draws")
local commands = require("commands")
local run = require("run")
local lobby = require("lobby")
local told = require("told")
local level = require("level")

local mode = {}

mode.toughens = rules.toughens
mode.health = rules.health
mode.bossTraits = rules.bossTraits
mode.bossHealth = rules.bossHealth
mode.regenerate = rules.regenerate
mode.champion = rules.champion
mode.record = rules.record

-- An act drawn (its number, its seed, the biomes it mustn't be, by name), and a guardian's fork.
function mode.roll(number: number, seed: number, ...: string)
    local notBiomes = {}
    for _, b in { ... } do
        notBiomes[b] = true
    end
    return acts.roll(number, seed, notBiomes)
end
mode.fork = acts.fork

-- A level: a fight's base (its depth, its seed), the level an act's way reaches, and a level's
-- objective or bot boss set (each the level, changed); the lobby's map.
mode.base = levels.base
mode.level = levels.level
mode.lobbyParams = levels.lobby
function mode.setObjective(level, objective: number)
    levels.setObjective(level, objective)
    return level
end
function mode.setBotBoss(level, class: number)
    levels.setBotBoss(level, class)
    return level
end

-- The draws as a level's played (draws.luau): its beginning's, a boss's traits, the Hunt's
-- quarry, a champion guardian's name. (An altar's three: level.atAltar's own.)
mode.more = draws.more
mode.beginLevel = draws.beginLevel
mode.carriers = draws.carriers
mode.keepers = draws.keepers
mode.barrels = draws.barrels
mode.elites = draws.elites
mode.bossDraw = draws.bossTraits
mode.hunt = draws.hunt
mode.championDraw = draws.champion

-- What a player's chat line asks for, and the replies to "?difficulty" and "?blessings"
-- (commands.luau).
mode.chat = commands.chat
mode.difficultyAsked = commands.difficulty
mode.blessingsAsked = commands.blessings

-- The lobby's hooks (lobby.luau), each as its watcher fires: a player come in, near a sign, come
-- up to the board, into the portal; and the records told (the board's, and "?record").
mode.lobbyArrive = lobby.arrive
mode.lobbySign = lobby.sign
mode.lobbyBoard = lobby.board
mode.lobbyPortal = lobby.portal
mode.records = lobby.records

-- The words about the run (told.luau): an act as a guardian's gate tells it, the bonus objective.
mode.describe = told.describe
mode.objectiveText = told.objectiveText

-- The level's hooks (level.luau): its first frame; and each as its watcher fires: a gate's sign
-- near, a gate taken, the guardian fallen, the level told, the party fallen.
mode.levelBegun = level.begun
mode.gateSign = level.gateSign
mode.gateTaken = level.gateTaken
mode.guardianFell = level.guardianFell
mode.levelTold = level.told
mode.partyFallen = level.fallen
mode.makeElite = level.makeElite
mode.heat = level.heat
mode.objectiveDone = level.objectiveDone
mode.purgeHalf = level.purgeHalf
mode.tooSlow = level.tooSlow
mode.atCampfire = level.atCampfire
mode.campfireUpgrade = level.campfireUpgrade
mode.atAltar = level.atAltar
mode.blessed = level.blessed
mode.shrine = level.shrine
mode.blessingKept = level.blessingKept
mode.unbless = level.unbless
mode.sealBroken = level.sealBroken
mode.barrelsBroken = level.barrels
mode.clearBar = level.clearBar
mode.sendRevokes = level.sendRevokes
mode.wipe = level.wipe

-- The run from level to level (run.luau): a run's seed, the way in, the plan, the levels built
-- ahead, Swift's gold.
mode.runSeed = run.seed
mode.choiceInto = run.choiceInto
mode.ahead = run.ahead
mode.plan = run.plan
mode.swift = run.swift

return mode
