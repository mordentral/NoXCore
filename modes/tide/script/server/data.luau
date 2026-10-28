-- Undead Tide's data: its tables (the auto-weapons, the passives, the pickups, the difficulties,
-- each biome's creatures, the lobby's signs). Built into the program; the director reads them
-- once at set-up (tide_data.cpp) while its rules are still its own C++.
--
-- Plain values only, no sums: a number written out here is read as the same C++ literal would be
-- (a float one rounded to nearest), where a sum the compiler folds might round its own way.
-- The game's own things by the names its files give them: creatures and items (thing.bin),
-- spells (SPELL_...), enchantments (ENCHANT_...). Classes: "warrior", "wizard", "conjurer".

local data = {}

-- The auto-weapons, in the order of their numbers (TideWeapon, the messages' kind byte):
--   name, text   its card's words;
--   classes      whose pick it is (none: no class's);
--   spell        the stock spell it casts (none: the director's own doing);
--   aim          how it finds what it casts at: "custom" (the director's own), "atFoe" (the
--                nearest foe; Multishot more of them), "onFoe" (the nearest foes without its
--                enchantment yet), "atSelf" (about the player, a foe within its reach), "onSelf"
--                (on the player, while it's without the enchantment and a foe is near),
--                "onStrongest" (the foe of most health within reach, not the night's guardian);
--   reach        how near a foe must be;
--   enchant      the enchantment it gives (none: none);
--   wait         frames between shots, less waitPerLevel a level past the first;
--   out          the type of what it leaves that's one at a time (a type's end matched; none: no
--                such limit);
--   icon         a spell's, an item's, or a warrior ability's (1-5).
data.weapons = {
    { name = "Sword", text = "A blow all round you; harder and wider each level.",
      classes = { "warrior" }, aim = "custom", reach = 200, wait = 24, waitPerLevel = 0,
      icon = { item = "Longsword" } },
    { name = "Magic Missile", text = "Missiles that seek the nearest foe; more in the air each level.",
      classes = { "wizard" }, spell = "SPELL_MAGIC_MISSILE", aim = "custom", reach = 380, wait = 36, waitPerLevel = 0,
      icon = { spell = "SPELL_MAGIC_MISSILE" } },
    { name = "Arrows", text = "A fan of arrows at the nearest foe; one more each level.",
      classes = { "warrior", "conjurer" }, aim = "custom", reach = 380, wait = 27, waitPerLevel = 0,
      icon = { item = "Bow" } },
    -- (Any class's pick: orbs circling the player for their life, 4 s, with the stock ball's harm.)
    { name = "Force of Nature", text = "Orbs of force circle you; more and faster each level.",
      classes = { "warrior", "wizard", "conjurer" }, aim = "custom", reach = 0, wait = 150, waitPerLevel = 15,
      icon = { spell = "SPELL_FORCE_OF_NATURE" } },
    { name = "Fireball", text = "A fireball at the nearest foe; stronger, and sooner, each level.",
      classes = { "wizard" }, spell = "SPELL_FIREBALL", aim = "atFoe", reach = 380, wait = 54, waitPerLevel = 5,
      icon = { spell = "SPELL_FIREBALL" } },
    { name = "Death Ray", text = "A ray on the nearest foe; stronger each level.",
      classes = { "wizard" }, spell = "SPELL_DEATH_RAY", aim = "atFoe", reach = 380, wait = 75, waitPerLevel = 7,
      icon = { spell = "SPELL_DEATH_RAY" } },
    { name = "Pixie Swarm", text = "Pixies that hunt the tide; more each level.",
      classes = { "conjurer" }, spell = "SPELL_PIXIE_SWARM", aim = "atSelf", reach = 380, wait = 120, waitPerLevel = 10,
      out = "Pixie", icon = { spell = "SPELL_PIXIE_SWARM" } },
    { name = "Meteor", text = "A meteor falls on the nearest foe.",
      classes = { "conjurer" }, spell = "SPELL_METEOR", aim = "atFoe", reach = 380, wait = 90, waitPerLevel = 8,
      out = "Meteor", icon = { spell = "SPELL_METEOR" } },
    { name = "Fist of Vengeance", text = "A fist of vengeance crushes the nearest foe.",
      classes = { "warrior", "conjurer" }, spell = "SPELL_FIST", aim = "atFoe", reach = 380, wait = 105, waitPerLevel = 9,
      out = "Fist", icon = { spell = "SPELL_FIST" } },
    { name = "Earthquake", text = "The ground shakes about you.",
      classes = { "warrior", "wizard" }, spell = "SPELL_EARTHQUAKE", aim = "atSelf", reach = -1, wait = 120, waitPerLevel = 10,
      icon = { spell = "SPELL_EARTHQUAKE" } },
    { name = "Toxic Cloud", text = "A poison cloud on the nearest foe.",
      classes = { "conjurer" }, spell = "SPELL_TOXIC_CLOUD", aim = "atFoe", reach = 380, wait = 150, waitPerLevel = 12,
      out = "ToxicCloud", icon = { spell = "SPELL_TOXIC_CLOUD" } },
    { name = "Force Field", text = "A force field round you, renewed while foes are near.",
      classes = { "wizard", "conjurer" }, spell = "SPELL_SHIELD", aim = "onSelf", reach = 380, enchant = "ENCHANT_SHIELD",
      wait = 600, waitPerLevel = 75, icon = { spell = "SPELL_SHIELD" } },
    -- (The second set.)
    { name = "Burn", text = "A flame bursts where the nearest foe stands; sooner each level.",
      classes = { "wizard" }, spell = "SPELL_BURN", aim = "atFoe", reach = 300, wait = 60, waitPerLevel = 5,
      icon = { spell = "SPELL_BURN" } },
    { name = "Ring of Fire", text = "A ring of flames springs up about you.",
      classes = { "warrior", "wizard" }, spell = "SPELL_CLEANSING_FLAME", aim = "atSelf", reach = 160, wait = 180, waitPerLevel = 15,
      out = "FlameCleanse", icon = { spell = "SPELL_CLEANSING_FLAME" } },
    { name = "Confuse", text = "Foes near you stagger about, lost.",
      classes = { "wizard", "conjurer" }, spell = "SPELL_CONFUSE", aim = "onFoe", reach = 300, enchant = "ENCHANT_CONFUSED",
      wait = 120, waitPerLevel = 10, icon = { spell = "SPELL_CONFUSE" } },
    { name = "Fear", text = "Foes near you flee in terror.",
      classes = { "warrior", "wizard", "conjurer" }, spell = "SPELL_FEAR", aim = "onFoe", reach = 250, enchant = "ENCHANT_AFRAID",
      wait = 150, waitPerLevel = 12, icon = { spell = "SPELL_FEAR" } },
    { name = "Freeze", text = "Foes near you are frozen where they stand.",
      classes = { "wizard", "conjurer" }, spell = "SPELL_FREEZE", aim = "onFoe", reach = 300, enchant = "ENCHANT_FREEZE",
      wait = 120, waitPerLevel = 10, icon = { spell = "SPELL_FREEZE" } },
    { name = "Slow", text = "Foes near you crawl.",
      classes = { "wizard", "conjurer" }, spell = "SPELL_SLOW", aim = "onFoe", reach = 300, enchant = "ENCHANT_SLOWED",
      wait = 90, waitPerLevel = 8, icon = { spell = "SPELL_SLOW" } },
    { name = "Stun", text = "Foes near you are held fast (the heaviest only slowed).",
      classes = { "warrior", "conjurer" }, spell = "SPELL_STUN", aim = "onFoe", reach = 250, enchant = "ENCHANT_HELD",
      wait = 120, waitPerLevel = 10, icon = { spell = "SPELL_STUN" } },
    { name = "Poison", text = "Poisons the nearest foes; more of them as it levels.",
      classes = { "conjurer" }, spell = "SPELL_POISON", aim = "atFoe", reach = 300, wait = 90, waitPerLevel = 8,
      icon = { spell = "SPELL_POISON" } },
    { name = "Shockwave", text = "The tide about you is thrown back and hurt.",
      classes = { "warrior", "wizard" }, aim = "custom", reach = 160, wait = 150, waitPerLevel = 15,
      icon = { spell = "SPELL_PUSH" } },
    { name = "Gather", text = "The tide about you is drawn in close.",
      classes = { "warrior", "wizard" }, aim = "custom", reach = 320, wait = 180, waitPerLevel = 15,
      icon = { spell = "SPELL_PULL" } },
    { name = "Shock", text = "Lightning about you: who strikes you is shocked.",
      classes = { "warrior", "wizard" }, spell = "SPELL_SHOCK", aim = "onSelf", reach = 200, enchant = "ENCHANT_SHOCK",
      wait = 300, waitPerLevel = 30, icon = { spell = "SPELL_SHOCK" } },
    { name = "Vampirism", text = "Your blows heal you.",
      classes = { "warrior", "conjurer" }, spell = "SPELL_VAMPIRISM", aim = "onSelf", reach = 200, enchant = "ENCHANT_VAMPIRISM",
      wait = 300, waitPerLevel = 30, icon = { spell = "SPELL_VAMPIRISM" } },
    { name = "Meteor Shower", text = "Meteors rain on the nearest foe.",
      classes = { "wizard", "conjurer" }, spell = "SPELL_METEOR_SHOWER", aim = "atFoe", reach = 380, wait = 300, waitPerLevel = 30,
      out = "MeteorShower", icon = { spell = "SPELL_METEOR_SHOWER" } },
    { name = "Inversion", text = "Foes' missiles turn back on them.",
      classes = { "wizard" }, spell = "SPELL_INVERSION", aim = "atSelf", reach = 300, wait = 120, waitPerLevel = 10,
      icon = { spell = "SPELL_INVERSION" } },
    { name = "Blind", text = "Foes near you are struck blind.",
      classes = { "warrior", "conjurer" }, spell = "SPELL_BLIND", aim = "onFoe", reach = 300, enchant = "ENCHANT_BLINDED",
      wait = 120, waitPerLevel = 10, icon = { spell = "SPELL_BLIND" } },
    { name = "Turn Undead", text = "The undead about you are burned by holy light.",
      classes = { "warrior", "conjurer" }, spell = "SPELL_TURN_UNDEAD", aim = "atSelf", reach = 200, wait = 180, waitPerLevel = 15,
      icon = { spell = "SPELL_TURN_UNDEAD" } },
    { name = "Doom", text = "The strongest foe near you dies in 15 s (not the guardian).",
      classes = { "wizard", "conjurer" }, spell = "SPELL_DEATH", aim = "onStrongest", reach = 380, enchant = "ENCHANT_DEATH",
      wait = 450, waitPerLevel = 45, icon = { spell = "SPELL_DEATH" } },
    -- (No class's pick: its flames, the stock firewalk tick's, are no one's and burn the party and
    -- the player itself crossing its trail.)
    { name = "Firewalk", text = "Flames spring up in your wake.",
      classes = {}, spell = "SPELL_FIREWALK", aim = "atSelf", reach = 250, wait = 300, waitPerLevel = 30,
      icon = { spell = "SPELL_FIREWALK" } },
    { name = "Cleave", text = "A great sweep that throws the struck back; wider each level.",
      classes = { "warrior" }, aim = "custom", reach = 200, wait = 40, waitPerLevel = 3,
      icon = { item = "GreatSword" } },
    { name = "Crossbow", text = "Heavy bolts at the nearest foes; one more each level.",
      classes = { "warrior", "conjurer" }, aim = "custom", reach = 380, wait = 45, waitPerLevel = 4,
      icon = { item = "CrossBow" } },
    { name = "War Cry", text = "Your cry stuns the foes about you.",
      classes = { "warrior" }, aim = "custom", reach = 150, wait = 240, waitPerLevel = 20,
      icon = { ability = 2 } },
    { name = "Harpoon", text = "The nearest foe is hauled in and hurt.",
      classes = { "warrior" }, aim = "custom", reach = 300, wait = 90, waitPerLevel = 8,
      icon = { ability = 3 } },
    { name = "Wolves", text = "Wolves fight beside you; more each level.",
      classes = { "conjurer" }, aim = "custom", reach = 380, wait = 450, waitPerLevel = 40,
      icon = { spell = "SPELL_SUMMON_WOLF" } },
    { name = "Bear", text = "A bear fights beside you (a stronger one from level 3).",
      classes = { "conjurer" }, aim = "custom", reach = 380, wait = 600, waitPerLevel = 50,
      icon = { spell = "SPELL_SUMMON_BEAR" } },
    { name = "Will-o'-wisps", text = "Wisps of lightning fight beside you; more each level.",
      classes = { "conjurer" }, aim = "custom", reach = 380, wait = 450, waitPerLevel = 40,
      icon = { spell = "SPELL_SUMMON_WILLOWISP" } },
}

-- A weapon's top level.
data.weaponMost = 5

-- The passives, in their numbers' order (TidePassive): name, text, top level.
data.passives = {
    { name = "Haste", text = "Your weapons ready 8% sooner.", most = 5 },
    { name = "Vitality", text = "15% more health.", most = 5 },
    { name = "Magnet", text = "Soul gems drawn from half again as far.", most = 5 },
    { name = "Recovery", text = "Health back over time.", most = 5 },
    { name = "Multishot", text = "Another arrow and ball; spells strike more foes.", most = 3 },
    { name = "Wisdom", text = "15% more from soul gems.", most = 5 },
}

-- How hard a run is, in their numbers' order (TideDifficulty; Delving Mode's four, the same
-- numbers): the tide's health and numbers times these, the minute elites begin, the traits they
-- have more.
data.difficulties = {
    { name = "Adventurer", about = "the Tide as it comes",
      health = 1.0, count = 1.0, eliteFrom = 3, moreTraits = 0 },
    { name = "Veteran", about = "a quarter more creatures, a quarter tougher, elites sooner",
      health = 1.25, count = 1.25, eliteFrom = 2, moreTraits = 0 },
    { name = "Nightmare", about = "half again as many creatures, tougher still, elites of more traits",
      health = 1.6, count = 1.5, eliteFrom = 2, moreTraits = 1 },
    { name = "Oblivion", about = "the night at its worst: hordes, twice as hard, elites from the first minute",
      health = 2.2, count = 1.75, eliteFrom = 1, moreTraits = 1 },
}

-- The tide's creatures (2026-10-11: skeletons and zombies in crypts, nature's creatures
-- outdoors...): the low ones most of the tide, the middling joining it as it goes on, the hard
-- ones the waves'. Every one a stock creature of monster.bin. (Since 2026-10-12: the same low
-- ones, more of the harder sprinkled in, and specials: creatures of their own ways, the game's
-- AI: spellcasting wizards and shamans, beholders, wisps' lightning, flying cherubs, mimics. Every
-- one in monster.bin with harm of its own: those it lacks, statues, and the harmless, left out.)
data.creatures = {
    crypt = {
        low = { "Zombie", "Bat" },
        mid = { "Skeleton", "Ghost", "VileZombie", "Swordsman" },
        hard = { "SkeletonLord", "Lich", "Shade", "Horrendous" },
        special = { "Mimic", "Wizard", "WillOWisp" },
    },
    dungeon = {
        low = { "Zombie", "GiantLeech", "SmallSpider", "Bat" },
        mid = { "Skeleton", "Spider", "Ghost", "Swordsman" },
        hard = { "SkeletonLord", "Troll", "OgreBrute", "Shade" },
        special = { "Beholder", "Mimic", "Wizard", "AlbinoSpider" },
    },
    castle = {
        low = { "Zombie", "Bat", "Imp" },
        mid = { "Skeleton", "Archer", "Swordsman", "GruntAxe" },
        hard = { "SkeletonLord", "OgreBrute", "MechanicalGolem", "FlyingGolem" },
        special = { "Wizard", "WizardGreen", "Mimic", "EvilCherub" },
    },
    cave = {
        low = { "Bat", "SmallSpider", "GiantLeech" },
        mid = { "Spider", "Scorpion", "GruntAxe", "SpittingSpider" },
        hard = { "OgreBrute", "StoneGolem", "OgreWarlord", "Horrendous" },
        special = { "Beholder", "AlbinoSpider", "UrchinShaman", "Mimic" },
    },
    wild = {
        low = { "Wolf", "Bat", "Wasp", "SmallSpider" },
        mid = { "BlackWolf", "Bear", "Spider", "Urchin", "Scorpion" },
        hard = { "WhiteWolf", "BlackBear", "Troll", "OgreBrute" },
        special = { "UrchinShaman", "EvilCherub", "WillOWisp", "WizardGreen" },
    },
    swamp = {
        low = { "GiantLeech", "Wasp", "Zombie", "SmallSpider" },
        mid = { "Spider", "SpittingSpider", "VileZombie", "Ghost" },
        hard = { "Troll", "AlbinoSpider", "Horrendous", "Shade" },
        special = { "WillOWisp", "UrchinShaman", "WizardGreen", "Mimic" },
    },
    ice = {
        low = { "Wolf", "Bat" },
        mid = { "WhiteWolf", "Urchin", "Ghost", "BlackWolf" },
        hard = { "StoneGolem", "Troll", "OgreWarlord", "Shade" },
        special = { "WizardWhite", "WillOWisp", "UrchinShaman", "EvilCherub" },
    },
    lava = {
        low = { "Imp", "Bat" },
        mid = { "EmberDemon", "Scorpion", "EvilCherub" },
        hard = { "MeleeDemon", "Demon", "Horrendous" },
        special = { "Wizard", "Beholder", "WillOWisp" },
    },
    sand = {
        low = { "SmallSpider", "Wasp", "Urchin" },
        mid = { "Scorpion", "Spider", "Archer", "GruntAxe" },
        hard = { "OgreBrute", "Troll", "Horrendous", "OgreWarlord" },
        special = { "Mimic", "UrchinShaman", "EvilCherub", "Beholder" },
    },
}
-- Each biome's creatures (mapgen's biome names); any other biome's the dungeon's.
data.biomeCreatures = {
    Crypt = "crypt",
    Castle = "castle",
    Cave = "cave", Mines = "cave",
    Forest = "wild", Grasslands = "wild",
    Swamp = "swamp",
    Ice = "ice",
    Lava = "lava",
    Desert = "sand", Beach = "sand",
}
data.otherBiomeCreatures = "dungeon"

