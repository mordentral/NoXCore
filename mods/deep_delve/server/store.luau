--!strict
-- Character records (design 4.1 and 5): made, read, brought up to date and written, through the
-- mod's store (nox.store) on the host. Single player and self-hosted games keep them on the
-- host's PC; the script never knows where.
--
-- A record is a plain table (numbers, strings, booleans, tables), its key
-- "char/<account>/<class>/<name>", each part made a safe key segment by nox.store.keyOf. The
-- account is what the client gave at its join (a label on a local host, design 5.2); none, "local".
-- Players' own .plr files are never written: the record is the character.

local classes = require("data/classes")
local progress = require("progress")
local atlas = require("atlas")

local store = {}

-- The record's layout version: a record of an older one is brought up to date as it's read.
--   1  the first (Phase 1).
--   2  world.progress: per difficulty tier and act ("<tier>-<act>"), the deepest area reached,
--      the waypoints found and whether the act is cleared; world.bosses counts each act boss's
--      kills by act ("<act>"). Version 1's world.waypoints list went into progress.
--   3  tree = { nodes = { passive node ids taken }, bossPoints (a point for each act cleared the
--      first time at a tier), respecs }; stash = { tabs = { [tab] = { [slot] = item record } } }
--      with every tab there; spells = { spells = { [spell name] = level }, abilities =
--      { [ability] = level }, creatures = { creature names } } (what the tree gives, written by
--      book.luau at every set-up and tree change, so `creatures`, added later in version 3, is
--      there from a record's next set-up; a reader takes none for missing); an item record may
--      carry its level. Version 2's tree list became tree.nodes, and each act it had cleared a
--      boss point.
--   4  options = { lootNames = "alt" | "always" }: the player's choices for its character (ground
--      loot's names shown while Alt is held, or always). Version 3's records get the defaults.
--   5  the acts: each act's progress gains `opened` (the act open to the character at that tier:
--      act 1 always, a later act once the act before it is cleared or once it has been there),
--      and world.act is the furthest act reached or opened at its tier (progress.luau). Version
--      4 had one act, so an act 1 clear left world.act at 1; its step marks every act it has
--      progress in opened, opens the act after each one it cleared, and moves world.act on.
--   6  act 2 opens with two more areas (the climb out of the catacombs) before its fields, so each
--      of its areas' indices is two more than before: its step moves act 2's progress on at every
--      tier (deepest and each waypoint found, + ACT2_OPENING), so a found waypoint is the same
--      area's and the deepest the same place; world.deepest follows.
--   7  atlas = { open, best, runs, cleared = { [biome] = { ranks } } }: the expeditions cleared
--      (atlas.luau). Version 6's records get an empty atlas, open when they cleared the last act.
store.VERSION = 7

-- How many areas version 6 put in front of act 2's (data/acts.luau's act 2: Old Workings, Cave
-- Mouth).
local ACT2_OPENING = 2

-- The options' defaults, and the values each may take (the first: the default).
store.OPTIONS = {
	lootNames = { "alt", "always" },
}

-- A record's option `name`, its default when it's missing or not one it may take.
function store.option(rec: any, name: string): string
	local allowed: { string }? = (store.OPTIONS :: any)[name]
	if not allowed then
		return ""
	end
	local o = if type(rec.options) == "table" then rec.options[name] else nil
	if type(o) == "string" and table.find(allowed, o) then
		return o
	end
	return allowed[1]
end

-- A record's option `name` set to `value`; whether it may take it.
function store.setOption(rec: any, name: string, value: string): boolean
	local allowed: { string }? = (store.OPTIONS :: any)[name]
	if not allowed or not table.find(allowed, value) then
		return false
	end
	if type(rec.options) ~= "table" then
		rec.options = {}
	end
	rec.options[name] = value
	return true
end

local STASH_TABS: number = require("data/vendor").stash.tabs

-- Whether this host keeps a store at all (a host without one plays, but keeps nothing).
function store.available(): boolean
	return nox.store ~= nil
end

-- A player's character key.
function store.keyFor(player: any): string
	local s = nox.store
	assert(s, "no store")
	local account = if player.account and player.account ~= "" then s.keyOf(player.account) else "local"
	return "char/" .. account .. "/" .. (player.class or "warrior") .. "/" .. s.keyOf(player.name)
