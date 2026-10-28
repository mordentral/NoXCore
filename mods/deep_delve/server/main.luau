--!strict
-- Deep Delve: the mode's hooks. Each hook hands its work to the module that owns it:
--
--   store.luau     character records: made, read, migrated, written (nox.store)
--   progress.luau  a character's progress through the acts (waypoints, deepest, cleared)
--   hero.luau      the players' characters in play: set up, read back, experience and levels
--   items.luau     item records and the game's items (modifiers, rarity, affixes)
--   loot.luau      what deaths drop, rarity rules, how loot shows on the ground
--   monsters.luau  packs, tiers, champions
--   boss.luau      the act boss: its fight, its drop, the act cleared
--   combat.luau    the damage tiers and affixes change (onDamage)
--   areas.luau     the act's chain of generated areas, the next built ahead, the ways between
--   waypoints.luau waypoints: found, kept, travelled
--   votes.luau     the party's vote to change area (the whole game moves)
--   portal.luau    the town portal (anyone's open portal takes the party back out, its owner gone
--                  or not)
--   party.luau     the party strip's numbers; the party's size for the scaling
--   scale.luau     party scaling: monsters' health, damage and experience, packs, loot
--   labels.luau    ground loot's labels on each player's UI
--   offers.luau    nox.choice offers and their answers
--   tree.luau      the passive tree's rules (points, nodes taken, respec, what it adds and gives)
--   book.luau      the spells (a warrior: the abilities) the tree gives, on the unit and its client
--   windows.luau   the bigger windows: which are open, the carried list, their beat
--   stash.luau     the stash at the town's chest
--   vendor.luau    the vendor at the town's anvil
--   treeview.luau  the passive tree's window
--   sheet.luau     the character sheet
--   town.luau      the town's stations
--   touch.luau     things worked by touching them
--   hud.luau       what each player's UI shows (ui/main.luau draws it)
--   data/*.luau    the tables it's all tuned by
--   ../gen/*.luau  the area generator (run by nox.generate in its own VM)
--
-- Alone or as a party: the town (its stash, vendor, healer and the passive tree's respec), act 1's
-- chain of areas to its boss, waypoints and the town portal, monsters by tier, loot by rarity
-- (named on the ground), a character that's kept with its passive tree, its spells from the tree,
-- and its stash. A party sees each other on the strip, votes to change area, shares its portals,
-- has a vendor stock each, splits each kill's experience and meets monsters scaled to its size.
--
-- The game is a plain game, not a quest; a dead player keeps everything it carries and wears
-- (keepThingsOnDeath), so a death costs nothing. The book is the mod's (bookIsTheMod): what the
-- passive tree grants is all a character can cast or use, and a spell is cast at its tree level.

local areas = require("areas")
local boss = require("boss")
local combat = require("combat")
local hero = require("hero")
local labels = require("labels")
local loot = require("loot")
local monsters = require("monsters")
local offers = require("offers")
local party = require("party")
local portal = require("portal")
local _sheet = require("sheet") -- (it registers the sheet's window)
local stash = require("stash")
local treeview = require("treeview")
local vendor = require("vendor")
local votes = require("votes")
local windows = require("windows")
local numbers = require("data/numbers")
local town = require("town")
local touch = require("touch")
local console = require("console")

local mode = {}

-- ---- The game

-- Before the game: the first area's map, generated now; a dead player keeps its things; the book
-- is the mod's (bookIsTheMod: a player starts with no spell, ability or field guide creature, may
-- use only what the tree grants, at the level granted, and the engine's own books and pedestals
-- give nothing).
function mode.configure(ctx: any)
	return { generate = areas.configure(ctx), keepThingsOnDeath = true, bookIsTheMod = true }
end

function mode.start()
	nox.every(numbers.timing.saveEvery, hero.saveAll, "every minute")
	party.start()
	labels.start()
end

function mode.stop()
	hero.saveAll("the game's end")
end

function mode.mapLoaded(name: string)
	areas.mapLoaded(name or nox.currentMap())
end

function mode.onGenerated(tag: string, ok: boolean, result: any)
	areas.generated(tag, ok, result)
end

-- ---- Players

function mode.onJoin(player: any)
	hero.join(player)
	party.changed()
end

-- A player left: its record written, its open portal kept for the others, no longer waited for in
-- a vote.
function mode.onLeave(player: any)
	portal.left(hero.byPeer(player.peer))
	hero.leave(player)
	offers.forget(player.peer)
	votes.left(player.peer)
	party.changed()
end

-- A player's client ready in the map (its join, and each map change once it has the new map):
-- its UI sent the strip, the vote and the ground's labels again.
function mode.playerReady(player: any)
	hero.clientReady(player)
	local h = hero.byPeer(player.peer)
	if h then
		party.resend(h)
		labels.resend(h)
		votes.resend(h)
	end
end

-- A player's death (its record written; it keeps its things).
function mode.onKill(victim: number, _killer: number?, _assist: number?)
	hero.died(victim)
end

-- The player's UI: its Town portal button, its windows (open and close; the stash's, the
-- vendor's and the tree's clicks), its options (the Character window's), its answer to a vote.
function mode.onUiMessage(player: any, value: any)
	if type(value) ~= "table" then
		return
	end
	local h = hero.byPeer(player.peer)
	if not h then
		return
	end
	local kind = value.kind
	if kind == "portal" then
		portal.open(h)
	elseif kind == "open" and type(value.window) == "string" then
		if value.fresh then
			h.treeSent = nil -- (a UI without the tree's nodes: sent again)
		end
		windows.open(h, value.window)
	elseif kind == "close" and type(value.window) == "string" then
		windows.close(h, value.window)
	elseif kind == "stash" then
		stash.message(h, value)
	elseif kind == "vendor" then
		vendor.message(h, value)
	elseif kind == "tree" then
		treeview.message(h, value)
	elseif kind == "option" then
		if type(value.name) == "string" and type(value.value) == "string" then
			hero.setOption(h, value.name, value.value)
		end
	elseif kind == "vote" then
		if value.answer == "yes" or value.answer == "no" then
			votes.cast(h, value.answer == "yes")
		elseif value.answer == "cancel" then
			votes.cancel(h)
		end
	end
end

-- Saying "portal" opens a town portal too; while a vote is under way, "yes" or "no" answers it.
function mode.onChat(player: any, text: string): boolean?
	local said = string.lower((string.gsub(text, "^%s*(.-)%s*$", "%1")))
	if said == "portal" then
		portal.open(hero.byPeer(player.peer))
		return true
	end
	if (said == "yes" or said == "no") and votes.active() then
		local h = hero.byPeer(player.peer)
		if h and votes.mark(h.peer) == "?" then
			votes.cast(h, said == "yes")
			return true
		end
	end
	return nil
end

-- An answer to an offer (the town's waypoints, the open portals).
function mode.onChoice(player: any, index: number)
	offers.chosen(player.peer, index)
end

-- A unit comes into play (its first spawn, after a death, in a new map): set up again from its
-- record; in town, or where a map change brought it (the area's start, or its waypoint).
function mode.respawnPoint(unit: number)
	local at = hero.respawned(unit)
	if at then
		return at.x, at.y
	end
	return town.respawnPoint()
end

-- ---- The world's events

function mode.collide(thing: number, other: number?)
	touch.collide(thing, other)
end

-- A monster the mod made died: experience to the heroes, its loot on the ground (the act boss:
-- its own drop, and the act cleared); both by the party's size (scale.luau).
function mode.onObjectKilled(e: any)
	local m = monsters.killed(e.id)
	if not m then
		return
	end
	hero.award(monsters.experienceOf(m), e.by)
	if m.boss then
		boss.killed(e.id, e.x, e.y)
	else
		loot.drop(e.x, e.y, m.level, loot.rollsFor(1 + m.drops), m.drops * 0.25)
	end
end

function mode.onDamage(target: number, source: number?, weapon: number?, amount: number, kind: number): number?
	return combat.onDamage(target, source, weapon, amount, kind)
end

function mode.onPickup(taker: number, item: number, _x: number, _y: number)
	loot.pickedUp(item)
	hero.pickedUp(taker, item)
end

function mode.onEquip(unit: number, _item: number, _worn: boolean)
	hero.equipChanged(unit)
end

-- ---- The host's console ("dd help")

function mode.console(line: string)
	return console.run(line)
end

return mode
