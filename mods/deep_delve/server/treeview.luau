--!strict
-- The passive tree's window: its nodes sent once to each player's UI (in parts: id, place, kind,
-- region, name, what it does, its links), then the hero's state whenever it changes (the nodes
-- taken, points, the respec's cost and whether the hero stands by the fountain to pay it). A
-- click on a node takes it; the Respec button, by the fountain, gives every node back for gold.

local hero = require("hero")
local hud = require("hud")
local tree = require("tree")
local windows = require("windows")

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
		table.insert(list, { i = id, x = n.x, y = n.y, k = KIND[n.kind] or "s", c = REGION[n.region] or 0, n = name, d = what, l = links })
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
	local total, spent, left = tree.points(h.record)
	local near = windows.near(h, "healer")
	h.shownNear = near
	hud.send(h.peer, "treeState", {
		kind = "treeState",
		taken = table.concat(marks),
		total = total,
		spent = spent,
		points = left,
		cost = tree.respecCost(h.record),
		near = near,
		class = h.record.identity.class,
	})
end

-- The window shown: its nodes (once a player) and the hero's state.
function treeview.view(h: any)
	if not h.treeSent then
		h.treeSent = true
		hud.parts(h.peer, "treeData", windows.serial(), nodeEntries(), { count = tree.count() })
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

-- A message from the tree's window: { op = "take", id } | { op = "respec" }.
function treeview.message(h: any, v: any)
	local ok, why = true, nil
	local id = tonumber(v.id)
	if v.op == "take" and id then
		ok, why = treeview.take(h, id)
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
