--!strict
-- Who a joining player is, for server characters (roster.luau): the PC it plays from.
--
-- The host proves its NoX Core clients (mode.json's "clientKeys"): each one has a key, 32 lowercase
-- hex digits, the same for one PC on this server every time it joins (made from the server's
-- realm and the PC's own secret), another for every other PC (`player.clientKey`,
-- nox.clientKey(peer)). That key is the PC's identity here.
--
-- Without one (a host that keeps no mod data, a client that named no secret), the identity is the
-- account the client named at its join: self-reported, so only as safe as the name. With neither,
-- a player has no identity and plays without a character kept.

local identity = {}

local function isKey(k: any): boolean
	return type(k) == "string" and #k == 32 and string.match(k, "^[0-9a-f]+$") ~= nil
end
identity.isKey = isKey

-- The player's proven key (32 lowercase hex digits), or nil.
function identity.clientKey(player: any): string?
	local k: any = player.clientKey
	if k == nil then
		k = nox.clientKey(player.peer)
	end
	if isKey(k) then
		return k
	end
	return nil
end

-- The player's identity as one store key segment ("k" and its key: proven; "a" and its account:
-- self-reported), and whether it's proven; nil when it has neither.
function identity.of(player: any): (string?, boolean)
	local k = identity.clientKey(player)
	if k then
		return "k" .. k, true
	end
	local s = nox.store
	local account = player.account
	if s and type(account) == "string" and account ~= "" then
		-- (a segment is at most 64; an account name of the game's 15 letters fits whole)
		return string.sub("a" .. s.keyOf(account), 1, 64), false
	end
	return nil, false
end

return identity