--!strict
-- The players' characters in play (design 4.2): each joined player's record, set on its unit
-- (gear, gold, health, mana and strength from its level and passive tree, the tree's spells),
-- read back from the unit when it's saved, its experience and levels.
--
-- A hero is { peer, unit, key, record, ready, bonus, need, since, arrival, fell, portal, open,
-- told, mapReady, stock, partySig, labelSig }:
--   ready  the unit carries the record's state (set up, not dead, not between maps): only then is
--          the unit read back into the record;
--   mapReady its client has this map loaded (playerReady), so world labels reach it;
--   stock  its own vendor stock (vendor.luau); partySig, labelSig what its UI was last sent of the
--          party strip and the ground's labels (party.luau, labels.luau); boardSig what this game
--          last put of its records row (records.luau);
--   bonus  what its worn affixes and its passive tree add: { damage (%), resist (%), health,
--          mana, strength, healthPct, manaPct, healthRegen, manaRegen (a second), manaOnKill,
--          spellCost, summonCost, summonHealth (%: the cast-cost keystones'), poison (% more poison
--          damage: the tree's Virulence and Miasma, combat.luau), own (% more by a hit's thing
--          type: a spell's own), skills (a warrior's passive skills by key, each its
--          rank: tree.totals), frost, dread, grip, reflect, ward, resilience, guard (%: the worn
--          affixes', affixes.luau), weapon (the worn weapon's id), ammo (its shot's type), shield
--          (the worn shield's id), venom (the worn weapon or quiver poisons: the game's Venom),
--          powers, sets, pieces, setSig (the worn uniques' powers and the worn sets: uniques.worn;
--          their active bonuses' stats are in the numbers above) };
--   powerState its worn powers' own state (uniquepowers.luau);
--   rally  a warrior's Rallying Cry on it: { till (the frame it ends), damage, resist (%) }, counted
--          by hero.dealt and hero.taken while it lasts (upgrades.luau);
--   regen  its passive regeneration's timer and state (regen.luau);
--   open   its windows open (windows.luau); sheetOf the party member whose sheet its sheet window
--          shows (nil: its own; sheet.luau);
--   told   what its client was told of its book (book.luau); barsTold: its abilities were told
--          as learned once this session, for its bars (book.luau);
--   need   the experience to its next level;
--   arrival where it comes into play in this map (the area's start or its waypoint, when it came
--          by a map change; nil: the town);
--   fell   it died in this map and hasn't come back yet: the game kept its things on it (the
--          mode's keepThingsOnDeath), so its respawn gives nothing again;
--   portal where its town portal stands in this map, when one is open (portal.luau).
--
-- Gear is given from the record when a unit first comes into play and in every new map (a map
-- change outside a quest strips a player); never after a death, which keeps everything. Each
-- set-up also puts the unit on the party's team (team.luau).
--
-- When the record is written: on an area change, a level up, a death, an option set, leaving, the
-- game's end and every numbers.timing.saveEvery frames.

local store = require("store")
local progress = require("progress")
local atlas = require("atlas")
local items = require("items")
local hud = require("hud")
local town = require("town")
local touch = require("touch")
local tree = require("tree")
local book = require("book")
local team = require("team")
local regen = require("regen")
local records = require("records")
local affixes = require("affixes")
local uniques = require("uniques")
local classes = require("data/classes")
local numbers = require("data/numbers")
local treeData = require("data/tree")
local tipData = require("data/tips")
local warriorData = require("data/warrior")
local uniqueData = require("data/uniques")

local hero = {}

local byPeer: { [number]: any } = {}
local byUnit: { [number]: any } = {}
-- The area being played (areas.info's), for those set up in it after it was entered.
local here: any = nil

function hero.byUnit(unit: number)
	return byUnit[unit]
end

function hero.byPeer(peer: number)
	return byPeer[peer]
end

-- Every hero (by peer).
function hero.all()
	return byPeer
end

-- ---- Levels and stats

-- The experience from level L to L + 1. (A hair added before rounding down: the server's maths
-- may run rounding toward zero, where a whole power such as 4 ^ 1.5 comes out a hair under 8.)
function hero.needFor(level: number): number
	local l = numbers.levels
	if level == 1 and l.first then
		return l.first
	end
	return math.floor(l.base * level ^ l.growth + 1e-6)
end

-- A class's stock maxima (gamedata.bin), read once a class.
local stock: { [string]: any } = {}
local function stockOf(class: string): any
	local s = stock[class]
	if not s then
		local names = ((classes :: any)[class] or classes.warrior).numbers
		s = {
			health = nox.gameNumber(names.health) or 100,
			mana = nox.gameNumber(names.mana) or 0,
			strength = nox.gameNumber(names.strength) or 50,
		}
		stock[class] = s
	end
	return s
end

-- What the worn affixes (from the unit's inventory, heard on each change: onEquip) and the
-- record's passive tree add up to.
local function bonusOf(unit: number, rec: any)
	local t = tree.totals(rec)
	local b = {
		damage = t.damage,
		health = t.health,
		mana = t.mana,
		strength = t.strength,
		healthPct = t.healthPct,
		manaPct = t.manaPct,
		resist = t.resist,
		healthRegen = t.healthRegen,
		manaRegen = t.manaRegen,
		manaOnKill = t.manaOnKill,
		spellCost = t.spellCost,
		summonCost = t.summonCost,
		summonHealth = t.summonHealth,
		poison = t.poison,
		own = t.own,
		skills = t.skills,
	}
	-- (each worn piece's affixes added to their stats, each up to its most; the worn weapon noted:
	-- frost, dread and grip go with its hits alone, affixes.luau; the worn uniques' powers and the
	-- worn sets' bonuses, uniques.luau)
	local special = {}
	-- (the worn affixes alone, by stat, each up to its most: the sheet's Affixes block)
	local sums = {}
	for _, it in nox.inventory(unit) do
		local facts = nox.item(it.id)
		if facts and facts.worn then
			local m = items.meta(it.id)
			if m and m.affixes then
				affixes.add(b, m.affixes)
				affixes.add(sums, m.affixes)
			end
			if m and (m.unique or m.set) then
				table.insert(special, m)
			end
			local kind = affixes.kindOf(facts.type)
			if kind == "weapon" then
				affixes.noteWeapon(b, it.id, facts.type)
			end
			-- (a worn weapon or quiver that poisons, the game's Venom: its hits note the hero as the
			-- monster's poisoner, combat.luau)
			if (kind == "weapon" or kind == "quiver") and affixes.venomous(facts) then
				(b :: any).venom = true
			end
			-- (a shield worn: the warrior's shield skills, warrior.luau)
			local tip = (tipData.items :: any)[facts.type]
			if tip and tip.kind == "shield" then
				(b :: any).shield = it.id
			end
		end
	end
	uniques.worn(b, special)
	-- (a warrior's skill whose stats count with a shield worn: Phalanx's)
	local ab: any = b
	if ab.shield then
		for key in ab.skills do
			local def = (warriorData.skills :: any)[key]
			if def and def.withShield then
				for stat, v in def.withShield do
					ab[stat] = (ab[stat] or 0) + v
				end
			end
		end
	end
	affixes.cap(b)
	affixes.cap(sums)
	b.affixSums = sums
	return b
end

-- Called with the hero each time its bonus is worked out again (its set-up, an equip, its tree):
-- uniquepowers.luau's, which starts or stops what its worn powers need.
local bonusHooks: { (any) -> () } = {}

function hero.onBonus(fn: (any) -> ())
	table.insert(bonusHooks, fn)
end

local function bonusChanged(h: any)
	for _, fn in bonusHooks do
		fn(h)
	end
end

-- The most health, most mana and strength a hero has from its level, worn affixes and tree:
-- (the class's stock most x its level's share, rounded, + the flat bonus) x (1 + the % bonus).
function hero.most(h: any): (number, number, number)
	local level = h.record.level.level
	local s = stockOf(h.record.identity.class)
	local st = numbers.stats
	local b = h.bonus
	local function share(part: any): number
		return part.atOne + part.perLevel * (level - 1)
	end
	local health = math.floor(s.health * share(st.health) + 0.5) + b.health
	if b.healthPct ~= 0 then
		health = math.floor(health * (1 + b.healthPct / 100) + 0.5)
	end
	local mana = 0
	if s.mana > 0 then
		mana = math.floor(s.mana * share(st.mana) + 0.5) + b.mana
		if b.manaPct ~= 0 then
			mana = math.floor(mana * (1 + b.manaPct / 100) + 0.5)
		end
	end
	local strength = math.floor(s.strength * share(st.strength) + 0.5) + b.strength
	return math.clamp(health, 1, 65535), math.clamp(mana, 0, 65535), math.max(1, strength)
end

-- The share of damage a hero takes after its resistance (1: all of it).
function hero.taken(h: any): number
	local resist = h.bonus.resist
	-- (a warrior's Rallying Cry on it while it lasts: upgrades.luau)
	local r = h.rally
	if r and nox.frame() < r.till then
		resist += r.resist
	end
	return 1 - math.clamp(resist, treeData.resist.least, treeData.resist.most) / 100
end

-- The share of damage a hero deals (1: as the game has it); `weapon`: what the hit came by (a
-- missile, a pixie), whose type may carry its spell's own bonus too (bonus.own).
function hero.dealt(h: any, weapon: number?): number
	local pct = h.bonus.damage
	local r = h.rally
	if r and nox.frame() < r.till then
		pct += r.damage
	end
	if weapon and next(h.bonus.own) ~= nil then
		local w = nox.object(weapon)
		if w then
			pct += h.bonus.own[w.type] or 0
		end
	end
	return math.max(0.1, 1 + pct / 100)
end

-- The unit's health, mana and strength from its level, worn affixes and tree; `fill`: health and
-- mana to their new most (a level up, a set-up), else kept at the same share.
local function applyStats(h: any, fill: boolean)
	local s = stockOf(h.record.identity.class)
	local mostHealth, mostMana, strength = hero.most(h)
	local o = nox.object(h.unit)
	if not o then
		return
	end
	local health = if fill then mostHealth else math.max(1, math.floor(o.health * mostHealth / math.max(1, o.maxHealth)))
	nox.setPlayerHealth(h.unit, math.min(health, mostHealth), mostHealth)
	if s.mana > 0 then
		local mana, oldMost = nox.mana(h.unit)
		local now = if fill then mostMana else math.floor(mana * mostMana / math.max(1, oldMost))
		nox.setMana(h.unit, math.min(now, mostMana), mostMana)
	end
	nox.setStrength(h.unit, strength)
end

-- ---- Records and the unit

-- The unit's state into the record: its gear (worn and carried, with any piece its set-up held
-- back), its gold, the time played.
function hero.snapshot(h: any)
	if not h.ready or not nox.present(h.unit) then
		return
	end
	local equipment, inventory = {}, {}
	-- (its secondary weapon, as its client holds it: its record marked `secondWeapon`, given again
	-- right after the worn gear (hero.setUp); `secondary` is the enchantment's slot)
	local second = nox.secondaryWeapon(h.unit)
	for _, it in nox.inventory(h.unit) do
		-- (an item being deleted this frame, as one put in the stash or sold, is gone already)
		local o = nox.object(it.id)
		local rec = if o and not o.pendingDelete then items.recordOf(it.id) else nil
		if rec then
			if rec.worn then
				rec.worn = nil
				table.insert(equipment, rec)
			else
				if it.id == second then
					rec.secondWeapon = true
				end
				table.insert(inventory, rec)
			end
		end
	end
	-- (a piece its set-up couldn't hand over stays the record's, carried)
	for _, rec in h.heldBack or {} do
		table.insert(inventory, rec)
	end
	local r = h.record
	r.equipment, r.inventory = equipment, inventory
	r.gold.carried = nox.gold(h.unit)
	local now = nox.frame()
	r.records.frames += now - h.since
	h.since = now
end

-- Writes a hero's record (read from its unit first when it's ready); whether the store took it.
function hero.save(h: any, why: string): boolean
	if not h.key then
		return false
	end
	hero.snapshot(h)
	-- (the map table opened for good once the last act is cleared: atlas.luau)
	atlas.note(h.record)
	local ok, err = store.save(h.key, h.record)
	if not ok then
		nox.log("deep_delve: " .. h.record.identity.name .. "'s record not saved (" .. why .. "): " .. tostring(err))
	else
		records.note(h) -- (the host's records row, when it has moved on: records.luau)
	end
	return ok
end

function hero.saveAll(why: string): number
	local n = 0
	for _, h in byPeer do
		if hero.save(h, why) then
			n += 1
		end
	end
	return n
end

-- The record set on the unit: what it carried taken away (for this game only: the player's own
-- .plr is never written), the record's gear given (worn as it was), its gold and numbers. `at`: a
-- point to put the unit at (nil: where it stands). `keep`: it still carries the record's gear and
-- gold (a respawn after a death), so only its numbers are set again.
function hero.setUp(h: any, at: any, keep: boolean?)
	if h.ready then
		return
	end
	local p = nox.player(h.peer)
	if not p or not nox.present(p.unit) then
		return
	end
	if h.unit ~= p.unit then
		byUnit[h.unit] = nil
		h.unit = p.unit
	end
	byUnit[h.unit] = h
	if not keep then
		-- (a map's stock things, or what the player joined with: gone)
		for _, it in nox.inventory(h.unit) do
			items.forget(it.id)
			nox.delete(it.id)
		end
		-- (each piece given back as the game gives its gear; one its class may not use, which the game
		-- won't give, made at its feet and taken into its inventory (items.hand). One that still can't be
		-- made is held back: kept in the record as carried (hero.snapshot), tried again at the next
		-- set-up, never dropped)
		-- Each list given from its last to its first: a given item goes to the front of the unit's
		-- inventory, so the inventory then reads in the record's order (the carried, then the worn) and
		-- the next snapshot keeps it, through any number of maps; its client, told in the giving's
		-- order, files the worn first and each part the oldest first.
		-- The secondary weapon (marked by hero.snapshot) is given right after the worn gear, before any
		-- other carried weapon: a given item is new to the client, and its pickup makes a weapon picked
		-- up with one in hand and no secondary carried its secondary, so it's the secondary again. It
		-- then reads last of the carried, and stays there.
		local heldBack = {}
		local equipment, inventory = h.record.equipment, h.record.inventory
		for i = #equipment, 1, -1 do
			local rec = equipment[i]
			local id = items.hand(h.unit, rec, true)
			if id then
				nox.equip(h.unit, id) -- (false and why when the class can't: then it's carried)
			else
				table.insert(heldBack, 1, rec)
			end
		end
		local held: { [number]: boolean } = {}
		local second: number? = nil
		for i, rec in inventory do
			if rec.secondWeapon then
				second = i
				break
			end
		end
		if second then
			held[second] = not items.hand(h.unit, inventory[second], true)
		end
		for i = #inventory, 1, -1 do
			if i ~= second and not items.hand(h.unit, inventory[i], true) then
				held[i] = true
			end
		end
		-- (in the record's order: the worn ones', then the carried ones')
		for i, rec in inventory do
			if held[i] then
				table.insert(heldBack, rec)
			end
		end
		if #heldBack > 0 then
			nox.log("deep_delve: " .. h.record.identity.name .. ": " .. #heldBack .. " piece(s) held back in the record, not carried now")
		end
		h.heldBack = heldBack
		nox.setGold(h.unit, h.record.gold.carried, true)
	end
	if here then
		progress.reached(h.record, here.act, here.index)
	end
	h.bonus = bonusOf(h.unit, h.record)
	applyStats(h, true)
	affixes.noteWear(h)
	bonusChanged(h)
	-- Only the tree's spells (a warrior: its abilities).
	book.apply(h)
	-- The hits it takes: the mod's monsters' scaled, the party's and its own Fireball's cancelled
	-- (combat.luau); and those it took, which hold its health's regeneration back (regen.luau).
	nox.watchEvents(h.unit, nox.events.damage + nox.events.damaged)
	-- On the party's team (team.luau; already on it, nothing happens).
	team.join(h.unit)
	-- A player joining (or in a new map) watches until it asks to play; here it's brought into
	-- play at once, in town (or at `at`).
	if bit32.band(nox.status(h.unit), 1) ~= 0 and nox.living(h.unit) then
		nox.backIntoPlay(h.unit)
		if not at then
			local x, y = town.respawnPoint()
			if x and y then
				nox.setPosition(h.unit, x, y)
			end
		end
	end
	if at then
		nox.setPosition(h.unit, at.x, at.y)
		nox.effect(0x89, at.x, at.y) -- (MSG_FX_TELEPORT)
	end
	-- (each set-up is an arrival: a join, a new map, a respawn; no way takes it at once: touch.luau)
	touch.arrived(h.unit)
	h.ready = true
	h.since = nox.frame()
	hud.hero(h)
end

-- A set-up hero found watching (a client that finished a new map's download after its unit was
-- set up is held watching by the game, and a watching unit can't walk or be taken by a
-- pentagram): brought back into play where it was to arrive.
local function keepInPlay(h: any)
	if h.ready and nox.present(h.unit) and bit32.band(nox.status(h.unit), 1) ~= 0 and nox.living(h.unit) then
		nox.backIntoPlay(h.unit)
		local at = h.arrival
		if at then
			nox.setPosition(h.unit, at.x, at.y)
		else
			local x, y = town.respawnPoint()
			if x and y then
				nox.setPosition(h.unit, x, y)
			end
		end
		touch.arrived(h.unit)
	end
end

-- A player's client is ready in the map (its join, and after each map change): its hero kept in
-- play now, and once more a few frames on in case the game sets it watching after this; its
-- client told its book again (what it holds now isn't known).
function hero.clientReady(p: any)
	local h = byPeer[p.peer]
	if not h then
		return
	end
	h.mapReady = true
	keepInPlay(h)
	if h.ready then
		book.apply(h, true)
	end
	nox.after(numbers.timing.readyRecheck, function()
		if byPeer[p.peer] == h then
			keepInPlay(h)
		end
	end)
end

-- Every hero set up again (a new map: `at` where those who came by the exit arrive, kept for
-- their spawn in it too).
function hero.setUpAll(at: any)
	for _, h in byPeer do
		h.arrival = at
		hero.setUp(h, at)
	end
end

-- A map change under way: nobody's unit is read back until it's set up again in the new map,
-- where everyone is given their gear again (the change strips them) and no portal is open.
function hero.mapChanging()
	for _, h in byPeer do
		h.ready = false
		h.mapReady = false -- (its client loads the new map: playerReady again)
		h.fell = false
		h.portal = nil
		h.told = nil -- (its client's book swept again in the new map)
	end
end

-- ---- Joining and leaving

-- A player's character into play: the one its join names (its account, class and name), or
-- `chosen` = { key, record, fresh, older } (a server character picked at character select,
-- roster.luau: held for this game already; key nil: played, but not kept).
function hero.join(p: any, chosen: any?)
	if not store.available() then
		nox.tell(p.unit, "Deep Delve: this host keeps no characters; you play, but nothing is kept.")
	end
	local key = if chosen then chosen.key elseif store.available() then store.keyFor(p) else nil
	local record, why, older = nil, nil, nil
	if chosen then
		record, older = chosen.record, chosen.older
	elseif key then
		record, why, older = store.load(key)
		if record == nil and why ~= "none" then
			nox.log("deep_delve: " .. key .. " not read (" .. tostring(why) .. "); a new character")
		end
		if not store.hold(key) then
			-- The same character in another game of this host: played, but not kept from here.
			nox.tell(p.unit, "Deep Delve: this character is in another game here; it won't be saved from this one.")
			key = nil
		end
	end
	local fresh = if chosen then chosen.fresh == true else record == nil
	record = record or store.new(p)
	-- (an act built since the record's last clear: opened now, the record written)
	if progress.openAfterCleared(record) then
		older = true
	end
	if atlas.note(record) then
		older = true
	end
	local h = {
		peer = p.peer,
		unit = p.unit,
		key = key,
		record = record,
		ready = false,
		bonus = { damage = 0, health = 0, mana = 0, strength = 0, healthPct = 0, manaPct = 0, resist = 0, healthRegen = 0, manaRegen = 0,
			manaOnKill = 0, spellCost = 0, summonCost = 0, summonHealth = 0, poison = 0, own = {}, skills = {}, powers = {}, sets = {}, pieces = {},
			setSig = "" },
		open = {},
		need = hero.needFor(record.level.level),
		since = nox.frame(),
	}
	if byPeer[p.peer] then
		regen.stop(byPeer[p.peer])
	end
	byPeer[p.peer] = h
	byUnit[p.unit] = h
	-- (its passive regeneration's timer, from now till it leaves: regen.luau)
	regen.start(h)
	nox.after(numbers.timing.setUpAfter, function()
		hero.setUp(h, nil)
		local lv = record.level
		nox.tell(h.unit, string.format("Deep Delve: %s, level %d %s%s.", record.identity.name, lv.level,
			tostring(((classes :: any)[record.identity.class] or classes.warrior).title), if fresh then " (a new character)" else ""))
		local _, _, left = tree.points(record)
		if left > 0 then
			nox.tell(h.unit, string.format("You have %d passive point%s: your spells come from the Tree (the button in your Deep Delve window).",
				left, if left == 1 then "" else "s"))
		end
	end)
	-- (a new character, or one brought up from an older record's layout: written now)
	if (fresh or older) and key then
		store.save(key, record)
	end
end

function hero.leave(p: any)
	local h = byPeer[p.peer]
	if not h then
		return
	end
	local saved = hero.save(h, "leaving")
	-- (what it carries and wears is its record's now: gone with it, so the game doesn't drop a copy
	-- of each piece where it stood for anyone to pick up, the player too when it's back)
	if saved and nox.present(h.unit) then
		for _, it in nox.inventory(h.unit) do
			items.forget(it.id)
			nox.delete(it.id)
		end
	end
	regen.stop(h)
	if h.key then
		store.release(h.key)
	end
	byPeer[p.peer] = nil
	byUnit[h.unit] = nil
	book.forget(h)
	hud.forget(p.peer)
end

-- ---- Death

-- A hero's unit died: its record written as it was. The game keeps everything it carried and
-- wore on it (the mode's keepThingsOnDeath), gold too, at every tier. Hardcore ends the
-- character here instead, once it's built.
function hero.died(unit: number)
	local h = byUnit[unit]
	if not h then
		return
	end
	h.record.records.deaths += 1
	hero.save(h, "death")
	h.ready = false
	h.fell = true
	h.arrival = nil -- (the fallen come back in town)
end

-- A hero's unit comes (back) into play (respawnPoint: its first spawn once its client has the
-- map, after a death, after a map change), set up again from its record a frame on: after a death
-- only its numbers (it kept its things), else its gear too. Returns where it should stand: the
-- area's start or its waypoint when it came by a map change, else nil (the caller's choice: the
-- town).
function hero.respawned(unit: number): any
	local h = byUnit[unit]
	if not h then
		local o = nox.object(unit)
		h = o and o.player and byPeer[o.peer]
	end
	if not h then
		return nil
	end
	h.ready = false
	local keep = h.fell == true
	h.fell = false
	nox.after(1, function()
		hero.setUp(h, nil, keep)
	end)
	return h.arrival
end

-- ---- Experience

local function levelUp(h: any)
	applyStats(h, true)
	local o = nox.object(h.unit)
	if o then
		local glow = nox.create("LevelUp", o.x, o.y)
		if glow then
			nox.after(numbers.timing.glow, function()
				if nox.present(glow) then
					nox.delete(glow)
				end
			end)
		end
	end
	nox.sound(h.unit, "LevelUp")
	nox.tell(h.unit, string.format("You reached level %d.", h.record.level.level))
	hero.save(h, "level up")
end

-- Experience to one hero; levels gained with it.
function hero.gain(h: any, amount: number)
	local lv = h.record.level
	if lv.level >= numbers.levels.cap then
		return
	end
	lv.experience += amount
	local rose = false
	while lv.level < numbers.levels.cap and lv.experience >= hero.needFor(lv.level) do
		lv.experience -= hero.needFor(lv.level)
		lv.level += 1
		lv.points += 1
		rose = true
	end
	h.need = hero.needFor(lv.level)
	if rose then
		levelUp(h)
	end
	hud.hero(h)
end

-- A kill's experience: split evenly between the heroes in play (at least 1 each). The killer's
-- manaOnKill (the passive tree's) comes back to it, up to its most.
function hero.award(amount: number, killer: number?)
	local ready = {}
	for _, h in byPeer do
		if h.ready then
			table.insert(ready, h)
		end
	end
	local each = math.max(1, math.floor(amount / math.max(1, #ready) + 0.5))
	for _, h in ready do
		hero.gain(h, each)
	end
	local k = killer and byUnit[killer]
	if k then
		k.record.records.kills += 1
		local gain = k.bonus.manaOnKill or 0
		if gain > 0 and nox.living(k.unit) then
			local mana, most = nox.mana(k.unit)
			if mana < most then
				nox.setMana(k.unit, math.min(most, mana + gain), most, "plain")
			end
		end
	end
end

-- ---- Gear

-- A unit picked an item up: a magic or better find is told to its finder (its modifiers and
-- affixes, which its label on the ground doesn't name). A unique or a set piece the mod put on the
-- ground (`fromGround`: loot, not one a player dropped) is told to everyone in the game too, one
-- line naming the finder and the item (by decision 2026-11-02), in the item's colour
-- (data/uniques.luau's foundColour).
function hero.pickedUp(unit: number, item: number, fromGround: boolean?)
	local h = byUnit[unit]
	local m = items.meta(item)
	-- (a piece handed over from the stash or the vendor by the pickup: no find)
	if items.wasHanded(item) or not h or not m or m.rarity == "normal" then
		return
	end
	local rec = items.recordOf(item)
	if rec then
		nox.tell(unit, "You found: " .. items.describe(rec))
		local name = uniques.nameOf(rec)
		if fromGround and name then
			nox.textToAll(string.format("%s found %s.", tostring(h.record.identity.name), name), (uniqueData.foundColour :: any)[rec.rarity])
		end
	end
end

-- A watched item put on or taken off: the affixes' bonus and the numbers again.
function hero.equipChanged(unit: number)
	local h = byUnit[unit]
	if not h or not h.ready then
		return
	end
	h.bonus = bonusOf(unit, h.record)
	applyStats(h, false)
	affixes.noteWear(h)
	bonusChanged(h)
end

-- The hero's passive tree changed (a node taken, a respec): its numbers and book again, the record
-- written.
function hero.treeChanged(h: any, why: string)
	if h.ready and nox.present(h.unit) then
		h.bonus = bonusOf(h.unit, h.record)
		applyStats(h, false)
		bonusChanged(h)
		book.apply(h)
	end
	hero.save(h, why)
	hud.hero(h)
end

-- A player's option for its character (store.OPTIONS: "lootNames" "alt" or "always") set, from
-- its UI or the console: kept in its record (written now) and its UI told. Whether it may take it.
function hero.setOption(h: any, name: string, value: string): (boolean, string?)
	if not store.setOption(h.record, name, value) then
		return false, "no such option or value"
	end
	hero.save(h, "option")
	hud.hero(h)
	return true, nil
end

-- An area entered (`info`: areas.info's): the deepest each character has been, in the act and
-- in all (and for each one set up in it later: hero.setUp). An expedition's map is no act's area:
-- it moves no act's progress.
function hero.reached(info: any)
	here = if info.expedition then nil else info
	for _, h in byPeer do
		if here then
			progress.reached(h.record, info.act, info.index)
		end
		records.note(h) -- (a new deepest area: the host's records row now, records.luau)
	end
end

return hero