-- The pickups (2026-10-12): what each does (pickups.luau's usePickup), the stock item shown, the
-- line told, how often among the drops (weight), the spell whose sound it makes.
data.pickups = {
    { effect = "heal", type = "RedPotion", says = "A healing potion: much of your health back.", weight = 14, sound = "SPELL_LESSER_HEAL" },
    { effect = "meat", type = "Meat", says = "Meat: a little health back.", weight = 14, sound = "SPELL_LESSER_HEAL" },
    { effect = "cure", type = "CurePoisonPotion", says = "A cure: the poison gone, some health back.", weight = 4, sound = "SPELL_CURE_POISON" },
    { effect = "haste", type = "HastePotion", says = "Haste: you're swift a while.", weight = 6, sound = "SPELL_HASTE" },
    { effect = "shield", type = "ShieldPotion", says = "A force field about you.", weight = 5, sound = "SPELL_SHIELD" },
    { effect = "vampire", type = "VampirismPotion", says = "Vampirism: your blows heal you a while.", weight = 4, sound = "SPELL_VAMPIRISM" },
    { effect = "invisible", type = "InvisibilityPotion", says = "Invisible: the tide loses sight of you a while.", weight = 3, sound = "SPELL_INVISIBILITY" },
    { effect = "invulnerable", type = "InvulnerabilityPotion", says = "Invulnerable, a few seconds!", weight = 2, sound = "SPELL_INVULNERABILITY" },
    { effect = "arcane", type = "BluePotion", says = "An arcane draught: every weapon ready at once.", weight = 4, sound = "SPELL_RESTORE_MANA" },
    { effect = "magnet", type = "AmuletOfClarity", says = "The soul gems of the whole night drawn to you!", weight = 2, sound = "SPELL_PULL" },
    -- (The bomb shown as an amulet: a BlackPowder pile is lit by the player's own blades, ring and
    -- flames and burns away before it's touched.)
    { effect = "bomb", type = "AmuletofCombat", says = "An amulet of war: the tide about you blasted!", weight = 2, sound = "SPELL_METEOR" },
    { effect = "freeze", type = "CrystalBall", says = "A crystal of ice: the tide about you frozen!", weight = 2, sound = "SPELL_FREEZE" },
    { effect = "fear", type = "AmuletofManipulation", says = "An amulet of dread: the tide about you flees!", weight = 2, sound = "SPELL_FEAR" },
    { effect = "ruby", type = "Ruby", says = "A ruby: its light becomes yours (XP).", weight = 8, sound = "SPELL_DETECT_MAGIC" },
    { effect = "emerald", type = "Emerald", says = "An emerald: its light becomes yours (more XP).", weight = 4, sound = "SPELL_DETECT_MAGIC" },
    { effect = "diamond", type = "Diamond", says = "A diamond: its light becomes yours (much XP).", weight = 2, sound = "SPELL_DETECT_MAGIC" },
}

-- The revive shrine: its type (the quest's soul gate's look: its own collide off, no checkpoint)
-- and how near a player in play must come to touch it.
data.shrine = { type = "SoulGate", reach = 45 }

-- The lobby: how near a sign speaks its rule (and how often, frames), how near the records board
-- tells its table, how near the gate begins the run, and how long a NoX Core player asked the
-- difficulty there may stand unanswered before it begins (frames).
data.lobby = { signNear = 60, signEvery = 150, boardNear = 100, gateNear = 40, askWait = 450 }
-- The thing types the director finds on the lobby once it loads (params.luau's lobby makes them):
-- its signs, its portal, its records board.
data.things = { signs = { "Sign1", "Sign2" }, portal = "GauntletExitA", board = "SignShop01" }

-- Soul gems on the ground: each drawn to the nearest player in play within its reach (70, half
-- again a Magnet level) a step a frame (6, its age in frames modulo 4, and 200 over its distance,
-- 20 at least: faster as it nears), taken within 18; gone after two minutes (frames); 150 at most
-- on the ground (the oldest go).
data.gems = { reach = 70, magnet = 0.5, take = 18, base = 6, jitter = 4, pull = 200, near = 20, life = 3600, most = 150 }
-- Pickups lying out: taken by the first player in play within 30; gone after a minute (frames) or
-- once something holds it; 25 at most (the oldest go).
data.pickupField = { touch = 30, life = 1800, most = 25 }
-- How long things stay (frames): a level's glow, a body (a zombie that got up joins the tide
-- again), an opened chest, a finished run's results before the lobby.
-- (And how often the passives and the charms are looked at, and how long an offer stays open.)
data.timers = { glow = 60, body = 90, openChest = 600, ended = 300, passives = 15, charms = 15, offer = 300 }

-- The arena's scenery walked through as a stage begins (trees, rocks, tombs, furniture: nothing to
-- snag on while the tide comes): things of these classes, none of the working ones, whose touch is
-- a plain one (none, these, or a rock's sound); a pad, hole, elevator, chest or trap keeps its own.
data.scenery = {
    classes = "IMMOBILE+OBSTACLE",
    working = "MISSILE+MONSTER+PLAYER+FOOD+EXIT+KEY+DOOR+TRIGGER+WAND+READABLE+WEAPON+ARMOR+TREASURE+FLAG",
    collides = { "", "DefaultCollide", "NoCollide" },
    collidePrefix = "AudioEventCollide",
}
-- Nothing to fight with by hand in a run: what's taken from a player each frame (these classes).
data.disarm = "WEAPON+WAND"-- The elites' chests (a touch opens one): one of these drawn.
data.chests = { "CryptChest1", "CryptChest2", "CryptChest3", "CryptChest4" }

-- The lobby's signs, west to east: one speech line each.
data.rules = {
    "Welcome to the Undead Tide: hold out in an open arena against an endless tide, more and worse every minute.",
    "To begin, step into the glowing gate in the north-west wall. Everyone here comes along; the way closes behind.",
    "How hard: say ?difficulty adventurer, veteran, nightmare or oblivion here before you begin (NoX SDL asks at the gate).",
    "You only walk: your weapons fire by themselves. You start with your class's; more come as you level.",
    "The slain drop soul gems: walk near to draw them in. Each level, say 1, 2 or 3 in chat to pick an upgrade.",
    "No extra lives: a fallen friend comes back at a revive shrine. At dawn the night's guardian comes: slay it and the tide carries you on to a new land, harder each time, till it takes you all.",
    "Playable on the stock client; the arena's map downloads by itself. Old UTD_ map folders are safe to delete.",
}

-- The night's guardian by biome (mapgen's biome names): the biome's worst; any other biome's
-- (the Forest's and the Grasslands') the last.
data.guardians = {
    Crypt = "LichLord",
    Castle = "MechanicalGolem",
    Cave = "OgreWarlord", Mines = "OgreWarlord", Desert = "OgreWarlord", Beach = "OgreWarlord",
    Swamp = "Horrendous", Dungeon = "Horrendous",
    Ice = "StoneGolem",
    Lava = "Demon",
}
data.otherGuardian = "Troll"

-- What the Tide tells players (chat lines, every client's; a stock client has nothing else), and
-- its names. Each {field} is filled in as it's told (fillText): the director's own values, the
-- fields each line has fixed (a line that drops or adds one is refused as the Tide sets up).
-- A line goes as one text message: 250 characters at most, filled in.
data.text = {
    -- The server's listed name, its maps' titles and author.
    listedLobby = "Undead Tide",
    listedRun = "Tide {minutes}m",
    arenaTitle = "Undead Tide",
    lobbyTitle = "Undead Tide Lobby",
    mapAuthor = "Undead Tide",

    -- The lobby: the difficulty ("?difficulty"), the run begun.
    difficultyIs = "The difficulty is {difficulty} ({about}). In the lobby say ?difficulty adventurer, veteran, nightmare or oblivion.",
    difficultyLocked = "The difficulty is chosen in the lobby; this run stays {difficulty}.",
    setsDifficulty = "{player} sets the difficulty: {difficulty} ({about}).",
    coming = "The tide is coming ({difficulty})! The way closes behind you.",

    -- A stage begun: where (a stage past the first, or the first), then the goal; a biome
    -- unknown is the dark.
    risesStage = "Stage {stage}: the tide follows you into {land}, harder still ({difficulty}): ",
    rises = "The Undead Tide rises in {land} ({difficulty}): ",
    darkLand = "the dark",
    goalDawn = "{where}survive till dawn ({dawn}), or slay the night's guardian when it comes ({guardian}).",
    goalGuardian = "{where}hold out till the night's guardian comes ({guardian}), then slay it to go on.",
    firstWeapon = "Your {weapon} fires by itself: walk. Soul gems from the slain level you up; say 1-3 to pick.",

    -- The players: back in a new stage, taken by the tide, the shrine.
    backInFight = "You're back in the fight: press Continue to close the Game Over window.",
    risesAgain = "{player} rises again in the new land.",
    tideTakes = "The tide takes {player}.",
    shrineRises = "A revive shrine rises! Touch it to bring back the fallen.",
    shrineBrings = "{player} is brought back by the shrine!",

    -- The clock, each minute.
    minuteDawn = "{clock}: the tide grows. {left} till dawn.",
    minuteGuardian = "{clock}: the night's guardian still stands. Slay it!",

    -- The tide: the specials (one, or a group), the waves and their elites, the guardian.
    specialOne = "Something stirs: {creature} joins the tide!",
    specialMany = "Something stirs: {creature}s join the tide!",
    wave = "The tide swells: {creature}s come!",
    elite = "An elite {creature} leads them: slay it for its chest!",
    guardianComesDawn = "The night's guardian comes: a {guardian}! Slay it and dawn breaks at once.",
    guardianComes = "The night's guardian comes: a {guardian}! Slay it and the tide carries you on.",
    guardianFallsStage = "The {guardian} falls! The tide breaks for now: in {seconds} s it carries you on to stage {next}, a new land and harder. Take up what lies about!",

    -- Drops: charmed armour, an elite's chest.
    wardingDrop = "A warded {item} lies there: worn, it shields you.",
    repulsionDrop = "A repelling {item} lies there: worn, it throws the tide back.",
    chestOpened = "{player} opens the elite's chest: a free upgrade!",

    -- A level's picks (a stock client's line; its items one after another), and the pick taken.
    offerTitle = "Level {level}! Say 1-{count}:",
    offerItem = "  {number}) {pick}",
    pickHeal = "Heal",
    pickNew = "{name} (new)",
    pickLevel = "{name} {level}",
    pickedForYou = "(The first was picked for you.)",
    youTake = "You take {what}.",
    healTaken = "a heal",

    -- "?tide" / "?me": the player's level, experience and kit.
    me = "Level {level} ({xp}/{need}), {kills} slain: {kit}",
    kitItem = "{name} {level}",
    listSeparator = ", ",

    -- The run's end, then its results: the elites, the stages before, the guardian, a line a runner.
    endGuardianDawn = "The {guardian} falls! Dawn breaks at once: the tide falls back at {clock}.",
    endDawn = "Dawn breaks! The tide falls back: you survived {clock}.",
    endLostStage = "The tide has taken everyone, in stage {stage} at {clock} ({total} in all).",
    endLost = "The tide has taken everyone, at {clock}.",
    results = "{elites} slain; {before}the night's guardian {fate}.",
    eliteOne = "{count} elite",
    eliteMany = "{count} elites",
    beforeOne = "stage {stage} reached, {count} guardian slain before; ",
    beforeMany = "stage {stage} reached, {count} guardians slain before; ",
    fateNeverCame = "never came",
    fateSlain = "slain ({guardian})",
    fateStanding = "still standing ({guardian})",
    runnerLine = "{player}: level {level}, {kills} slain ({kit})",

    -- The records' board: a new place, the table, a run's line (its outcome, a difficulty past
    -- the first, the party's best four, the rest counted, the kills, the biome and the date).
    boardPlace = "A run for the board: number {place} of the tide's best!",
    boardEmpty = "The Tide's best: no runs on record yet. Be the first!",
    boardTitle = "The Tide's best runs:",
    boardLine = "{place}. {run}",
    recordLine = "{outcome}{on}: {members}{more}; {kills} slain{biome}{date}",
    recordGuardian = "Guardian slain at {clock}",
    recordDawn = "Dawn ({clock})",
    recordStage = "Stage {stage}, fell at {clock}",
    recordFell = "Fell at {clock}",
    recordOn = " on {difficulty}",
    recordMember = "{name} ({class} {level})",
    recordMore = " and {count} more",
    recordBiome = ", {biome}",
    recordDate = ", {date}",
}

return data
