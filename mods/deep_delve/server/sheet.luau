--!strict
-- The character sheet: what the mod makes of a character, in one value to its UI while the sheet
-- is open (sent again only when it changes) and as lines for "dd sheet": name, class, level and
-- experience, health, mana and strength, regeneration (health and mana a second, the mod's), damage
-- dealt and taken, resistance, gold carried and
-- stashed, passive points, the act's progress, its spells (a warrior: its abilities; a conjurer
-- also its summons) and its records.
--
-- A party member's sheet: a click on its card on the party strip asks for it (the open message
-- with the member's peer); the same window shows the member's numbers instead of its own, read
-- only, with `of` (the member's peer) in the value, kept current the same way while it's open.
-- The member leaving closes it on every UI showing it. A click on one's own card, or the Sheet
-- button, shows one's own again.

local acts = require("data/acts")
local classes = require("data/classes")
local treeData = require("data/tree")
local hero = require("hero")
local hud = require("hud")
local progress = require("progress")
local regen = require("regen")
local tree = require("tree")
local windows = require("windows")

local sheet = {}

-- The sheet's numbers for a hero.
function sheet.numbers(h: any): any
	local r = h.record
	local o = if nox.present(h.unit) then nox.object(h.unit) else nil
	local mana, mostMana = 0, 0
	local strength = 0
	if o then
		mana, mostMana = nox.mana(h.unit)
		strength = nox.strength(h.unit)
	end
	local total, spent, left = tree.points(r)
	local act = r.world.act or 1
	local p = progress.of(r, act)
	local found = {}
	for _, i in p.waypoints do
		table.insert(found, tostring(i))
	end
	local skills = {}
	for name, level in (r.spells and r.spells.spells) or {} do
		table.insert(skills, string.format("%s %d", (treeData.spellTitles :: any)[name] or name, level))
	end
	for key, level in (r.spells and r.spells.abilities) or {} do
		local title = key
		for _, a in treeData.abilities do
			if a.key == key then
				title = a.title
			end
		end
		table.insert(skills, string.format("%s %d", title, level))
	end
	table.sort(skills)
	local skillText = table.concat(skills, ", ")
	-- (a conjurer's summons after them)
	local summons = {}
	for _, name in (r.spells and r.spells.creatures) or {} do
		table.insert(summons, tree.creatureTitle(name))
	end
	if #summons > 0 then
		skillText = (if skillText ~= "" then skillText .. "; " else "") .. "summons " .. table.concat(summons, ", ")
	end
	if #skillText > 180 then
		skillText = string.sub(skillText, 1, 177) .. "..."
	end
	local resist = math.clamp(h.bonus.resist, treeData.resist.least, treeData.resist.most)
	local healthRegen, manaRegen = regen.rates(h)
	return {
		name = r.identity.name,
		class = ((classes :: any)[r.identity.class] or classes.warrior).title,
		level = r.level.level,
		experience = r.level.experience,
		need = h.need,
		health = if o then o.health else 0,
		maxHealth = if o then o.maxHealth else 0,
		mana = mana,
		maxMana = mostMana,
		strength = strength,
		-- (the mod's regeneration a second, base and tree, the game's own refill not counted)
		healthRegen = string.format("%.1f", healthRegen),
		manaRegen = string.format("%.1f", if mostMana > 0 then manaRegen else 0),
		damage = h.bonus.damage,
		dealt = math.floor(hero.dealt(h) * 100 + 0.5),
		resist = resist,
		taken = math.floor(hero.taken(h) * 100 + 0.5),
		gold = if o then nox.gold(h.unit) else r.gold.carried,
		stashed = r.gold.stashed or 0,
		points = left,
		earned = total,
		spent = spent,
		act = act,
		actName = if (acts :: any)[act] then (acts :: any)[act].name else "",
		deepest = p.deepest,
		cleared = p.cleared == true,
		waypoints = table.concat(found, " "),
		bosses = r.world.bosses[tostring(act)] or 0,
		kills = r.records.kills,
		deaths = r.records.deaths,
		minutes = math.floor((r.records.frames + (nox.frame() - (h.since or nox.frame()))) / 1800),
		skills = skillText,
	}
end

-- The order "dd sheet" lists them in.
local ORDER = { "name", "class", "level", "experience", "need", "health", "maxHealth", "mana", "maxMana", "strength", "healthRegen", "manaRegen",
	"damage", "dealt", "resist",
	"taken", "gold", "stashed", "points", "earned", "spent", "act", "actName", "deepest", "cleared", "waypoints", "bosses", "kills", "deaths", "skills" }

function sheet.lines(h: any): { string }
	local n = sheet.numbers(h)
	local out = {}
	for _, k in ORDER do
		table.insert(out, k .. " " .. tostring(n[k]))
	end
	return out
end

-- Whose sheet the hero's window shows: the party member it asked for (still in the game), else
-- its own.
local function shownOf(h: any): any
	local of = h.sheetOf and hero.byPeer(h.sheetOf)
	if of and of ~= h then
		return of
	end
	h.sheetOf = nil
	return h
end

-- The sheet to the hero's UI (only when it changed since it was last sent): its own, or the party
-- member's it asked for (`of`, that member's peer).
function sheet.view(h: any)
	local who = shownOf(h)
	local n = sheet.numbers(who)
	n.kind = "sheet"
	if who ~= h then
		n.of = who.peer
	end
	local parts = { tostring(n.of) }
	for _, k in ORDER do
		table.insert(parts, tostring(n[k]))
	end
	table.insert(parts, tostring(n.minutes))
	local key = table.concat(parts, "|")
	if key == h.shownSheet then
		return
	end
	h.shownSheet = key
	hud.send(h.peer, "sheet", n)
end

local function beat(h: any)
	sheet.view(h)
end

windows.register("sheet", {
	view = function(h: any)
		h.shownSheet = nil
		sheet.view(h)
	end,
	beat = beat,
})

-- The hero's sheet window shows party member `peer`'s sheet (nil, its own peer, or one not in the
-- game: its own), opened if it wasn't. Whether it opened.
function sheet.show(h: any, peer: number?): boolean
	local of = peer and hero.byPeer(peer)
	h.sheetOf = if of and of ~= h then peer else nil
	return windows.open(h, "sheet")
end

-- A player left: every sheet showing it closed (its UI told).
function sheet.left(peer: number)
	for _, h in hero.all() do
		if h.sheetOf == peer then
			h.sheetOf = nil
			h.shownSheet = nil
			if windows.isOpen(h, "sheet") then
				windows.close(h, "sheet", true)
			end
		end
	end
end

return sheet
