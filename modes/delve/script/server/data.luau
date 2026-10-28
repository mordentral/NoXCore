-- Delving Mode's data: its tables (the creatures by tier, the guardians, the difficulties, the
-- lobby's words, the rooms' and climates' words, the names given, the champion's kit). Built into
-- the program; the director reads them once at set-up (delve_data.cpp) while its rules are still
-- its own C++.
--
-- Plain values only, no sums: a number written out here is read as the same C++ literal would be
-- (a float one rounded to nearest), where a sum the compiler folds might round its own way.
-- The game's own things by the names its files give them: creatures and items (thing.bin),
-- spells (SPELL_...).

local data = {}

-- The stock creatures by how dangerous they are; a level draws mostly from its depth's tier, some
-- from the one below. (A barrel's hiding creature is the first tier's.)
data.tiers = {
    { "Bat", "GiantLeech", "SmallSpider", "Wasp", "Urchin", "Imp" },
    { "Wolf", "Spider", "Zombie", "Skeleton", "Ghost", "Archer", "BlackWolf" },
    { "Scorpion", "SkeletonLord", "Shade", "VileZombie", "Swordsman", "EmberDemon", "Bear", "AlbinoSpider", "SpittingSpider" },
    { "Troll", "OgreBrute", "GruntAxe", "Necromancer", "MeleeDemon", "WhiteWolf", "BlackBear", "Horrendous" },
    { "OgreWarlord", "Demon", "WillOWisp", "Lich", "StoneGolem", "MechanicalGolem" },
}

-- Each act's guardians (the last for every act past them).
data.guardians = {
    { "OgreWarlord", "Horrendous", "Troll" },
    { "Lich", "StoneGolem", "Demon" },
    { "MechanicalGolem", "LichLord", "Demon", "StoneGolem" },
}

-- A run's difficulty, in their numbers' order (DelveDifficulty): monsters' health and count times
-- these; elites from this depth, one more every so many levels, at most so many, of so many
-- traits; bot bosses that play hard.
data.difficulties = {
    { name = "Adventurer", about = "the Delve as it comes",
      health = 1.0, count = 1.0, eliteFrom = 4, eliteEvery = 3, eliteCap = 3, eliteTraits = 1, hardBots = false },
    { name = "Veteran", about = "a quarter more monsters, a quarter tougher, elites sooner",
      health = 1.25, count = 1.25, eliteFrom = 3, eliteEvery = 2, eliteCap = 4, eliteTraits = 1, hardBots = false },
    { name = "Nightmare", about = "half again as many monsters, tougher still, elites of two traits",
      health = 1.6, count = 1.5, eliteFrom = 2, eliteEvery = 2, eliteCap = 5, eliteTraits = 2, hardBots = true },
    { name = "Oblivion", about = "the deep at its worst: hordes, twice as hard, elites everywhere",
      health = 2.2, count = 1.75, eliteFrom = 2, eliteEvery = 1, eliteCap = 6, eliteTraits = 2, hardBots = true },
}

-- The lobby's signs, west to east: one speech line each (a text message's length is a byte).
data.rules = {
    "Welcome to the Delve: an endless dungeon, built anew for every level, deeper and deadlier each time.",
    "To begin, step into the glowing gate in the north-west wall. Everyone here comes along; the way closes behind.",
    "Each act is six levels: fights, elites, shops, mysteries (?), a campfire to strengthen a spell at, and a guardian at its end.",
    "Levels end in two gates, a sign by each telling what lies beyond; the gate most of you take wins. Slay the guardian for its two.",
    "A quest's rules hold: a few lives, more from Ankhs. If everyone leaves, the next delve starts again from level 1.",
    "Each level's map downloads by itself. Old DEL_ folders in your Nox maps folder are leftovers: safe to delete.",
    "Playable on the stock client, but the Delve is best experienced with the NoX Core client at noxhub.net.",
    "How hard: say ?difficulty adventurer, veteran, nightmare or oblivion here before you begin.",
}

