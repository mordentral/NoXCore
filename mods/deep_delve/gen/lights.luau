-- Lights written into an area map: a ColorLight thing with its light data (colour, intensity, a
-- flicker), through w:thing. A client builds a light from its own copy of the map, so a light
-- written here shows its colour on every client; one made during play wouldn't (the loot keeps
-- its sparks for that reason).
--
-- Each kind's fields are the map editor's names; the radius is left out (the game works it out
-- from the intensity). A flicker is the intensity's keys stepped through slowly.

local lights = {}

local KINDS = {
	-- A wall torch's warm glow.
	torch = { intensity = 34, color = { 255, 150, 70 } },
	-- A brazier's fire, a little unsteady.
	brazier = { intensity = 42, color = { 255, 125, 45 }, intensityKeys = { 42, 37, 44, 39 }, intensityStep = 6 },
	-- The healer's fountain: cool blue, a gentle flicker.
	fountain = { intensity = 56, color = { 40, 110, 255 }, intensityKeys = { 56, 48, 60, 50 }, intensityStep = 8 },
	-- The town's portal pentagram.
	portal = { intensity = 44, color = { 180, 60, 255 } },
	-- A waypoint's pentagram, in town and out in an area.
	waypoint = { intensity = 44, color = { 40, 220, 255 } },
	-- The act boss's arena: red fire round its floor.
	arena = { intensity = 46, color = { 255, 70, 35 }, intensityKeys = { 46, 40, 48, 42 }, intensityStep = 5 },
}

-- A light of `kind` written at (x, y); its extent.
function lights.put(w, kind: string, x: number, y: number): number
	local k = KINDS[kind]
	if not k then
		gen.refuse("deep_delve: no light kind " .. tostring(kind))
	end
	return w:thing("ColorLight", x, y, { light = k })
end

return lights
