--!strict
-- Deep Delve's UI on a NoX Core client (UI API 2). The server sends the numbers (server/hud.luau
-- and the window modules windows.luau names) when they change; this draws them and sends the
-- clicks back. Its windows:
--
--   hero    the character's name, level and experience, the area, the Town portal, Tree and
--           Sheet buttons, the act (cleared or not);
--   boss    while an act boss lives: a bar with its health under its name;
--   stash   (touching the town's chest) the stash's tabs and what the character carries: a click
--           moves an item across;
--   vendor  (touching the town's anvil) its stock and what the character carries: a click buys or
--           sells, a right click rerolls a magic or rare piece;
--   tree    the passive tree: panning (the arrows, on screen or the keys), zoom (1-3), nodes and
--           the lines between them, a node's words under the mouse, a click takes it; Respec by
--           the fountain;
--   sheet   the character sheet.
--
-- A list too long for one value comes in parts with a serial (hud.parts); the newest whole list
-- is drawn, the one before it until the next is whole.
--
-- What the toolbox doesn't give a window, worked round here: no lines but boxes (the tree's links
-- are drawn square, across then down), no dragging (items move by a click, the tree pans by
-- buttons and keys), no hover text outside a window (an item's or a node's words are written
-- at its window's foot).

local ui = {}

local W, H = 168, 84
local BOSS_W, BOSS_H = 220, 26

local shown: any = nil
local bossShown: any = nil
local opened: { [string]: boolean } = {}
local notes: { [string]: any } = {}

-- ---- Lists in parts

-- kind -> the newest whole list { header, items }; and the one coming in.
local whole: { [string]: any } = {}
local coming: { [string]: any } = {}

local function takePart(v: any)
	local kind = v.kind
	local c = coming[kind]
	if not c or v.serial > c.serial then
		c = { serial = v.serial, parts = v.parts, got = {}, n = 0 }
		coming[kind] = c
	end
	if v.serial < c.serial or c.got[v.part] then
		return
	end
	c.got[v.part] = v.list
	c.n += 1
	c.header = v
	if c.n >= c.parts then
		local items = {}
		for i = 1, c.parts do
			for _, e in c.got[i] or {} do
				table.insert(items, e)
			end
		end
		whole[kind] = { header = c.header, items = items, serial = c.serial }
	end
end

local PARTS: { [string]: boolean } = { carried = true, stash = true, stock = true, treeData = true }

-- ---- Drawing helpers

local RARITY: { [string]: { number } } = {
	normal = { 150, 138, 112 },
	magic = { 100, 150, 245 },
	rare = { 240, 205, 70 },
	unique = { 195, 120, 255 },
}

-- Clickable places drawn this frame, by window: { x, y, w, h, fn(button) }.
local zones: { [string]: { any } } = {}

local function zone(window: string, x: number, y: number, w: number, h: number, fn: any)
	local list = zones[window]
	if list then
		table.insert(list, { x = x, y = y, w = w, h = h, fn = fn })
	end
end

local function button(window: string, x: number, y: number, w: number, h: number, label: string, on: boolean, fn: any)
	nox.box(x, y, w, h, if on then { 60, 44, 24 } else { 30, 26, 20 })
	nox.outline(x, y, w, h, if on then { 200, 150, 60 } else "grey")
	nox.text(label, x + (w - nox.textWidth(label)) // 2, y + (h - 10) // 2, if on then "bright" else "grey")
	if on then
		zone(window, x, y, w, h, fn)
	end
end

local function closeBox(window: string, w: number)
	button(window, w - 16, 2, 14, 13, "x", true, function()
		opened[window] = nil
		nox.closeWindow(window)
		nox.send({ kind = "close", window = window })
	end)
end

-- Words cut into lines no wider than `width`.
local function wrap(text: string, width: number): { string }
	local lines = {}
	local line = ""
	for word in string.gmatch(text, "%S+") do
		local try = if line == "" then word else line .. " " .. word
		if nox.textWidth(try) > width and line ~= "" then
			table.insert(lines, line)
			line = word
		else
			line = try
		end
	end
	if line ~= "" then
		table.insert(lines, line)
	end
	return lines
end

local function mouseIn(window: string): (number?, number?)
	local w, x, y = nox.mouse()
	if w == window then
		return x, y
	end
	return nil, nil
end

local function over(mx: number?, my: number?, x: number, y: number, w: number, h: number): boolean
	return mx ~= nil and my ~= nil and mx >= x and mx < x + w and my >= y and my < y + h
end

local function drawNote(window: string, x: number, y: number)
	local n = notes[window]
	if n and nox.time() < n.til then
		nox.text(n.text, x, y, "red")
	end
end

-- An item's cell: its frame in its rarity's colour, its picture, a price under it.
local function cell(x: number, y: number, size: number, e: any, hot: boolean, price: number?)
	nox.box(x, y, size, size, if hot then { 50, 40, 26 } else { 22, 18, 14 })
	nox.outline(x, y, size, size, RARITY[e.r] or RARITY.normal)
	pcall(nox.thing, e.t, x + size // 2, y + size // 2 - (if price then 4 else 0))
	if price then
		local s = tostring(price)
		nox.text(s, x + size - 2 - nox.textWidth(s), y + size - 11, "gold")
	end
end

local function emptyCell(x: number, y: number, size: number)
	nox.box(x, y, size, size, { 16, 13, 10 })
	nox.outline(x, y, size, size, { 60, 52, 40 })
end

-- The item lines at a window's foot.
local function drawWords(text: string?, x: number, y: number, width: number, lines: number, colour: any)
	if not text then
		return
	end
	for i, l in wrap(text, width) do
		if i > lines then
			break
		end
		nox.text(l, x, y + (i - 1) * 11, colour or "bright")
	end
end

-- ---- The hero's window

local function openTreeOrSheet(name: string)
	if opened[name] then
		opened[name] = nil
		nox.closeWindow(name)
		nox.send({ kind = "close", window = name })
		return
	end
	nox.send({ kind = "open", window = name, fresh = name == "tree" and whole.treeData == nil })
end

local function drawHero()
	if not shown then
		nox.text("...", 4, 4, "dim")
		return
	end
	nox.text(string.format("%s, level %d", shown.name, shown.level), 4, 2, "gold")
	local need = math.max(1, shown.need)
	local fill = math.floor((W - 8) * math.clamp(shown.experience / need, 0, 1))
	nox.box(4, 15, W - 8, 6, { 40, 30, 20 })
	nox.box(4, 15, fill, 6, { 200, 160, 60 })
	nox.outline(4, 15, W - 8, 6, "dim")
	nox.text(string.format("%s, area level %d", shown.area, shown.areaLevel), 4, 25, "bright")
	-- The Town portal button (an open portal: the way back is the town's violet pentagram).
	local open = shown.portal == true
	nox.box(4, 38, W - 8, 13, if open then { 50, 30, 70 } else { 60, 40, 90 })
	nox.outline(4, 38, W - 8, 13, if open then "dim" else { 190, 120, 255 })
	local label = if open then "Portal open" else "Town portal"
	nox.text(label, 4 + (W - 8 - nox.textWidth(label)) // 2, 40, if open then "dim" else "bright")
	if not open then
		zone("hero", 4, 38, W - 8, 13, function()
			nox.send({ kind = "portal" })
		end)
	end
	local points = shown.points or 0
	button("hero", 4, 54, 78, 13, if points > 0 then "Tree (" .. points .. ")" else "Tree", true, function()
		openTreeOrSheet("tree")
	end)
	button("hero", 86, 54, 78, 13, "Sheet", true, function()
		openTreeOrSheet("sheet")
	end)
	if shown.act then
		local line: string = "Act " .. tostring(shown.act) .. (if shown.cleared then " cleared" else "")
		nox.text(line, 4, 70, if shown.cleared then "gold" else "dim")
	end
end

local function drawBoss()
	local v = bossShown
	if not v then
		return
	end
	local w = BOSS_W - 8
	local fill = math.floor(w * math.clamp(v.health / math.max(1, v.most), 0, 1))
	nox.box(4, 6, w, 10, { 50, 10, 10 })
	nox.box(4, 6, fill, 10, { 200, 30, 30 })
	nox.outline(4, 6, w, 10, "dim")
end

-- ---- The stash and the vendor

local SW, SH = 568, 410
local CELL = 44

local stashTab = 1

-- The carried grid at (x, y), `cols` wide and `rows` high; returns the entry under the mouse.
local function drawCarried(window: string, x: number, y: number, cols: number, rows: number, priced: boolean, onClick: any): any
	local mx, my = mouseIn(window)
	local list = whole.carried and whole.carried.items or {}
	local hot = nil
	for k = 1, cols * rows do
		local cx = x + ((k - 1) % cols) * CELL
		local cy = y + ((k - 1) // cols) * CELL
		local e = list[k]
		if e then
			local isHot = over(mx, my, cx, cy, CELL - 2, CELL - 2)
			if isHot then
				hot = e
			end
			cell(cx, cy, CELL - 2, e, isHot, if priced then e.p else nil)
			zone(window, cx, cy, CELL - 2, CELL - 2, function(b: number)
				onClick(e, b)
			end)
		else
			emptyCell(cx, cy, CELL - 2)
		end
	end
	return hot
end

local function drawStash()
	local w = "stash"
	closeBox(w, SW)
	local s = whole.stash
	local header = s and s.header or { tab = 1, tabs = 4, size = 40, stashed = 0, counts = {} }
	stashTab = header.tab or stashTab
	for t = 1, header.tabs or 4 do
		local count = header.counts and header.counts[t] or 0
		local label = "Tab " .. t .. " (" .. count .. ")"
		local x = 4 + (t - 1) * 76
		nox.box(x, 2, 72, 14, if t == stashTab then { 70, 52, 26 } else { 30, 26, 20 })
		nox.outline(x, 2, 72, 14, if t == stashTab then "gold" else "grey")
		nox.text(label, x + (72 - nox.textWidth(label)) // 2, 4, if t == stashTab then "bright" else "grey")
		zone(w, x, 2, 72, 14, function()
			nox.send({ kind = "stash", op = "tab", tab = t })
		end)
	end
	local carriedGold = whole.carried and whole.carried.header.gold or 0
	nox.text(string.format("Gold: %d carried, %d stashed", carriedGold, header.stashed or 0), 4, 21, "gold")
	button(w, 300, 19, 74, 14, "Deposit", carriedGold > 0, function()
		nox.send({ kind = "stash", op = "deposit" })
	end)
	button(w, 380, 19, 74, 14, "Withdraw", (header.stashed or 0) > 0, function()
		nox.send({ kind = "stash", op = "withdraw" })
	end)
	-- The tab's slots.
	local mx, my = mouseIn(w)
	local bySlot: { [number]: any } = {}
	for _, e in (s and s.items or {}) do
		bySlot[e.s] = e
	end
	local hot: any = nil
	local size = header.size or 40
	for slot = 1, size do
		local cx = 4 + ((slot - 1) % 10) * CELL
		local cy = 38 + ((slot - 1) // 10) * CELL
		local e = bySlot[slot]
		if e then
			local isHot = over(mx, my, cx, cy, CELL - 2, CELL - 2)
			if isHot then
				hot = e
			end
			cell(cx, cy, CELL - 2, e, isHot)
			zone(w, cx, cy, CELL - 2, CELL - 2, function()
				nox.send({ kind = "stash", op = "take", tab = stashTab, slot = slot })
			end)
		else
			emptyCell(cx, cy, CELL - 2)
		end
	end
	for i, l in { "Your own stash, kept", "with your character.", "", "Click a stashed item to", "take it; click one you", "carry to stash it in", "this tab.", "", "Worn things are taken", "off first." } do
		nox.text(l, 452, 38 + (i - 1) * 11, "dim")
	end
	nox.text("Carried", 4, 218, "gold")
	local hotCarried = drawCarried(w, 4, 230, 12, 3, false, function(e: any)
		nox.send({ kind = "stash", op = "put", item = e.i, tab = stashTab })
	end)
	nox.box(0, 366, SW, 1, { 90, 70, 40 })
	local words = if hot then hot.d elseif hotCarried then hotCarried.d else nil
	drawWords(words, 4, 370, SW - 8, 3, RARITY[(hot or hotCarried or {}).r or "normal"])
	drawNote(w, 4, 396)
end

local function drawVendor()
	local w = "vendor"
	closeBox(w, SW)
	local st = whole.stock
	nox.text(string.format("For sale (area level %d)", st and st.header.level or 1), 4, 4, "gold")
	local mx, my = mouseIn(w)
	local hotStock: any = nil
	local list = st and st.items or {}
	for k = 1, 20 do
		local cx = 4 + ((k - 1) % 10) * CELL
		local cy = 18 + ((k - 1) // 10) * CELL
		local e = list[k]
		if e then
			local isHot = over(mx, my, cx, cy, CELL - 2, CELL - 2)
			if isHot then
				hotStock = e
			end
			cell(cx, cy, CELL - 2, e, isHot, e.p)
			zone(w, cx, cy, CELL - 2, CELL - 2, function(b: number)
				if b == 1 then
					nox.send({ kind = "vendor", op = "buy", index = e.i })
				end
			end)
		else
			emptyCell(cx, cy, CELL - 2)
		end
	end
	local gold = whole.carried and whole.carried.header.gold or 0
	nox.text(string.format("Your gold: %d", gold), 452, 18, "gold")
	for i, l in { "Click to buy.", "", "Click a piece you", "carry to sell it.", "Right click a magic or", "rare piece to reroll", "its modifiers." } do
		nox.text(l, 452, 32 + (i - 1) * 11, "dim")
	end
	nox.text("Carried (the vendor's price)", 4, 110, "gold")
	local hotCarried = drawCarried(w, 4, 122, 12, 4, true, function(e: any, b: number)
		if b == 2 then
			nox.send({ kind = "vendor", op = "reroll", item = e.i })
		else
			nox.send({ kind = "vendor", op = "sell", item = e.i })
		end
	end)
	nox.box(0, 302, SW, 1, { 90, 70, 40 })
	local hot = hotStock or hotCarried
	if hot then
		drawWords(hot.d, 4, 306, SW - 8, 3, RARITY[hot.r] or RARITY.normal)
		local line = if hotStock then "Buy for " .. tostring(hot.p) .. " gold"
			else "Sells for " .. tostring(hot.p) .. " gold" .. (if hot.q then "; reroll " .. tostring(hot.q) .. " gold" else "")
		nox.text(line, 4, 342, "gold")
	end
	drawNote(w, 4, 396)
end

-- ---- The passive tree

local TW, TH = 600, 420
local CANVAS_H = 344
local ZOOMS = { 18, 26, 36 }
local zoom = 2
local panX, panY = 0, 0
local centred = false

local REGION_COLOUR: { [number]: { number } } = {
	[0] = { 200, 170, 110 },
	[1] = { 215, 95, 75 },
	[2] = { 95, 145, 235 },
	[3] = { 95, 205, 115 },
}
local CLASS_REGION: { [string]: number } = { warrior = 1, wizard = 2, conjurer = 3 }
local SIZE: { [string]: number } = { r = 14, s = 8, n = 12, k = 16, p = 12, a = 12, c = 12 }

local nodeIndex: any = nil -- id -> node, made when a new tree comes
local nodeNear: { [number]: { number } } = {} -- id -> its neighbours, both ways
local nodeIndexOf: number = -1

local function treeNodes(): any
	local d = whole.treeData
	if not d then
		return nil
	end
	if nodeIndexOf ~= d.serial then
		nodeIndex = {}
		nodeNear = {}
		for _, n in d.items do
			nodeIndex[n.i] = n
			nodeNear[n.i] = nodeNear[n.i] or {}
			for _, id in n.l or {} do
				nodeNear[id] = nodeNear[id] or {}
				table.insert(nodeNear[n.i], id)
				table.insert(nodeNear[id], n.i)
			end
		end
		nodeIndexOf = d.serial
		centred = false
	end
	return nodeIndex
end

local function centre()
	local st = whole.treeState
	local nodes = treeNodes()
	if not st or not nodes then
		return
	end
	-- (on the class's root: the region's node of kind "r")
	local region = CLASS_REGION[st.header and st.header.class or ""] or 0
	for _, n in nodes do
		if n.k == "r" and n.c == region then
			local unit = ZOOMS[zoom]
			panX, panY = -n.x * unit, -n.y * unit
		end
	end
	centred = true
end

local function pan(dx: number, dy: number)
	local unit = ZOOMS[zoom]
	panX += dx * unit * 3
	panY += dy * unit * 3
end

local function setZoom(z: number)
	local old = ZOOMS[zoom]
	zoom = math.clamp(z, 1, #ZOOMS)
	local new = ZOOMS[zoom]
	panX, panY = panX * new / old, panY * new / old
end

-- A box clipped to the canvas.
local function clipBox(x: number, y: number, w: number, h: number, colour: any)
	local x2, y2 = math.min(x + w, TW), math.min(y + h, CANVAS_H)
	x, y = math.max(x, 0), math.max(y, 0)
	if x2 > x and y2 > y then
		nox.box(x, y, x2 - x, y2 - y, colour)
	end
end

local function drawTree()
	local w = "tree"
	local nodes = treeNodes()
	local st = whole.treeState
	if not nodes or not st then
		nox.text("The tree is on its way...", 8, 8, "dim")
		closeBox(w, TW)
		return
	end
	local state = st.header
	if not centred then
		centre()
	end
	local unit = ZOOMS[zoom]
	local ox, oy = TW / 2 + panX, CANVAS_H / 2 + panY
	nox.box(0, 0, TW, CANVAS_H, { 9, 7, 5, 215 })
	local taken = state.taken or ""
	local function isTaken(id: number): boolean
		return string.sub(taken, id, id) == "1"
	end
	local function at(n: any): (number, number)
		return ox + n.x * unit, oy + n.y * unit
	end
	-- The links: across, then down (the toolbox draws boxes, not lines).
	for _, n in nodes do
		local x1, y1 = at(n)
		for _, id in n.l or {} do
			local m = nodes[id]
			if m then
				local x2, y2 = at(m)
				local lit = isTaken(n.i) and isTaken(m.i)
				local col = if lit then { 230, 190, 90 } else { 70, 64, 54 }
				clipBox(math.min(x1, x2), y1 - 1, math.abs(x2 - x1) + 1, 2, col)
				clipBox(x2 - 1, math.min(y1, y2), 2, math.abs(y2 - y1) + 1, col)
			end
		end
	end
	-- The nodes.
	local mx, my = mouseIn(w)
	local hot: any = nil
	for _, n in nodes do
		local x, y = at(n)
		local s = SIZE[n.k] or 8
		if x > -s and x < TW + s and y > -s and y < CANVAS_H + s then
			local have = isTaken(n.i)
			local open = false
			if not have then
				for _, id in nodeNear[n.i] or {} do
					open = open or isTaken(id)
				end
			end
			local rc = REGION_COLOUR[n.c] or REGION_COLOUR[0]
			local fillC = if have then { 235, 195, 90 } elseif open then { rc[1] // 2, rc[2] // 2, rc[3] // 2 } else { 34, 30, 26 }
			local edge = if have then "white" elseif open then rc else { 90, 80, 66 }
			local bx, by = x - s / 2, y - s / 2
			clipBox(bx, by, s, s, fillC)
			if by >= 0 and by + s <= CANVAS_H then
				nox.outline(bx, by, s, s, edge)
				if n.k == "p" or n.k == "a" then
					nox.outline(bx + 3, by + 3, s - 6, s - 6, if have then { 120, 60, 20 } else rc)
				elseif n.k == "c" then
					-- (a summon: a solid heart where a spell has a ring)
					clipBox(bx + 4, by + 4, s - 8, s - 8, if have then { 120, 60, 20 } else rc)
				elseif n.k == "k" then
					nox.outline(bx - 2, by - 2, s + 4, s + 4, if have then "gold" else { 160, 60, 50 })
				end
			end
			if over(mx, my, bx - 2, by - 2, s + 4, s + 4) and (my or 0) < CANVAS_H then
				hot = n
				nox.outline(bx - 3, by - 3, s + 6, s + 6, "bright")
			end
			if (not have) and by >= 0 and by + s <= CANVAS_H then
				zone(w, bx - 2, by - 2, s + 4, s + 4, function()
					nox.send({ kind = "tree", op = "take", id = n.i })
				end)
			end
		end
	end
	-- The foot: the node under the mouse, the points, the buttons.
	nox.box(0, CANVAS_H, TW, TH - CANVAS_H, { 14, 11, 8 })
	nox.box(0, CANVAS_H, TW, 1, { 110, 84, 44 })
	if hot then
		nox.text(hot.n, 4, CANVAS_H + 4, RARITY.rare)
		local status = if isTaken(hot.i) then "(yours)" else "(click to take: 1 point)"
		nox.text(status, 8 + nox.textWidth(hot.n), CANVAS_H + 4, "dim")
		drawWords(hot.d, 4, CANVAS_H + 16, TW - 8, 2, "bright")
	else
		nox.text("A node next to one you have takes a point. Spells and summons come from their branch's nodes.", 4, CANVAS_H + 4, "dim")
	end
	nox.text(string.format("Points left: %d of %d", state.points or 0, state.total or 0), 4, CANVAS_H + 42, "gold")
	local cost = state.cost or 0
	button(w, 150, CANVAS_H + 40, 150, 14, "Respec (" .. cost .. " gold)", state.near == true and (state.spent or 0) > 0, function()
		nox.send({ kind = "tree", op = "respec" })
	end)
	if state.near ~= true then
		nox.text("(at the fountain)", 304, CANVAS_H + 42, "dim")
	end
	local bx = TW - 196
	button(w, bx, CANVAS_H + 58, 22, 14, "<", true, function() pan(1, 0) end)
	button(w, bx + 24, CANVAS_H + 58, 22, 14, "^", true, function() pan(0, 1) end)
	button(w, bx + 48, CANVAS_H + 58, 22, 14, "v", true, function() pan(0, -1) end)
	button(w, bx + 72, CANVAS_H + 58, 22, 14, ">", true, function() pan(-1, 0) end)
	button(w, bx + 98, CANVAS_H + 58, 22, 14, "+", zoom < #ZOOMS, function() setZoom(zoom + 1) end)
	button(w, bx + 122, CANVAS_H + 58, 22, 14, "-", zoom > 1, function() setZoom(zoom - 1) end)
	button(w, bx + 148, CANVAS_H + 58, 44, 14, "Home", true, centre)
	nox.text("Arrows pan, 1-3 zoom, 0 home", 4, CANVAS_H + 60, "dim")
	drawNote(w, 300, CANVAS_H + 60)
	closeBox(w, TW)
end

-- ---- The character sheet

local HW, HH = 260, 330

local function drawSheet()
	local w = "sheet"
	closeBox(w, HW)
	local s = whole.sheet
	if not s then
		nox.text("...", 4, 4, "dim")
		return
	end
	nox.text(string.format("%s, level %d %s", s.name, s.level, s.class), 4, 2, "gold")
	local rows = {
		{ "Experience", string.format("%d / %d", s.experience, s.need) },
		{ "Health", string.format("%d / %d", s.health, s.maxHealth) },
		{ "Mana", string.format("%d / %d", s.mana, s.maxMana) },
		{ "Strength", tostring(s.strength) },
		{ "Damage dealt", string.format("%d%% (%+d%%)", s.dealt, s.damage) },
		{ "Resistance", string.format("%d%% (takes %d%%)", s.resist, s.taken) },
		{ "Gold", string.format("%d carried, %d stashed", s.gold, s.stashed) },
		{ "Passive points", string.format("%d left of %d", s.points, s.earned) },
		{ "Act", string.format("%d, %s", s.act, s.actName) },
		{ "Deepest area", tostring(s.deepest) },
		{ "Act cleared", if s.cleared then "yes" else "not yet" },
		{ "Waypoints", if s.waypoints ~= "" then s.waypoints else "none" },
		{ "Boss kills", tostring(s.bosses) },
		{ "Kills", tostring(s.kills) },
		{ "Deaths", tostring(s.deaths) },
		{ "Time played", string.format("%d min", s.minutes or 0) },
	}
	for i, r in rows do
		local y = 20 + (i - 1) * 13
		nox.text(r[1], 4, y, "dim")
		nox.text(r[2], 110, y, "bright")
	end
	local y = 20 + #rows * 13 + 4
	nox.text("Spells and abilities", 4, y, "gold")
	drawWords(if s.skills ~= "" then s.skills else "none yet: take them in the Tree", 4, y + 13, HW - 8, 6, "bright")
end

-- ---- The hooks

function ui.start()
	nox.window("hero", { title = "Character", x = 6, y = 24, w = W, h = H })
end

local SPEC: { [string]: any } = {
	stash = { title = "Stash", x = 36, y = 34, w = SW, h = SH },
	vendor = { title = "Vendor", x = 36, y = 34, w = SW, h = SH },
	tree = { title = "Passive tree", x = 20, y = 34, w = TW, h = TH },
	sheet = { title = "Character sheet", x = 190, y = 60, w = HW, h = HH },
}

function ui.onMessage(value: any)
	if type(value) ~= "table" then
		return
	end
	local kind = value.kind
	if kind == "hero" then
		shown = value
	elseif kind == "boss" then
		if value.most and value.most > 0 then
			if not bossShown then
				nox.window("boss", { title = value.name, x = 320 - BOSS_W // 2, y = 24, w = BOSS_W, h = BOSS_H })
			end
			bossShown = value
		elseif bossShown then
			bossShown = nil
			nox.closeWindow("boss")
		end
	elseif kind == "open" and SPEC[value.window] then
		opened[value.window] = true
		nox.window(value.window, SPEC[value.window])
	elseif kind == "close" and SPEC[value.window] then
		opened[value.window] = nil
		nox.closeWindow(value.window)
	elseif PARTS[kind] then
		takePart(value)
	elseif kind == "treeState" or kind == "sheet" then
		whole[kind] = if kind == "sheet" then value else { header = value }
	elseif kind == "note" then
		notes[value.window] = { text = value.text, til = nox.time() + 4000 }
		nox.gameSound("ButtonPress", 60)
	end
end

function ui.draw(window: string)
	zones[window] = {}
	if window == "hero" then
		drawHero()
	elseif window == "boss" then
		drawBoss()
	elseif window == "stash" then
		drawStash()
	elseif window == "vendor" then
		drawVendor()
	elseif window == "tree" then
		drawTree()
	elseif window == "sheet" then
		drawSheet()
	end
end

-- A click: whatever was drawn there this frame (the last zone drawn on top wins).
function ui.click(window: string, x: number, y: number, button: number?): boolean
	local list = zones[window]
	if not list then
		return false
	end
	for i = #list, 1, -1 do
		local z = list[i]
		if x >= z.x and x < z.x + z.w and y >= z.y and y < z.y + z.h then
			z.fn(button or 1)
			return true
		end
	end
	return false
end

function ui.key(window: string, key: string): boolean
	if window == "tree" then
		if key == "left" then
			pan(1, 0)
		elseif key == "right" then
			pan(-1, 0)
		elseif key == "up" then
			pan(0, 1)
		elseif key == "down" then
			pan(0, -1)
		elseif key == "1" or key == "2" or key == "3" then
			setZoom(tonumber(key) or 2)
		elseif key == "0" then
			centre()
		elseif key == "escape" then
			opened.tree = nil
			nox.closeWindow("tree")
			nox.send({ kind = "close", window = "tree" })
		else
			return false
		end
		return true
	end
	if key == "escape" and SPEC[window] then
		opened[window] = nil
		nox.closeWindow(window)
		nox.send({ kind = "close", window = window })
		return true
	end
	return false
end

return ui