-- The rooms, in their numbers' order (DelveRoom): the name players are told ("fight", "?"), the
-- phrase ("a fight"), many of them (one's name and an s, unless given), and what the level's line
-- says of it (a mystery's, once it's known, is its kind's: mysteries below).
data.rooms = {
    { name = "fight", phrase = "a fight", told = "a fight" },
    { name = "elite", phrase = "an elite", told = "an elite: a champion guards its spoils at the end" },
    { name = "?", phrase = "a mystery", many = "mysteries", told = "a mystery" },
    { name = "shop", phrase = "a shop", told = "a shop: trade your gold with the shopkeeper" },
    { name = "campfire", phrase = "a campfire", told = "a campfire: stand by the fire to strengthen one of your spells, once each" },
    { name = "guardian", phrase = "the guardian", told = "the act's guardian. Slay it to open the way on" },
    { name = "blessing", phrase = "a blessing", told = "an altar: go up to it to take one of three blessings for the rest of the delve" },
}

-- What a mystery turns out to be, in their numbers' order (DelveMystery), as the level's line
-- says it.
data.mysteries = {
    "a mystery: a stash someone left behind",
    "a mystery: a purse, forgotten",
    "a mystery: a shrine whose waters heal, once each",
    "a mystery: an obelisk humming with mana",
    "a mystery: a few creatures guard a small find",
    "a mystery: a campfire! Stand by it to strengthen a spell, once each",
    "a mystery: a lone champion, and something rare on it",
}

-- An elite's traits, in their order (swift, vampiric, reflective, tough, regenerating), as told.
data.traits = { "swift", "vampiric", "reflective", "tough", "regenerating" }

-- The blessings, in their numbers' order (the altar's): each one's name and what it does (the
-- altar's lines, and noxsdl's window).
data.blessings = {
    { name = "Vigor", text = "A quarter more health" },
    { name = "Wisdom", text = "A quarter more mana" },
    { name = "Might", text = "A quarter more strength" },
    { name = "Swiftness", text = "Hasted for good" },
    { name = "Thirst", text = "Your blows heal you (vampirism)" },
    { name = "Warding", text = "Warded from fire, shock and poison" },
    { name = "Renewal", text = "Health and mana slowly come back" },
    { name = "Fortune", text = "Gold at every level's start" },
}

