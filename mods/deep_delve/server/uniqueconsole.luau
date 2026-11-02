--!strict
-- The host console's "dd uniques ..." (console.luau hands it on):
--
--   dd uniques [list]    each unique "unique ID TYPE CLASS FROM POWER MODS AFFIXES NAME", each set
--                        piece "set SETID PIECE TYPE CLASS FROM-TO MODS AFFIXES NAME", each set
--                        bonus "bonus SETID N WORDS" (MODS "slot:Name,...", AFFIXES
--                        "id:low-high,...", "-" for none)
--   dd uniques check     the data checked against the game (uniques.check): "ok", or what's wrong
--   dd uniques odds N LEVEL TIER [BOSS]   N deaths of a TIER monster (normal, magic, rare,
--                        champion, boss: its drop's rolls and its own) at the area level rolled,
--                        records only (nothing made): "rolls R gear G uniques U sets S favoured F",
--                        then "unique ID n" and "set SETID n" for each one seen
--   dd uniques drop unique|set ID [LEVEL]   one put at the first player's feet, shown as loot
--                        lies: "ok ITEMID"
--   dd uniques worn      the first player's worn sets ("set ID N"), pieces ("piece ID"), powers
--                        ("power KIND") and the stats its bonus has from them all
--   dd uniques powers    the first player's worn powers ("power KIND"), how often each went off
--                        ("count WHAT N") and their state (uniquepowers.lines)

local loot = require("loot")
local uniquePowers = require("uniquepowers")
local uniques = require("uniques")
local monsterData = require("data/monsters")
local numbers = require("data/numbers")

local uc = {}

local function odds(args: { string }): { string }
	local n = math.clamp(math.floor(tonumber(args[3]) or 1000), 1, 200000)
	local level = math.max(1, math.floor(tonumber(args[4]) or 1))
	local tier = args[5] or "normal"
	local boss = args[6]
	local t: any = (monsterData.tiers :: any)[tier]
	if not t then
		return { "no tier " .. tier }
	end
	local fav: { [string]: boolean } = {}
	for _, id in (if boss then (require("data/uniques").favoured :: any)[boss] else nil) or {} do
		fav[id] = true
	end
	local rolls, gear, nu, ns, nf = 0, 0, 0, 0, 0
	local seen: { [string]: number } = {}
	local function note(rec: any)
		if rec.unique then
			nu += 1
			seen["unique " .. rec.unique] = (seen["unique " .. rec.unique] or 0) + 1
			if fav[rec.unique] then
				nf += 1
			end
		elseif rec.set then
			ns += 1
			local s = uniques.setOf(rec.set)
			local key = "set " .. (if s then s.id else rec.set)
			seen[key] = (seen[key] or 0) + 1
		end
	end
	for _ = 1, n do
		local r, better, special = 1 + t.drops, t.drops * 0.25, nil
		if tier == "boss" then
			local d = numbers.boss.drop
			r, better = d.rolls, d.better
			special = uniques.rollSpecial(level + d.levelUp, "boss", boss)
		elseif tier == "champion" then
			special = uniques.rollSpecial(level, "champion", nil)
		end
		for _ = 1, r do
			rolls += 1
			local d = loot.rollOne(level, better)
			if d and d.kind == "gear" then
				gear += 1
				note(d.record)
			end
		end
		for _, rec in special or {} do
			note(rec)
		end
	end
	local out = { string.format("rolls %d gear %d uniques %d sets %d favoured %d", rolls, gear, nu, ns, nf) }
	local keys = {}
	for k in seen do
		table.insert(keys, k)
	end
	table.sort(keys)
	for _, k in keys do
		table.insert(out, k .. " " .. tostring(seen[k]))
	end
	return out
end

-- The stats a worn bonus may have from uniques and sets (and the affixes beside them).
local STATS = { "health", "mana", "damage", "resist", "manaRegen", "healthRegen", "strength", "summonHealth", "manaOnKill" }

function uc.uniques(args: { string }, h: any): { string }
	local op = args[2] or "list"
	if op == "list" then
		return uniques.lines()
	elseif op == "check" then
		return uniques.check(uniquePowers.KINDS)
	elseif op == "odds" then
		return odds(args)
	end
	if not h then
		return { "no player" }
	end
	if op == "drop" then
		local level = tonumber(args[5])
		local rec = if args[3] == "unique" then uniques.make(args[4] or "", level) elseif args[3] == "set" then uniques.makePiece(args[4] or "", level) else nil
		if not rec or not nox.present(h.unit) then
			return { "not: no such " .. tostring(args[3]) .. " " .. tostring(args[4]) }
		end
		local x, y = nox.position(h.unit)
		local px, py = nox.clearPointNear(x + 40, y, 30)
		local id = loot.place(rec, px, py)
		return { if id then "ok " .. tostring(id) else "not: not made" }
	elseif op == "powers" then
		return uniquePowers.lines(h)
	elseif op == "worn" then
		local out = {}
		local b = h.bonus or {}
		local sets = {}
		for id in b.sets or {} do
			table.insert(sets, id)
		end
		table.sort(sets)
		for _, id in sets do
			table.insert(out, "set " .. id .. " " .. tostring(b.sets[id]))
		end
		local pieces = {}
		for id in b.pieces or {} do
			table.insert(pieces, id)
		end
		table.sort(pieces)
		for _, id in pieces do
			table.insert(out, "piece " .. id)
		end
		local kinds = {}
		for kind in b.powers or {} do
			table.insert(kinds, kind)
		end
		table.sort(kinds)
		for _, kind in kinds do
			table.insert(out, "power " .. kind)
		end
		for _, stat in STATS do
			table.insert(out, "stat " .. stat .. " " .. tostring(b[stat] or 0))
		end
		return out
	end
	return { "dd uniques [list | check | odds N LEVEL TIER [BOSS] | drop unique|set ID [LEVEL] | worn | powers]" }
end

return uc
