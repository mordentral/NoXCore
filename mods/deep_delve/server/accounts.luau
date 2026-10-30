--!strict
-- Server characters' accounts (roster.luau): which PCs share a set of characters, and the link
-- codes that add a PC to one. Kept in the mod's store beside the characters:
--
--   pc/<identity>    { account, fails, failFrom }: the account a PC (identity.luau's segment)
--                    plays, and its wrong link codes lately
--   acct/<id>        { version, pcs = { { pc, label, since } }, labels, made, code }: the PCs of
--                    an account in the order they came (labels "PC 1", "PC 2"...: never the key),
--                    and its link code now
--   link/<code>      { account, by, expires }: a link code (lowercase), till it's used, replaced
--                    or out of time
--   guard/links      { from, n }: every wrong code on this store lately
--   srv/<id>/<class>/<name>   a character of the account (store.luau's record)
--
-- A PC is given an account of its own the first time it comes. A link code is made on a PC of an
-- account (one at a time: a new one replaces the last), shown to that player alone, and is good
-- for numbers.links.minutes and one use. Entered on another PC (before it chooses a character),
-- it moves that PC into the account: it sees and plays the account's characters from then on. The
-- PC's own characters go with it when no other PC was left on its old account (one of the same
-- class and name gets a number on its key).
--
-- Wrong codes are counted for the PC that tries them (numbers.links.tries in the window, then none
-- till the window's end) and for the whole store (numbers.links.storeTries in the window: then
-- nobody's, till its end).
--
-- Every read is straight from the store and written back at once: two games of the pool share
-- it, so nothing here is kept between calls.

local numbers = require("data/numbers")

local accounts = {}

-- The letters a code is made of: no 0, O, 1, I, L (each looks like another).
accounts.ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
local CODE_LENGTH = 8

-- The time now in seconds (the console may move it on, for the tests of a code's time).
local ahead = 0
function accounts.now(): number
	return os.time() + ahead
end
function accounts.moveClock(seconds: number)
	ahead += seconds
end

local function store(): any
	local s = nox.store
	assert(s, "no store")
	return s
end

-- A new account's id: "a" and ten letters and digits, one no account has.
local function newId(): string
	local s = store()
	local chars = "abcdefghijklmnopqrstuvwxyz0123456789"
	for _ = 1, 20 do
		local t = { "a" }
		local mix = math.floor(os.clock() * 1000003) + accounts.now()
		for i = 1, 10 do
			local k = (nox.random(1, #chars) + mix + i * 13) % #chars + 1
			mix = mix // 7
			table.insert(t, string.sub(chars, k, k))
		end
		local id = table.concat(t)
		if s.get("acct/" .. id) == nil then
			return id
		end
	end
	error("no free account id")
end

-- The account a PC plays: its id (made now, with the PC its first, when it has none).
function accounts.of(pc: string): string
	local s = store()
	local rec = s.get("pc/" .. pc)
	if type(rec) == "table" and type(rec.account) == "string" and type(s.get("acct/" .. rec.account)) == "table" then
		return rec.account
	end
	local id = newId()
	local now = accounts.now()
	s.put("acct/" .. id, { version = 1, pcs = { { pc = pc, label = "PC 1", since = now } }, labels = 1, made = now })
	s.put("pc/" .. pc, { account = id })
	return id
end

-- An account's record, or nil.
function accounts.get(id: string): any
	local rec = store().get("acct/" .. id)
	return if type(rec) == "table" then rec else nil
end

-- An account's PCs as the UI lists them: { { label, me } } in their order (`pc`: the asking one).
function accounts.pcs(id: string, pc: string): { any }
	local out = {}
	local rec = accounts.get(id)
	for _, e in (rec and rec.pcs or {}) do
		table.insert(out, { label = tostring(e.label), me = e.pc == pc })
	end
	return out
end

-- A link code for the account (the last one gone): the code, and how many minutes it's good for.
function accounts.makeCode(id: string, pc: string): (string?, string?)
	local s = store()
	local rec = accounts.get(id)
	if not rec then
		return nil, "no such account"
	end
	if type(rec.code) == "string" then
		s.remove("link/" .. string.lower(rec.code))
	end
	local code = ""
	for _ = 1, 20 do
		local t = {}
		local mix = math.floor(os.clock() * 1000003) + accounts.now() * 31
		for i = 1, CODE_LENGTH do
			local k = (nox.random(1, #accounts.ALPHABET) + mix + i * 17) % #accounts.ALPHABET + 1
			mix = mix // 5
			table.insert(t, string.sub(accounts.ALPHABET, k, k))
		end
		code = table.concat(t)
		if s.get("link/" .. string.lower(code)) == nil then
			break
		end
	end
	local expires = accounts.now() + numbers.links.minutes * 60
	s.put("link/" .. string.lower(code), { account = id, by = pc, expires = expires })
	rec.code = code
	s.put("acct/" .. id, rec)
	return code, nil
end

-- What a player typed made a code: upper case, spaces and dashes out; nil when it isn't one.
function accounts.normalise(text: string): string?
	local t = string.upper((string.gsub(text, "[%s%-]", "")))
	if #t ~= CODE_LENGTH then
		return nil
	end
	for i = 1, #t do
		if not string.find(accounts.ALPHABET, string.sub(t, i, i), 1, true) then
			return nil
		end
	end
	return t
end

-- Whether wrong codes stop this PC (or everyone) now: nil, or why.
local function stopped(s: any, pc: string, now: number): string?
	local window = numbers.links.window
	local g = s.get("guard/links")
	if type(g) == "table" and now < g.from + window and g.n >= numbers.links.storeTries then
		return "too many wrong link codes on this server: try again in a few minutes"
	end
	local p = s.get("pc/" .. pc)
	if type(p) == "table" and type(p.failFrom) == "number" and now < p.failFrom + window and (p.fails or 0) >= numbers.links.tries then
		local left = math.max(1, math.ceil((p.failFrom + window - now) / 60))
		return string.format("too many wrong link codes: try again in %d minute%s", left, if left == 1 then "" else "s")
	end
	return nil
end

-- A wrong code counted, for the PC and for the store.
local function counted(s: any, pc: string, now: number)
	local window = numbers.links.window
	local g = s.get("guard/links")
	if type(g) ~= "table" or now >= g.from + window then
		g = { from = now, n = 0 }
	end
	g.n += 1
	s.put("guard/links", g)
	local p = s.get("pc/" .. pc)
	if type(p) ~= "table" then
		return
	end
	if type(p.failFrom) ~= "number" or now >= p.failFrom + window then
		p.failFrom, p.fails = now, 0
	end
	p.fails = (p.fails or 0) + 1
	s.put("pc/" .. pc, p)
end

-- The characters of account `from` moved to `to` (a key taken there: a number after its name).
local function moveCharacters(s: any, from: string, to: string): number
	local moved = 0
	for _, key in s.list("srv/" .. from .. "/") do
		local rec = s.get(key)
		local rest = string.sub(key, #("srv/" .. from .. "/") + 1)
		if type(rec) == "table" and s.lock(key) then
			local target = "srv/" .. to .. "/" .. rest
			local n = 1
			while s.get(target) ~= nil and n < 100 do
				n += 1
				target = "srv/" .. to .. "/" .. rest .. "-" .. n
			end
			if type(rec.identity) == "table" then
				rec.identity.account = to
			end
			if s.put(target, rec) then
				s.remove(key)
				moved += 1
			end
			s.unlock(key)
		end
	end
	return moved
end

-- A code entered on PC `pc`: the PC moved into the code's account. Whether it was (and the
-- account), else why.
function accounts.enter(pc: string, text: string): (boolean, string)
	local s = store()
	local now = accounts.now()
	local why = stopped(s, pc, now)
	if why then
		return false, why
	end
	local code = accounts.normalise(text)
	if not code then
		return false, "a link code is 8 letters and digits"
	end
	local key = "link/" .. string.lower(code)
	local link = s.get(key)
	if type(link) == "table" and type(link.expires) == "number" and now >= link.expires then
		s.remove(key)
		link = nil
	end
	local target = if type(link) == "table" then accounts.get(tostring(link.account)) else nil
	if not target then
		counted(s, pc, now)
		return false, "no such link code, or it's out of time"
	end
	local id = tostring(link.account)
	local mine = accounts.of(pc)
	if mine == id then
		return false, "this PC is on that account already"
	end
	-- (the code used up)
	s.remove(key)
	target.code = nil
	target.labels = (tonumber(target.labels) or #target.pcs) + 1
	table.insert(target.pcs, { pc = pc, label = "PC " .. target.labels, since = now })
	s.put("acct/" .. id, target)
	s.put("pc/" .. pc, { account = id })
	-- (its old account: without it; with no PC left, its characters come along and it's gone)
	local old = accounts.get(mine)
	if old then
		local left = {}
		for _, e in old.pcs or {} do
			if e.pc ~= pc then
				table.insert(left, e)
			end
		end
		old.pcs = left
		if #left == 0 then
			moveCharacters(s, mine, id)
			if #s.list("srv/" .. mine .. "/") == 0 then
				if type(old.code) == "string" then
					s.remove("link/" .. string.lower(old.code))
				end
				s.remove("acct/" .. mine)
			else
				s.put("acct/" .. mine, old)
			end
		else
			s.put("acct/" .. mine, old)
		end
	end
	return true, id
end

-- PC number `index` of the account (as accounts.pcs lists them) taken off it by PC `pc` of the
-- account, one linked before it (never itself; so the account's first PC may unlink any other, and
-- a PC linked by a code that got out can't unlink the ones before it): it gets an account of its
-- own when it next comes. Whether it was, else why.
function accounts.unlink(id: string, pc: string, index: number): (boolean, string?)
	local s = store()
	local rec = accounts.get(id)
	if not rec then
		return false, "no such account"
	end
	local e = rec.pcs[index]
	if type(e) ~= "table" then
		return false, "no such PC"
	end
	if e.pc == pc then
		return false, "unlink this PC from another one"
	end
	local mine = nil
	for i, x in rec.pcs do
		if x.pc == pc then
			mine = i
		end
	end
	if not mine then
		return false, "not your account"
	end
	if mine > index then
		return false, "only a PC linked before that one can unlink it"
	end
	table.remove(rec.pcs, index)
	s.put("acct/" .. id, rec)
	s.remove("pc/" .. tostring(e.pc))
	return true, nil
end

return accounts
