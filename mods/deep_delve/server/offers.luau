--!strict
-- Offers (nox.choice) and their answers: the open portals, when the town's portal pentagram has
-- more than one to choose from (the waypoints have a window of their own). A player has one offer
-- open at a time: a newer one replaces the older, whose answer then means nothing.

local offers = {}

-- peer -> { fn(index), frame }
local open: { [number]: any } = {}

-- Offers `answers` to a player; its pick calls fn(index).
function offers.make(peer: number, title: string, answers: { string }, fn: (number) -> ())
	open[peer] = { fn = fn, frame = nox.frame() }
	nox.choice(peer, title, answers)
end

-- The frame the player's open offer was made (nil: none open).
function offers.since(peer: number): number?
	local o = open[peer]
	return o and o.frame
end

-- onChoice: the player's pick handed to its offer's fn; whether one was open.
function offers.chosen(peer: number, index: number): boolean
	local o = open[peer]
	if not o then
		return false
	end
	open[peer] = nil
	o.fn(index)
	return true
end

-- A player gone, or every offer (a new map: what they offered is gone).
function offers.forget(peer: number?)
	if peer then
		open[peer] = nil
	else
		table.clear(open)
	end
end

return offers
