--!strict
-- The passive tree's window: its nodes sent once to each player's UI (in parts: id, place, kind,
-- region, name, what it does, its links, its picture or the game's icon's name, and for a spell,
-- ability or summon node which classes may use it; with them each class's stats' worth in words),
-- then the hero's state whenever it changes (the nodes taken, points, the respec's cost, which
-- nodes may be given back alone and for what, and whether the hero stands by the fountain for a
-- respec). A click on a node takes it; a right click on one taken gives it back alone for gold,
-- anywhere; the Respec button, by the fountain, gives every node back for gold.

local hero = require("hero")
local hud = require("hud")
local tree = require("tree")
local windows = require("windows")
local treeData = require("data/tree")

local treeview = {}

local KIND: { [string]: string } = { root = "r", small = "s", notable = "n", keystone = "k", spell = "p", ability = "a", summon = "c" }
local REGION: { [string]: number } = { middle = 0, warrior = 1, wizard = 2, conjurer = 3 }

-- The nodes as the UI takes them, worked out once.
local entries: { any }? = nil
local function nodeEntries(): { any }
	if entries then
		return entries
	end
	local list = {}
	for id = 1, tree.count() do
		local n = tree.node(id)
		local name, what = tree.describe(n)
		local links = {}
		for _, other in n.links do
			table.insert(links, other)
		end
		-- (ic: its picture's short name, the asset "tree/<ic>", for a root, small, notable or
		-- keystone; si: the game's icon's name for a spell, ability or summon, for nox.spellIcon;
		-- u: for a spell, ability or summon, the classes that may use it, 1 warrior + 2 wizard + 4
		-- conjurer)
		table.insert(list, { i = id, x = n.x, y = n.y, k = KIND[n.kind] or "s", c = REGION[n.region] or 0, n = name, d = what, l = links, ic = n.icon,
			si = n.spellIcon, u = tree.usableMask(n) })
	end
	entries = list
	return list
end

-- The hero's state on the tree.
function treeview.state(h: any)
	local taken = tree.taken(h.record)
	local marks = table.create(tree.count(), "0")
	for id in taken do
		marks[id] = "1"
	end
	-- (each node taken: "1" may be given back alone, "h" others hang on it; the root "r")
	local refund = table.create(tree.count(), "0")
	for id in taken do
		refund[id] = if tree.canRefund(h.record, id) then "1" elseif id == treeData.roots[h.record.identity.class] then "r" else "h"
	end
	local total, spent, left = tree.points(h.record)
	local near = windows.near(h, "healer")
	h.shownNear = near
	hud.send(h.peer, "treeState", {
		kind = "treeState",
		taken = table.concat(marks),
		refund = table.concat(refund),
		total = total,
		spent = spent,
		points = left,
		cost = tree.respecCost(h.record),
		refundCost = tree.refundCost(h.record),
		near = near,
		class = h.record.identity.class,
	})
end

-- Each class's stats' worth in words (what a spell, ability or summon node it can't use gives it).
local function worthWords(): any
	local w = {}
	for _, class in { "warrior", "wizard", "conjurer" } do
		w[class] = tree.worthText(class)
	end
	return w
end

-- The window shown: its nodes (once a player) and the hero's state.
function treeview.view(h: any)
	if not h.treeSent then
		h.treeSent = true
		hud.parts(h.peer, "treeData", windows.serial(), nodeEntries(), { count = tree.count(), worth = worthWords() })
	end
	treeview.state(h)
end

-- Node `id` taken by the hero; whether it was, else why.
function treeview.take(h: any, id: number): (boolean, string?)
	if not h.ready then
		return false, "not now"
	end
	local ok, why = tree.take(h.record, id)
	if not ok then
		return false, why
	end
	hero.treeChanged(h, "passive tree")
	if windows.isOpen(h, "tree") then
		treeview.state(h)
	end
	return true, nil
end

-- Every node given back, by the fountain, for gold; whether it was, else why.
function treeview.respec(h: any): (boolean, string?)
	if not h.ready then
		return false, "not now"
	end
	if not windows.near(h, "healer") then
		return false, "a respec is bought at the town's fountain"
	end
	local _, spent = tree.points(h.record)
	if spent == 0 then
		return false, "no nodes to give back"
	end
	local cost = tree.respecCost(h.record)
	local gold = nox.gold(h.unit)
	if gold < cost then
		return false, "not enough gold (" .. cost .. ")"
	end
	nox.setGold(h.unit, gold - cost, true)
	tree.respec(h.record)
	hero.treeChanged(h, "respec")
	if windows.isOpen(h, "tree") then
		treeview.state(h)
	end
	return true, nil
end

-- Node `id` given back alone, anywhere, for gold (its spell, ability or creature taken
-- back as a respec takes them: hero.treeChanged tells the book again); whether it was, else why.
function treeview.refund(h: any, id: number): (boolean, string?)
	if not h.ready then
		return false, "not now"
	end
	local can, cant = tree.canRefund(h.record, id)
	if not can then
		return false, cant
	end
	local cost = tree.refundCost(h.record)
	local gold = nox.gold(h.unit)
	if gold < cost then
		return false, "not enough gold (" .. cost .. ")"
	end
	nox.setGold(h.unit, gold - cost, true)
	tree.refund(h.record, id)
	hero.treeChanged(h, "node given back")
	if windows.isOpen(h, "tree") then
		treeview.state(h)
	end
	return true, nil
end

-- A message from the tree's window: { op = "take", id } | { op = "refund", id } |
-- { op = "respec" }.
function treeview.message(h: any, v: any)
	local ok, why = true, nil
	local id = tonumber(v.id)
	if v.op == "take" and id then
		ok, why = treeview.take(h, id)
	elseif v.op == "refund" and id then
		ok, why = treeview.refund(h, id)
	elseif v.op == "respec" then
		ok, why = treeview.respec(h)
	end
	if not ok then
		windows.note(h, "tree", tostring(why))
	end
end

-- Kept current while open: whether the hero stands by the fountain.
local function beat(h: any)
	if windows.near(h, "healer") ~= h.shownNear then
		treeview.state(h)
	end
end

windows.register("tree", { view = treeview.view, beat = beat })

return treeview
