--!strict
-- Server characters (design 5): on a server that keeps its players' characters itself (the mode
-- option characters=server, as the bot server's pool sets it), a player chooses its character at
-- character select. Elsewhere (a player's own host, single player) none of this runs: a player's
-- character is the one its join names (store.keyFor), as always.
--
-- Here the characters are the server's, kept under the PC's account (accounts.luau), never the
-- account the client names:
--
--   * At its join a player is held at character select (its unit given nothing, its own things
--     taken away for the game: hero.setUp's rule) and its UI shown the account's characters, each
--     to play when it's of the class the player joined as; a new one of that class, named as the
--     player is; the account's PCs (to unlink one); a link code to make; a code to enter.
--   * The client sends only what it chose (a row's number, "new", a code typed): it never sends
--     a record. Every change after is an action the host checks, as on any host.
--   * A character is held for its game (store.hold): never played in two games of the pool at
--     once, nor by two players of one game.
--   * Without the mod's UI (a client that has none, `fallback` frames on) the player plays the
--     account's character of its class and name, or a new one.
--   * A link code may also be entered by saying "link CODE" (taken, never shown to anyone), and
--     made by saying "link" (told to the player alone).
--
-- While at character select a player isn't a hero (hero.luau): it counts for nothing in the
-- party, its votes and its scaling until it plays.

local accounts = require("accounts")
local hero = require("hero")
local hud = require("hud")
local identity = require("identity")
local items = require("items")
local store = require("store")
local numbers = require("data/numbers")
local acts = require("data/acts")

local roster = {}

-- Each player's seat here: { peer, name, class, pc, proven, account, list = { key },
-- playing (its character chosen), serial, last (the frame of its last action) }.
local seats: { [number]: any } = {}
local serial = 0
local active: boolean? = nil
-- What happens when a player's character is chosen: main.luau's (hero.join and what follows).
local start: ((p: any, chosen: any) -> ())? = nil

-- A client without the mod's UI plays its own character this many frames after its map is in.
local FALLBACK = 30 * 10

-- Whether this game keeps server characters.
function roster.active(): boolean
	if active == nil then
		active = nox.option("characters") == "server" and nox.store ~= nil
		hud.serverCharacters = active == true
	end
	return active == true
end

-- How a chosen character comes into play (main.luau's).
function roster.onChosen(fn: (p: any, chosen: any) -> ())
	start = fn
end

local function note(peer: number, text: string, good: boolean?)
	hud.send(peer, "note:chars", { kind = "note", window = "chars", text = text, good = good == true })
end

-- Whether another player of this game plays the character `key`.
local function playedHere(key: string, peer: number): boolean
	for p, h in hero.all() do
		if p ~= peer and h.key == key then
			return true
		end
	end
	return false
end

-- The account's characters as rows: { key, n (name), c (class), l (level), a (act), p (playable
-- now: the class the player joined as), busy (played by another here) }.
local function rowsOf(s: any): { any }
	local out = {}
	for _, key in nox.store and (nox.store :: any).list("srv/" .. s.account .. "/") or {} do
		local rec = store.load(key)
		if type(rec) == "table" and type(rec.identity) == "table" then
			local class = tostring(rec.identity.class)
			table.insert(out, {
				key = key,
				n = string.sub(tostring(rec.identity.name), 1, 16),
				c = class,
				l = rec.level.level,
				a = rec.world.act,
				p = class == s.class,
				busy = playedHere(key, s.peer),
				me = s.key == key,
			})
		end
	end
	return out
end

-- The character select window's value to the player's UI (the rows in parts: hud.parts; one value
-- each once the big sends are in, A11), with whether its window is open (`open`: open while the
-- player chooses, and while one playing has it open from its Character window). A newer value
-- replaces one not sent yet, so the window's state goes with every value, never alone.
local function send(s: any)
	local rows = rowsOf(s)
	s.list = {}
	local list = {}
	local haveNew = false
	for i, r in rows do
		s.list[i] = r.key
		haveNew = haveNew or (r.c == s.class and r.key == store.serverKey(s.account, s.class, s.name))
		table.insert(list, { n = r.n, c = r.c, l = r.l, a = r.a, p = r.p, busy = r.busy, me = r.me })
	end
	serial += 1
	hud.parts(s.peer, "chars", serial, list, {
		show = s.open == true,
		playing = s.playing == true,
		class = s.class,
		name = string.sub(tostring(s.name), 1, 16),
		new = not s.playing and not haveNew and #rows < numbers.roster.most,
		full = #rows >= numbers.roster.most,
		proven = s.proven,
		pcs = accounts.pcs(s.account, s.pc),
		acts = #acts,
	})
end

-- The others at character select on account `account` shown its list again (a character of it
-- taken into play here, or let go).
local function others(account: string?, peer: number)
	for p, s in seats do
		if p ~= peer and s.pc and not s.playing and s.account == account then
			send(s)
		end
	end
end

-- A player's own things taken away for the game (its .plr is never written), as a character's
-- set-up does: nothing of its own while it chooses.
local function strip(unit: number)
	if not nox.present(unit) then
		return
	end
	for _, it in nox.inventory(unit) do
		items.forget(it.id)
		nox.delete(it.id)
	end
	nox.setGold(unit, 0, true)
end

-- A character chosen: held for this game, into play. Whether it was, else why.
local function choose(s: any, key: string?, rec: any, fresh: boolean, older: boolean?): (boolean, string?)
	if s.playing then
		return false, "playing already"
	end
	if key then
		if playedHere(key, s.peer) then
			return false, "another player here plays that character"
		end
		if not store.hold(key) then
			return false, "that character is being played in another game"
		end
	end
	local p = nox.player(s.peer)
	if not p or not start then
		if key then
			store.release(key)
		end
		return false, "not in the game"
	end
	s.playing = true
	s.key = key
	s.open = false
	send(s)
	local go: any = start
	go(p, { key = key, record = rec, fresh = fresh, older = older, ready = s.mapReady == true })
	others(s.account, s.peer)
	return true, nil
end

-- Row `i` of the list the player was last sent, played.
function roster.play(s: any, i: number): (boolean, string?)
	local key = s.list and s.list[i]
	if not key then
		return false, "no such character"
	end
	local rec, why, older = store.load(key)
	if not rec then
		return false, "that character can't be read (" .. tostring(why) .. ")"
	end
	if rec.identity.class ~= s.class then
		return false, "join as a " .. tostring(rec.identity.class) .. " to play that character"
	end
	return choose(s, key, rec, false, older)
end

-- A new character of the player's class, named as the player is.
function roster.new(s: any): (boolean, string?)
	local key = store.serverKey(s.account, s.class, s.name)
	if store.load(key) ~= nil then
		return false, "you have that character already: play it"
	end
	if #(nox.store :: any).list("srv/" .. s.account .. "/") >= numbers.roster.most then
		return false, "an account keeps " .. numbers.roster.most .. " characters at most"
	end
	local rec = store.new({ name = s.name, class = s.class, account = s.account })
	rec.identity.account = s.account
	return choose(s, key, rec, true, false)
end

-- The player's own character without the UI: the account's of its class and name, else new.
local function fallback(s: any)
	if s.playing or not seats[s.peer] then
		return
	end
	local key = store.serverKey(s.account, s.class, s.name)
	local rec, _, older = store.load(key)
	local ok, why
	if rec then
		ok, why = choose(s, key, rec, false, older)
	else
		ok, why = roster.new(s)
	end
	if not ok then
		local p = nox.player(s.peer)
		if p then
			nox.tell(p.unit, "Deep Delve: " .. tostring(why))
		end
	end
end

-- A link code for the player's account, to its UI alone (and its own text line without a UI).
function roster.code(s: any): (string?, string?)
	local code, why = accounts.makeCode(s.account, s.pc)
	if not code then
		return nil, why
	end
	hud.send(s.peer, "linkCode", { kind = "linkCode", code = code, minutes = numbers.links.minutes })
	if not nox.hasUi(s.peer) then
		local p = nox.player(s.peer)
		if p then
			nox.tell(p.unit, string.format("Deep Delve: your link code is %s (%d minutes, once). Say \"link %s\" on your other PC.", code,
				numbers.links.minutes, code))
		end
	end
	return code, nil
end

-- A code typed on this PC (at character select only): the PC into the code's account.
function roster.enter(s: any, text: string): (boolean, string?)
	if s.playing then
		return false, "enter a link code at character select"
	end
	local ok, result = accounts.enter(s.pc, text)
	if not ok then
		note(s.peer, result)
		return false, result
	end
	s.account = result
	note(s.peer, "This PC is linked: its account's characters are yours to play.", true)
	send(s)
	-- (the account's players here see the new PC in their lists, and their code used up)
	for _, x in seats do
		if x ~= s and x.pc and x.account == result then
			send(x)
			hud.send(x.peer, "linkCode", { kind = "linkCode", code = "", minutes = 0 })
		end
	end
	return true, nil
end

-- PC `i` of the account unlinked.
function roster.unlink(s: any, i: number): (boolean, string?)
	local ok, why = accounts.unlink(s.account, s.pc, i)
	if ok then
		note(s.peer, "That PC is unlinked.", true)
	else
		note(s.peer, tostring(why))
	end
	send(s)
	return ok, why
end

-- ---- The hooks (main.luau's)

-- A player joined: held at character select (true), or not ours (false: a game without server
-- characters).
function roster.joined(p: any): boolean
	if not roster.active() then
		return false
	end
	local pc, proven = identity.of(p)
	if not pc then
		-- (no identity at all: played, never kept)
		nox.tell(p.unit, "Deep Delve: your client isn't known to this server; you play, but nothing is kept.")
		seats[p.peer] = { peer = p.peer, playing = true }
		if start then
			local go: any = start
			go(p, { key = nil, record = store.new(p), fresh = true })
		end
		return true
	end
	local s = {
		peer = p.peer,
		name = tostring(p.name),
		class = tostring(p.class or "warrior"),
		pc = pc,
		proven = proven,
		account = accounts.of(pc),
		list = {},
		playing = false,
		open = true,
		last = -100,
	}
	seats[p.peer] = s
	strip(p.unit)
	send(s)
	return true
end

-- A player's client has the map: character select shown again (a client without the UI: its own
-- character, a moment on).
function roster.ready(p: any)
	local s = seats[p.peer]
	if not s or s.playing or not s.pc then
		return
	end
	s.mapReady = true
	strip(p.unit)
	send(s)
	nox.after(FALLBACK, function()
		if seats[p.peer] == s and not s.playing and not nox.hasUi(p.peer) then
			fallback(s)
		end
	end)
end

function roster.left(peer: number)
	local s = seats[peer]
	seats[peer] = nil
	if s and s.playing then
		others(s.account, peer)
	end
end

-- A new map: nobody's client has it yet (playerReady again).
function roster.mapLoaded()
	for _, s in seats do
		s.mapReady = false
	end
end

-- A server's game closed to newcomers once numbers.roster.lockAt players are in, open again below
-- it (nox.lock reaches the pool, which opens another game for the next to come). Whether it's
-- closed now.
local closed = false
function roster.checkLock(): boolean
	local at = numbers.roster.lockAt
	if not roster.active() or not at then
		return closed
	end
	local n = #nox.players()
	if n >= at and not closed then
		closed = true
		nox.lock()
		nox.log("deep_delve: " .. n .. " players in: the game closed to newcomers")
	elseif n < at and closed then
		closed = false
		nox.unlock()
		nox.log("deep_delve: " .. n .. " players in: the game open again")
	end
	return closed
end

-- Whether a player is at character select.
function roster.choosing(peer: number): boolean
	local s = seats[peer]
	return s ~= nil and not s.playing
end

-- A value from the player's UI ({ kind = "chars", op, i, code }): whether it was ours.
function roster.message(p: any, value: any): boolean
	if value.kind ~= "chars" then
		return false
	end
	local s = seats[p.peer]
	if not s or not s.pc then
		return true
	end
	-- (the same action again within a few frames: once)
	local op = value.op
	local now = nox.frame()
	if op == s.lastOp and now - s.last < 5 then
		return true
	end
	s.last, s.lastOp = now, op
	local ok, why = true, nil
	if op == "play" then
		ok, why = roster.play(s, tonumber(value.i) or 0)
	elseif op == "new" then
		ok, why = roster.new(s)
	elseif op == "code" then
		local code, err = roster.code(s)
		ok, why = code ~= nil, err
	elseif op == "enter" and type(value.code) == "string" then
		roster.enter(s, string.sub(value.code, 1, 32))
		return true
	elseif op == "unlink" then
		roster.unlink(s, tonumber(value.i) or 0)
		return true
	elseif op == "open" then
		s.open = true
		send(s)
	elseif op == "close" then
		-- (closed on its UI: playing only; at character select it stays)
		if s.playing then
			s.open = false
		end
	end
	if not ok then
		note(p.peer, tostring(why))
	end
	return true
end

-- A chat line: "link" makes a code (told to the player alone), "link CODE" enters one. Taken
-- (true) so nobody else ever sees it.
function roster.chat(p: any, text: string): boolean
	local s = seats[p.peer]
	if not s or not s.pc then
		return false
	end
	local word, rest = string.match(text, "^%s*(%S+)%s*(.-)%s*$")
	if not word or string.lower(word) ~= "link" then
		return false
	end
	if rest == "" then
		local code, why = roster.code(s)
		if code then
			nox.tell(p.unit, string.format("Deep Delve: your link code is %s (%d minutes, once).", code, numbers.links.minutes))
		else
			nox.tell(p.unit, "Deep Delve: " .. tostring(why))
		end
	else
		local ok, why = roster.enter(s, tostring(rest))
		nox.tell(p.unit, if ok then "Deep Delve: this PC is linked." else "Deep Delve: " .. tostring(why))
	end
	return true
end

-- ---- The console ("dd chars ...")

local function seatNamed(name: string?): any
	for _, s in seats do
		if s.name == name then
			return s
		end
	end
	return nil
end

-- "dd chars": every seat; "dd chars clock SECONDS"; "dd chars lockat N|none"; "dd chars NAME
-- [list | play I | new | code | enter CODE | unlink I | pcs]".
function roster.console(args: { string }): { string }
	local what = args[2]
	if what == nil then
		local out = { "server characters " .. (if roster.active() then "on" else "off") .. (if closed then ", closed" else "") }
		for peer, s in seats do
			table.insert(out, string.format("peer %d: %s (%s) %s, %s, account %s", peer, tostring(s.name), tostring(s.class),
				if s.playing then "playing" else "choosing", if s.proven then "proven" else "not proven", tostring(s.account)))
		end
		return out
	end
	if what == "lockat" then
		-- (the game's lock policy's number for this game, for the tests; "none": never closed)
		local n = tonumber(args[3])
		local r: any = numbers.roster
		r.lockAt = n
		return { if roster.checkLock() then "closed" else "open" }
	end
	if what == "clock" then
		accounts.moveClock(tonumber(args[3]) or 0)
		return { "ok" }
	end
	local s = seatNamed(what)
	if not s or not s.pc then
		return { "no player " .. tostring(what) .. " with a seat" }
	end
	local op = args[3] or "list"
	local function done(ok: boolean, why: string?): { string }
		return { if ok then "ok" else "not: " .. tostring(why) }
	end
	if op == "list" then
		local out = {}
		for i, r in rowsOf(s) do
			s.list[i] = r.key
			table.insert(out, string.format("%s %s %s level %s act %s%s%s", tostring(i), tostring(r.n), tostring(r.c), tostring(r.l), tostring(r.a),
				if r.p then "" else " (other class)", if r.busy then " (played here)" else ""))
		end
		return if #out > 0 then out else { "none" }
	elseif op == "play" then
		return done(roster.play(s, tonumber(args[4]) or 0))
	elseif op == "new" then
		return done(roster.new(s))
	elseif op == "code" then
		local code, why = roster.code(s)
		return if code then { code } else done(false, why)
	elseif op == "enter" then
		return done(roster.enter(s, args[4] or ""))
	elseif op == "unlink" then
		return done(roster.unlink(s, tonumber(args[4]) or 0))
	elseif op == "pcs" then
		local out = {}
		for i, e in accounts.pcs(s.account, s.pc) do
			table.insert(out, string.format("%s %s%s", tostring(i), tostring(e.label), if e.me then " (this PC)" else ""))
		end
		return out
	end
	return { "not: dd chars NAME [list | play I | new | code | enter CODE | unlink I | pcs]" }
end

return roster
