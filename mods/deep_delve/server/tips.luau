--!strict
-- Items' tooltips: the lines an item's tooltip shows, made here (the server knows the record, the
-- game item's facts, the type's stats and the viewer) and sent with the stash's, the vendor's and
-- the carried lists (windows.entry); and every item a hero holds (worn too) by its net code, for
-- its UI's tooltip over the game's inventory and weapon panel (ui.itemTip), sent as it changes.
--
-- A line is a string whose first letter says what it is (the UI colours it by that), the rest its
-- words: n the item's name (its rarity's colour; a unique's or a set piece's own name, then a k
-- line with its type's), k its rarity, kind and area level, b its damage or armour (its total, its
-- modifiers applied as the engine applies them), m one of the game's modifiers (or "no effect"
-- where it does nothing), a one of Deep Delve's affixes, then a unique's power (u) or a set
-- piece's set (g its name, w and o its pieces worn or not, v and q its bonuses on or not:
-- uniques.tipLines), s its durability, charges or use, y or x whether the viewer's class may use
-- it. At most data/tips.most lines (a unique's or a set piece's, data/uniques' tip.most); the
-- window adds its price.
--
-- The numbers are the game's: a type's from nox.itemStats (its name, most durability and charges,
-- damage, armour, classes), a modifier's from nox.modifier (its identify line, its word, its
-- effects' numbers), a live item's durability and charges from nox.item. Each type's and
-- modifier's are read once.

local hero = require("hero")
local hud = require("hud")
local items = require("items")
local loot = require("loot")
local uniques = require("uniques")
local words = require("data/tips")

local tips = {}

-- Read once each: type -> its stats (false: none); type -> its name; "type|modifier" -> its line.
local statsOf: { [string]: any } = {}
local names: { [string]: string } = {}
local modLines: { [string]: string } = {}

local function stats(thingType: string): any
	local s = statsOf[thingType]
	if s == nil then
		s = nox.itemStats(thingType) or false
		statsOf[thingType] = s
	end
	return s or nil
end

-- A name split into words: "LesserFireballWand" -> "Lesser Fireball Wand".
local function split(name: string): string
	local s = (string.gsub(name, "(%l)(%u)", "%1 %2"))
	s = (string.gsub(s, "(%u)(%u%l)", "%1 %2"))
	return (string.gsub(s, "(%a)(%d)", "%1 %2"))
end

-- An item type's name: the game's; on a host without its string table, data/tips's or the split
-- name (a wand is a staff, armor armour).
function tips.name(thingType: string): string
	local n = names[thingType]
	if n then
		return n
	end
	local st = stats(thingType)
	local def = (words.items :: any)[thingType]
	if st and type(st.name) == "string" and st.name ~= "" then
		n = st.name
	elseif def and def.name then
		n = def.name
	else
		n = (string.gsub(split(thingType), " Wand$", " Staff"))
		n = (string.gsub(n :: string, "Armor", "Armour"))
	end
	names[thingType] = n :: string
	return n :: string
end

-- An effect's number as data/tips reads it ("+8% weapon damage"); nil: none it reads.
local function effectText(fx: { [string]: number }?): string?
	if not fx then
		return nil
	end
	for fn: string, how: any in words.effects :: any do
		local v: number? = fx[fn]
		if v then
			local said: string = tostring(how.words)
			if how.how == "mul" then
				local p: number = math.floor((v - 1) * 100 + 0.5)
				return if p == 0 then nil else string.format("%+d%% %s", p, said)
			elseif how.how == "add" then
				return string.format("+%s %s", tostring(v), said)
			elseif how.how == "wear" then
				local p: number = math.floor((v - 1) * 100 + 0.5)
				return if p == 0 then nil else string.format("%d%% %s %s", math.abs(p), if p > 0 then "less" else "more", said)
			else
				return string.format("%+d%% %s", math.floor(v * 100 + 0.5), said)
			end
		end
	end
	return nil
end

-- A game modifier's label (its identify line, else its word, else its family's name and step) and
-- its live line: the label, then its effect's number where data/tips reads one ("Level 2 Fire: +6
-- fire damage", "Mighty: +8% weapon damage"), or what its family does. Read once a modifier.
local function readModifier(name: string): (string, string)
	local m = nox.modifier(name)
	local family, step = string.match(name, "^(.-)(%d+)$")
	local def = (words.modifiers :: any)[family or name]
	local label: string
	local identify = m and m.identify
	if identify and identify ~= "" then
		label = identify
	elseif m and m.text and m.text ~= "" then
		label = m.text
	elseif def then
		label = def.name .. (if step then " " .. step .. (if def.of then " of " .. tostring(def.of) else "") else "")
	else
		label = split(name)
	end
	local fx = m and m.effects
	local number = effectText(fx)
	local text = label
	if number then
		text ..= ": " .. number
	elseif not (identify and identify ~= "") and def and def.does then
		text ..= ": " .. def.does
	end
	return label, text