end

-- A server character's key (roster.luau): "srv/<account id>/<class>/<name>", the account the
-- server's own (accounts.luau), never what the client named.
function store.serverKey(account: string, class: string, name: string): string
	local s = nox.store
	assert(s, "no store")
	return "srv/" .. account .. "/" .. class .. "/" .. string.sub(s.keyOf(name), 1, 60)
end

-- A new level-1 character's record, its class's starting gear in it.
function store.new(player: any): any
	local class = player.class or "warrior"
	local rec = {
		version = store.VERSION,
		identity = {
			name = player.name,
			class = class,
			account = player.account or "",
			hardcore = false, -- (design 7: chosen when the character is made, never changed; later)
			tier = 1, -- the difficulty tier reached (design 7)
		},
		-- points: the levels gained (the passive tree counts its points from the level itself)
		level = { level = 1, experience = 0, points = 0 },
		tree = { nodes = {}, bossPoints = 0, respecs = 0 }, -- see VERSION (tree.luau)
		equipment = {}, -- item records worn
		inventory = {}, -- item records carried
		stash = { tabs = {} }, -- see VERSION (stash.luau)
		gold = { carried = 0, stashed = 0 },
		-- act: the furthest act reached or opened at its tier; deepest: the deepest area index
		-- reached in any act; progress and bosses: see VERSION (progress.luau reads and writes
		-- them).
		world = { act = 1, deepest = 0, progress = {}, bosses = {} },
		spells = { spells = {}, abilities = {}, creatures = {} }, -- what its passive tree gives (book.luau)
		records = { kills = 0, deaths = 0, frames = 0 }, -- frames: time played, in game frames
		options = { lootNames = "alt" }, -- see VERSION
		atlas = atlas.fresh(), -- see VERSION (atlas.luau)
	}
	for t = 1, STASH_TABS do
		rec.stash.tabs[t] = {}
	end
	for _, g in ((classes :: any)[class] or classes.warrior).start do
		local item = { type = g.type, rarity = "normal" }
		if g.worn then
			table.insert(rec.equipment, item)
		else
			table.insert(rec.inventory, item)
		end
	end
	return rec
end

-- Version 1 -> 2: the world's waypoints (a list of area indices, act 1, tier 1) and its deepest
-- area become act 1's progress at the character's tier; bosses (a list, never written by version
-- 1) become kills by act.
local function toVersion2(rec: any)
	local old = if type(rec.world) == "table" then rec.world else {}
	local deepest = if type(old.deepest) == "number" then old.deepest else 0
	local found = {}
	if type(old.waypoints) == "table" then
		for _, v in old.waypoints do
			if type(v) == "number" and not table.find(found, v) then
				table.insert(found, v)
			end
		end
		table.sort(found)
	end
	local tier = if type(rec.identity) == "table" and type(rec.identity.tier) == "number" then rec.identity.tier else 1
	local bosses: { [string]: number } = {}
	if type(old.bosses) == "table" then
		for _, act in old.bosses do
			if type(act) == "number" then
				local key = tostring(act)
				local was: number = bosses[key] or 0
				bosses[key] = was + 1
			end
		end
	end
	rec.world = {
		act = if type(old.act) == "number" then old.act else 1,
		deepest = deepest,
		progress = { [tostring(tier) .. "-1"] = { deepest = deepest, cleared = false, waypoints = found } },
		bosses = bosses,
	}
	rec.version = 2
end

-- Version 2 -> 3: the tree's list of node ids into tree.nodes, a boss point for each act it had
-- cleared at each tier, every stash tab there (old stash contents, never written by version 2,
-- kept where they are), spells in their new layout (worked out again at the next set-up).
local function toVersion3(rec: any)
	local nodes = {}
	if type(rec.tree) == "table" then
		for _, id in rec.tree do
			if type(id) == "number" then
				table.insert(nodes, id)
			end
		end
	end
	local cleared = 0
	if type(rec.world) == "table" and type(rec.world.progress) == "table" then
		for _, p in rec.world.progress do
			if type(p) == "table" and p.cleared == true then
				cleared += 1
			end
		end
	end
	rec.tree = { nodes = nodes, bossPoints = cleared, respecs = 0 }
	local stash = if type(rec.stash) == "table" then rec.stash else {}
	local tabs = if type(stash.tabs) == "table" then stash.tabs else {}
	for t = 1, STASH_TABS do
		if type(tabs[t]) ~= "table" then
			tabs[t] = {}
		end
	end
	rec.stash = { tabs = tabs }
	rec.spells = { spells = {}, abilities = {}, creatures = {} }
	if type(rec.gold) ~= "table" then
		rec.gold = { carried = 0, stashed = 0 }
	end
	rec.version = 3
