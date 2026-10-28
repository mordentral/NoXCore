--!strict
-- The character sheet: what the mod makes of a character, in one value to its UI while the sheet
-- is open (sent again only when it changes) and as lines for "dd sheet": name, class, level and
-- experience, health, mana and strength, damage dealt and taken, resistance, gold carried and
-- stashed, passive points, the act's progress, its spells (a warrior: its abilities; a conjurer
-- also its summons) and its records.

local acts = require("data/acts")
local classes = require("data/classes")
local treeData = require("data/tree")
local hero = require("hero")
local hud = require("hud")
local progress = require("progress")
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
local ORDER = { "name", "class", "level", "experience", "need", "health", "maxHealth", "mana", "maxMana", "strength", "damage", "dealt", "resist",
	"taken", "gold", "stashed", "points", "earned", "spent", "act", "actName", "deepest", "cleared", "waypoints", "bosses", "kills", "deaths", "skills" }

function sheet.lines(h: any): { string }
	local n = sheet.numbers(h)
	local out = {}
	for _, k in ORDER do
		table.insert(out, k .. " " .. tostring(n[k]))
	end
	return out
end

-- The sheet to the hero's UI (only when it changed since it was last sent).
function sheet.view(h: any)
	local n = sheet.numbers(h)
	n.kind = "sheet"
	local parts = {}
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

return sheet
