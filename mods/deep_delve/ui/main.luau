--!strict
-- Deep Delve's UI on a NoX Core client (UI API 4: client level 5). The server sends the numbers
-- (server/hud.luau and the modules it names) when they change; this draws them and sends the
-- clicks back. Its windows:
--
--   party   the party strip across the top of the screen (no frame, a HUD piece): a card for each
--           member (name, class, level, health and mana, where they are, dead or not); during a
--           vote to change area, its row under the cards (who asks, where to, the time left, Yes /
--           No, or Cancel for its asker) and each member's answer under their card, and the
--           finished vote's line for a few seconds after. It takes the mouse only over its cards
--           and, during a vote, its row (nox.hitArea); a click anywhere else is the game's. A
--           player alone in the game has none: it opens when a second player joins and closes
--           when they leave;
--   hero    (the Character window) the character's name, level and experience, the area, the Town
--           portal, Tree and Sheet buttons, the loot names option (shown while Alt is held, or
--           always; kept with the character), the act (cleared or not);
--   boss    while an act boss lives: a bar with its health under its name;
--   stash   (touching the town's chest) the stash's tabs and what the character carries: a click
--           moves an item across;
--   vendor  (touching the town's anvil) its own stock and what the character carries: a click buys
--           or sells, a right click rerolls a magic or rare piece;
--   tree    the passive tree: nodes and the lines between them, panned by dragging (or the arrows,
--           on screen or the keys) and zoomed by the wheel (or 1-3); a click takes a node; Respec
--           by the fountain;
--   sheet   the character sheet.
--
-- The pictures are the mod's own (assets\tree\): the tree's backdrop under its links and nodes; a
-- root's, small's, notable's or keystone's picture as that node (the server names it, "ic"), framed
-- by its state (taken, open, locked); the class's root picture on each party card; the town
-- portal's, the stash's, the vendor's and the fountain's (by Respec) beside their buttons. A
-- spell, ability or summon node is the game's own icon for it (nox.spellIcon; the server names
-- it, "si": a SPELL_* or ABILITY_* name, a summon's spell), framed the same way.
--
-- An item's words are a tooltip at the mouse; a tree node's are drawn in the tree's window,
-- beside its picture. The loot lying near the player is named over each item in its rarity's
-- colour while Alt is held, or always as the character's option has it (nox.label by the item's
-- net code; the server sends the list as it changes). Each label is set once, as its item comes
-- into the list, kept while the item is out of the client's world (keep), and cleared as it
-- leaves the list (picked up, gone, or out of reach) or another map's list comes.
--
-- ui.tick (once a frame, the HUD shown or not) keeps what time alone changes: the strip sized back
-- to its cards once a finished vote's line is over, and a label the client refused tried again.
--
-- A list too long for one value comes in parts with a serial (hud.parts); the newest whole list
-- is drawn, the one before it until the next is whole.

local ui = {}

local W, H = 168, 104
local BOSS_W, BOSS_H = 220, 26
-- The framed windows start below the party strip.
local TOP = 84

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

local PARTS: { [string]: boolean } = { carried = true, stash = true, stock = true, treeData = true, labels = true }

-- ---- Drawing helpers

local RARITY: { [string]: { number } } = {
	normal = { 150, 138, 112 },
	magic = { 100, 150, 245 },
	rare = { 240, 205, 70 },
	unique = { 195, 120, 255 },
}

-- How a picture is dimmed (a tree node that can't be taken yet, a button that can't be used): a
-- dark see-through box over it.
local LOCKED_SHADE = { 4, 3, 2, 140 }

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

-- An item's tooltip at the mouse: its words in its rarity's colour, and `extra` (a price) under
-- them.
local function tipItem(e: any, extra: string?)
	local lines: { any } = {}
	for _, l in wrap(e.d or e.t or "", 250) do
		if #lines < 12 then
			table.insert(lines, { l, RARITY[e.r] or RARITY.normal })
		end
	end
	if extra then
		table.insert(lines, { extra, { 230, 190, 80 } })
	end
	nox.tooltip(lines)
end

-- ---- The party strip

local CLASS_COLOUR: { [string]: { number } } = {
	warrior = { 215, 95, 75 },
	wizard = { 95, 145, 235 },
	conjurer = { 95, 205, 115 },
}
local CLASS_TITLE: { [string]: string } = { warrior = "Warrior", wizard = "Wizard", conjurer = "Conjurer" }
-- Each class's picture: its root's on the passive tree.
local CLASS_ICON: { [string]: string } = { warrior = "tree/root_warrior", wizard = "tree/root_wizard", conjurer = "tree/root_conjurer" }
local CARD_ICON = 24
local CARD_H = 40
local GAP = 4
local VOTE_Y, VOTE_H = 46, 18
local STRIP_H = VOTE_Y + VOTE_H + 2
local VOTE_W = 380

local party: { any } = {}
local vote: any = nil -- the vote's value ({ active, by, to, how, left, yes, of, mine, asker, done })
local voteEnds = 0 -- (the client's clock when it runs out)
local doneUntil = 0 -- (a finished vote's line shown till then)
-- How the strip's window was last opened ("hidden": closed, the player alone in the game), its
-- width, and whether it had the vote's row.
local stripSpec: string = "hidden"
local stripW = 0
local stripRow = false

local function cardWidth(n: number): number
	return math.min(124, (640 + GAP) // math.max(1, n) - GAP)
end

-- Whether the strip shows the vote's row: a vote under way, or a finished one's line, for a while.
local function rowShown(): boolean
	return vote ~= nil and (vote.active == true or (vote.done ~= nil and nox.time() < doneUntil))
end

-- The strip's window as the party and the vote want it (opened again only when that changes): the
-- cards (and the answer line under them), with the vote's row below while it shows; none while the
-- player is alone in the game (the server's list of one card, or none yet). Called as the party or
-- the vote comes, and from the beat once a finished vote's line is over. It takes the mouse only
-- where drawParty marks it (nox.hitArea): the cards, and a vote's row.
local function placeStrip()
	local n = #party
	if n < 2 then
		stripRow = false
		if stripSpec ~= "hidden" then
			stripSpec = "hidden"
			nox.closeWindow("party")
		end
		return
	end
	local cardsW = n * (cardWidth(n) + GAP) - GAP
	local row = rowShown()
	local w = if row then math.max(cardsW, VOTE_W) else cardsW
	local h = if row then STRIP_H else CARD_H + 4
	stripRow = row
	stripW = w
	local spec = string.format("%d/%d", w, h)
	if spec == stripSpec then
		return
	end
	stripSpec = spec
	-- (placed so the cards stay where they are as the row comes and goes)
	nox.window("party", { frame = false, hud = true, x = (640 - cardsW) // 2 - (w - cardsW) // 2, y = 2, w = w, h = h })
end

-- Text cut to `width` (an ellipsis where it was cut).
local function fit(text: string, width: number): string
	if nox.textWidth(text) <= width then
		return text
	end
	local s = text
	while #s > 1 and nox.textWidth(s .. "..") > width do
		s = string.sub(s, 1, #s - 1)
	end
	return s .. ".."
end

local function bar(x: number, y: number, w: number, share: number, fill: { number }, back: { number })
	nox.box(x, y, w, 4, back)
	nox.box(x, y, math.floor(w * math.clamp(share, 0, 100) / 100 + 0.5), 4, fill)
end

local function drawCard(m: any, x: number, w: number, me: boolean)
	local cc = CLASS_COLOUR[m.c] or { 180, 170, 150 }
	nox.box(x, 0, w, CARD_H, { 14, 11, 8, 205 })
	nox.outline(x, 0, w, CARD_H, if me then { 220, 180, 80 } else { cc[1] // 2 + 40, cc[2] // 2 + 40, cc[3] // 2 + 40 })
	-- (the class's picture at the card's left, the words beside it)
	local icon = CLASS_ICON[m.c]
	local tx = x + 4
	if icon then
		nox.image(icon, x + 2, 2, CARD_ICON, CARD_ICON)
		tx = x + CARD_ICON + 5
	end
	local level = "L" .. tostring(m.l)
	local lw = nox.textWidth(level)
	nox.text(fit(tostring(m.n), x + w - tx - lw - 8), tx, 2, if me then "gold" else "bright")
	nox.text(level, x + w - 4 - lw, 2, "dim")
	local where = if m.d then "Dead" else tostring(m.w)
	local ww = nox.textWidth(where)
	nox.text(fit(CLASS_TITLE[m.c] or tostring(m.c), x + w - tx - ww - 8), tx, 14, cc)
	nox.text(where, x + w - 4 - ww, 14, if m.d then "red" else "dim")
	bar(x + 4, 27, w - 8, m.h or 0, { 200, 40, 36 }, { 50, 16, 14 })
	if (m.m or -1) >= 0 then
		bar(x + 4, 33, w - 8, m.m, { 60, 90, 220 }, { 16, 20, 50 })
	end
	if m.d then
		nox.box(x + 1, 1, w - 2, CARD_H - 2, { 60, 0, 0, 90 })
	end
	-- (during a vote, its answer under the card: green yes, red no, amber waiting)
	if vote and vote.active and m.v then
		local col = if m.v == "y" then { 70, 200, 80 } elseif m.v == "n" then { 220, 50, 40 } else { 220, 170, 50 }
		nox.box(x, CARD_H + 1, w, 3, col)
	end
end

local function drawVote(w: number)
	local v = vote
	local now = nox.time()
	local x0 = (w - VOTE_W) // 2
	if v and v.active then
		local left = math.max(0, math.ceil((voteEnds - now) / 1000))
		-- (the row takes the mouse, its buttons with it)
		nox.hitArea(x0, VOTE_Y, VOTE_W, VOTE_H)
		nox.box(x0, VOTE_Y, VOTE_W, VOTE_H, { 20, 16, 10, 225 })
		nox.outline(x0, VOTE_Y, VOTE_W, VOTE_H, { 200, 160, 70 })
		local line = tostring(v.by) .. ": on to " .. tostring(v.to) .. (if v.how == "waypoint" then " (waypoint)" else "") .. "? "
			.. tostring(v.yes or 0) .. "/" .. tostring(v.of or 0) .. ", " .. tostring(left) .. " s"
		local room = VOTE_W - 8 - (if v.mine == "?" then 76 elseif v.asker then 56 else 0)
		nox.text(fit(line, room), x0 + 4, VOTE_Y + 4, "bright")
		if v.mine == "?" then
			button("party", x0 + VOTE_W - 74, VOTE_Y + 2, 34, 14, "Yes", true, function()
				nox.send({ kind = "vote", answer = "yes" })
			end)
			button("party", x0 + VOTE_W - 38, VOTE_Y + 2, 34, 14, "No", true, function()
				nox.send({ kind = "vote", answer = "no" })
			end)
		elseif v.asker then
			button("party", x0 + VOTE_W - 54, VOTE_Y + 2, 50, 14, "Cancel", true, function()
				nox.send({ kind = "vote", answer = "cancel" })
			end)
		end
	elseif v and v.done and now < doneUntil and stripRow then
		-- (a finished vote's line: shown, but the game's to click through)
		nox.box(x0, VOTE_Y, VOTE_W, VOTE_H, { 20, 16, 10, 200 })
		nox.text(fit(tostring(v.done), VOTE_W - 8), x0 + 4, VOTE_Y + 4, "dim")
	end
end

-- ---- Ground labels

local LABEL_COLOUR: { [string]: { number } } = {
	normal = { 225, 222, 210 },
	magic = { 110, 160, 255 },
	rare = { 250, 220, 80 },
	unique = { 200, 130, 255 },
	gold = { 225, 175, 60 },
}
-- The newest list (net code -> { t = text, r = rarity }), and the labels set on the client now
-- (each a kept one: it waits while its item is out of the client's world). The server's list is
-- at most 96 (well under the client's 128), so every label it names fits.
local wanted: { [number]: any } = {}
local labelled: { [number]: any } = {}
local labelSerial = -1
-- The map the labels are of (the list's `map`: another means the client let them all go).
local labelMap: any = nil
-- A label the client refused (its 128 set): tried again from the beat, RETRY ms apart.
local labelsShort = false
local labelRetryAt = 0
local RETRY = 1000
-- The character's option: "alt" (shown while Alt is held) or "always".
local lootNames = "alt"

local function setLabel(code: number, l: any): boolean
	return nox.label(code, l.t, LABEL_COLOUR[l.r] or LABEL_COLOUR.normal, { show = if lootNames == "always" then "always" else "alt", keep = true })
end

-- The labels wanted and not set yet, set.
local function setMissing()
	labelsShort = false
	for code, l in wanted do
		if not labelled[code] then
			if setLabel(code, l) then
				labelled[code] = l
			else
				labelsShort = true
			end
		end
	end
	labelRetryAt = nox.time() + RETRY
end

-- The newest whole list of labels: the ones no longer in it cleared (picked up, gone, out of
-- reach), the new ones set; another map's list starts afresh.
local function takeLabels()
	local w = whole.labels
	if not w or w.serial == labelSerial then
		return
	end
	labelSerial = w.serial
	if w.header.map ~= labelMap then
		-- (the client let the old map's labels go as it loaded the new one)
		labelMap = w.header.map
		nox.clearLabels()
		labelled = {}
	end
	wanted = {}
	for _, l in w.items do
		if type(l.o) == "number" then
			wanted[l.o] = l
		end
	end
	for code in labelled do
		if not wanted[code] then
			nox.clearLabel(code)
			labelled[code] = nil
		end
	end
	setMissing()
end

-- Every label set again with the option's show (the option changed).
local function relabel()
	for code, l in labelled do
		setLabel(code, l)
	end
end

local function drawParty()
	local n = #party
	if n == 0 then
		return
	end
	local cw = cardWidth(n)
	local cardsW = n * (cw + GAP) - GAP
	local w = stripW
	local me = nox.player()
	local myName = me and string.sub(me.name, 1, 16)
	-- (each card takes the mouse, its answer line under it during a vote; the gaps between are the
	-- game's)
	local cardH = if vote and vote.active then CARD_H + 4 else CARD_H
	for i, m in party do
		local x = (w - cardsW) // 2 + (i - 1) * (cw + GAP)
		nox.hitArea(x, 0, cw, cardH)
		drawCard(m, x, cw, m.n == myName)
	end
	drawVote(w)
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
	-- The Town portal button, its picture at its left (an open portal: the way back is the town's
	-- violet pentagram).
	local open = shown.portal == true
	nox.box(4, 37, W - 8, 18, if open then { 50, 30, 70 } else { 60, 40, 90 })
	nox.image("tree/ui_portal", 5, 38, 16, 16)
	if open then
		nox.box(5, 38, 16, 16, LOCKED_SHADE)
	end
	nox.outline(4, 37, W - 8, 18, if open then "dim" else { 190, 120, 255 })
	local label = if open then "Portal open" else "Town portal"
	nox.text(label, 22 + (W - 26 - nox.textWidth(label)) // 2, 41, if open then "dim" else "bright")
	if not open then
		zone("hero", 4, 37, W - 8, 18, function()
			nox.send({ kind = "portal" })
		end)
	end
	local points = shown.points or 0
	button("hero", 4, 58, 78, 13, if points > 0 then "Tree (" .. points .. ")" else "Tree", true, function()
		openTreeOrSheet("tree")
	end)
	button("hero", 86, 58, 78, 13, "Sheet", true, function()
		openTreeOrSheet("sheet")
	end)
	-- The loot names option: a click changes it (the server keeps it with the character and sends
	-- it back).
	local always = lootNames == "always"
	button("hero", 4, 74, W - 8, 13, if always then "Loot names: always" else "Loot names: with Alt", true, function()
		nox.send({ kind = "option", name = "lootNames", value = if always then "alt" else "always" })
	end)
	local mx, my = mouseIn("hero")
	if over(mx, my, 4, 74, W - 8, 13) then
		nox.tooltip({
			{ "Names over the loot on the ground", "bright" },
			{ if always then "Shown always." else "Shown while Alt is held.", { 230, 190, 80 } },
			{ "Click to change; kept with your character.", "dim" },
		})
	end
	if shown.act then
		local line: string = "Act " .. tostring(shown.act) .. (if shown.cleared then " cleared" else "")
		nox.text(line, 4, 90, if shown.cleared then "gold" else "dim")
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
	-- (the chest's picture by the gold line)
	nox.image("tree/ui_stash", 4, 18, 18, 18)
	nox.text(string.format("Gold: %d carried, %d stashed", carriedGold, header.stashed or 0), 26, 21, "gold")
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
	if hot then
		tipItem(hot, "Click to take it")
	elseif hotCarried then
		tipItem(hotCarried, "Click to stash it")
	else
		nox.text("Point at an item to read it.", 4, 372, "dim")
	end
	drawNote(w, 4, 396)
end

local function drawVendor()
	local w = "vendor"
	closeBox(w, SW)
	local st = whole.stock
	-- (the vendor's picture by its heading)
	nox.image("tree/ui_vendor", 4, 1, 16, 16)
	nox.text(string.format("For sale (area level %d)", st and st.header.level or 1), 24, 4, "gold")
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
		local line = if hotStock then "Buy for " .. tostring(hot.p) .. " gold"
			else "Sells for " .. tostring(hot.p) .. " gold" .. (if hot.q then "; reroll " .. tostring(hot.q) .. " gold" else "")
		tipItem(hot, line)
		nox.text(line, 4, 306, "gold")
	else
		nox.text("Point at an item to read it.", 4, 306, "dim")
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
-- A node's size by its kind and the zoom: at the biggest zoom a keystone 30, a root or notable 26,
-- a spell, ability or summon 22, a small 18; smaller with the zoom's unit at the others, never
-- below the flat sizes the nodes had before they had pictures; even, so a node sits whole on its
-- place (where its lines meet).
local SIZE_TOP: { [string]: number } = { k = 30, r = 26, n = 26, p = 22, a = 22, c = 22, s = 18 }
local SIZE_LEAST: { [string]: number } = { k = 16, r = 14, n = 12, p = 12, a = 12, c = 12, s = 8 }
local SIZES: { { [string]: number } } = {}
for z, unit in ZOOMS do
	local t: { [string]: number } = {}
	for k, top in SIZE_TOP do
		t[k] = math.max(SIZE_LEAST[k], 2 * math.round(top * unit / ZOOMS[#ZOOMS] / 2))
	end
	SIZES[z] = t
end
local TAKEN_EDGE = { 255, 214, 96 }
local TAKEN_INNER = { 255, 248, 226 }
local LOCKED_EDGE = { 64, 58, 48 }
-- A node's tooltip picture's size.
local TIP_ICON = 48

local nodeIndex: any = nil -- id -> node, made when a new tree comes
local nodeNear: { [number]: { number } } = {} -- id -> its neighbours, both ways
local nodeIcon: { [number]: string } = {} -- id -> its picture's asset name ("tree/..."), if any
local nodeSpell: { [number]: string } = {} -- id -> the game's icon's name (nox.spellIcon), if any
local nodeIndexOf: number = -1

local function treeNodes(): any
	local d = whole.treeData
	if not d then
		return nil
	end
	if nodeIndexOf ~= d.serial then
		nodeIndex = {}
		nodeNear = {}
		nodeIcon = {}
		nodeSpell = {}
		for _, n in d.items do
			nodeIndex[n.i] = n
			if type(n.ic) == "string" then
				nodeIcon[n.i] = "tree/" .. n.ic
			elseif type(n.si) == "string" then
				nodeSpell[n.i] = n.si
			end
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

-- A line clipped to the canvas (its part inside, if any).
local function clipLine(x1: number, y1: number, x2: number, y2: number, colour: any, width: number)
	local t0, t1 = 0, 1
	local dx, dy = x2 - x1, y2 - y1
	for _, e in { { -dx, x1 }, { dx, TW - x1 }, { -dy, y1 }, { dy, CANVAS_H - y1 } } do
		local p, q = e[1], e[2]
		if p == 0 then
			if q < 0 then
				return
			end
		else
			local r = q / p
			if p < 0 then
				if r > t1 then
					return
				end
				t0 = math.max(t0, r)
			else
				if r < t0 then
					return
				end
				t1 = math.min(t1, r)
			end
		end
	end
	nox.line(x1 + t0 * dx, y1 + t0 * dy, x1 + t1 * dx, y1 + t1 * dy, colour, width)
end

-- A tree node's tooltip, drawn in the tree's window (nox.tooltip carries words alone): its
-- picture (the mod's `img`, or the game's icon `spell`) at TIP_ICON beside its words, by the
-- mouse, kept inside the window.
local function tipNode(lines: { any }, img: string?, spell: string?, mx: number, my: number)
	local lineH = 11
	local widest = 0
	for _, l in lines do
		widest = math.max(widest, nox.textWidth(l[1]))
	end
	local pictured = img ~= nil or spell ~= nil
	local left = if pictured then TIP_ICON + 8 else 0
	local w = 4 + left + widest + 6
	local h = math.max(#lines * lineH + 6, if pictured then TIP_ICON + 8 else 0)
	local x: number = math.clamp(if mx + 14 + w <= TW then mx + 14 else mx - 8 - w, 0, math.max(0, TW - w))
	local y: number = math.clamp(if my + 14 + h <= TH then my + 14 else my - 8 - h, 0, math.max(0, TH - h))
	nox.box(x, y, w, h, { 10, 8, 6, 240 })
	nox.outline(x, y, w, h, { 160, 120, 40 })
	if img then
		nox.image(img, x + 4, y + 4, TIP_ICON, TIP_ICON)
	elseif spell then
		nox.spellIcon(spell, x + 4, y + 4, TIP_ICON, TIP_ICON)
	end
	if pictured then
		nox.outline(x + 4, y + 4, TIP_ICON, TIP_ICON, { 90, 70, 40 })
	end
	for i, l in lines do
		nox.text(l[1], x + 4 + left, y + 3 + (i - 1) * lineH, l[2])
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
	local sizes = SIZES[zoom]
	local ox, oy = TW / 2 + panX, CANVAS_H / 2 + panY
	-- The canvas: the engraved floor, fixed while the tree pans over it.
	nox.image("tree/backdrop", 0, 0, TW, CANVAS_H)
	local taken = state.taken or ""
	local function isTaken(id: number): boolean
		return string.sub(taken, id, id) == "1"
	end
	local function at(n: any): (number, number)
		return ox + n.x * unit, oy + n.y * unit
	end
	-- The links: a line each, kept to the canvas.
	for _, n in nodes do
		local x1, y1 = at(n)
		for _, id in n.l or {} do
			local m = nodes[id]
			if m then
				local x2, y2 = at(m)
				local lit = isTaken(n.i) and isTaken(m.i)
				clipLine(x1, y1, x2, y2, if lit then { 230, 190, 90 } else { 80, 72, 60 }, if lit then 2 else 1)
			end
		end
	end
	-- The nodes.
	local mx, my = mouseIn(w)
	local hot: any = nil
	-- (a node reaching past the canvas: the window cuts it at the sides and the top, the foot drawn
	-- after covers it below)
	for _, n in nodes do
		local x, y = at(n)
		local s = sizes[n.k] or sizes.s
		if x > -s and x < TW + s and y > -s and y < CANVAS_H + s then
			local have = isTaken(n.i)
			local open = false
			if not have then
				for _, id in nodeNear[n.i] or {} do
					open = open or isTaken(id)
				end
			end
			local rc = REGION_COLOUR[n.c] or REGION_COLOUR[0]
			local bx, by = x - s / 2, y - s / 2
			local img = nodeIcon[n.i]
			local spell = nodeSpell[n.i]
			if img or spell then
				-- A root, small, notable or keystone: its picture; a spell, ability or summon: the
				-- game's icon for it. Taken, a bright frame; open (next to a taken one), its region's
				-- colour; else dimmed.
				if img then
					nox.image(img, bx, by, s, s)
				else
					nox.spellIcon(spell :: string, bx, by, s, s)
				end
				if have then
					nox.outline(bx - 1, by - 1, s + 2, s + 2, TAKEN_EDGE)
					nox.outline(bx, by, s, s, TAKEN_INNER)
				elseif open then
					nox.outline(bx - 1, by - 1, s + 2, s + 2, rc)
					nox.outline(bx, by, s, s, rc)
				else
					nox.box(bx, by, s, s, LOCKED_SHADE)
					nox.outline(bx, by, s, s, LOCKED_EDGE)
				end
			else
				-- A node with neither (a server sending no names): a box with a ring (a summon: a
				-- heart) in its region's colour.
				local fillC = if have then { 235, 195, 90 } elseif open then { rc[1] // 2, rc[2] // 2, rc[3] // 2 } else { 34, 30, 26 }
				local edge = if have then "white" elseif open then rc else { 90, 80, 66 }
				clipBox(bx, by, s, s, fillC)
				if by >= 0 and by + s <= CANVAS_H then
					nox.outline(bx, by, s, s, edge)
					local inset = math.max(3, s // 4)
					if n.k == "p" or n.k == "a" then
						nox.outline(bx + inset, by + inset, s - 2 * inset, s - 2 * inset, if have then { 120, 60, 20 } else rc)
					elseif n.k == "c" then
						-- (a summon: a solid heart where a spell has a ring)
						clipBox(bx + inset + 1, by + inset + 1, s - 2 * inset - 2, s - 2 * inset - 2, if have then { 120, 60, 20 } else rc)
					end
				end
			end
			if over(mx, my, bx - 2, by - 2, s + 4, s + 4) and (my or 0) < CANVAS_H then
				hot = n
				nox.outline(bx - 3, by - 3, s + 6, s + 6, "bright")
			end
			if not have then
				-- (its click kept to the canvas: the foot's own below)
				local zy = math.max(by - 2, 0)
				local zh = math.min(by + s + 2, CANVAS_H) - zy
				if zh > 0 then
					zone(w, bx - 2, zy, s + 4, zh, function()
						nox.send({ kind = "tree", op = "take", id = n.i })
					end)
				end
			end
		end
	end
	-- The foot: the node under the mouse, the points, the buttons.
	nox.box(0, CANVAS_H, TW, TH - CANVAS_H, { 14, 11, 8 })
	nox.box(0, CANVAS_H, TW, 1, { 110, 84, 44 })
	if hot then
		local status = if isTaken(hot.i) then "(yours)" else "(click to take: 1 point)"
		nox.text(hot.n, 4, CANVAS_H + 4, RARITY.rare)
		nox.text(status, 8 + nox.textWidth(hot.n), CANVAS_H + 4, "dim")
	else
		nox.text("A node next to one you have takes a point. Spells and summons come from their branch's nodes.", 4, CANVAS_H + 4, "dim")
	end
	nox.text(string.format("Points left: %d of %d", state.points or 0, state.total or 0), 4, CANVAS_H + 42, "gold")
	local cost = state.cost or 0
	local canRespec = state.near == true and (state.spent or 0) > 0
	-- (the fountain's picture beside its button)
	nox.image("tree/ui_respec", 152, CANVAS_H + 39, 16, 16)
	if not canRespec then
		nox.box(152, CANVAS_H + 39, 16, 16, LOCKED_SHADE)
	end
	button(w, 172, CANVAS_H + 40, 150, 14, "Respec (" .. cost .. " gold)", canRespec, function()
		nox.send({ kind = "tree", op = "respec" })
	end)
	if state.near ~= true then
		nox.text("(at the fountain)", 326, CANVAS_H + 42, "dim")
	end
	local bx = TW - 196
	button(w, bx, CANVAS_H + 58, 22, 14, "<", true, function() pan(1, 0) end)
	button(w, bx + 24, CANVAS_H + 58, 22, 14, "^", true, function() pan(0, 1) end)
	button(w, bx + 48, CANVAS_H + 58, 22, 14, "v", true, function() pan(0, -1) end)
	button(w, bx + 72, CANVAS_H + 58, 22, 14, ">", true, function() pan(-1, 0) end)
	button(w, bx + 98, CANVAS_H + 58, 22, 14, "+", zoom < #ZOOMS, function() setZoom(zoom + 1) end)
	button(w, bx + 122, CANVAS_H + 58, 22, 14, "-", zoom > 1, function() setZoom(zoom - 1) end)
	button(w, bx + 148, CANVAS_H + 58, 44, 14, "Home", true, centre)
	nox.text("Drag or arrows pan, wheel or 1-3 zoom, 0 home", 4, CANVAS_H + 60, "dim")
	drawNote(w, 300, CANVAS_H + 60)
	closeBox(w, TW)
	-- The node under the mouse: its tooltip over everything in the window.
	if hot and mx and my then
		local status = if isTaken(hot.i) then "(yours)" else "(click to take: 1 point)"
		local tip: { any } = { { tostring(hot.n), RARITY.rare }, { status, { 160, 150, 130 } } }
		for _, l in wrap(hot.d or "", 260) do
			if #tip < 14 then
				table.insert(tip, { l, { 235, 230, 215 } })
			end
		end
		tipNode(tip, nodeIcon[hot.i], nodeSpell[hot.i], mx, my)
	end
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
	nox.window("hero", { title = "Character", x = 6, y = TOP, w = W, h = H })
	placeStrip()
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
		local want = if value.lootNames == "always" then "always" else "alt"
		if want ~= lootNames then
			-- (the option changed: every label set again with it)
			lootNames = want
			relabel()
		end
	elseif kind == "boss" then
		if value.most and value.most > 0 then
			if not bossShown then
				nox.window("boss", { title = value.name, x = 320 - BOSS_W // 2, y = TOP, w = BOSS_W, h = BOSS_H })
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
	elseif kind == "party" then
		party = if type(value.list) == "table" then value.list else {}
		placeStrip()
	elseif kind == "vote" then
		local was = vote
		vote = value
		if value.active then
			voteEnds = nox.time() + (tonumber(value.left) or 0) * 1000
			if not (was and was.active) then
				nox.gameSound("ButtonPress", 70)
			end
		elseif value.done and (was == nil or was.active or was.done ~= value.done) then
			doneUntil = nox.time() + 6000
		end
		placeStrip()
	elseif PARTS[kind] then
		takePart(value)
		if kind == "labels" then
			takeLabels()
		end
	elseif kind == "treeState" or kind == "sheet" then
		whole[kind] = if kind == "sheet" then value else { header = value }
	elseif kind == "note" then
		notes[value.window] = { text = value.text, til = nox.time() + 4000 }
		nox.gameSound("ButtonPress", 60)
	end
end

-- The beat, once a frame (the HUD shown or hidden, any window drawn or none): what time alone
-- changes. Two number compares unless one of them is due.
function ui.tick(now: number)
	-- (a finished vote's line over: the strip back to its cards)
	if stripRow and now >= doneUntil and not (vote and vote.active) then
		placeStrip()
	end
	if labelsShort and now >= labelRetryAt then
		setMissing()
	end
end

function ui.draw(window: string)
	zones[window] = {}
	if window == "party" then
		drawParty()
	elseif window == "hero" then
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

-- The wheel over the tree zooms it.
function ui.wheel(window: string, _x: number, _y: number, steps: number): boolean
	if window == "tree" and steps ~= 0 then
		setZoom(zoom + (if steps > 0 then 1 else -1))
		return true
	end
	return false
end

-- Dragging the tree pans it.
local dragFrom: any = nil
function ui.drag(window: string, phase: string, x: number, y: number, _button: number): boolean
	if window ~= "tree" then
		return false
	end
	if phase == "start" then
		dragFrom = { x = x, y = y, px = panX, py = panY }
	elseif dragFrom then
		panX = dragFrom.px + (x - dragFrom.x)
		panY = dragFrom.py + (y - dragFrom.y)
		if phase == "end" then
			dragFrom = nil
		end
	end
	return true
end

return ui
