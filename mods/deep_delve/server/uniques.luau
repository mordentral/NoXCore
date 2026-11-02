--!strict
-- Unique items and item sets (data/uniques.luau): their records, their drops, what the pieces a
-- hero wears add up to, and their tooltip lines. Their powers at work are powers.luau's.
--
-- A unique's record: { type, rarity = "unique", unique = its id, level, its fixed modifiers by
-- slot, affixes (its fixed ones, and its ranged ones as drawn when it dropped) }. A set piece's:
-- { type, rarity = "set", set = the piece's id, level, modifiers, affixes }. The store keeps them as
-- it keeps any item record, so they come back the same through a save, the stash and a hand-over;
-- the power and the set come from the data by id.
--
-- Worn: the hero's bonus (hero.luau's bonusOf) gets `powers` (a power's kind -> its numbers: a
-- unique's first, then a set bonus's of a kind no unique gave), `sets` (set id -> pieces worn),
-- `pieces` (piece id -> true) and `setSig` (the sets worn, as words), and each active set bonus's
-- stats added in. Only the pieces worn count; two of one piece count once.

local data = require("data/uniques")
local affixes = require("affixes")

local uniques = {}

-- Indexes, made once.
local byId: { [string]: any } = {}
local pieceById: { [string]: any } = {}
local setById: { [string]: any } = {}
local setOfPiece: { [string]: any } = {}
for _, def in data.uniques :: { any } do
	byId[def.id] = def
end
for _, s in data.sets :: { any } do
	setById[s.id] = s
	for _, p in s.pieces do
		pieceById[p.id] = p
		setOfPiece[p.id] = s
	end
end

function uniques.def(id: string): any
	return byId[id]
end

function uniques.piece(id: string): any
	return pieceById[id]
end

function uniques.set(id: string): any
	return setById[id]
end

-- The set a piece is of.
function uniques.setOf(pieceId: string): any
	return setOfPiece[pieceId]
end

-- ---- Draws
--
-- Their own stream (a seeded mt19937, made at the first draw from the frame then), so the rolls
-- here never move the mod's own stream that the rest of the loot draws from.

local stream: any = nil

local function draw(): number
	local s = stream
	if not s then
		s = nox.mt19937(nox.mul32(nox.frame() + 1, 2654435761))
		stream = s
	end
	return s:next() / 4294967296
end

local function drawIndex(n: number): number
	return math.min(n, math.floor(draw() * n) + 1)
end

-- ---- Records

local function affixValue(a: any): number
	if a.value then
		return a.value
	end
	return math.floor(a.low + draw() * (a.high - a.low) + 0.5)
end

local function fill(rec: any, def: any)
	for slot, name in def.mods or {} do
		rec[slot] = name
	end
	local list = {}
	for _, a in def.affixes or {} do
		table.insert(list, { id = a.id, value = affixValue(a) })
	end
	if #list > 0 then
		rec.affixes = list
	end
end

-- A unique's record at the area level (its ranged affixes drawn); nil: no such unique.
function uniques.make(id: string, level: number?): any
	local def = byId[id]
	if not def then
		return nil
	end
	local rec: any = { type = def.type, rarity = "unique", unique = id, level = math.max(level or def.from, def.from) }
	fill(rec, def)
	return rec
end

-- A set piece's record at the area level; nil: no such piece.
function uniques.makePiece(id: string, level: number?): any
	local p = pieceById[id]
	if not p then
		return nil
	end
	local s = setOfPiece[id]
	local rec: any = { type = p.type, rarity = "set", set = id, level = math.max(level or s.from, s.from) }
	fill(rec, p)
	return rec
end

-- A record's own name: a unique's or a set piece's; nil for any other.
function uniques.nameOf(rec: any): string?
	if rec.unique then
		local def = byId[rec.unique]
		return if def then def.name else nil
	elseif rec.set then
		local p = pieceById[rec.set]
		return if p then p.name else nil
	end
	return nil
end

-- ---- Drops

-- The uniques the area level allows, and the set pieces of the sets whose levels hold it; made
-- once a level.
local eligibleOf: { [number]: any } = {}