-- Everything else the Delve tells players, and its names: templates whose {fields} the director
-- fills in (each line's fields are fixed by the director, delve_data.cpp: a line that drops or
-- adds one is refused as the script is read). A text message's length is a byte: the lines stay
-- short.
data.text = {
    -- The server's listed name, the maps' titles, a shopkeeper's script name (its window's title
    -- is its last part).
    listedLobby = "DELVE lobby",
    listedLevel = "DELVE {depth}",
    lobbyTitle = "Delve Lobby",
    levelTitle = "Delve {depth}",
    shopkeeper = "Delve:{name}",

    -- The lobby.
    clientGreeting = "Welcome to the Delve! It plays on this client, but it's best with the free NoX Core client from noxhub.net.",
    begins = "The delve begins! The way closes behind you.",
    difficultyIs = "The difficulty is {difficulty} ({about}). In the lobby say ?difficulty adventurer, veteran, nightmare or oblivion.",
    difficultyLocked = "The difficulty is chosen in the lobby; this delve stays {difficulty}.",
    setsDifficulty = "{player} sets the difficulty: {difficulty} ({about}).",

    -- A level's line: where, what (rooms' and mysteries' told), the ways on, the air.
    levelLine = "Depth {depth}, act {act}{difficulty} in {place}: {what}.{ways}{air}",
    levelDifficulty = " ({difficulty})",
    levelGates = " Two gates on: the left to {left}, the right to {right}.",
    levelAhead = " Ahead: {next}.",
    levelAir = " {air}",
    -- A biome the map generator doesn't know, by its name.
    placeUnknown = "the {biome}",

    -- An act as a guardian's gate tells it, and the lists in it.
    actPlan = "{first}{ways}, then its guardian",
    actWays = ", then ways through {rooms}",
    actCount = "{count} {rooms}",
    listComma = ", ",
    listAnd = " and ",

    -- The gates: their signs, and who goes through which.
    gateLeft = "LEFT GATE",
    gateRight = "RIGHT GATE",
    gateToAct = "{side}: {place}{climate}. {act}.",
    gateToRoom = "{side}: {room}{then}.",
    thenGuardian = ", then the guardian",
    thenRooms = ", then {left} or {right}",
    takesLeft = "{player} takes the LEFT gate: {where}.",
    takesRight = "{player} takes the RIGHT gate: {where}.",
    guardianFalls = "The guardian falls! Two gates open. Most of you through one decides the way on.",

    -- The run's events.
    partyFallen = "The party has fallen at depth {depth}.",
    wiped = "Back to the lobby with nothing: step into the gate to delve again.",
    newDeepest = "A new deepest delve: depth {depth}, past the record of {best}!",
    shrine = "{player} drinks from the shrine, whole again.",
    elite = "An elite {creature}: {traits}!",
    someone = "Someone",

    -- The campfire.
    fireNothing = "The fire warms you, but you know no spell it could strengthen.",
    fireOffer = "The fire's warmth: say a number to strengthen that spell a level (once, free).",
    fireItem = "{number}) {spell} {level}>{next}",
    fireGap = "   ",
    fireRises = "{player} draws strength from the fire: {spell} rises to level {level}!",
    -- A spell the game has no title for, by its number.
    spellUnknown = "spell {id}",

    -- The altar and the blessings.
    altarOffer = "The altar offers a blessing for the rest of the delve: say its number to take it.",
    altarItem = "{number}) {name}: {text}",
    altarHeldOne = " (you have it {count} time)",
    altarHeldMany = " (you have it {count} times)",
    blessed = "{player} is blessed with {name}: {text}!",
    fortune = "Fortune smiles: {gold} gold.",
    blessingsNone = "No blessings yet: find an altar.",
    blessingsYours = "Your blessings: {list}.",
    blessingHeld = "{name}",
    blessingHeldMany = "{name} x{count}",

    -- The bonus objectives: each one's words (Swift's to noxsdl's panel short), and how it goes.
    bonus = "Bonus: {objective}, for a reward.",
    bonusDone = "Bonus done: {objective}! A reward at each one's feet.",
    objPurge = "Slay every monster",
    objHunt = "Slay the quarry, the {quarry}",
    objSeals = "Break the seals (touch the orb pools)",
    objSwift = "Reach a gate within {minutes}:{seconds}",
    objSwiftShort = "Reach a gate in time",
    purgeHalf = "Bonus: half the monsters slain, {left} to go.",
    sealBroken = "{player} breaks a seal ({done} of {of}).",
    tooSlow = "Too slow: the bonus is lost.",
    swiftDone = "Bonus done: through in time! {gold} gold each.",

    -- A champion guardian (a bot boss), its team's name.
    championIs = "The guardian is a champion: {name} the {word}! Slay them to open the way on.",
    championLate = "The champion never came: {creature} guards the way instead.",
    championTeam = "Champion",

    -- The records (the board, ?record).
    recordsNone = "No delve on record here yet: be the first!",
    recordsTitle = "The deepest delves:",
    recordLine = "{place}. depth {depth} (act {act}), {date}: {party}",
    recordMember = "{name} ({class})",
    recordMore = " and {count} more",
}

