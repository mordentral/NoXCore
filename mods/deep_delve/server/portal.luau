--!strict
-- The town portal: back to town at any time, and back out to where you were.
--
--   * Opening: the Town portal button in the player's Deep Delve window (ui/main.luau sends
--     { kind = "portal" }), or saying "portal". Out in the area, the portal opens where the player
--     stands and the player goes home at once. In town it does nothing.
--   * Where it stands: the portal's violet ring on the floor (decals.luau, as on the town's
--     portal pentagram) and violet sparks every so often, so it reads as a way through. Each
--     player has one at a time; a new one moves it.
--   * Back out: the town's portal pentagram (an invisible one under the violet ring, east of the
--     fountain) takes anyone in the party through any open portal: the toucher's own straight away; with only another's open,
--     through that one; with several, an offer of them. Its owner going back through it closes it;
--     the others may use it as often as they like while it's open.
--   * It lasts until its owner goes back through it, its owner opens another, or the map changes (a
--     portal leads only within its map). Its owner's death leaves it open, and so does its owner
--     leaving the game: it stays for the others, anyone's to use, until the map changes.
--
-- An open portal is kept on its owner's hero (hero.luau's `portal`); one whose owner left is kept
-- here (`left`), with its owner's name, until the map changes. One shared beat sparks them all.

local decals = require("decals")
local hero = require("hero")
local hud = require("hud")
local offers = require("offers")
local town = require("town")
local touch = require("touch")
local numbers = require("data/numbers")

local portal = {}

local beat: number? = nil
-- The portals whose owners left the game: { name, x, y }, in the order they left.
local left: { any } = {}

-- An open portal as it's offered and taken: { name, x, y, owner (its hero; nil: it left) }.
local function entryOf(h: any): any
	return { name = tostring(h.record.identity.name), x = h.portal.x, y = h.portal.y, owner = h }
end

local function sparks()
	local any = false
	for _, h in hero.all() do
		local p = h.portal
		if p then
			nox.effect(numbers.portal.sparks, p.x, p.y)
			any = true
		end
	end
	for _, p in left do
		nox.effect(numbers.portal.sparks, p.x, p.y)
		any = true
	end
	if not any and beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

local function sparking()
	if not beat then
		beat = nox.every(numbers.portal.every, sparks)
	end
end

-- A hero opens a town portal; what came of it ("opened", "in town", "not now").
function portal.open(h: any): string
	if not h or not h.ready then
		return "not now"
	end
	local o = nox.object(h.unit)
	if not o or not o.alive then
		return "not now"
	end
	if town.contains(o.x, o.y) then
		town.say(h.unit, "You're in town already.")
		return "in town"
	end
	if h.portal then
		decals.remove(h.portal.decal) -- (its old one closes)
	end
	h.portal = { x = o.x, y = o.y, decal = decals.put("portal", o.x, o.y) }
	nox.effect(0x89, o.x, o.y) -- (MSG_FX_TELEPORT)
	town.sendHome(h.unit)
	nox.tell(h.unit, "Your portal stays open where you stood: the violet ring in town takes you (and your party) back.")
	sparking()
	hud.hero(h)
	return "opened"
end

-- The open portals as `h` is offered them: its own first, then the others' by peer, then those
-- whose owners left, in the order they left.
function portal.openFor(h: any): { any }
	local owned = {}
	for _, owner in hero.all() do
		if owner.portal then
			table.insert(owned, owner)
		end
	end
	table.sort(owned, function(a: any, b: any): boolean
		if (a == h) ~= (b == h) then
			return a == h
		end
		return a.peer < b.peer
	end)
	local list = {}
	for _, owner in owned do
		table.insert(list, entryOf(owner))
	end
	for _, p in left do
		table.insert(list, { name = p.name, x = p.x, y = p.y, owner = nil, gone = p })
	end
	return list
end

-- Whether an entry of openFor's is still open.
local function stillOpen(e: any): boolean
	if e.owner then
		return e.owner.portal ~= nil and hero.byPeer(e.owner.peer) == e.owner
	end
	return table.find(left, e.gone) ~= nil
end

-- A hero through an open portal (an entry of openFor's; its owner's own closes behind it);
-- whether it went.
function portal.through(h: any, e: any): boolean
	if not h or not e or not stillOpen(e) or not nox.present(h.unit) then
		return false
	end
	-- (an owner's portal where it stands now: it may have opened another since the offer)
	local at = if e.owner then e.owner.portal else e
	local x0, y0 = at.x, at.y
	if e.owner == h then
		decals.remove(h.portal.decal)
		h.portal = nil
		hud.hero(h)
	end
	local x, y = nox.clearPointNear(x0, y0, 40)
	nox.setPosition(h.unit, x, y)
	nox.effect(0x89, x, y) -- (MSG_FX_TELEPORT)
	return true
end

-- The town's portal pentagram touched: out through an open portal (see the top).
function portal.touched(unit: number)
	local h = hero.byUnit(unit)
	if not h then
		return
	end
	local list = portal.openFor(h)
	if #list == 0 then
		town.say(unit, "No portal is open. Open one out in the field (the Town portal button, or say \"portal\").")
		return
	end
	if list[1].owner == h or #list == 1 then
		portal.through(h, list[1])
		return
	end
	local since = offers.since(h.peer)
	if since and nox.frame() - since < 30 * 10 then
		return
	end
	local answers = {}
	for i, e in list do
		if i <= 9 then
			table.insert(answers, e.name .. "'s portal")
		end
	end
	local peer = h.peer
	offers.make(peer, "Portals", answers, function(i: number)
		local who = hero.byPeer(peer)
		local e = list[i]
		-- (still in town; through() checks the portal is still open)
		if who and e and nox.present(who.unit) then
			local x, y = nox.position(who.unit)
			if town.contains(x, y) then
				portal.through(who, e)
			end
		end
	end)
end

-- A player leaving the game (before its hero goes): its open portal stays for the others.
function portal.left(h: any)
	local p = h and h.portal
	if not p then
		return
	end
	table.insert(left, { name = tostring(h.record.identity.name), x = p.x, y = p.y, decal = p.decal })
	h.portal = nil
	sparking()
end

-- A map's town: its portal pentagram set to work.
function portal.place(townSpots: any)
	touch.adopt("InvisibleTeleportPentagram", townSpots.portal.x, townSpots.portal.y, portal.touched, 45)
end

-- The open portals as lines, "owner x y" (an owner who left: "owner(left) x y"; for "dd portals").
function portal.lines(): { string }
	local out = {}
	for _, h in hero.all() do
		if h.portal then
			table.insert(out, string.format("%s %.1f %.1f", tostring(h.record.identity.name), tonumber(h.portal.x) or 0, tonumber(h.portal.y) or 0))
		end
	end
	for _, p in left do
		table.insert(out, string.format("%s(left) %.1f %.1f", tostring(p.name), tonumber(p.x) or 0, tonumber(p.y) or 0))
	end
	table.sort(out)
	return out
end

-- A new map: every portal closed (hero.mapChanging forgets them on the heroes; the ones whose
-- owners left go here), and every offer of them with it.
function portal.reset()
	left = {}
	offers.forget()
	if beat then
		nox.cancelTimer(beat)
		beat = nil
	end
end

return portal