end

-- Version 3 -> 4: the options, each at its default.
local function toVersion4(rec: any)
	local options = {}
	for name, allowed in store.OPTIONS :: any do
		options[name] = allowed[1]
	end
	rec.options = options
	rec.version = 4
end

-- Version 4 -> 5: every act it has progress in (in version 4, act 1 at its tiers) opened, the act
-- after each one it cleared opened, world.act moved on to the furthest open at its tier.
local function toVersion5(rec: any)
	local world = rec.world
	if type(world.progress) ~= "table" then
		world.progress = {}
	end
	if type(world.bosses) ~= "table" then
		world.bosses = {}
	end
	if type(world.act) ~= "number" then
		world.act = 1
	end
	for _, p in world.progress do
		if type(p) == "table" then
			p.opened = true
		end
	end
	progress.openAfterCleared(rec)
	rec.version = 5
end

-- Version 5 -> 6: act 2's progress at every tier ("<tier>-2") moved on by the areas put in front of
-- it: its deepest (when it had reached any) and each waypoint found; world.deepest at least the
-- deepest any act now has.
local function toVersion6(rec: any)
	local world = rec.world
	for key, p in world.progress do
		if type(p) == "table" and string.match(tostring(key), "^%d+%-2$") then
			if type(p.deepest) == "number" and p.deepest > 0 then
				p.deepest += ACT2_OPENING
				if p.deepest > (tonumber(world.deepest) or 0) then
					world.deepest = p.deepest
				end
			end
			if type(p.waypoints) == "table" then
				local moved = {}
				for _, i in p.waypoints do
					if type(i) == "number" then
						table.insert(moved, i + ACT2_OPENING)
					end
				end
				table.sort(moved)
				p.waypoints = moved
			end
		end
	end
	rec.version = 6
end

-- Version 6 -> 7: an empty atlas, the map table open when it has cleared the last act.
local function toVersion7(rec: any)
	rec.atlas = atlas.fresh()
	atlas.note(rec)
	rec.version = 7
end

-- An older record brought up to date (each version's step in turn).
local function migrate(rec: any): any
	if rec.version < 2 then
		toVersion2(rec)
	end
	if rec.version < 3 then
		toVersion3(rec)
	end
	if rec.version < 4 then
		toVersion4(rec)
	end
	if rec.version < 5 then
		toVersion5(rec)
	end
	if rec.version < 6 then
		toVersion6(rec)
	end
	if rec.version < 7 then
		toVersion7(rec)
	end
	rec.version = store.VERSION
	return rec
end

-- The record under `key` (and whether it was brought up from an older version); nil and why when
-- there's none ("none") or it was damaged (the store set the file aside: "damaged").
function store.load(key: string): (any, string?, boolean?)
	local s = nox.store
	assert(s, "no store")
	local rec, err = s.get(key)
	if rec == nil then
		return nil, err or "none", nil
	end
	if type(rec) ~= "table" or type(rec.version) ~= "number" or rec.version > store.VERSION then
		return nil, "unreadable", nil
	end
	local older = rec.version < store.VERSION
	return migrate(rec), nil, older
end

-- Writes a record whole (the store queues it and writes it off the game's thread); whether it was
-- taken, and why not.
function store.save(key: string, rec: any): (boolean, string?)
	local s = nox.store
	if not s then
		return false, "no store"
	end
	return s.put(key, rec)
end

-- Holds a character for this game (one character in two games of this host at once is refused).
function store.hold(key: string): boolean
	local s = nox.store
	return s ~= nil and (s.lock(key))
end

function store.release(key: string)
	local s = nox.store
	if s then
		s.unlock(key)
	end
end

return store