end

-- A game modifier's line on an item of `thingType`: its live line, or "LABEL: no effect" when it
-- does nothing there (loot.doesOn: no effect the engine runs, none at all, or only one that scales
-- a number the type hasn't: an armour quality on a shield). Read once a type and modifier.
function tips.modifier(name: string, thingType: string): string
	local key = thingType .. "|" .. name
	local line = modLines[key]
	if line then
		return line
	end
	local label, text = readModifier(name)
	line = if loot.doesOn(thingType, name) then text else label .. ": no effect"
	modLines[key] = line
	return line
end

-- A blow's damage with the item, as the engine works it out: its record's (`hit`: the weapon's,
-- or a bow's arrow's) damageMin plus its coefficient for each point of strength over the
-- requirement, made a float; then each of its slots' DamageMultiplierEffect in turn, each product
-- a float (a bow's arrows carry the quiver's modifiers, not the bow's: `shot`, none); rounded as a
-- blow's damage is, halves up. No strength (no hero): its damageMin.
function tips.damage(rec: any, hit: any, strength: number?, shot: boolean): number
	local d: number = hit.damageMin
	if strength and hit.damageCoefficient then
		d = nox.f32((strength - (hit.requiredStrength or 0)) * hit.damageCoefficient + hit.damageMin)
	end
	if not shot then
		for _, slot in items.SLOTS do
			local m = if rec[slot] then nox.modifier(rec[slot]) else nil
			local v = m and m.effects.DamageMultiplierEffect
			if v then
				d = nox.f32(nox.f32(v) * d)
			end
		end
	end
	return math.max(0, math.floor(d + 0.5))
end

-- A piece of armour's value (the share of a hit it takes off), as the engine works it out: its
-- type's ARMOR_VALUE (`base`) by its effectiveness's ArmorMultiplierEffect (a
-- DurabilityMultiplierEffect there: by 2 less its number), the product a float.
function tips.armour(rec: any, base: number): number
	local m = if rec.effectiveness then nox.modifier(rec.effectiveness) else nil
	if not m then
		return base
	end
	local a, du = m.effects.ArmorMultiplierEffect, m.effects.DurabilityMultiplierEffect
	if a then
		return nox.f32(nox.f32(a) * base)
	elseif du then
		return nox.f32(((1 - nox.f32(du)) + 1) * base)
	end
	return base
end

-- A share as a percentage, always to a tenth ("25.0%", "20.8%"; by decision 2026-11-01).
local function percent(share: number): string
	return string.format("%.1f%%", math.floor(share * 1000 + 0.5) / 10)
end

-- An item's tooltip lines: its record, the game item's facts when it's a live one (its durability
-- and charges as they are; else the record's against its type's most), and the hero looking (its
-- class's line, its strength's damage; nil: neither).
function tips.lines(rec: any, facts: any?, h: any?): { string }
	-- (a unique or a set piece: its own name, then its type's as its base)
	local own = uniques.nameOf(rec)
	local out = { "n" .. (own or tips.name(rec.type)) }
	if own then
		table.insert(out, "k" .. tips.name(rec.type))
	end
	local st = stats(rec.type)
	local def = (words.items :: any)[rec.type]
	local kind = if def then def.kind else "item"
	local k: string
	if kind == "potion" then
		k = "Potion"
	else
		k = ((words.rarity :: any)[rec.rarity or "normal"] or "Normal") .. " " .. kind
	end
	if rec.level then
		k ..= ", area level " .. tostring(rec.level)
	end
	table.insert(out, "k" .. k)
	-- Its damage (a melee weapon's blow, or a bow's arrow's) or armour: the item's total as the
	-- engine works it out (tips.damage, tips.armour); its modifier lines say what each part does.
	local hit = if def and def.ammo then stats(def.ammo) else st
	if hit and hit.damageMin and hit.damageMin > 0 and kind ~= "spell staff" then
		local strength = if h and nox.present(h.unit) then nox.strength(h.unit) else nil
		local shot = def ~= nil and def.ammo ~= nil
		table.insert(out, string.format("bDamage %d%s", tips.damage(rec, hit, strength, shot), if shot then " a shot" else ""))
	elseif st and st.armor and st.armor > 0 then
		table.insert(out, "bArmour " .. percent(tips.armour(rec, st.armor)))
	end
	for _, slot in items.SLOTS do
		if rec[slot] then
			table.insert(out, "m" .. tips.modifier(rec[slot], rec.type))
		end
	end
	for _, a in rec.affixes or {} do
		table.insert(out, "a" .. items.affixText(a))
	end
	-- (a unique's power; a set piece's set, its pieces and bonuses as the viewer wears them)
	for _, l in uniques.tipLines(rec, h) do
		table.insert(out, l)
	end
	if def and def.does then
		table.insert(out, "s" .. def.does)
	end
	local mostD: number = if facts then facts.maxDurability elseif st then st.durability else 0
	if mostD > 0 then
		local d: number = if facts then facts.durability else rec.durability or mostD
		table.insert(out, string.format("sDurability %d/%d", d, mostD))
	end
	local mostC: number = if facts then facts.maxCharges elseif st then st.charges else 0
	if mostC > 0 then
		local c: number = if facts then facts.charges else rec.charges or mostC
		table.insert(out, string.format("sCharges %d/%d", c, mostC))
	end
	local class = h and h.record.identity.class
	if st and st.classes and class and kind ~= "potion" then
		table.insert(out, if st.classes[class] then "yYour class can use it" else "xYour class can't use it")
	end
	local most = uniques.mostLines(rec) or words.most
	while #out > most do
		table.remove(out)
	end
	return out
end

-- ---- What the hero holds, by net code
--
-- For its UI's tooltip over the game's inventory and weapon panel (ui.itemTip; the UI asks with
-- { kind = "tips", on = true } the first time the client hands it an item to tip): each item the
-- hero holds, worn or not, as { kind = "tip", n = net code, r = rarity, tl = lines }, sent as the
-- hero picks it up, puts it on or takes it off, and as its durability or charges change;
-- { kind = "tip", n } once it's gone. Each under its own key ("tip<code>"), so a newer value of an
-- item replaces one not sent yet. A beat per hero that asked looks every half second (an item's
-- facts change with no hook: a hit wears it, a cast spends a charge); a pickup or an equip looks
-- at once; the hero's strength changing (its damage lines) sends them all again.

local BEAT = 15

-- Its held items looked at: what changed sent.
function tips.check(h: any)
	local t = h.tips
	if not t or not h.ready or not nox.present(h.unit) then
		return
	end
	local strength = nox.strength(h.unit)
	if strength ~= t.strength then
		t.strength = strength
		t.shown = {}
	end
	local shown = t.shown
	local now: { [number]: string } = {}
	for _, it in nox.inventory(h.unit) do
		local o = nox.object(it.id)
		local f = nox.item(it.id)
		if o and not o.pendingDelete and f then
			local code = o.netCode
			local sig = string.format("%d:%s:%d:%d", it.id, if f.worn then "w" else "c", f.durability, f.charges)
			-- (a set piece's lines change with the set's pieces worn)
			local m = items.meta(it.id)
			if m and m.set and h.bonus then
				sig ..= ":" .. tostring(h.bonus.setSig or "")
			end
			now[code] = sig
			if shown[code] ~= sig then
				local rec = items.recordOf(it.id)
				if rec then
					hud.send(h.peer, "tip" .. code, { kind = "tip", n = code, r = rec.rarity, tl = tips.lines(rec, f, h) })
				end
			end
		end
	end
	for code in shown do
		if now[code] == nil then
			hud.send(h.peer, "tip" .. code, { kind = "tip", n = code })
		end
	end
	t.shown = now
end

-- The hero's UI asks for its held items' tooltips (on), or stops; asked again, all of them again.
function tips.want(h: any, on: boolean)
	if not on then
		if h.tips then
			nox.cancelTimer(h.tips.beat)
			h.tips = nil
		end
		return
	end
	if h.tips then
		h.tips.shown = {}
	else
		local peer = h.peer
		local t: any = { shown = {} }
		h.tips = t
		t.beat = nox.every(BEAT, function()
			-- (the hero gone, or another in its place: its beat ends)
			if hero.byPeer(peer) ~= h or h.tips ~= t then
				nox.cancelTimer(t.beat)
				return
			end
			tips.check(h)
		end)
	end
	tips.check(h)
end

-- Something changed in a unit's inventory (a pickup, an equip): looked at now.
function tips.changed(unit: number)
	local h = hero.byUnit(unit)
	if h and h.tips then
		tips.check(h)
	end
end

-- What a hero's UI was last sent ("dd tips"): "code id:worn:durability:charges" each, sorted.
function tips.shownLines(h: any): { string }
	if not h.tips then
		return { "off" }
	end
	local out = {}
	for code, sig in h.tips.shown do
		table.insert(out, tostring(code) .. " " .. sig)
	end
	table.sort(out)
	return out
end

return tips