-- The records as told: so many on the lobby's board (and "?record"), so many of each party named
-- there (a line's length), so many sent to a NoX Core client's table.
data.board = { shown = 5, named = 4, sent = 10 }

-- How near a player is to something for it to count (in world units): a lobby's sign speaking,
-- its board telling the records, its portal beginning the delve; a boss found at its spot; a gate's
-- sign found by its spot and speaking to a player; the shrine, a campfire and an altar; a seal
-- touched.
-- The thing types the director finds on its maps once they load (levels.luau makes them): the
-- lobby's signs, portal and records board; a level's gate signs (the same signs).
data.things = { signs = { "Sign1", "Sign2" }, portal = "GauntletExitA", board = "SignShop01" }

data.near = {
    sign = 60, board = 100, portal = 40, boss = 40, gateSign = 20, gateSignSpeaks = 70,
    shrine = 60, campfire = 80, altar = 80, seal = 45,
}

-- How often, in frames (30 a second): a lobby's sign speaks at most, its board tells a player
-- the records at most; a level's line told so far in; the party's fall to the wipe; a gate's sign
-- speaks at most; Renewal's beat; Regenerating's (and the health raised kept); Purge's count; a
-- celebration's length and its sparks' beat; a champion guardian's wait to come in, and its time
-- down before it's gone.
data.every = {
    sign = 150, board = 600, levelLine = 60, wipe = 90, gateSign = 150, renewal = 60, regenerate = 30,
    purge = 15, celebration = 75, sparks = 3, championWait = 600, championFallen = 90,
}

-- A class's first spells after a wipe (a quest's start's), by class: warrior, wizard, conjurer.
data.firstSpells = { {}, { "SPELL_FIREBALL" }, { "SPELL_CHARM", "SPELL_LESSER_HEAL" } }

-- The heat's bite (a scorching biome's air): this much fire every so many frames.
data.heat = { damage = 2, every = 90 }

-- What a biome's air does (mapgen's climates; a mild one says nothing): the line told on the
-- level, the gate's warning.
data.climates = {
    Freezing = { line = "The bitter cold slows you while you're here.", tag = " (bitter cold: slowed)" },
    Scorching = { line = "The heat burns you, slowly, until you get out.", tag = " (searing heat: burns)" },
}

-- A shopkeeper's name (the shop's window titles it "NPC:" and its script name's last part): one
-- of the game's townsfolk's (their NPC: lines), none of the story's.
data.shopkeepers = {
    "Albi", "Aldwyn", "Beatrice", "Bing", "Brenneth", "Bryan", "Clyde", "Delwin", "Dudley", "Evelyn", "Fenton",
    "Floyd", "Garrit", "Geoff", "Gretchen", "Harold", "Henrick", "Horst", "Jacob", "Joyce", "Kalen", "Kayla",
    "Kelvin", "Lena", "Lydia", "Millard", "Naldo", "Osborn", "Rayner", "Seth", "Tommy", "Tyler",
}

-- The breakable barrels and crates (each something to give as it breaks), and the gems one may
-- hold.
data.breakables = {
    "Barrel", "Barrel2", "Crate1", "Crate2", "DarkCrate1", "DarkCrate2", "LargeBarrel1", "LargeBarrel2",
    "PiledBarrels1", "PiledBarrels2", "PiledBarrels3", "PiledBarrels4",
}
data.barrelGems = { "Ruby", "Emerald", "Diamond" } -- (thing.bin's names; the Tide's pickups use the same)

-- A champion (a bot boss: a foe, not one of the players' bots): its names; by class (warrior,
-- wizard, conjurer) the word for it, the gear it wears, its spells (a warrior: its abilities).
data.champions = {
    "Valdris", "Morgath", "Kessra", "Thorne", "Ilyana", "Draven", "Sabine", "Grimwald",
    "Orsolya", "Varek", "Selwyn", "Cassia", "Brannoc", "Ysolde", "Halvar", "Mirela",
}
data.championClasses = {
    { word = "warrior",
      gear = { "GreatSword", "OrnateHelm", "SteelShield", "PlateBreast", "PlateArms", "PlateBoots" },
      spells = {} },
    { word = "wizard",
      gear = { "LesserFireballWand", "WizardHelm", "WizardRobe", "LeatherBoots" },
      spells = { "SPELL_FIREBALL", "SPELL_SHOCK", "SPELL_MAGIC_MISSILE", "SPELL_BLINK", "SPELL_HASTE", "SPELL_SHIELD", "SPELL_DEATH_RAY" } },
    { word = "conjurer",
      gear = { "CrossBow", "Quiver", "LeatherHelm", "LeatherArmor", "LeatherBoots" },
      spells = { "SPELL_POISON", "SPELL_PIXIE_SWARM", "SPELL_METEOR", "SPELL_FIST", "SPELL_SLOW", "SPELL_STUN", "SPELL_LESSER_HEAL" } },
}
-- The spell a champion of the early acts goes without (rules.champion's late).
data.championLateSpell = "SPELL_DEATH_RAY"

return data
