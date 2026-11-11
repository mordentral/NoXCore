--!strict
-- Deep Delve: the mode's hooks. Each hook hands its work to the module that owns it:
--
--   store.luau     character records: made, read, migrated, written (nox.store)
--   roster.luau    server characters (the mode option characters=server, the bot server's):
--                  character select, the PC's account, link codes (accounts.luau), who a PC is
--                  (identity.luau)
--   progress.luau  a character's progress through the acts (waypoints, deepest, cleared)
--   hero.luau      the players' characters in play: set up, read back, experience and levels
--   regen.luau     passive health and mana regeneration (a class's base and its tree's), a timer
--                  per hero
--   items.luau     item records and the game's items (modifiers, rarity, affixes)
--   loot.luau      what deaths drop, rarity rules, how loot shows on the ground
--   breakables.luau an area's barrels, coffins and the like: each broken may give a small find
--   monsters.luau  packs, tiers, champions
--   boss.luau      the act boss: its fight, its drop, the act cleared
--   gate.luau      the gate in a lair to the next act: opened by its boss's death, kept for a
--                  character who has killed it
--   combat.luau    the damage tiers and affixes change, no damage within the party, no splash
--                  from a hero's own Fireball (onDamage); reflection's turn (onReflect)
--   affixes.luau   Deep Delve's affixes: which gear kind each rolls on, and the six from the
--                  game's modifiers (frost, dread, grip, reflect, ward, resilience) at work
--   uniques.luau   unique items and item sets: their records, drops, worn bonuses, tooltip lines
--   uniquepowers.luau the uniques' and set bonuses' powers at work (hits, kills, casts, prices,
--                  a timer per hero wearing a timed one)
--   warrior.luau   the warrior's passive skills from the tree at work (its blows, the hits it
--                  takes, Second Wind, the Rage bar; a timer per hero with a timed one)
--   actives.luau   the warrior's active abilities (Leap Slam, Whirlwind): granted by the tree,
--                  their buttons on the strip over the quickbar (onModAbility, onLand)
--   upgrades.luau  the stock abilities' upgrades from the tree (Trample, Blood Rush, Rallying
--                  Cry, Terror, Reel and Rend, Chain): onAbility's news, a kill for Blood Rush
--   casts.luau     a hero's casts: its missiles larger at the tree levels that say so
--                  (onSpellCast; Fireball 4 and 5); the cast-cost keystones' prices (castCost)
--   summons.luau   the creatures the heroes summon: found as they come, watched
--   team.luau      the party's one team: every hero and its creatures on it (the game's own "who
--                  is an enemy" then spares the party: monsters' and creatures' foes, homing
--                  missiles, pixies)
--   areas.luau     the acts' chains of generated areas, the next built ahead, the ways between,
--                  each outdoor area's weather
--   waypoints.luau waypoints: found, kept, travelled (the town's waypoints window)
--   expeditions.luau the endgame's runs from the town's map table (a biome at a rank, its
--                  champion, its reward); atlas.luau keeps what each character has cleared
--   votes.luau     the party's vote to change area (the whole game moves)
--   portal.luau    the town portal (anyone's open portal takes the party back out, its owner gone
--                  or not)
--   decals.luau    the pads' rings on the floor (waypoints cyan, portals violet; nox.decal)
--   secrets.luau   secret rooms (a secret wall and a lever, or a locked door and its key guard)
--                  and side rooms' chests, some trapped: their loot, their traps, their guards
--   party.luau     the party strip's numbers; the party's size for the scaling
--   scale.luau     party scaling: monsters' health, damage and experience, packs, loot
--   labels.luau    ground loot's labels and named monsters' name plates on each player's UI
--   offers.luau    nox.choice offers and their answers (the open portals)
--   tree.luau     the passive tree's rules (points, nodes taken, respec, what it adds and gives)
--   book.luau      the spells (a warrior: the abilities) the tree gives, on the unit and its client
--   windows.luau   the bigger windows: which are open, the carried list, their beat
--   tips.luau      items' tooltip lines (the stash's, the vendor's, the carried list's); the held
--                  items' by net code for a UI that tips the game's inventory
--   stash.luau     the stash at the town's chest
--   vendor.luau    the vendor at the town's anvil
--   treeview.luau  the passive tree's window
--   sheet.luau     the character sheet (one's own, or a party member's from the strip)
--   town.luau     the town's stations
--   touch.luau     things worked by touching them
--   hud.luau       what each player's UI shows (ui/main.luau draws it)
--   data/*.luau    the tables it's all tuned by
--   ../gen/*.luau  the area generator (run by nox.generate in its own VM)
--
-- Alone or as a party: the town (its stash, vendor, healer and the passive tree's respec), act 1's
-- chain of areas to its boss and the gate it opens to act 2's (outdoors, with weather),
-- waypoints and the town portal, monsters by tier, loot by rarity
-- (named on the ground), a character that's kept with its passive tree, its spells from the tree,
-- and its stash. A party sees each other on the strip (and each other's sheets), votes to change
-- area, shares its portals, has a vendor stock each, splits each kill's experience, meets monsters
-- scaled to its size, does no damage to itself or its creatures, and is one team (so the game's
-- monsters, creatures and homing spells never take a member for a foe).
--
-- The game is a plain game, not a quest; a dead player keeps everything it carries and wears
-- (keepThingsOnDeath), so a death costs nothing. The book is the mod's (bookIsTheMod): what the
-- passive tree grants is all a character can cast or use, and a spell is cast at its tree level.

local areas = require("areas")
local affixes = require("affixes")
local boss = require("boss")
local breakables = require("breakables")
local casts = require("casts")
local combat = require("combat")
local expeditions = require("expeditions")
local gate = require("gate")
local hero = require("hero")
local items = require("items")
local labels = require("labels")
local loot = require("loot")
local monsters = require("monsters")
local offers = require("offers")
local party = require("party")
local portal = require("portal")
local uniquePowers = require("uniquepowers")
local warrior = require("warrior")
local actives = require("actives")
local upgrades = require("upgrades")
local roster = require("roster")
local regen = require("regen")
local secrets = require("secrets")
local sheet = require("sheet")
local stash = require("stash")
local summons = require("summons")
local team = require("team")
local tips = require("tips")
local treeview = require("treeview")
local vendor = require("vendor")
local votes = require("votes")
local waypoints = require("waypoints")
local windows = require("windows")
local numbers = require("data/numbers")
local town = require("town")
local touch = require("touch")
local console = require("console")

local mode = {}

-- ---- The game

-- Before the game: the first area's map, generated now; monsters that see all about them; a dead
-- player keeps its things; the book
-- is the mod's (bookIsTheMod: a player starts with no spell, ability or field guide creature, may
-- use only what the tree grants, at the level granted, and the engine's own books and pedestals
-- give nothing).
function mode.configure(ctx: any)
	-- (a server that keeps the characters, the bot server's pool: listed as the game, not the mod's
	-- longer name, which a listing cuts at 15 letters)
	local listed = if nox.option("characters") == "server" then "Deep Delve" else nil
	-- (monstersAlert: the monsters look for foes all about them, not only ahead, as Delving Mode's
	-- do; without it one facing away stood still until it was hit)
	return { generate = areas.configure(ctx), keepThingsOnDeath = true, bookIsTheMod = true, monstersAlert = true, serverName = listed }
end

function mode.start()
	roster.active()
	team.start()
	nox.every(numbers.timing.saveEvery, hero.saveAll, "every minute")
	party.start()
	labels.start()
end

function mode.stop()
	hero.saveAll("the game's end")
end

function mode.mapLoaded(name: string)
	uniquePowers.reset()
	warrior.reset()
	actives.reset()
	upgrades.reset()
	roster.mapLoaded()
	areas.mapLoaded(name or nox.currentMap())
end

function mode.onGenerated(tag: string, ok: boolean, result: any)
	areas.generated(tag, ok, result)
end

-- ---- Players

-- A hero's client ready in the map: its UI sent the strip, the vote, the ground's labels and the
-- name plates again.
local function heroReady(player: any)
	hero.clientReady(player)
	local h = hero.byPeer(player.peer)
	if h then
		party.resend(h)
		labels.resend(h)
		votes.resend(h)
	end
end

-- A player's character into play (`chosen`: a server character from character select); in a
-- lair, the gate to the next act opened if its character has cleared the act.
local function play(player: any, chosen: any?)
	hero.join(player, chosen)
	gate.joined(hero.byPeer(player.peer))
	party.changed()
	if chosen and chosen.ready then
		-- (its client had the map already)
		heroReady(player)
	end
end
roster.onChosen(play)

-- A player joins (a rejoin too): onto the party's team at once, then its character (on a server
-- that keeps the characters, once it's chosen one at character select).
function mode.onJoin(player: any)
	team.join(player.unit)
	if not roster.joined(player) then
		play(player, nil)
	end
	roster.checkLock()
end

-- A player left: its record written, its open portal kept for the others, no longer waited for in
-- a vote, its sheet closed on the UIs showing it. (A server's game: open again to newcomers below
-- its most; left empty, back to the first area a moment on.)
function mode.onLeave(player: any)
	-- (a locked room's key it carries dropped at its feet, before its things go with it)
	secrets.left(player.unit)
	portal.left(hero.byPeer(player.peer))
	hero.leave(player)
	roster.left(player.peer)
	offers.forget(player.peer)
	votes.left(player.peer)
	sheet.left(player.peer)
	party.changed()
	if roster.active() then
		-- (a frame on: the one leaving gone from the list)
		nox.after(1, function()
			roster.checkLock()
		end)
		nox.after(numbers.timing.homeAfter, function()
			if #nox.players() == 0 then
				areas.home()
			end
		end)
	end
end

-- A player's client ready in the map (its join, and each map change once it has the new map):
-- character select shown again to one choosing; a hero's UI sent its strip and the rest again.
function mode.playerReady(player: any)
	roster.ready(player)
	heroReady(player)
end

-- A player's death (its record written; it keeps its things).
function mode.onKill(victim: number, _killer: number?, _assist: number?)
	hero.died(victim)
end

-- An item picked up (by the game's pickup, or a label's click): out of the ground's index, its
-- label cleared on every UI at once; a magic or better find told to its finder, a unique or a set
-- piece off the mod's ground to everyone.
local function pickedUp(taker: number, item: number)
	local fromGround = loot.pickedUp(item)
	if fromGround then
		labels.changed()
	end
	hero.pickedUp(taker, item, fromGround)
	tips.changed(taker)
end

-- A click on a ground item's label (its net code): picked up by the game's own pickup (gold into
-- the purse, a potion drunk, the game's refusals with their lines; onPickup hears it) when the hero
-- stands within numbers.pick.reach of it (the game's own click-pickup reach), else "too far" told
-- to that player (now and then). An item gone, held already or not the mod's ground loot is let
-- be; so is a dead hero's click.
local function pick(h: any, code: any)
	local id, g = loot.byCode(tonumber(code) or -1)
	if not id or not g or not nox.living(h.unit) then
		return
	end
	local o = nox.object(id)
	if not o or o.holder ~= 0 then
		return
	end
	local x, y = nox.position(h.unit)
	local dx, dy = o.x - x, o.y - y
	local p = numbers.pick
	if dx * dx + dy * dy > p.reach * p.reach then
		local now = nox.frame()
		if not h.farAt or now - h.farAt >= p.toldEvery then
			h.farAt = now
			nox.tell(h.unit, "Too far.")
		end
		return
	end
	nox.pickUp(h.unit, id)
end

-- The player's UI: its Town portal button, its windows (open and close; the stash's, the
-- vendor's, the tree's and the waypoints' clicks; the sheet, its own or a party member's), its
-- options (the Character window's), its answer to a vote.
function mode.onUiMessage(player: any, value: any)
	if type(value) ~= "table" then
		return
	end
	-- (character select's: choosing, a link code; from the Character window too)
	if roster.message(player, value) then
		return
	end
	local h = hero.byPeer(player.peer)
	if not h then
		return
	end
	local kind = value.kind
	if kind == "portal" then
		portal.open(h)
	elseif kind == "open" and value.window == "sheet" then
		-- (`peer`: a party member's, from its card on the strip; none: its own)
		sheet.show(h, tonumber(value.peer))
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
	elseif kind == "waypoints" then
		waypoints.message(h, value)
	elseif kind == "maps" then
		expeditions.message(h, value)
	elseif kind == "pick" then
		pick(h, value.code)
	elseif kind == "tips" then
		-- (a UI that shows its own tooltips over the game's inventory: the held items' lines)
		tips.want(h, value.on == true)
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
	-- ("link" and "link CODE" on a server that keeps the characters: never relayed)
	if roster.chat(player, text) then
		return true
	end
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

-- An answer to an offer (the open portals).
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
-- its own drop, and the act cleared); both by the party's size (scale.luau). A zombie gives them
-- at its first death alone (monsters.killed: nil after).
function mode.onObjectKilled(e: any)
	-- (a barrel or the like of the area's broken: what its kind gives; breakables.luau)
	if breakables.killed(e) then
		return
	end
	summons.killed(e.id)
	local m = monsters.killed(e.id)
	-- (a locked secret room's key guard: its key dropped)
	secrets.killed(e.id, e.x, e.y, m and m.name)
	-- (a summoner's minion: no experience, no loot, no kill on the record, none of the killer's
	-- worn powers on a kill; powers.luau)
	if not m or m.minion then
		return
	end
	hero.award(monsters.experienceOf(m), e.by)
	-- (Blood Rush: a kill soon after the killer's charge readies it; upgrades.luau)
	upgrades.killed(e)
	-- (the killer's worn powers: a burst, a heal, a frenzy; uniquepowers.luau)
	uniquePowers.killed(m, e)
	if m.boss then
		boss.killed(e.id, e.x, e.y)
	else
		loot.drop(e.x, e.y, m.level, loot.rollsFor(1 + m.drops), m.drops * 0.25)
		-- (a champion's own rolls for a unique and a set piece)
		if m.tier == "champion" then
			loot.dropSpecial(e.x, e.y, m.level, "champion")
		end
		-- (an expedition's champion: the run cleared, its reward)
		expeditions.killed(m, e.x, e.y)
	end
end

-- A watched thing gone from the world: a zombie of the mod's (it rises again, so it stays the mod's
-- while it lies dead) forgotten as its body goes, burnt away or deleted (monsters.luau).
function mode.onDestroyed(e: any)
	monsters.gone(e.id, e.type)
end

function mode.onDamage(target: number, source: number?, weapon: number?, amount: number, kind: number, cause: string): ...number?
	return combat.onDamage(target, source, weapon, amount, kind, cause)
end

-- A missile about to hit a watched thing: a hero's reflection may turn a monster's (combat.luau).
function mode.onReflect(target: number, missile: number, front: boolean): boolean
	return combat.onReflect(target, missile, front)
end

-- A hero took a hit: its health's regeneration waits a moment (regen.luau); its worn pieces' wear
-- is looked at a frame on (its resilience affixes: affixes.luau); Second Wind below its share
-- (warrior.luau).
function mode.onDamaged(target: number, amount: number)
	local h = hero.byUnit(target)
	if h then
		regen.hurt(h, amount)
		affixes.worn(h)
		warrior.hurt(h)
	end
end

-- A player's spell's price before it's paid: the cast-cost keystones' (casts.luau); nil the game's.
function mode.castCost(caster: number, spell: string, cost: number, _how: string): (number?, number?)
	local mana, health = casts.cost(caster, spell, cost)
	-- (a worn power that pays it in health: uniquepowers.luau)
	local m2, h2 = uniquePowers.cost(caster, mana or cost, health)
	if m2 then
		return m2, h2
	end
	return mana, health
end

-- A spell cast (a player's; `made` what it made this frame): a hero's missiles sized by its tree
-- level (casts.luau); a hero's summoning watched for its creature (summons.luau).
function mode.onSpellCast(caster: number, spell: any, target: number?, x: number, y: number, made: { number }?)
	casts.cast(caster, spell, made)
	summons.cast(caster, spell)
	uniquePowers.cast(caster, spell, target, x, y)
end

-- An item picked up: out of the ground's index, its label cleared on every UI at once.
function mode.onPickup(taker: number, item: number, _x: number, _y: number)
	pickedUp(taker, item)
end

function mode.onEquip(unit: number, _item: number, _worn: boolean)
	hero.equipChanged(unit)
	tips.changed(unit)
end

-- A piece handed over (the stash, the vendor, a reroll, "dd give") that the game put on as it came,
-- which onEquip never hears: the same (items.luau).
items.onHandedWorn(function(unit: number)
	hero.equipChanged(unit)
	tips.changed(unit)
end)

-- A hero sent home within the map (the way home, a town portal, a run's way out): its creatures
-- come too (summons.luau).
town.onSentHome(function(unit: number, fromX: number, fromY: number)
	summons.bring(unit, fromX, fromY)
end)

-- A warrior's active ability used (its button or key): Leap Slam's leap, Whirlwind's spin and its
-- end (actives.luau); false refuses a press.
function mode.onModAbility(unit: number, id: string, x: number?, y: number?, phase: string): boolean?
	return actives.used(unit, id, x, y, phase)
end

-- A warrior's stock ability's news (its start, end, a harpoon's hold): their upgrades from the tree
-- (upgrades.luau).
function mode.onAbility(unit: number, ability: string, info: any)
	upgrades.used(unit, ability, info)
end

-- A leap came down: Leap Slam's slam where it landed (actives.luau).
function mode.onLand(unit: number, x: number, y: number, cut: boolean)
	actives.landed(unit, x, y, cut)
end

-- ---- The host's console ("dd help")

function mode.console(line: string)
	return console.run(line)
end

return mode