local function eligible(level: number): any
	local e = eligibleOf[level]
	if e then
		return e
	end
	e = { uniques = {}, pieces = {} }
	for _, def in data.uniques :: { any } do
		if def.from <= level then
			table.insert(e.uniques, def.id)
		end
	end
	for _, s in data.sets :: { any } do
		if s.from <= level and level <= s.to then
			for _, p in s.pieces do
				table.insert(e.pieces, p.id)
			end
		end
	end
	eligibleOf[level] = e
	return e
end

local function anyOf(list: { string }): string?
	if #list == 0 then
		return nil
	end
	return list[drawIndex(#list)]
end

-- A unique's record for a roll at the area level (`boss`: a boss's id, whose favoured uniques take
-- their share), or nil when none may drop there.
local function pickUnique(level: number, boss: string?): any
	local e = eligible(level)
	local fav = if boss then (data.favoured :: any)[boss] else nil
	if fav and draw() < data.drops.favoured then
		local allowed = {}
		for _, id in fav do
			if byId[id] and byId[id].from <= level then
				table.insert(allowed, id)
			end
		end
		local id = anyOf(allowed)
		if id then
			return uniques.make(id, level)
		end
	end
	local id = anyOf(e.uniques)
	return if id then uniques.make(id, level) else nil
end

local function pickPiece(level: number): any
	local id = anyOf(eligible(level).pieces)
	return if id then uniques.makePiece(id, level) else nil
end

-- On a loot roll that gives gear at the area level (`better`: the monster's betterness): a unique's
-- or a set piece's record in its place, or nil.
function uniques.rollGear(level: number, better: number?): any
	local d = data.drops
	local boost = 1 + d.boost * (better or 0)
	if draw() < d.unique.gear * boost then
		local rec = pickUnique(level, nil)
		if rec then
			return rec
		end
	end
	if draw() < d.set.gear * boost then
		return pickPiece(level)
	end
	return nil
end

-- A champion's or a boss's own rolls (`tier` "champion" or "boss"; `boss` its id): the records
-- they give (none, one or two).
function uniques.rollSpecial(level: number, tier: string, boss: string?): { any }
	local out = {}
	local d = data.drops
	local cu: number? = (d.unique :: any)[tier]
	local cs: number? = (d.set :: any)[tier]
	if cu and draw() < cu then
		local rec = pickUnique(level, boss)
		if rec then
			table.insert(out, rec)
		end
	end
	if cs and draw() < cs then
		local rec = pickPiece(level)
		if rec then
			table.insert(out, rec)
		end
	end
	return out
end

-- ---- Worn

-- The worn pieces' metas (items.meta's: { unique } or { set }) into the hero's bonus `b`: its
-- powers, its sets and their active bonuses' stats.
function uniques.worn(b: any, worn: { any })
	local powers: { [string]: any } = {}
	local sets: { [string]: number } = {}
	local pieces: { [string]: boolean } = {}
	for _, m in worn do
		if m.unique then
			local def = byId[m.unique]
			if def and not powers[def.power.kind] then
				powers[def.power.kind] = def.power
			end
		elseif m.set and pieceById[m.set] and not pieces[m.set] then
			pieces[m.set] = true
			local s = setOfPiece[m.set]
			sets[s.id] = (sets[s.id] or 0) + 1
		end
	end
	local sig = {}
	for _, s in data.sets :: { any } do
		local n = sets[s.id]
		if n then
			table.insert(sig, s.id .. ":" .. tostring(n))
			for _, bonus in s.bonuses do
				if n >= bonus.n then
					for stat, v in bonus.stats or {} do
						local def = affixes.def(stat)
						local key = if def then def.stat else stat
						b[key] = (b[key] or 0) + v
					end
					if bonus.power and not powers[bonus.power.kind] then
						powers[bonus.power.kind] = bonus.power
					end
				end
			end
		end
	end
	b.powers, b.sets, b.pieces, b.setSig = powers, sets, pieces, table.concat(sig, " ")
end

-- ---- Words

-- A power's line, from its numbers.
function uniques.powerText(p: any): string
	local t: any = (data.powerText :: any)[p.kind]
	if not t then
		return p.kind
	end
	local args = {}
	for i = 2, #t do
		table.insert(args, p[t[i]] or 0)
	end
	return string.format(t[1], table.unpack(args))
end

-- A set bonus's words: its stats ("+25 health"), then its power's line.
function uniques.bonusText(bonus: any): string
	local parts = {}
	local stats = {}
	for stat in bonus.stats or {} do
		table.insert(stats, stat)
	end
	table.sort(stats)
	for _, stat in stats do
		local def = affixes.def(stat)
		table.insert(parts, if def then string.format(def.text, bonus.stats[stat]) else stat)
	end
	if bonus.power then
		table.insert(parts, uniques.powerText(bonus.power))
	end
	return table.concat(parts, ", ")
end

-- Words cut into lines of at most `width` letters.
local function wrap(text: string, width: number): { string }
	local out = {}
	local line = ""
	for word in string.gmatch(text, "%S+") do
		local try = if line == "" then word else line .. " " .. word
		if #try > width and line ~= "" then
			table.insert(out, line)
			line = word
		else
			line = try
		end
	end
	if line ~= "" then
		table.insert(out, line)
	end
	return out
end

-- The lines a unique's or a set piece's tooltip adds after its affixes (tips.luau): a unique's
-- power (u); a set's name and how many of it the viewer wears (g), each piece (w worn by the
-- viewer, o not), each bonus (v active for the viewer, q not). `h` the hero looking (nil: none).
function uniques.tipLines(rec: any, h: any?): { string }
	local out = {}
	local width = data.tip.wrap
	if rec.unique then
		local def = byId[rec.unique]
		if def then
			for _, l in wrap(uniques.powerText(def.power), width) do
				table.insert(out, "u" .. l)
			end
		end
	elseif rec.set then
		local s = setOfPiece[rec.set]
		if s then
			local b: any = h and h.bonus
			local worn: number = if b and b.sets then b.sets[s.id] or 0 else 0
			local mine: { [string]: boolean } = if b and b.pieces then b.pieces else {}
			local of: number = #s.pieces
			table.insert(out, "g" .. tostring(s.name) .. " (" .. tostring(worn) .. " of " .. tostring(of) .. " worn)")
			for _, p in s.pieces do
				table.insert(out, (if mine[p.id] then "w" else "o") .. "  " .. p.name)
			end
			for _, bonus in s.bonuses do
				local tag = if worn >= bonus.n then "v" else "q"
				for i, l in wrap(string.format("(%d) %s", bonus.n, uniques.bonusText(bonus)), width) do
					table.insert(out, tag .. (if i > 1 then "    " else "") .. l)
				end
			end
		end
	end
	return out
