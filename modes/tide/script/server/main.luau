-- Undead Tide's server script: the run's rules, called by the native director, which owns this
-- script. Each function is a rule the director asks (tide.cpp's kRuleNames, and the maps'
-- parameters).
local rules = require("rules")
local pickups = require("pickups")
local spawns = require("spawns")
local levels = require("levels")
local run = require("run")
local shrines = require("shrines")
local chat = require("chat")
local weapons = require("weapons")
local fall = require("fall")
local lobby = require("lobby")
local told = require("told")
local arena = require("arena")
local params = require("params")

local mode = {}

mode.xpFor = rules.xpFor
mode.cooldown = rules.cooldown
mode.setOptions = spawns.setOptions
mode.beginStage = spawns.beginStage
mode.spawn = spawns.spawn
mode.spawnBoss = spawns.spawnBoss
mode.gainXp = levels.gainXp
mode.offerPicks = levels.offerPicks
mode.pick = levels.pick
mode.stageDamage = run.stageDamage
mode.advance = run.advance
mode.chestOpened = run.chestOpened
mode.record = run.record
mode.filed = run.filed
mode.riseShrine = shrines.rise
mode.bringBack = shrines.bringBack
mode.onChat = chat.onChat
-- (The director's native blocks set up once: the horde's rules, the weapons' ways.)
function mode.setUp()
    spawns.setUp()
    weapons.setUp()
end
mode.shoot = weapons.shoot
mode.shape = weapons.shape
mode.forget = weapons.forget
mode.fell = fall.fell
mode.comeBack = fall.comeBack
mode.sign = lobby.sign
mode.board = lobby.board
mode.lobbyLoaded = lobby.loaded
mode.guardianAt = run.guardianAt
mode.prepare = run.prepare
mode.stageBack = run.stageBack
mode.coming = told.coming
mode.setsDifficulty = told.setsDifficulty
mode.rises = told.rises
mode.minute = told.minute
mode.taken = told.taken
mode.pickedForYou = told.pickedForYou
mode.ended = told.ended
mode.keptSafe = told.keptSafe
mode.recordLine = told.record
mode.gemTaken = pickups.gemTaken
mode.pickupTaken = pickups.taken
mode.gemReach = pickups.gemReach
mode.passiveHealth = arena.passiveHealth
mode.charm = arena.charm
mode.bodyDue = arena.bodyDue
mode.runOver = arena.runOver
mode.arenaParams = params.arena
mode.lobbyParams = params.lobby

return mode
