--!strict
-- Deep Delve's UI on a NoX Core client (UI API 5: client level 6). The server sends the numbers
-- (server/hud.luau and the modules it names) when they change; this draws them and sends the
-- clicks back. Its windows:
--
--   party   the party strip across the top of the screen (no frame, a HUD piece): a card for each
--           member (name, class, level, health and mana, where they are, dead or not); during a
--           vote to change area, its row under the cards (who asks, where to, the time left, Yes /
--           No, or Cancel for its asker) and each member's answer under their card, and the
--           finished vote's line for a few seconds after. It takes the mouse only over its cards
--           and, during a vote, its row (nox.hitArea); a click anywhere else is the game's. A
--           click on a card shows that member's character sheet (one's own card: one's own). A
--           player alone in the game has none: it opens when a second player joins and closes
--           when they leave;
--   hero    (the Character window) the character's name, level and experience, the area, the Town
--           portal, Tree and Sheet buttons, the loot names option (shown while Alt is held, or
--           always; kept with the character), the act (cleared or not), or on an expedition its
--           rank (cleared or not);
--   boss    while an act boss lives: a bar with its health under its name;
--   stash   (touching the town's chest) the stash's tabs and what the character carries: a click
--           moves an item across;
--   vendor  (touching the town's anvil) its own stock and what the character carries: a click buys
--           or sells, a right click rerolls a magic or rare piece;
--   tree    the passive tree: nodes and the lines between them, panned by dragging (or the arrows,
--           on screen or the keys) and zoomed by the wheel (or 1-3); a click takes a node, a
--           right click on one taken gives it back alone (anywhere, for gold); a node's
--           tooltip says what it gives the hero's class; Respec by the fountain;
--   sheet   the character sheet: one's own, or (from a card on the strip) a party member's, read
--           only, kept current while it shows;
--   waypoints (stepping on the town's waypoints pentagram) the waypoints by act (each act open to
--           the character) and area, the ones found to click (this area's: a step, no vote;
--           another's, in any act listed: the party's vote, or at once when alone), the area being
--           played marked;
--   maps    (stepping on the pad at the town's map table: "Expeditions") the biomes offered, the
--           rank ladder (each rank's area level, open, locked, or cleared in the biome picked), a
--           biome and a rank picked by a click, and the way out (alone at once; in a party, the
--           party's vote); while the table is shut to the character, why;
--   chars   (a server that keeps the characters: character select) the account's characters, one
--           of the class joined as to play, a new one, "Link another PC" (a code shown here alone),
--           a code from another PC typed on its keys (or the number keys) at character select,
--           the account's PCs (one unlinked); from the Character window's Characters button too,
--           to make a code or unlink a PC while playing.
--
-- The pictures are the mod's own (assets\tree\): the tree's backdrop under its links and nodes; a
-- root's, small's, notable's or keystone's picture as that node (the server names it, "ic"), framed
-- by its state (taken, open, locked); the class's root picture on each party card; the town
-- portal's, the stash's, the vendor's, the waypoints' (in its window's heading) and the
-- fountain's (by Respec) beside their buttons. A
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
-- A named monster (a rare, a champion, the act boss) has its name over its head in its tier's
-- colour, always (the loot names option doesn't touch it): the server sends the plates as their
-- own list (net code, name, colour) as the named set changes, and each is a kept label like the
-- loot's, raised by the list's offset over the creature's top. A plate is set before any loot
-- label: if the client's 128 are taken, a loot label gives way (set again once there's room).
--
-- ui.tick (once a frame, the HUD shown or not) keeps what time alone changes: the strip sized back
-- to its cards once a finished vote's line is over, and a label the client refused tried again.
--
-- A list too long for one value comes in parts with a serial (hud.parts); the newest whole list
-- is drawn, the one before it until the next is whole.
--
-- The Character window is in the screen's top left corner, and the party strip beside it to its right
-- (both anchor "topleft": they keep to that corner on a screen wider or taller than 4:3); the boss's
-- bar is centred under the strip (anchor "top"); the station windows (stash, vendor, waypoints,
-- tree, sheet) stay centred.

local ui = {}

local W, H = 168, 104
-- The Character window's corner: the screen's top left (a framed window's highest is y 20).
local HERO_X, HERO_Y = 6, 20
-- The boss's bar: a bare HUD piece (no frame, click-through) under the party strip with its vote's
-- row: the boss's name as its title over the bar, in the boss's colour on a dark band, the bar
-- under it. The title is drawn at text size BOSS_TITLE (UI API 5's nox.text size).
local BOSS_W, BOSS_H = 300, 40
local BOSS_Y = 76
local BOSS_TITLE = 1.5
local BOSS_BAR_Y = 25

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

local PARTS: { [string]: boolean } = { carried = true, stash = true, stock = true, treeData = true, labels = true, plates = true, chars = true }

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
		nox.text(n.text, x, y, if n.good then "gold" else "red")
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
-- The strip: just right of the Character window, its top level with the top of the window's frame
-- (its title bar, 14 above its y); its cards share what's left of the 640 at 4:3 (narrower with more
-- members, a long name cut with an ellipsis).
local STRIP_X = HERO_X + W + 6
local STRIP_Y = HERO_Y - 14
local STRIP_ROOM = 640 - STRIP_X - 4
local VOTE_Y, VOTE_H = 46, 18
local STRIP_H = VOTE_Y + VOTE_H + 2
local VOTE_W = 380

local party: { any } = {}
local vote: any = nil -- the vote's value ({ active, by, to, how, left, yes, of, mine, asker, done })
local voteEnds = 0 -- (the client's clock when it runs out)
local doneUntil = 0 -- (a finished vote's line shown till then)
-- How the strip's window was last opened ("hidden": closed, the player alone in the game), and
-- whether it had the vote's row.
local stripSpec: string = "hidden"
local stripRow = false

local function cardWidth(n: number): number
	return math.min(124, (STRIP_ROOM + GAP) // math.max(1, n) - GAP)
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
	local spec = string.format("%d/%d", w, h)
	if spec == stripSpec then
		return
	end
	stripSpec = spec
	-- (the cards and the row from its left edge: the cards stay where they are as the row comes and
	-- goes)
	nox.window("party", { frame = false, hud = true, anchor = "topleft", x = STRIP_X, y = STRIP_Y, w = w, h = h })
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

local function drawVote()
	local v = vote
	local now = nox.time()
	local x0 = 0
	if v and v.active then
		local left = math.max(0, math.ceil((voteEnds - now) / 1000))
		-- (the row takes the mouse, its buttons with it)
		nox.hitArea(x0, VOTE_Y, VOTE_W, VOTE_H)
		nox.box(x0, VOTE_Y, VOTE_W, VOTE_H, { 20, 16, 10, 225 })
		nox.outline(x0, VOTE_Y, VOTE_W, VOTE_H, { 200, 160, 70 })
		local line = tostring(v.by) .. ": on to " .. tostring(v.to) .. (if v.how == "waypoint" then " (waypoint)" elseif v.how == "gate" then " (gate)" else "") .. "? "
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
-- at most 96 and its plates at most 32: the client's 128 together.
local wanted: { [number]: any } = {}
local labelled: { [number]: any } = {}
local labelSerial = -1
-- The newest plates (net code -> { t = name, c = colour }), those set on the client now, how far
-- over the creature's top they sit (the list's offset).
local plateWanted: { [number]: any } = {}
local plated: { [number]: any } = {}
local plateSerial = -1
local plateOffset = 6
-- The map the labels and plates are of (a list's `map`: another means the client let them all go).
local labelMap: any = nil
-- A label or plate the client refused (its 128 set): tried again from the beat, RETRY ms apart.
local labelsShort = false
local platesShort = false
local labelRetryAt = 0
local RETRY = 1000
-- The character's option: "alt" (shown while Alt is held) or "always".
local lootNames = "alt"

-- (a loot label is clickable (its entry's `p`): a click on its name asks the server to pick it up,
-- which it does by the game's own pickup when the hero is in reach (gold to the purse, a potion
-- drunk). A name plate never is: a click on a clickable label is always the UI's, never the game's.)
local function setLabel(code: number, l: any): boolean
	return nox.label(code, l.t, LABEL_COLOUR[l.r] or LABEL_COLOUR.normal, { show = if lootNames == "always" then "always" else "alt", keep = true, click = l.p == true })
end

-- A plate's label: always shown, kept while its monster is out of the client's world.
local function setPlate(code: number, p: any): boolean
	local c: any = if type(p.c) == "table" then p.c else "white"
	return nox.label(code, tostring(p.t), c, { show = "always", keep = true, offset = plateOffset })
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

-- The plates wanted and not set yet, set; one the client refuses (its 128 taken) has a loot
-- label give way to it (that one set again once there's room).
local function setPlates()
	platesShort = false
	for code, p in plateWanted do
		if not plated[code] then
			local ok = setPlate(code, p)
			if not ok then
				local give = next(labelled)
				if give ~= nil then
					nox.clearLabel(give)
					labelled[give] = nil
					labelsShort = true
					ok = setPlate(code, p)
				end
			end
			if ok then
				plated[code] = p
			else
				platesShort = true
			end
		end
	end
	labelRetryAt = nox.time() + RETRY
end

-- A list of another map than the labels set now (`kind`'s): the client let them all go as it
-- loaded the new map; the other kind's list is the old map's until its own comes.
local function toMap(map: any, kind: string)
	if map == labelMap then
		return
	end
	labelMap = map
	nox.clearLabels()
	labelled = {}
	plated = {}
	if kind ~= "labels" then
		wanted = {}
	end
	if kind ~= "plates" then
		plateWanted = {}
	end
end

-- The newest whole list of plates: the ones no longer in it cleared (killed, gone), the new ones
-- set; another map's list starts afresh.
local function takePlates()
	local w = whole.plates
	if not w or w.serial == plateSerial then
		return
	end
	plateSerial = w.serial
	toMap(w.header.map, "plates")
	if type(w.header.offset) == "number" then
		plateOffset = w.header.offset
	end
	plateWanted = {}
	for _, p in w.items do
		if type(p.o) == "number" then
			plateWanted[p.o] = p
		end
	end
	for code in plated do
		if not plateWanted[code] then
			nox.clearLabel(code)
			plated[code] = nil
		end
	end
	setPlates()
end

-- The newest whole list of labels: the ones no longer in it cleared (picked up, gone, out of
-- reach), the new ones set; another map's list starts afresh.
local function takeLabels()
	local w = whole.labels
	if not w or w.serial == labelSerial then
		return
	end
	labelSerial = w.serial
	toMap(w.header.map, "labels")
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
	local me = nox.player()
	local myName = me and string.sub(me.name, 1, 16)
	-- (each card takes the mouse, its answer line under it during a vote; the gaps between are the
	-- game's. A click on a card shows that member's sheet: one's own card, one's own.)
	local cardH = if vote and vote.active then CARD_H + 4 else CARD_H
	local mx, my = mouseIn("party")
	local hot: any = nil
	for i, m in party do
		local x = (i - 1) * (cw + GAP)
		local mine = m.n == myName
		nox.hitArea(x, 0, cw, cardH)
		drawCard(m, x, cw, mine)
		zone("party", x, 0, cw, CARD_H, function()
			nox.send({ kind = "open", window = "sheet", peer = if mine then nil else m.p })
		end)
		if over(mx, my, x, 0, cw, CARD_H) then
			hot = m
			nox.outline(x + 1, 1, cw - 2, CARD_H - 2, { 120, 100, 60 })
		end
	end
	drawVote()
	if hot then
		nox.tooltip({
			{ tostring(hot.n), if hot.n == myName then { 230, 190, 80 } else "bright" },
			{ if hot.n == myName then "Click: your character sheet." else "Click: their character sheet.", "dim" },
		})
	end
end

-- ---- The hero's window

local function openTreeOrSheet(name: string)
	-- (the sheet showing a party member's: one's own shown in its place)
	local sheetNow: any = whole.sheet
	local showsMember = name == "sheet" and sheetNow ~= nil and sheetNow.of ~= nil
	if opened[name] and not showsMember then
		opened[name] = nil
		nox.closeWindow(name)
		nox.send({ kind = "close", window = name })
		return
	end
	nox.send({ kind = "open", window = name, fresh = name == "tree" and whole.treeData == nil })
end

local function drawHero()
	if not shown then
		nox.text(if opened.chars then "Choose a character first." else "...", 4, 4, "dim")
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
	elseif type(shown.run) == "table" then
		-- (an expedition's map: its rank, cleared once its champion is dead)
		local run: any = shown.run
		local line: string = "Expedition, rank " .. tostring(run.r) .. (if run.c then " cleared" else "")
		nox.text(line, 4, 90, if run.c then "gold" else "dim")
	end
	-- (a server that keeps the characters: the account's characters, its PCs and a link code)
	if shown.server then
		button("hero", W - 76, 88, 72, 13, "Characters", true, function()
			nox.send({ kind = "chars", op = "open" })
		end)
	end
end

local function drawBoss()
	local v = bossShown
	if not v then
		return
	end
	-- (a dark band, the name centred over the bar in the boss's colour, at the title size, with a
	-- shadow under it; a client that draws one text size shows it plain, the same place)
	nox.box(0, 0, BOSS_W, BOSS_H, { 0, 0, 0, 150 })
	local name = tostring(v.name)
	local colour: any = if type(v.c) == "table" then v.c else { 240, 70, 50 }
	local tx = math.max(4, (BOSS_W - nox.textWidth(name, BOSS_TITLE)) // 2)
	nox.text(name, tx + 1, 3, { 0, 0, 0 }, BOSS_TITLE)
	nox.text(name, tx, 2, colour, BOSS_TITLE)
	local w = BOSS_W - 12
	local fill = math.floor(w * math.clamp(v.health / math.max(1, v.most), 0, 1))
	nox.box(6, BOSS_BAR_Y, w, 10, { 50, 10, 10 })
	nox.box(6, BOSS_BAR_Y, fill, 10, { 200, 30, 30 })
	nox.outline(6, BOSS_BAR_Y, w, 10, "dim")
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
--
-- What a frame draws is worked out only when it changes: the tree's links and clicks once a tree
-- (treeNodes), which nodes are taken or open once a state (treeMarks), and where each link's
-- piece and each node in view goes once a zoom, pan, state or tree (layTree). A frame then draws
-- what was laid out, finds the node under the mouse, and hands the laid-out clicks on; it builds
-- no strings and makes no tables.

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
-- (a node with no picture: its fill while open, half its region's colour)
local REGION_HALF: { [number]: { number } } = {}
for c, rc in REGION_COLOUR do
	REGION_HALF[c] = { rc[1] // 2, rc[2] // 2, rc[3] // 2 }
end
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
local LINK_LIT = { 230, 190, 90 }
local LINK_DIM = { 80, 72, 60 }
local BOX_TAKEN = { 235, 195, 90 }
local BOX_LOCKED = { 34, 30, 26 }
local BOX_EDGE = { 90, 80, 66 }
local BOX_MARK = { 120, 60, 20 }
local FOOT_BACK = { 14, 11, 8 }
local FOOT_LINE = { 110, 84, 44 }
local TIP_WORDS = { 235, 230, 215 }
local TIP_STATUS = { 160, 150, 130 }
local TIP_BACK = { 10, 8, 6, 240 }
local TIP_EDGE = { 160, 120, 40 }
local TIP_FRAME = { 90, 70, 40 }
-- A node's tooltip picture's size.
local TIP_ICON = 48
local HINT = "A node next to one you have takes a point. A right click on one you have gives it back."

local nodeIndex: any = nil -- id -> node, made when a new tree comes
local nodeNear: { [number]: { number } } = {} -- id -> its neighbours, both ways
local nodeIcon: { [number]: string } = {} -- id -> its picture's asset name ("tree/..."), if any
local nodeSpell: { [number]: string } = {} -- id -> the game's icon's name (nox.spellIcon), if any
local nodeIndexOf: number = -1
-- Made with the index: the nodes in the order they're drawn; the links as drawn (each node's own,
-- in that order), link k from linkA[k] to linkB[k]; each node's click (taking it).
local nodeOrder: { any } = {}
local linkA: { any } = {}
local linkB: { any } = {}
local nodeTake: { [number]: () -> () } = {}
-- Each node's click once taken: a right click asks to give it back alone (any other click isn't
-- the tree's: false).
local nodeRefund: { [number]: (number) -> boolean } = {}

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
		nodeTake = {}
		nodeRefund = {}
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
		nodeOrder = {}
		linkA = {}
		linkB = {}
		for _, n in nodeIndex do
			table.insert(nodeOrder, n)
			local id = n.i
			nodeTake[id] = function()
				nox.send({ kind = "tree", op = "take", id = id })
			end
			nodeRefund[id] = function(b: number): boolean
				if b ~= 2 then
					return false
				end
				nox.send({ kind = "tree", op = "refund", id = id })
				return true
			end
			for _, other in n.l or {} do
				local m = nodeIndex[other]
				if m then
					table.insert(linkA, n)
					table.insert(linkB, m)
				end
			end
		end
		nodeIndexOf = d.serial
		centred = false
	end
	return nodeIndex
end

-- Which nodes are taken and which open (next to a taken one), and which links lit (both ends
-- taken): worked out once a state (its taken marks) and tree.
local marksOf: string? = nil
local marksTree = -1
local nodeHave: { [number]: boolean } = {}
local nodeOpen: { [number]: boolean } = {}
local linkLit: { boolean } = {}

local function treeMarks(taken: string)
	if taken == marksOf and marksTree == nodeIndexOf then
		return
	end
	marksOf = taken
	marksTree = nodeIndexOf
	table.clear(nodeHave)
	table.clear(nodeOpen)
	for _, n in nodeOrder do
		local id = n.i
		local have = string.sub(taken, id, id) == "1"
		local open = false
		if not have then
			for _, near in nodeNear[id] or {} do
				open = open or string.sub(taken, near, near) == "1"
			end
		end
		nodeHave[id] = have
		nodeOpen[id] = open
	end
	table.clear(linkLit)
	for k = 1, #linkA do
		linkLit[k] = nodeHave[linkA[k].i] == true and nodeHave[linkB[k].i] == true
	end
end

-- One edge of the canvas cutting a link's piece (t0..t1 along it): whether any is left, and what.
local function clipEdge(p: number, q: number, t0: number, t1: number): (boolean, number, number)
	if p == 0 then
		return q >= 0, t0, t1
	end
	local r = q / p
	if p < 0 then
		if r > t1 then
			return false, t0, t1
		end
		return true, math.max(t0, r), t1
	end
	if r < t0 then
		return false, t0, t1
	end
	return true, t0, math.min(t1, r)
end

-- The canvas as laid out for a zoom, a pan, a state and a tree (the key below): each link's piece
-- inside the canvas (4 numbers each in segs, lit or not in segLit), the nodes in view (inView: an
-- entry a node, { n, bx, by, s }, kept by node id and filled afresh), and their clicks (canvasZones:
-- a zone a node, kept by node id and moved afresh: one not taken is taken by a click, one taken
-- given back by a right click).
local laidTree = -1
local laidMarks: string? = nil
local laidZoom = 0
local laidX, laidY = 0, 0
local segs: { number } = {}
local segLit: { boolean } = {}
local segCount = 0
local inView: { any } = {}
local inViewCount = 0
local canvasZones: { any } = {}
local nodeEntry: { [number]: any } = {}
local nodeZone: { [number]: any } = {}

local function layTree()
	laidTree, laidMarks, laidZoom, laidX, laidY = nodeIndexOf, marksOf, zoom, panX, panY
	local unit = ZOOMS[zoom]
	local sizes = SIZES[zoom]
	local ox, oy = TW / 2 + panX, CANVAS_H / 2 + panY
	-- The links: each one's part inside the canvas, if any.
	segCount = 0
	for k = 1, #linkA do
		local n, m = linkA[k], linkB[k]
		local x1, y1 = ox + n.x * unit, oy + n.y * unit
		local x2, y2 = ox + m.x * unit, oy + m.y * unit
		local dx, dy = x2 - x1, y2 - y1
		local ok, t0, t1 = clipEdge(-dx, x1, 0, 1)
		if ok then
			ok, t0, t1 = clipEdge(dx, TW - x1, t0, t1)
		end
		if ok then
			ok, t0, t1 = clipEdge(-dy, y1, t0, t1)
		end
		if ok then
			ok, t0, t1 = clipEdge(dy, CANVAS_H - y1, t0, t1)
		end
		if ok then
			segCount += 1
			local b = segCount * 4
			segs[b - 3] = x1 + t0 * dx
			segs[b - 2] = y1 + t0 * dy
			segs[b - 1] = x1 + t1 * dx
			segs[b] = y1 + t1 * dy
			segLit[segCount] = linkLit[k]
		end
	end
	-- The nodes in view (a node reaching past the canvas: the window cuts it at the sides and the
	-- top, the foot drawn after covers it below), and their clicks, kept to the canvas (the foot's
	-- own below).
	inViewCount = 0
	table.clear(canvasZones)
	for _, n in nodeOrder do
		local x, y = ox + n.x * unit, oy + n.y * unit
		local s = sizes[n.k] or sizes.s
		if x > -s and x < TW + s and y > -s and y < CANVAS_H + s then
			local id = n.i
			local e = nodeEntry[id]
			if not e then
				e = {}
				nodeEntry[id] = e
			end
			local bx, by = x - s / 2, y - s / 2
			e.n, e.bx, e.by, e.s = n, bx, by, s
			inViewCount += 1
			inView[inViewCount] = e
			-- (not taken: a click takes it; taken: a right click gives it back)
			local zy = math.max(by - 2, 0)
			local zh = math.min(by + s + 2, CANVAS_H) - zy
			if zh > 0 then
				local z = nodeZone[id]
				if not z then
					z = {}
					nodeZone[id] = z
				end
				z.x, z.y, z.w, z.h = bx - 2, zy, s + 4, zh
				z.fn = if nodeHave[id] then nodeRefund[id] else nodeTake[id]
				table.insert(canvasZones, z)
			end
		end
	end
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

-- The foot's buttons' clicks.
local function panWest()
	pan(1, 0)
end
local function panNorth()
	pan(0, 1)
end
local function panSouth()
	pan(0, -1)
end
local function panEast()
	pan(-1, 0)
end
local function zoomIn()
	setZoom(zoom + 1)
end
local function zoomOut()
	setZoom(zoom - 1)
end
local function respec()
	nox.send({ kind = "tree", op = "respec" })
end

-- A box clipped to the canvas.
local function clipBox(x: number, y: number, w: number, h: number, colour: any)
	local x2, y2 = math.min(x + w, TW), math.min(y + h, CANVAS_H)
	x, y = math.max(x, 0), math.max(y, 0)
	if x2 > x and y2 > y then
		nox.box(x, y, x2 - x, y2 - y, colour)
	end
end

-- A tree node's tooltip, drawn in the tree's window (nox.tooltip carries words alone): its
-- picture (the mod's `img`, or the game's icon `spell`) at TIP_ICON beside its words (`widest`
-- the widest line), by the mouse, kept inside the window.
local function tipNode(lines: { any }, widest: number, img: string?, spell: string?, mx: number, my: number)
	local lineH = 11
	local pictured = img ~= nil or spell ~= nil
	local left = if pictured then TIP_ICON + 8 else 0
	local w = 4 + left + widest + 6
	local h = math.max(#lines * lineH + 6, if pictured then TIP_ICON + 8 else 0)
	local x: number = math.clamp(if mx + 14 + w <= TW then mx + 14 else mx - 8 - w, 0, math.max(0, TW - w))
	local y: number = math.clamp(if my + 14 + h <= TH then my + 14 else my - 8 - h, 0, math.max(0, TH - h))
	nox.box(x, y, w, h, TIP_BACK)
	nox.outline(x, y, w, h, TIP_EDGE)
	if img then
		nox.image(img, x + 4, y + 4, TIP_ICON, TIP_ICON)
	elseif spell then
		nox.spellIcon(spell, x + 4, y + 4, TIP_ICON, TIP_ICON)
	end
	if pictured then
		nox.outline(x + 4, y + 4, TIP_ICON, TIP_ICON, TIP_FRAME)
	end
	for i, l in lines do
		nox.text(l[1], x + 4 + left, y + 3 + (i - 1) * lineH, l[2])
	end
end

-- The words a state gives the foot (made once a state), and the tooltip's lines for a node, taken
-- or not (made once each, and again with each state).
local footOf: any = nil
local footPoints = ""
local footRespec = ""
local tipOf: any = nil
local tipTaken = false
local tipState: any = nil
local tipLines: { any } = {}
local tipWidest = 0
local TIP_FOR_YOU = { 150, 200, 250 }
local TIP_REFUND = { 230, 190, 80 }
local CLASS_BIT: { [string]: number } = { warrior = 1, wizard = 2, conjurer = 4 }

-- What a spell, ability or summon node gives a hero of `class` (the server's tree.forClass makes
-- the same line): its own region's "as above"; another region's it may use "as above" too; else
-- its class's stats' worth in its place (the words the tree came with). Nil for any other node.
local function forYou(n: any, class: string?): string?
	local mask = n.u
	if type(mask) ~= "number" or not class then
		return nil
	end
	local title = CLASS_TITLE[class] or class
	if n.c == CLASS_REGION[class] then
		return "For you: as above"
	elseif bit32.band(mask, CLASS_BIT[class] or 0) ~= 0 then
		return "For you: as above (a " .. title .. " can use it too)"
	end
	local d = whole.treeData
	local worth = d and d.header and d.header.worth
	return "For you: " .. tostring(worth and worth[class] or "its stats' worth") .. " in its place (a " .. title .. " can't use it)"
end

-- A node's tooltip lines (and the widest): its name, taken or not, what it does, what it gives
-- the hero's class, and, taken, whether a right click gives it back alone and for what.
local function nodeTip(n: any, taken: boolean, state: any): ({ any }, number)
	local tip: { any } = { { tostring(n.n), RARITY.rare }, { if taken then "(yours)" else "(click to take: 1 point)", TIP_STATUS } }
	local function add(text: string, colour: any)
		for _, l in wrap(text, 260) do
			if #tip < 16 then
				table.insert(tip, { l, colour })
			end
		end
	end
	add(n.d or "", TIP_WORDS)
	local mine = forYou(n, state.class)
	if mine then
		add(mine, TIP_FOR_YOU)
	end
	if taken then
		local mark = string.sub(state.refund or "", n.i, n.i)
		if mark == "1" then
			add("Right click: give it back for " .. tostring(state.refundCost or 0) .. " gold", TIP_REFUND)
		elseif mark == "h" then
			add("Can't be given back alone: other nodes you have hang on it", TIP_STATUS)
		end
	end
	local widest = 0
	for _, l in tip do
		widest = math.max(widest, nox.textWidth(l[1]))
	end
	return tip, widest
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
	treeMarks(state.taken or "")
	if laidTree ~= nodeIndexOf or laidMarks ~= marksOf or laidZoom ~= zoom or laidX ~= panX or laidY ~= panY then
		layTree()
	end
	-- The canvas: the engraved floor, fixed while the tree pans over it.
	nox.image("tree/backdrop", 0, 0, TW, CANVAS_H)
	-- The links: a line each, kept to the canvas.
	for k = 1, segCount do
		local b = k * 4
		if segLit[k] then
			nox.line(segs[b - 3], segs[b - 2], segs[b - 1], segs[b], LINK_LIT, 2)
		else
			nox.line(segs[b - 3], segs[b - 2], segs[b - 1], segs[b], LINK_DIM, 1)
		end
	end
	-- The nodes in view.
	local mx, my = mouseIn(w)
	local mouseOn = mx ~= nil and my ~= nil and my < CANVAS_H
	local hot: any = nil
	for k = 1, inViewCount do
		local e = inView[k]
		local n = e.n
		local id = n.i
		local bx, by, s = e.bx, e.by, e.s
		local have = nodeHave[id]
		local open = nodeOpen[id]
		local img = nodeIcon[id]
		local spell = nodeSpell[id]
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
				local rc = REGION_COLOUR[n.c] or REGION_COLOUR[0]
				nox.outline(bx - 1, by - 1, s + 2, s + 2, rc)
				nox.outline(bx, by, s, s, rc)
			else
				nox.box(bx, by, s, s, LOCKED_SHADE)
				nox.outline(bx, by, s, s, LOCKED_EDGE)
			end
		else
			-- A node with neither (a server sending no names): a box with a ring (a summon: a
			-- heart) in its region's colour.
			local rc = REGION_COLOUR[n.c] or REGION_COLOUR[0]
			local fillC = if have then BOX_TAKEN elseif open then (REGION_HALF[n.c] or REGION_HALF[0]) else BOX_LOCKED
			local edge = if have then "white" elseif open then rc else BOX_EDGE
			clipBox(bx, by, s, s, fillC)
			if by >= 0 and by + s <= CANVAS_H then
				nox.outline(bx, by, s, s, edge)
				local inset = math.max(3, s // 4)
				if n.k == "p" or n.k == "a" then
					nox.outline(bx + inset, by + inset, s - 2 * inset, s - 2 * inset, if have then BOX_MARK else rc)
				elseif n.k == "c" then
					-- (a summon: a solid heart where a spell has a ring)
					clipBox(bx + inset + 1, by + inset + 1, s - 2 * inset - 2, s - 2 * inset - 2, if have then BOX_MARK else rc)
				end
			end
		end
		if mouseOn and over(mx, my, bx - 2, by - 2, s + 4, s + 4) then
			hot = n
			nox.outline(bx - 3, by - 3, s + 6, s + 6, "bright")
		end
	end
	-- The nodes' clicks (zones' first, before the foot's).
	local list = zones[w]
	if list then
		table.move(canvasZones, 1, #canvasZones, #list + 1, list)
	end
	-- The foot: the node under the mouse, the points, the buttons.
	if state ~= footOf then
		footOf = state
		footPoints = string.format("Points left: %d of %d", state.points or 0, state.total or 0)
		footRespec = "Respec (" .. (state.cost or 0) .. " gold)"
	end
	nox.box(0, CANVAS_H, TW, TH - CANVAS_H, FOOT_BACK)
	nox.box(0, CANVAS_H, TW, 1, FOOT_LINE)
	local hotTaken = hot ~= nil and nodeHave[hot.i] == true
	if hot then
		nox.text(hot.n, 4, CANVAS_H + 4, RARITY.rare)
		nox.text(if hotTaken then "(yours)" else "(click to take: 1 point)", 8 + nox.textWidth(hot.n), CANVAS_H + 4, "dim")
	else
		nox.text(HINT, 4, CANVAS_H + 4, "dim")
	end
	nox.text(footPoints, 4, CANVAS_H + 42, "gold")
	local canRespec = state.near == true and (state.spent or 0) > 0
	-- (the fountain's picture beside its button)
	nox.image("tree/ui_respec", 152, CANVAS_H + 39, 16, 16)
	if not canRespec then
		nox.box(152, CANVAS_H + 39, 16, 16, LOCKED_SHADE)
	end
	button(w, 172, CANVAS_H + 40, 150, 14, footRespec, canRespec, respec)
	if state.near ~= true then
		nox.text("(at the fountain)", 326, CANVAS_H + 42, "dim")
	end
	local bx = TW - 196
	button(w, bx, CANVAS_H + 58, 22, 14, "<", true, panWest)
	button(w, bx + 24, CANVAS_H + 58, 22, 14, "^", true, panNorth)
	button(w, bx + 48, CANVAS_H + 58, 22, 14, "v", true, panSouth)
	button(w, bx + 72, CANVAS_H + 58, 22, 14, ">", true, panEast)
	button(w, bx + 98, CANVAS_H + 58, 22, 14, "+", zoom < #ZOOMS, zoomIn)
	button(w, bx + 122, CANVAS_H + 58, 22, 14, "-", zoom > 1, zoomOut)
	button(w, bx + 148, CANVAS_H + 58, 44, 14, "Home", true, centre)
	nox.text("Drag or arrows pan, wheel or 1-3 zoom, 0 home", 4, CANVAS_H + 60, "dim")
	drawNote(w, 300, CANVAS_H + 60)
	closeBox(w, TW)
	-- The node under the mouse: its tooltip over everything in the window.
	if hot and mx and my then
		if hot ~= tipOf or hotTaken ~= tipTaken or state ~= tipState then
			tipOf, tipTaken, tipState = hot, hotTaken, state
			tipLines, tipWidest = nodeTip(hot, hotTaken, state)
		end
		tipNode(tipLines, tipWidest, nodeIcon[hot.i], nodeSpell[hot.i], mx, my)
	end
end

-- ---- The character sheet

local HW, HH = 260, 343
-- The sheet window's title: one's own sheet's, or the party member's whose sheet it shows (the
-- window opened again with it when that changes).
local OWN_SHEET = "Character sheet"
local sheetTitle = OWN_SHEET

local function drawSheet()
	local w = "sheet"
	closeBox(w, HW)
	local s = whole.sheet
	if not s then
		nox.text("...", 4, 4, "dim")
		return
	end
	-- (a party member's: read only, nothing on it acts on them)
	local other = s.of ~= nil
	nox.text(string.format("%s, level %d %s", s.name, s.level, s.class), 4, 2, if other then "bright" else "gold")
	local rows = {
		{ "Experience", string.format("%d / %d", s.experience, s.need) },
		{ "Health", string.format("%d / %d", s.health, s.maxHealth) },
		{ "Mana", string.format("%d / %d", s.mana, s.maxMana) },
		{ "Strength", tostring(s.strength) },
		-- (the mod's regeneration a second, on top of the game's own slow refill)
		{ "Regeneration", if (tonumber(s.maxMana) or 0) > 0 then string.format("%s health, %s mana /s", tostring(s.healthRegen), tostring(s.manaRegen))
			else string.format("%s health /s", tostring(s.healthRegen)) },
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
	drawWords(if s.skills ~= "" then s.skills elseif other then "none yet" else "none yet: take them in the Tree", 4, y + 13, HW - 8, 6, "bright")
end

-- The sheet's title for its value (one's own, or a member's: its name).
local function titleOfSheet(s: any): string
	if s and s.of ~= nil then
		return string.sub(tostring(s.name), 1, 16) .. "'s sheet"
	end
	return OWN_SHEET
end

-- ---- The waypoints

local WPW = 250
local WP_ROW = 20
local WP_TOP = 24
local WP_ACT = 16
local WP_FOOT = 34
-- The server's list ({ act, here, names, list = { { a, i, t, l, f } } }), nil until it comes.
local waypointsShown: any = nil

-- The window's height for the list: its heading, each act's line, a row an area, the foot.
local function waypointsHeight(): number
	local rows, actLines = 0, 0
	local last: any = nil
	for _, e in (waypointsShown and waypointsShown.list or {}) do
		rows += 1
		if e.a ~= last then
			actLines += 1
			last = e.a
		end
	end
	return WP_TOP + actLines * WP_ACT + math.max(1, rows) * WP_ROW + WP_FOOT
end

local function drawWaypoints()
	local w = "waypoints"
	closeBox(w, WPW)
	-- (the waypoint's picture in the heading)
	nox.image("tree/ui_waypoint", 4, 2, 18, 18)
	nox.text("Travel by waypoint", 26, 6, "gold")
	local v = waypointsShown
	if not v then
		nox.text("...", 4, WP_TOP, "dim")
		return
	end
	local mx, my = mouseIn(w)
	local hot: any = nil
	local y = WP_TOP
	local last: any = nil
	local found = 0
	for _, e in v.list or {} do
		if e.a ~= last then
			last = e.a
			nox.text(string.format("Act %d: %s", tonumber(e.a) or 0, tostring(v.names and v.names[e.a] or "")), 4, y + 2, "dim")
			y += WP_ACT
		end
		local here = e.a == v.act and e.i == v.here
		-- (a found waypoint of any act listed: another act's moves the party there too)
		local can = e.f == true
		if e.f then
			found += 1
		end
		local isHot = over(mx, my, 4, y, WPW - 8, WP_ROW - 2)
		if isHot then
			hot = { e = e, here = here, can = can }
		end
		nox.box(4, y, WPW - 8, WP_ROW - 2, if can and isHot then { 60, 44, 24 } elseif can then { 30, 26, 20 } else { 18, 15, 12 })
		if here then
			nox.outline(3, y - 1, WPW - 6, WP_ROW, TAKEN_EDGE)
		end
		nox.outline(4, y, WPW - 8, WP_ROW - 2, if here then "gold" elseif can then { 200, 150, 60 } else { 60, 52, 40 })
		nox.text(tostring(e.t), 10, y + 4, if can then "bright" else "grey")
		local right = if here then "you are here" elseif not e.f then "not found yet" else string.format("area level %d", tonumber(e.l) or 0)
		nox.text(right, WPW - 10 - nox.textWidth(right), y + 4, if here then "gold" elseif e.f then "dim" else "grey")
		if can then
			local act, area = e.a, e.i
			zone(w, 4, y, WPW - 8, WP_ROW - 2, function()
				nox.send({ kind = "waypoints", op = "go", act = act, area = area })
			end)
		end
		y += WP_ROW
	end
	local footY = waypointsHeight() - WP_FOOT
	nox.box(0, footY, WPW, 1, { 90, 70, 40 })
	if found == 0 then
		nox.text("None found yet: look for them out in the areas.", 4, footY + 4, "dim")
	else
		nox.text("Another area moves the whole party: a vote.", 4, footY + 4, "dim")
	end
	drawNote(w, 4, footY + 17)
	if hot then
		local e = hot.e
		local what = if hot.here then "A step to its waypoint: you alone, no vote."
			elseif hot.can then (if #party >= 2 then "The whole party goes there: everyone votes." else "You go there at once.")
			else "Not found yet: find it out in the area."
		nox.tooltip({
			{ tostring(e.t), if hot.can then "bright" else "grey" },
			{ what, if hot.can then { 230, 190, 80 } else "dim" },
		})
	end
end

-- ---- The map table (expeditions)

local MW = 300
local MAP_TOP = 24
local MAP_ROW = 16
local MAP_FOOT = 58
local MAP_LEFT = 132
-- The server's value ({ open, why, free, top, best, runs, biomes = { { n, c } }, ranks = { { r, l } },
-- here }), nil until it comes; the biome and rank picked in the window (kept while it's closed).
local mapsShown: any = nil
local mapBiome: string? = nil
local mapRank: number? = nil

-- The window's height: the heading and the reason alone while the table is shut; else the ladder's
-- rows (or the biomes', the longer) and the foot.
local function mapsHeight(): number
	local v = mapsShown
	if not v or not v.open then
		return MAP_TOP + 40
	end
	return MAP_TOP + 14 + math.max(#(v.biomes or {}), #(v.ranks or {})) * MAP_ROW + MAP_FOOT
end

-- The picks kept within what the value allows: a biome it lists (the run being played's, else the
-- first), a rank at most the highest open (else the highest open).
local function mapPicks(v: any)
	local names: { [string]: boolean } = {}
	for _, b in v.biomes or {} do
		names[b.n] = true
	end
	if not mapBiome or not names[mapBiome] then
		mapBiome = if v.here and names[v.here.b] then v.here.b elseif v.biomes and v.biomes[1] then v.biomes[1].n else nil
	end
	local top = tonumber(v.top) or 0
	if not mapRank or mapRank > top or mapRank < 1 then
		mapRank = if top >= 1 then top else nil
	end
end

local function drawMaps()
	local w = "maps"
	closeBox(w, MW)
	nox.image("tree/ui_waypoint", 4, 2, 18, 18)
	nox.text("The map table", 26, 6, "gold")
	local v = mapsShown
	if not v then
		nox.text("...", 4, MAP_TOP, "dim")
		return
	end
	if not v.open then
		drawWords(tostring(v.why or "The map table isn't open to you yet."), 6, MAP_TOP + 4, MW - 12, 2, "dim")
		drawNote(w, 6, MAP_TOP + 26)
		return
	end
	mapPicks(v)
	local mx, my = mouseIn(w)
	local hot: any = nil
	local y0 = MAP_TOP + 14
	nox.text("Biome", 8, MAP_TOP, "dim")
	nox.text("Rank", MAP_LEFT + 12, MAP_TOP, "dim")
	-- The biomes: each one's best rank cleared at its right.
	local cleared: { number } = {}
	for i, b in v.biomes or {} do
		local y = y0 + (i - 1) * MAP_ROW
		local picked = b.n == mapBiome
		local isHot = over(mx, my, 4, y, MAP_LEFT - 8, MAP_ROW - 2)
		nox.box(4, y, MAP_LEFT - 8, MAP_ROW - 2, if picked then { 60, 44, 24 } elseif isHot then { 40, 32, 22 } else { 26, 22, 17 })
		nox.outline(4, y, MAP_LEFT - 8, MAP_ROW - 2, if picked then "gold" else { 90, 70, 40 })
		nox.text(tostring(b.n), 10, y + 2, if picked then "bright" else "grey")
		local best = 0
		for _, r in b.c or {} do
			best = math.max(best, tonumber(r) or 0)
		end
		if best > 0 then
			local s = "best " .. best
			nox.text(s, MAP_LEFT - 8 - nox.textWidth(s), y + 2, "dim")
		end
		if picked then
			cleared = b.c or {}
		end
		local name = b.n
		zone(w, 4, y, MAP_LEFT - 8, MAP_ROW - 2, function()
			mapBiome = name
		end)
	end
	-- The ladder: each rank's area level, open or locked, and cleared in the biome picked.
	local top = tonumber(v.top) or 0
	local rx, rw = MAP_LEFT + 4, MW - MAP_LEFT - 8
	for i, e in v.ranks or {} do
		local y = y0 + (i - 1) * MAP_ROW
		local r = tonumber(e.r) or i
		local open = r <= top
		local picked = open and r == mapRank
		local done = table.find(cleared, r) ~= nil
		local isHot = open and over(mx, my, rx, y, rw, MAP_ROW - 2)
		if isHot then
			hot = { r = r, l = e.l }
		end
		nox.box(rx, y, rw, MAP_ROW - 2, if picked then { 60, 44, 24 } elseif isHot then { 40, 32, 22 } elseif open then { 26, 22, 17 } else { 16, 13, 10 })
		nox.outline(rx, y, rw, MAP_ROW - 2, if picked then "gold" elseif open then { 90, 70, 40 } else { 50, 44, 34 })
		nox.text("Rank " .. r, rx + 6, y + 2, if picked then "bright" elseif open then "grey" else { 90, 80, 66 })
		local right = if not open then "locked" elseif done then "cleared, L" .. tostring(e.l) else "level " .. tostring(e.l)
		nox.text(right, rx + rw - 6 - nox.textWidth(right), y + 2, if done then "gold" elseif open then "dim" else { 90, 80, 66 })
		if open then
			zone(w, rx, y, rw, MAP_ROW - 2, function()
				mapRank = r
			end)
		end
	end
	-- The foot: what was picked, and the way out.
	local rows = math.max(#(v.biomes or {}), #(v.ranks or {}))
	local footY = y0 + rows * MAP_ROW + 2
	nox.box(0, footY, MW, 1, { 90, 70, 40 })
	local level = 0
	for _, e in v.ranks or {} do
		if e.r == mapRank then
			level = tonumber(e.l) or 0
		end
	end
	local can = mapBiome ~= nil and mapRank ~= nil
	local what = if can then string.format("%s, rank %d: area level %d", tostring(mapBiome), mapRank or 0, level) else "Pick a biome and a rank."
	nox.text(what, 6, footY + 5, if can then "bright" else "dim")
	nox.text("A champion waits at its end; its fall opens the next rank.", 6, footY + 18, "dim")
	local label = if #party >= 2 then "Ask the party" else "Set out"
	local biome, rank = mapBiome, mapRank
	button(w, MW - 100, footY + 31, 94, 15, label, can, function()
		nox.send({ kind = "maps", op = "go", biome = biome, rank = rank })
	end)
	drawNote(w, 6, footY + 34)
	if hot then
		nox.tooltip({
			{ string.format("Rank %d: area level %d", tonumber(hot.r) or 0, tonumber(hot.l) or 0), "bright" },
			{ if #party >= 2 then "Setting out moves the whole party: everyone votes." else "You set out at once.", { 230, 190, 80 } },
		})
	end
end

-- ---- Character select (a server that keeps the characters)

local CW = 300
local CH_TOP = 18
local CH_ROW = 18
local KEY_W, KEY_H, KEY_COLS = 34, 14, 8
-- The letters of a link code (the server's: no 0, O, 1, I or L).
local CODE_LETTERS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
local CODE_LENGTH = 8
-- The code made on this PC ({ code, til }: shown till it's out of time) and the code being typed.
local linkCode: any = nil
local typed = ""

-- The window's height for what it shows: the rows, a new character's button, making a code, typing
-- one (at character select only), the PCs, a note's line.
local function charsHeight(): number
	local c = whole.chars
	local h = c and c.header or {}
	local rows = math.max(1, c and #c.items or 0)
	local y = CH_TOP + rows * CH_ROW + 4
	if h.new or h.full then
		y += 20
	end
	y += 8 + 18 + 14 -- (the line, Link another PC, its code)
	if not h.playing then
		y += 14 + 18 + math.ceil(#CODE_LETTERS / KEY_COLS) * (KEY_H + 2) + 4
	end
	y += 14 + #(h.pcs or {}) * 14 + 16
	return y
end

local function openChars()
	opened.chars = true
	-- (below the party strip's cards; a long list higher, as far as the screen's bottom takes it)
	nox.window("chars", { title = "Characters", x = 170, y = 64, w = CW, h = charsHeight() })
end

local function closeChars()
	opened.chars = nil
	nox.closeWindow("chars")
end

-- (its x, or escape: playing only)
local function putAway()
	closeChars()
	nox.send({ kind = "chars", op = "close" })
end

local function typeLetter(ch: string)
	if #typed < CODE_LENGTH then
		typed ..= ch
	end
end

local function sendCode()
	if #typed == CODE_LENGTH then
		nox.send({ kind = "chars", op = "enter", code = typed })
		typed = ""
	end
end

local function drawChars()
	local w = "chars"
	local c = whole.chars
	if not c then
		nox.text("...", 4, 4, "dim")
		return
	end
	local h = c.header
	local playing = h.playing == true
	if playing then
		button(w, CW - 16, 2, 14, 13, "x", true, putAway)
	end
	nox.text(if playing then "Your characters on this server" else "Choose a character", 4, 3, "gold")
	if h.proven == false then
		local s = "(known by account)"
		nox.text(s, CW - (if playing then 22 else 6) - nox.textWidth(s), 3, "dim")
	end
	local y = CH_TOP
	if #c.items == 0 then
		nox.text("None yet on this server.", 8, y + 4, "dim")
	end
	local mx, my = mouseIn(w)
	for i, e in c.items do
		local hot = over(mx, my, 4, y, CW - 8, CH_ROW - 2)
		local can = e.p == true and not playing and not e.busy
		nox.box(4, y, CW - 8, CH_ROW - 2, if can and hot then { 60, 44, 24 } elseif e.me then { 50, 40, 20 } else { 22, 18, 14 })
		nox.outline(4, y, CW - 8, CH_ROW - 2, if e.me then "gold" elseif can then { 200, 150, 60 } else { 60, 52, 40 })
		local cls = tostring(e.c)
		local icon = CLASS_ICON[cls]
		if icon then
			nox.image(icon, 6, y + 1, 14, 14)
		end
		nox.text(tostring(e.n), 24, y + 3, if e.p then "bright" else "grey")
		nox.text(string.format("level %d %s", tonumber(e.l) or 1, CLASS_TITLE[cls] or cls), 126, y + 3, CLASS_COLOUR[cls] or "dim")
		local right = if e.me then "playing" elseif e.busy then "in play" elseif not e.p then "other class" elseif playing then "" else "Play"
		nox.text(right, CW - 10 - nox.textWidth(right), y + 3, if can then "gold" else "dim")
		if can then
			zone(w, 4, y, CW - 8, CH_ROW - 2, function()
				nox.send({ kind = "chars", op = "play", i = i })
			end)
		end
		if hot and not e.p then
			nox.tooltip({ { "Join as a " .. (CLASS_TITLE[cls] or cls) .. " to play this one.", "dim" } })
		end
		y += CH_ROW
	end
	y = CH_TOP + math.max(1, #c.items) * CH_ROW + 4
	if h.new then
		local cls = tostring(h.class)
		button(w, 4, y, CW - 8, 16, "New " .. (CLASS_TITLE[cls] or cls) .. ": " .. tostring(h.name), true, function()
			nox.send({ kind = "chars", op = "new" })
		end)
		y += 20
	elseif h.full then
		nox.text("An account keeps 10 characters at most.", 6, y + 3, "dim")
		y += 20
	end
	nox.box(0, y + 3, CW, 1, { 90, 70, 40 })
	y += 8
	button(w, 4, y, 140, 15, "Link another PC", true, function()
		nox.send({ kind = "chars", op = "code" })
	end)
	nox.text("one code, 10 minutes", 152, y + 3, "dim")
	y += 18
	local lc = linkCode
	if lc and nox.time() < lc.til then
		local left = math.max(1, math.ceil((lc.til - nox.time()) / 60000))
		local shown = string.sub(lc.code, 1, 4) .. "-" .. string.sub(lc.code, 5)
		nox.text(string.format("Code: %s  (%d min; type it on the other PC)", shown, left), 6, y, "gold")
	end
	y += 14
	if not playing then
		nox.text("Enter a link code from your other PC:", 4, y, "bright")
		y += 14
		nox.box(4, y, 120, 15, { 16, 13, 10 })
		nox.outline(4, y, 120, 15, { 200, 150, 60 })
		nox.text(typed .. (if #typed < CODE_LENGTH and (nox.time() // 500) % 2 == 0 then "_" else ""), 8, y + 3, "bright")
		button(w, 130, y, 40, 15, "Del", #typed > 0, function()
			typed = string.sub(typed, 1, #typed - 1)
		end)
		button(w, 174, y, 50, 15, "Link", #typed == CODE_LENGTH, sendCode)
		y += 18
		for k = 1, #CODE_LETTERS do
			local ch = string.sub(CODE_LETTERS, k, k)
			local kx = 4 + ((k - 1) % KEY_COLS) * (KEY_W + 2)
			local ky = y + ((k - 1) // KEY_COLS) * (KEY_H + 2)
			button(w, kx, ky, KEY_W, KEY_H, ch, #typed < CODE_LENGTH, function()
				typeLetter(ch)
			end)
		end
		y += math.ceil(#CODE_LETTERS / KEY_COLS) * (KEY_H + 2) + 4
	end
	nox.text("This account's PCs:", 4, y, "bright")
	y += 14
	-- (a PC may unlink the ones linked after it)
	local mine = #(h.pcs or {}) + 1
	for i, pc in h.pcs or {} do
		if pc.me then
			mine = i
		end
	end
	for i, pc in h.pcs or {} do
		nox.text(tostring(pc.label) .. (if pc.me then " (this PC)" else ""), 10, y + 1, if pc.me then "gold" else "bright")
		if i > mine then
			button(w, CW - 64, y, 58, 13, "Unlink", true, function()
				nox.send({ kind = "chars", op = "unlink", i = i })
			end)
		end
		y += 14
	end
	drawNote(w, 4, y + 2)
end

-- ---- The hooks

function ui.start()
	nox.window("hero", { title = "Character", anchor = "topleft", x = HERO_X, y = HERO_Y, w = W, h = H })
	-- (the game's buff icons draw down the screen's top left: by decision they start below this
	-- window, its frame's bottom and a margin; UI API 5)
	nox.hudReserve({ x = 0, y = 0, w = HERO_X + W + 4, h = HERO_Y + H + 4, anchor = "topleft" })
	placeStrip()
end

local SPEC: { [string]: any } = {
	stash = { title = "Stash", x = 36, y = 34, w = SW, h = SH },
	vendor = { title = "Vendor", x = 36, y = 34, w = SW, h = SH },
	tree = { title = "Passive tree", x = 20, y = 34, w = TW, h = TH },
	sheet = { title = OWN_SHEET, x = 190, y = 60, w = HW, h = HH },
	waypoints = { title = "Waypoints", x = 195, y = 60, w = WPW, h = 0 },
	maps = { title = "Expeditions", x = 170, y = 60, w = MW, h = 0 },
}

-- A window's spec as it opens now: the sheet's title whoever's it shows, the waypoints' height
-- for its list.
local function specOf(window: string): any
	local spec = SPEC[window]
	if window == "sheet" then
		spec = table.clone(spec)
		spec.title = sheetTitle
	elseif window == "waypoints" then
		spec = table.clone(spec)
		spec.h = waypointsHeight()
	elseif window == "maps" then
		spec = table.clone(spec)
		spec.h = mapsHeight()
	end
	return spec
end

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
				nox.window("boss", { frame = false, clickThrough = true, hud = true, anchor = "top", x = 320 - BOSS_W // 2, y = BOSS_Y, w = BOSS_W, h = BOSS_H })
			end
			bossShown = value
		elseif bossShown then
			bossShown = nil
			nox.closeWindow("boss")
		end
	elseif kind == "open" and SPEC[value.window] then
		opened[value.window] = true
		nox.window(value.window, specOf(value.window))
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
	elseif kind == "linkCode" and type(value.code) == "string" then
		-- ("": used up)
		linkCode = if value.code ~= "" then { code = value.code, til = nox.time() + (tonumber(value.minutes) or 10) * 60000 } else nil
	elseif PARTS[kind] then
		takePart(value)
		if kind == "labels" then
			takeLabels()
		elseif kind == "plates" then
			takePlates()
		elseif kind == "chars" then
			-- (the whole list in: the window open (sized to it) or closed, as the server has it)
			local c = whole.chars
			if c and c.serial == value.serial then
				if c.header.show then
					openChars()
				elseif opened.chars then
					closeChars()
					typed = ""
				end
			end
		end
	elseif kind == "sheet" then
		whole.sheet = value
		-- (one's own or a member's: the window's title follows)
		local title = titleOfSheet(value)
		if title ~= sheetTitle then
			sheetTitle = title
			if opened.sheet then
				nox.window("sheet", specOf("sheet"))
			end
		end
	elseif kind == "waypoints" then
		waypointsShown = value
		if opened.waypoints then
			nox.window("waypoints", specOf("waypoints"))
		end
	elseif kind == "maps" then
		mapsShown = value
		if opened.maps then
			nox.window("maps", specOf("maps"))
		end
	elseif kind == "treeState" then
		whole[kind] = { header = value }
	elseif kind == "note" then
		notes[value.window] = { text = value.text, til = nox.time() + 4000, good = value.good == true }
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
	if (labelsShort or platesShort) and now >= labelRetryAt then
		-- (the plates first: a loot label gives way to one)
		if platesShort then
			setPlates()
		end
		if labelsShort then
			setMissing()
		end
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
	elseif window == "waypoints" then
		drawWaypoints()
	elseif window == "maps" then
		drawMaps()
	elseif window == "chars" then
		drawChars()
	end
end

-- A click on a ground item's name (a loot label, set clickable): the server picks it up when the
-- hero is in reach, else says it's too far.
function ui.labelClick(object: number)
	nox.send({ kind = "pick", code = object })
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
			-- (a zone's click may say it isn't its own: false)
			return z.fn(button or 1) ~= false
		end
	end
	return false
end

function ui.key(window: string, key: string): boolean
	-- (character select: a link code's digits typed, left deletes, enter sends it)
	if window == "chars" then
		local c = whole.chars
		local choosing = c ~= nil and not c.header.playing
		if choosing and #key == 1 and string.find(CODE_LETTERS, key, 1, true) then
			typeLetter(key)
		elseif choosing and key == "left" then
			typed = string.sub(typed, 1, #typed - 1)
		elseif choosing and key == "enter" then
			sendCode()
		elseif key == "escape" and not choosing then
			putAway()
		else
			return false
		end
		return true
	end
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