end

-- The most lines a record's tooltip may take (nil: the ordinary most).
function uniques.mostLines(rec: any): number?
	return if rec.unique or rec.set then data.tip.most else nil
end

-- ---- The console's lists ("dd uniques")

-- A piece's modifiers ("slot:Name,..." in slot order, "-" for none) and affixes ("id:low-high,...").
local SLOTS = { "effectiveness", "material", "primary", "secondary" }
local function partsOf(def: any): (string, string)
	local mods = {}
	for _, slot in SLOTS do
		if def.mods and def.mods[slot] then
			table.insert(mods, slot .. ":" .. def.mods[slot])
		end
	end
	local list = {}
	for _, a in def.affixes or {} do
		local lo: number, hi: number = a.value or a.low, a.value or a.high
		table.insert(list, a.id .. ":" .. tostring(lo) .. "-" .. tostring(hi))
	end
	return (if #mods > 0 then table.concat(mods, ",") else "-"), (if #list > 0 then table.concat(list, ",") else "-")
end

-- Each unique: "unique ID TYPE CLASS FROM KIND MODS AFFIXES NAME"; each set piece: "set SETID
-- PIECE TYPE CLASS FROM-TO MODS AFFIXES NAME"; each set's bonuses: "bonus SETID N WORDS".
function uniques.lines(): { string }
	local out = {}
	for _, def in data.uniques :: { any } do
		local mods, list = partsOf(def)
		local words: { string } = { "unique", tostring(def.id), tostring(def.type), tostring(def.class), tostring(def.from), tostring(def.power.kind), mods, list, tostring(def.name) }
		table.insert(out, table.concat(words, " "))
	end
	for _, s in data.sets :: { any } do
		for _, p in s.pieces do
			local mods, list = partsOf(p)
			local words: { string } = { "set", tostring(s.id), tostring(p.id), tostring(p.type), tostring(s.class), tostring(s.from) .. "-" .. tostring(s.to), mods, list, tostring(p.name) }
			table.insert(out, table.concat(words, " "))
		end
		for _, bonus in s.bonuses do
			table.insert(out, string.format("bonus %s %d %s", s.id, bonus.n, uniques.bonusText(bonus)))
		end
	end
	return out
end

-- The data checked against the game: every type thing.bin's, every modifier one that fits its
-- slot of the type (the game's rule), every affix known, the class it's for may use it (a unique;
-- a set: each piece), every power's kind known, its spells the game's; "ok", or a line each wrong.
function uniques.check(known: any): { string }
	local out = {}
	local function piece(what: string, def: any, class: string)
		if not nox.thingType(def.type) then
			table.insert(out, what .. ": no type " .. tostring(def.type))
			return
		end
		for slot, name in def.mods or {} do
			if not nox.modifier(name) or not nox.modifierFits(def.type, slot, name) then
				table.insert(out, what .. ": " .. tostring(name) .. " doesn't fit a " .. def.type .. "'s " .. tostring(slot))
			end
		end
		for _, a in def.affixes or {} do
			if not affixes.def(a.id) then
				table.insert(out, what .. ": no affix " .. tostring(a.id))
			end
		end
		local st = nox.itemStats(def.type)
		if not st or not (st.classes :: any)[class] then
			table.insert(out, what .. ": a " .. class .. " can't use a " .. def.type)
		end
	end
	local function power(what: string, p: any)
		if not known[p.kind] or not (data.powerText :: any)[p.kind] then
			table.insert(out, what .. ": no power " .. tostring(p.kind))
		end
		if p.spell and not nox.spellId(p.spell) then
			table.insert(out, what .. ": no spell " .. tostring(p.spell))
		end
		for _, sp in p.spells or {} do
			if not nox.spellId(sp) then
				table.insert(out, what .. ": no spell " .. tostring(sp))
			end
		end
		if p.creature and not nox.thingType(p.creature) then
			table.insert(out, what .. ": no creature " .. tostring(p.creature))
		end
	end
	local seen: { [string]: boolean } = {}
	for _, def in data.uniques :: { any } do
		if seen[def.id] then
			table.insert(out, def.id .. ": twice")
		end
		seen[def.id] = true
		piece(def.id, def, def.class)
		power(def.id, def.power)
	end
	for _, s in data.sets :: { any } do
		if #s.pieces < 2 or #s.pieces > 4 then
			table.insert(out, s.id .. ": " .. #s.pieces .. " pieces")
		end
		for _, p in s.pieces do
			if seen[p.id] then
				table.insert(out, p.id .. ": twice")
			end
			seen[p.id] = true
			piece(s.id .. "/" .. p.id, p, s.class)
		end
		for _, bonus in s.bonuses do
			if bonus.power then
				power(s.id .. " (" .. bonus.n .. ")", bonus.power)
			end
			for stat in bonus.stats or {} do
				if not affixes.def(stat) then
					table.insert(out, s.id .. ": no stat " .. stat)
				end
			end
		end
	end
	for boss, list in data.favoured :: any do
		for _, id in list do
			if not byId[id] then
				table.insert(out, "favoured " .. tostring(boss) .. ": no unique " .. tostring(id))
			end
		end
	end
	return if #out > 0 then out else { "ok" }
end

return uniques
