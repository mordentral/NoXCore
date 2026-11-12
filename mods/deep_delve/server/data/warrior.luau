--!strict
-- The warrior's passive skills (by decision 2026-11-02): what each tree node of a skill does, in
-- numbers (a first pass, for the balance pass). The nodes themselves are data/tree.luau's (each
-- names its skill, `skill`); the skills at work are warrior.luau's.
--
-- A skill is a warrior's alone (tree.usable): any other class taking its node gets the node's
-- stats' worth instead (data/tree.luau's crossWorth). Taking more nodes of one skill raises its
-- rank (a weapon mastery's second node: rank 2); a skill's numbers are its `ranks[rank]`, or the
-- skill's own when it has one rank.
--
-- Numbers: `pct` and `chance` in hundredths, `seconds` where a time is meant (frames worked out
-- from it), `frames` where a few frames are meant, `reach` in world units, `arc` in the game's
-- 256 steps of a turn (each side of the warrior's facing).
--
-- A blow: the warrior's own swing landing (onDamage's cause "blow", its unit the source). What a
-- skill deals itself (a splash, a bleed, a shockwave, the spin) is dealt from the warrior a frame
-- after the blow, as a share of what the blow dealt (all the warrior's bonuses in it), and is no
-- blow: no skill or worn power rolls on it again.

local w = {}

w.skills = {
	-- A blow also hits up to `most` other monsters beside its target, within `reach` of the warrior
	-- and `arc` of its facing, for `pct` of what the blow dealt.
	cleave = { pct = 50, reach = 90, arc = 40, most = 3 },
	-- Each blow in a row on the same monster `step` more damage, up to `most` (the first blow none);
	-- another target, or `seconds` without a blow, starts the row again.
	momentum = { step = 5, most = 25, seconds = 4 },
	-- A blow on a monster below `below` of its most health deals `times` as much.
	execute = { below = 20, times = 2 },
	-- Falling below `below` of most health heals `pct` of it over `seconds`, once in `every` seconds.
	secondWind = { below = 30, pct = 30, seconds = 5, every = 60 },
	-- A blow makes its target bleed `pct` of what it dealt over `seconds` (the mod's own bleed: the
	-- warrior's damage a beat at a time, never the game's poison).
	rend = { pct = 40, seconds = 4 },
	-- With a shield worn: a monster's blow taken has `chance` to stagger the monster (the game's
	-- hold for `frames`; a boss's `bossFrames`).
	shieldWall = { chance = 20, frames = 24, bossFrames = 8 },
	-- With a shield worn: `withShield`'s stats, and `chance` more to stagger (Shield Wall's).
	phalanx = { chance = 10, withShield = { resist = 8 } },
	-- The warrior's rage (0-`most`): `dealt` a blow landed, `taken` a monster's hit taken; after
	-- `idle` seconds with neither it drains `drain` a second. Full, the next blow is a spin: every
	-- monster within `reach` of the warrior takes `times` what the blow dealt, and the rage empties.
	rage = { most = 100, dealt = 8, taken = 5, idle = 4, drain = 8, reach = 110, times = 3 },
	-- Tread Lightly's upgrade: the first blow out of its sneak (landing within `window` frames of the
	-- sneak's end: the swing ends it) deals `times` as much.
	ambush = { times = 3, window = 40 },
	-- Envenom (by decision 2026-11-12): every hit of the warrior's weapon on one of the mod's monsters
	-- (any weapon: its blow, a thrown chakram; a worn bow's arrow too, should one be worn) raises the
	-- monster's poison a level,
	-- to at most `most` (the game's poison: the game's Poison spell at that level, so a monster
	-- immune to poison or resisting it takes none; its bites as numbers.poison scales them). Rank 2
	-- (Envenom II) a level more.
	envenom = { ranks = { { most = 2 }, { most = 3 } } },
	-- Eye of the Wolf's upgrade: while its sight is on, the party's hits on a monster the warrior
	-- sees (within `sight`, no wall between) deal `pct` more; the secret room's lever and the trapped
	-- chests within `reveal` of the warrior are shown by a ring on the floor while it lasts.
	huntersMark = { pct = 15, sight = 400, reveal = 600 },

	-- The stock abilities' upgrades (by decision 2026-11-02; upgrades.luau), a choice of two on
	-- each ability's last node (data/tree.luau's `excludes`: one of the pair at a time). Their hits
	-- are shares (`pct`) of the warrior's blow, as the actives' (actives.blow x its damage bonus).
	--
	-- Berserker Charge's Trample: a charge stopped by a creature or a wall strikes the creature it
	-- hit and every monster within `reach` of where it stopped for `pct` of the blow (crush), and
	-- knocks them aside off the charge's line (the game's push, `force`; never a boss).
	trample = { pct = 120, reach = 80, force = 45, kind = 2 },
	-- Berserker Charge's Blood Rush: a kill by any blow of the warrior's within `seconds` of a
	-- charge's end readies the charge again (once a charge). (The game's own: a kill by the charge
	-- itself readies it.)
	bloodRush = { seconds = 3 },
	-- War Cry's Rallying Cry: every party member within `reach` of the warrior (the warrior too)
	-- deals `damage`% more and takes `resist`% less for `seconds`, and is healed `heal`% of its
	-- most health.
	rallyingCry = { reach = 300, seconds = 8, damage = 15, resist = 10, heal = 10 },
	-- War Cry's Terror: every monster within `reach` of the warrior flees for `seconds` (the dread
	-- affix's fear, its spared tiers: never a boss).
	terror = { reach = 300, seconds = 3 },
	-- Harpoon's Reel and Rend: the creature it hooks bleeds `pct` of the blow over `seconds` (Rend's
	-- bleed; the stronger runs), and once the pull ends is held `held` frames (a boss `bossHeld`).
	reelAndRend = { pct = 60, seconds = 4, held = 30, bossHeld = 8 },
	-- Harpoon's Chain: as it hooks a monster, the nearest other monster within `reach` of it is
	-- struck for `pct` of the blow (blade, as the harpoon's own) and pulled toward the warrior
	-- (the game's push reversed, `force`; never a boss).
	chain = { pct = 50, reach = 120, force = 45, kind = 0 },
}

-- The weapon masteries: a family each, judged from the item type data (data/tips.luau's `kind`
-- of the worn weapon: `kinds`); a thrown weapon by its missile in flight (`thrown`). Each family's
-- numbers by rank (its nodes taken).
w.mastery = {
	-- Every `every`th blow by the family's weapon sends a shockwave: monsters within `reach` of the
	-- target take `pct` of the blow (the target itself has the blow), held `held` frames at rank 2.
	hammer = {
		title = "Hammer and Mace",
		kinds = { "hammer", "mace" },
		ranks = { { every = 4, pct = 50, reach = 70 }, { every = 4, pct = 75, reach = 85, held = 15 } },
	},
	-- A monster's blow on the warrior has `chance` to be turned fully (no damage).
	sword = { title = "Sword", kinds = { "sword" }, ranks = { { chance = 8 }, { chance = 15 } } },
	-- A blow makes its target bleed `pct` of what it dealt over `seconds` (as Rend's; the stronger
	-- bleed of the two runs).
	axe = { title = "Axe", kinds = { "axe" }, ranks = { { pct = 25, seconds = 3 }, { pct = 40, seconds = 3 } } },
	-- A thrown chakram's hit also strikes the nearest other monster within `reach` of its target
	-- for `pct` of it.
	chakram = {
		title = "Chakram",
		kinds = { "chakram" },
		thrown = { "RoundChakramInMotion", "FanChakramInMotion" },
		ranks = { { pct = 50, reach = 150 }, { pct = 75, reach = 200 } },
	},
	-- A blow throws its target back (the game's push, `force`), held `held` frames at rank 2.
	staff = { title = "Staff", kinds = { "staff" }, ranks = { { force = 45 }, { force = 65, held = 10 } } },
}

-- The warrior's active abilities (by decision 2026-11-02): mod abilities (nox.defineAbility), a
-- button each on a NoX Core client's strip over the quickbar, granted by their tree nodes (a
-- warrior's alone; actives.luau). The node's second rank raises the numbers (`ranks`).
--
-- Their damage is a share (`pct`) of the warrior's blow: its worn melee weapon's at its strength
-- now (the tooltip's "Damage"; bare-handed or with a bow, `bare`), times its damage bonus (the
-- tree's and the worn affixes'). Execute and Hunter's Mark count on each hit; the hits fill the
-- rage (`rage` each landing or turn that hits something) but never spend it.
--
-- `icon`: the button's picture, the mod's own (assets\abilities\<name>.png, 64 x 64); the
-- active's tree nodes name the same picture.
w.actives = {
	-- Leap to the cursor's point (at most `range` away: the point on the line at that reach),
	-- `time` frames in the air, drawn `height` high; on landing every monster within `radius`
	-- takes `pct` of the blow, is thrown back from the landing point (the game's push, `force`;
	-- never a boss, nor one held already) and then held (the game's hold) `held` frames (a boss
	-- `bossHeld`), `holdAfter` frames on.
	leapSlam = {
		title = "Leap Slam",
		icon = "abilities/leap_slam",
		target = "point",
		cooldown = 180,
		range = 250,
		time = 20,
		height = 40,
		kind = 2, -- (the damage type: crush)
		holdAfter = 6, -- (the hold put on once the throw has carried them: the hold stops motion)
		rage = 8,
		-- (the body drawn swinging a hammer over the leap, whatever it holds: nox.playAnim)
		anim = { name = "HAMMER_STRIKE", weapon = "HAMMER" },
		ranks = {
			{ pct = 150, radius = 90, held = 20, bossHeld = 6, force = 40 },
			{ pct = 200, radius = 115, held = 25, bossHeld = 8, force = 50 },
		},
	},
	-- Held: the warrior spins for up to `hold` frames, busy (no blows, spells or abilities) and
	-- slowed; every `every` frames (the first at once) each monster within `reach` takes `pct` of
	-- the blow. Let go (or its hold run out), it stops and its `cooldown` runs from then.
	whirlwind = {
		title = "Whirlwind",
		icon = "abilities/whirlwind",
		target = "hold",
		hold = 75,
		cooldown = 240,
		every = 10,
		turns = 3, -- (the body's turns a second as it spins: drawn only)
		kind = 0, -- (the damage type: blade)
		rage = 3,
		-- (the body held in its own strike's last frame as it spins, its weapon out: nox.playAnim)
		anim = { name = "ATTACK", hold = "last" },
		ranks = { { pct = 35, reach = 100 }, { pct = 50, reach = 115 } },
	},
}
-- The blow without a melee weapon (bare hands, a bow): `least`, or `perStrength` of its strength.
w.bare = { least = 5, perStrength = 0.1 }
-- The sounds the actives make (the game's names): the leap's start, its landing, each turn.
w.sounds = { leap = "BerserkerChargeInvoke", slam = "BerserkerCrash", turn = "SwordMissing" }

-- The order the masteries' nodes are laid in (data/tree.luau's Arms branch).
w.masteryOrder = { "sword", "hammer", "axe", "chakram", "staff" }

-- The beat a hero with a timed skill keeps (frames): the bleeds' ticks, the rage's drain, Second
-- Wind's heal, Ambush's watch on the sneak, Hunter's Mark's sight.
w.beat = 6
-- A bleed's tick (frames).
w.bleedEvery = 15
-- The most bleeding monsters a warrior keeps at once (the oldest let go first).
w.bleedMost = 24

-- The floor ring Hunter's Mark shows at a hidden thing (decals.luau's ring, coloured): the lever's
-- gold, a trapped chest's red.
w.reveal = {
	lever = { asset = "fx/waypoint", size = 56, spin = 30, pulse = 0.4, period = 1.2, colour = { 255, 200, 80, 255 } },
	trapped = { asset = "fx/portal", size = 50, spin = -30, pulse = 0.4, period = 1.2, colour = { 255, 70, 50, 255 } },
}

-- Whirlwind's swirl on the floor while the warrior spins (decals.luau's, following the warrior;
-- removed as the spin stops): `across` times the turn's reach wide (so it shows the reach),
-- turning `spin` degrees a second the same way as the body (nox.setSpin's turns: both clockwise
-- on screen; the engine's most is 720), its size swinging by `pulse` over `period` seconds
-- (`scale`), drawn additive (glow) in its own colours, `colour` a tint.
w.swirl = { asset = "fx/whirl", across = 2, spin = 720, pulse = 0.15, period = 0.33, scale = true, colour = { 255, 255, 255, 255 } }

-- The point effects (nox.effect's MSG_FX_* codes) a skill shows.
w.fx = {
	cleave = 0x8B, -- (MSG_FX_DAMAGE_POOF, at each monster cleaved)
	spin = 0x88, -- (MSG_FX_THIN_EXPLOSION, round the warrior)
	shockwave = 0x8A, -- (MSG_FX_SMOKE_BLAST, at the target)
	parry = 0x96, -- (MSG_FX_RICOCHET, at the warrior)
	chakram = 0x82, -- (MSG_FX_YELLOW_SPARKS, at the second monster)
	stagger = 0x81, -- (MSG_FX_BLUE_SPARKS, at the staggered monster)
	secondWind = 0x9A, -- (MSG_FX_WHITE_FLASH, at the warrior)
	rageFull = 0x84, -- (MSG_FX_VIOLET_SPARKS, at the warrior as its rage fills)
	slam = 0x8A, -- (MSG_FX_SMOKE_BLAST: Leap Slam's dust where it lands)
	slamHit = 0x8B, -- (MSG_FX_DAMAGE_POOF, at each monster the slam strikes)
	whirl = 0x86, -- (MSG_FX_LESSER_EXPLOSION: Whirlwind's orange burst at the warrior, each turn)
	whirlHit = 0x96, -- (MSG_FX_RICOCHET: sparks at each monster a turn of Whirlwind strikes)
	trample = 0x86, -- (MSG_FX_LESSER_EXPLOSION: Trample's impact where the charge stopped)
	trampleHit = 0x8B, -- (MSG_FX_DAMAGE_POOF, at each monster Trample or Chain strikes)
	bloodRush = 0x84, -- (MSG_FX_VIOLET_SPARKS, at the warrior as its charge is ready again)
	rally = 0x82, -- (MSG_FX_YELLOW_SPARKS, at each party member Rallying Cry reaches)
	terror = 0x87, -- (MSG_FX_COUNTERSPELL_EXPLOSION, round the warrior as Terror goes off)
	reel = 0x81, -- (MSG_FX_BLUE_SPARKS, at the creature Reel and Rend holds)
}

-- A skill's numbers at a rank: its ranks' (the highest it has, at most), or its own.
function w.numbers(key: string, rank: number): any
	local m = (w.mastery :: any)[key]
	local def: any = m or (w.skills :: any)[key] or (w.actives :: any)[key]
	if not def then
		return nil
	end
	if def.ranks then
		return def.ranks[math.clamp(rank, 1, #def.ranks)]
	end
	return def
end

-- What a skill node says (its tooltip's words), by its skill and the rank it brings.
function w.words(key: string, rank: number): string
	local s: any = w.skills
	if key == "cleave" then
		local c = s.cleave
		return string.format("Your blows also hit up to %d monsters beside the target, in front of you, for %d%% of the blow", c.most, c.pct)
	elseif key == "momentum" then
		local m = s.momentum
		return string.format("Each blow in a row on the same monster deals %d%% more, up to %d%%; another target starts again", m.step, m.most)
	elseif key == "execute" then
		local e = s.execute
		return string.format("Your blows on monsters below %d%% health deal %d times as much", e.below, e.times)
	elseif key == "secondWind" then
		local sw = s.secondWind
		return string.format("Falling below %d%% health heals %d%% of your most health over %d seconds, once a minute", sw.below, sw.pct, sw.seconds)
	elseif key == "rend" then
		local r = s.rend
		return string.format("Your blows make the target bleed %d%% of the blow over %d seconds", r.pct, r.seconds)
	elseif key == "shieldWall" then
		local sh = s.shieldWall
		return string.format("With a shield worn: a monster's blow on you has %d%% to stagger it", sh.chance)
	elseif key == "phalanx" then
		local p = s.phalanx
		return string.format("With a shield worn: +%d%% resistance, and %d%% more to stagger with Shield Wall", p.withShield.resist, p.chance)
	elseif key == "rage" then
		return "A Rage bar, filled by blows landed and taken, draining out of combat; full, your next blow spins: everything around you takes "
			.. s.rage.times
			.. " times the blow"
	elseif key == "ambush" then
		return string.format("Tread Lightly's upgrade: your first blow out of its sneak deals %d times as much", s.ambush.times)
	elseif key == "envenom" then
		local n = w.numbers(key, rank)
		if rank >= 2 then
			return string.format("Envenom's poison up to level %d", n.most)
		end
		return string.format(
			"Every hit of your weapon (a blow, a thrown chakram) poisons the monster a level more, up to level %d; the poison bites harder in deeper areas (never one poison can't hurt)",
			n.most)
	elseif key == "huntersMark" then
		return string.format(
			"Eye of the Wolf's upgrade: while it lasts, the party deals %d%% more to monsters you see, and hidden levers and trapped chests near you show",
			s.huntersMark.pct
		)
	elseif key == "trample" then
		local t = s.trample
		return string.format(
			"Berserker Charge's upgrade: where your charge stops against a creature or a wall, it and the monsters within %d take %d%% of your blow and are knocked aside",
			t.reach, t.pct)
	elseif key == "bloodRush" then
		return string.format("Berserker Charge's upgrade: a kill within %d seconds of your charge's end readies the charge again", s.bloodRush.seconds)
	elseif key == "rallyingCry" then
		local r = s.rallyingCry
		return string.format("War Cry's upgrade: you and the party near you deal %d%% more and take %d%% less for %d seconds, and heal %d%% of your health", r.damage,
			r.resist, r.seconds, r.heal)
	elseif key == "terror" then
		return string.format("War Cry's upgrade: the monsters near you flee for %d seconds (never a boss)", s.terror.seconds)
	elseif key == "reelAndRend" then
		local r = s.reelAndRend
		return string.format("Harpoon's upgrade: the creature it hooks bleeds %d%% of your blow over %d seconds and is stunned a moment once the pull ends", r.pct,
			r.seconds)
	elseif key == "chain" then
		local c = s.chain
		return string.format("Harpoon's upgrade: as it hooks a monster, the nearest other within %d takes %d%% of your blow and is pulled toward you", c.reach, c.pct)
	end
	local a: any = (w.actives :: any)[key]
	if a then
		local n = w.numbers(key, rank)
		if key == "leapSlam" then
			if rank >= 2 then
				return string.format("Leap Slam's landing deals %d%% of your blow within %d", n.pct, n.radius)
			end
			return string.format(
				"An ability: leap to a point up to %d away; the landing deals %d%% of your blow to the monsters within %d, stuns them a moment and throws them back (every %d seconds)",
				a.range, n.pct, n.radius, a.cooldown // 30)
		elseif key == "whirlwind" then
			if rank >= 2 then
				return string.format("Whirlwind's turns deal %d%% of your blow within %d", n.pct, n.reach)
			end
			return string.format(
				"An ability, held: spin up to %.1f seconds, slowed, striking every monster within %d for %d%% of your blow %d times a second (then %d seconds to wait)",
				a.hold / 30, n.reach, n.pct, 30 // a.every, a.cooldown // 30)
		end
	end
	local m: any = (w.mastery :: any)[key]
	if not m then
		return key
	end
	local n = w.numbers(key, rank)
	local with = " with a " .. string.lower(m.title)
	if key == "hammer" then
		return string.format("Every %dth blow%s sends a shockwave: monsters near the target take %d%% of it%s", n.every, " with a hammer or mace", n.pct,
			if n.held then ", stunned a moment" else "")
	elseif key == "sword" then
		return string.format("%d%% to turn a monster's blow on you fully, a sword in hand", n.chance)
	elseif key == "axe" then
		return string.format("Your blows%s make the target bleed %d%% of the blow over %d seconds", with, n.pct, n.seconds)
	elseif key == "chakram" then
		return string.format("Your thrown chakram's hits also strike the next monster near the target for %d%%", n.pct)
	elseif key == "staff" then
		return string.format("Your blows%s throw the target back%s", with, if n.held then ", stunned a moment" else "")
	end
	return key
end

return w
