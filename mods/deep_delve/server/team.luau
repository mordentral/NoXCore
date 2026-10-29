--!strict
-- The party's one team (by decision): every hero's unit on it, and every creature a hero summons,
-- so the game itself counts the party as one side wherever it asks who is whose enemy: the
-- monsters' and creatures' choice of a foe, the homing missiles' and pixies' targets, the
-- warrior's War Cry and harpoon, a melee swing's victims. Monsters have no team, so they stay
-- every member's enemy.
--
--   * Made once, the first time it's needed (a game has at most 16 teams), named "Party", in the
--     colour after the clients' nine team colours, so a member's name is drawn as a player's with
--     no team. Made again only if something removed it (the host's own team settings).
--   * A unit joins at its player's join and at each set-up (after a death, in a new map); the
--     engine keeps a member through a map change and lets go of a leaver, and joining twice does
--     nothing. A summoned creature joins as summons.luau finds it (the engine already puts a
--     team member's new creature on its team; this covers the rest).
--   * Damage is not the team's: combat.luau still cancels every hit within the party.
--   * mode.json pins the host's team options off (team damage, auto-assign, team play), so a
--     host can't take the team's protections away or have each map share players out afresh.
--
-- Nothing walks the world: nox.teams() is read only when a unit joins.

local team = {}

local NAME = "Party"
local COLOUR = 10

local id: number? = nil
local refused = false

-- The party's team, made when there is none (nil: the game had no room for it).
local function current(): number?
	if id then
		for _, t in nox.teams() do
			if t.id == id and t.name == NAME then
				return id
			end
		end
		id = nil
	end
	local made = nox.createTeam(NAME, COLOUR)
	if not made then
		if not refused then
			refused = true
			nox.log("deep_delve: no room for the party's team; the party's hits are still cancelled")
		end
		return nil
	end
	id = made
	return made
end

-- A hero's unit, or a hero's creature, onto the party's team.
function team.join(thing: number)
	if not nox.present(thing) then
		return
	end
	local t = current()
	if t then
		nox.joinTeam(thing, t)
	end
end

-- The game's start: the team made before anyone joins.
function team.start()
	current()
end

-- "dd team": the team ("team ID Party colour C score S", or "no team"), the host's team options
-- ("options teamDamage B autoAssign B teamPlay B") and each hero's unit, on it or not ("NAME on",
-- "NAME off"), by name.
function team.lines(heroes: any): { string }
	local out: { string } = {}
	local t: any = nil
	for _, row in nox.teams() do
		if row.id == id and row.name == NAME then
			t = row
		end
	end
	table.insert(out, if t then string.format("team %d %s colour %d score %d", t.id, t.name, t.colour, t.score) else "no team")
	local g = nox.gameplay()
	table.insert(out, string.format("options teamDamage %s autoAssign %s teamPlay %s", tostring(g.teamDamage), tostring(g.autoAssign), tostring(g.teamPlay)))
	local rows: { string } = {}
	for _, h in heroes do
		local o = nox.present(h.unit) and nox.object(h.unit) or nil
		local on = t ~= nil and o ~= nil and o.team == t.id
		table.insert(rows, tostring(h.record.identity.name) .. (if on then " on" else " off"))
	end
	table.sort(rows)
	for _, r in rows do
		table.insert(out, r)
	end
	return out
end

return team
