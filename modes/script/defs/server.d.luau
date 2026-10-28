-- The server toolbox: the global `nox` a mode's server scripts see (modes/script/src/scripted_mode.cpp
-- registers it; a test checks that every function there is declared here). Read by the map
-- editor's Luau diagnostics and its Mod Reference panel. Each function: one "--- " doc line, then
-- its declaration on one line.

type NoxPlayer = { peer: number, name: string, unit: number, identified: boolean, account: string?, class: string?, level: number?, experience: number? }
type NoxItemSpec = { effectiveness: string?, material: string?, primary: string?, secondary: string?, durability: number?, charges: number? }
type NoxItemFacts = { type: string, effectiveness: string?, material: string?, primary: string?, secondary: string?, durability: number, maxDurability: number, charges: number, maxCharges: number, worn: boolean, holder: number? }
type NoxModifier = { name: string, section: string, position: string, color: { r: number, g: number, b: number }, worth: number, desc: string }
type NoxEvents = { death: number, destroyed: number, damage: number, damaged: number, drop: number, equip: number }
type NoxWeather = { kind: string, strength: number?, wind: { angle: number?, speed: number? }?, fade: number? }
type NoxGenerate ={ map: string, module: string?, fn: string?, params: any?, arena: { [string]: any }?, seed: number?, title: string?, ahead: boolean? }
type NoxStore = {
    get: (key: string) -> (any, string?),
    put: (key: string, value: any) -> (boolean, string?),
    remove: (key: string) -> boolean,
    list: (prefix: string) -> { string },
    lock: (key: string) -> (boolean, string?),
    unlock: (key: string) -> (),
    keyOf: (text: string) -> string,
}
type NoxObject = { id: number, type: string, x: number, y: number, team: number, holder: number, health: number, maxHealth: number, radius: number, mass: number, direction: number, owner: number, pendingDelete: boolean, netCode: number, alive: boolean, player: boolean, observing: boolean, lessons: number, deaths: number, peer: number, flag: boolean, colour: number, home: { x: number, y: number }, dropped: number }
type NoxItem = { id: number, type: string, flag: boolean }
type NoxUnitFilter = { monsters: boolean?, players: boolean?, alive: boolean?, notDead: boolean?, notGone: boolean?, enemiesOf: number? }
type NoxAttack = { damage: number?, type: number?, range: number?, area: boolean?, sides: number?, x: number?, y: number? }
type NoxMark = { frame: number, n: number, last: number }
type NoxMonsterInfo = { experience: number, health: number, questHealth: number, speed: number }
type NoxSpellInfo = { flags: number, onceOnly: boolean }
type NoxTeam = { id: number, name: string, score: number, colour: number, players: number }
type NoxStream = { next: (self: NoxStream) -> number }

declare nox: {
    --- # Players and messages
    --- Writes a line to the host's log.
    log: (text: string) -> (),
    --- The players in the game now, as { peer, name, unit, identified } tables.
    players: () -> { NoxPlayer },
    --- One player by its peer as { peer, name, unit, identified }; nil when no such player is in.
    player: (peer: number) -> NoxPlayer?,
    --- A mode option's value as text (mode.json's options, as the host set them); nil when unset.
    option: (key: string) -> string?,
    --- The game frame now (30 a second); 0 before the game is made.
    frame: () -> number,
    --- Sends a value to a player's UI (its onMessage); false when it has no UI ready or is too far behind.
    send: (peer: number, value: any) -> boolean,
    --- Whether a player's client runs the mode's UI (and so draws views as data).
    hasUi: (peer: number) -> boolean,
    --- A text line to one player's unit alone, as the native modes tell one (250 characters at most).
    tell: (unit: number, text: string) -> (),
    --- A text line to everyone as the engine broadcasts one (250 characters at most).
    textToAll: (text: string) -> (),
    --- # Views (say, choice, table)
    --- Says a line to one player: a say view to its UI, else a text line.
    say: (peer: number, text: string) -> (),
    --- Says a line to everyone: a say view to each UI, else a text line.
    sayAll: (text: string) -> (),
    --- Asks one player to pick one of 1-9 answers; its pick comes to mode.onChoice.
    choice: (peer: number, title: string, answers: { any }) -> (),
    --- Shows a titled table of rows (each a list of cells, or one cell) to one player, or nil: everyone.
    table: (peer: number?, title: string, rows: { any }) -> (),
    --- # The world
    --- What a thing is now as a table (player and flag fields only on those); nil when it's gone.
    object: (id: number) -> NoxObject?,
    --- The ids of the things of a type (thing.bin's name) in the world: a whole-world walk, for set-up.
    objects: (thingType: string) -> { number },
    --- The ids of the players' units in the world (observing ones too), in the world's order.
    units: () -> { number },
    --- What a thing carries, as { id, type, flag } tables.
    inventory: (id: number) -> { NoxItem },
    --- Sends the thing's touches to mode.collide from now on, instead of its own collide.
    watchCollide: (id: number) -> (),
    --- Puts a thing from the ground into a player's unit's inventory, as a pickup does.
    take: (unit: number, item: number) -> (),
    --- Drops an item out of a player's unit's inventory at (x, y); whether it was dropped.
    drop: (unit: number, item: number, x: number, y: number) -> boolean,
    --- Puts a thing at (x, y): moved on the ground, or out of its holder's inventory into the world.
    place: (id: number, x: number, y: number) -> (),
    --- A clear spot within r of (x, y), as the game finds one.
    clearPointNear: (x: number, y: number, r: number) -> (number, number),
    --- Ends a unit's enchantments from spells whose flags share any bit of the mask.
    endSpellEffects: (id: number, mask: number) -> (),
    --- Marks the thing on (true) or off every player's minimap.
    minimap: (id: number, on: boolean?) -> (),
    --- Plays a sound (its number or name) at the thing.
    sound: (id: number, sound: string | number) -> (),
    --- # Engine paths
    --- Gives health as the game heals (up to its most), its report with it.
    heal: (id: number, amount: number) -> (),
    --- Casts a spell (name or number) as an item's effect would, at a target (nil: none); whether it worked.
    cast: (spell: string | number, caster: number, target: number?, x: number, y: number, power: number?) -> boolean,
    --- Puts an enchantment (name or bit) on for that many frames (0: for good); frames below 0 take it off.
    enchant: (id: number, enchantment: string | number, frames: number, power: number?) -> (),
    --- Whether a thing has an enchantment (name or bit) on.
    enchanted: (id: number, enchantment: string | number) -> boolean,
    --- The engine's damage from a source (nil: none) of a damage type 0-17; what the target took.
    damage: (target: number, source: number?, amount: number, damageType: number, weapon: number?) -> number,
    --- The ids of the units within the radius of (x, y), in the grid's order, filtered.
    unitsNear: (x: number, y: number, radius: number, filter: NoxUnitFilter?) -> { number },
    --- How many units unitsNear would list (no list made).
    countNear: (x: number, y: number, radius: number, filter: NoxUnitFilter?) -> number,
    --- A point effect every client draws (a MSG_FX_* message that carries only x and y).
    effect: (op: number, x: number, y: number) -> (),
    --- A spell's sound's name (which: 0 its cast, 1 on, 2 off) for nox.sound; nil when none.
    spellSound: (spell: string | number, which: number?) -> string?,
    --- Makes a thing of a type (name or number) at (x, y), owned by maker if given, an item with a spec's modifiers; its id, nil for no such type.
    create: (thingType: string | number, x: number, y: number, maker: number?, spec: NoxItemSpec?) -> number?,
    --- The floor tile index at a point; -1 when none.
    floorAt: (x: number, y: number) -> number,
    --- Whether the wall cell (in 23-unit cells) holds a wall.
    wallAt: (cx: number, cy: number) -> boolean,
    --- A creature type's monster.bin numbers as { experience, health, questHealth, speed }; nil when none.
    monsterInfo: (thingType: string) -> NoxMonsterInfo?,
    --- Sets a thing's health and most health as fields (no report; nox.touch sends them).
    setHealth: (id: number, health: number, most: number) -> (),
    --- Marks every part of a thing to be sent to the clients again.
    touch: (id: number) -> (),
    --- A creature's sight range; given a range, sets it instead.
    sightRange: (id: number, range: number?) -> number,
    --- The ids of the live enemy creatures a unit sees within range, nearest first, at most `most`.
    nearestFoes: (unit: number, range: number, most: number, losTested: number) -> { number },
    --- Pushes a thing away from (fromX, fromY) as the engine does; a negative force pulls.
    applyForce: (id: number, fromX: number, fromY: number, force: number) -> (),
    --- A blow as the engine swings one from the attacker's facing; whether it hit anything.
    attack: (attacker: number, attack: NoxAttack) -> boolean,
    --- A thing's facing, 0-255; given a direction (0-255), sets it instead.
    direction: (id: number, d: number?) -> number,
    --- The 256-step direction of a vector, as the engine works it out.
    directionOf: (dx: number, dy: number) -> number,
    --- The unit vector of a 256-step direction, from the engine's own table.
    directionVector: (d: number) -> (number, number),
    --- Where the world's list of new things stands now, for nox.madeSince.
    newMark: () -> NoxMark,
    --- The ids of the things made since a mark (nox.newMark's), oldest first.
    madeSince: (mark: NoxMark) -> { number },
    --- A spell's thing.bin record as { flags, onceOnly }; nil when none.
    spellInfo: (spell: string | number) -> NoxSpellInfo?,
    --- An enchantment's frames left on a thing (0: on for good, or not on).
    enchantTime: (id: number, enchantment: string | number) -> number,
    --- A gamedata.bin number as this game reads it (index from 0); nil for a name it hasn't.
    gameNumber: (name: string, index: number?) -> number?,
    --- Whether no wall stands between two points; blocked by a wall, also where it met it and its cell.
    traceRay: (x1: number, y1: number, x2: number, y2: number, flags: number) -> (boolean, number?, number?, number?, number?),
    --- Looses a missile of a type from the owner's edge in a direction; its id, nil when a wall is in the way.
    shoot: (thingType: string | number, owner: number, direction: number) -> number?,
    --- Summons a creature as a summoning spell does, owned by owner (nil: nobody's); its id, nil for no such type.
    summon: (thingType: string | number, x: number, y: number, owner: number?, direction: number) -> number?,
    --- An ability's sound's name (which: 0 its start, 1, 2 its end) for nox.sound; nil when none.
    abilitySound: (ability: number, which: number?) -> string?,
    --- Puts a thing on the map at (x, y) as the engine places one.
    setPosition: (id: number, x: number, y: number) -> (),
    --- Sets a thing's velocity per frame, -1000 to 1000 each.
    setVelocity: (id: number, vx: number, vy: number) -> (),
    --- Sets a thing's owner as the engine does (nil clears it); never a player's unit.
    setOwner: (id: number, owner: number?) -> (),
    --- Sets a thing's team id 0-255; never a player's unit (nox.joinTeam is for those).
    setTeam: (id: number, team: number) -> (),
    --- Sets or clears a thing's NO_COLLIDE flag: nothing touches it, it pushes nothing.
    setNoCollide: (id: number, on: boolean?) -> (),
    --- The engine's area damage about (x, y): whole within `full`, less towards `reach`; then the walls.
    damageAt: (x: number, y: number, reach: number, full: number, damage: number, damageType: number, source: number?, except: number?, spareSource: boolean?) -> (),
    --- Deletes a thing as the engine does (gone at the frame's end); never a player's unit.
    delete: (id: number) -> (),
    --- A type's number (thing.bin's, from 1) for the cheap checks; nil when there's no such type.
    thingType: (name: string) -> number?,
    --- Whether a thing is in the world and not going, and of the type number if one is given.
    present: (id: number?, thingType: number?) -> boolean,
    --- Whether a thing is present, not flagged dead, and has some health.
    living: (id: number?) -> boolean,
    --- Sets a thing's collide by name: "DefaultCollide" or "ModeCollide".
    setCollide: (id: number, name: string) -> (),
    --- Brings a watching (observing, not dead) player's unit back into play as the quest does.
    backIntoPlay: (unit: number) -> (),
    --- The spot beside a soul gate where its fallen come back.
    nearSoulGate: (gate: number) -> (number, number),
    --- A present thing's x, y and radius (an error when it isn't present).
    position: (id: number) -> (number, number, number),
    --- Compacts a list of ids in place to those nox.present would pass; its new length.
    keepPresent: (ids: { number }, thingType: number?) -> number,
    --- Compacts a list of ids in place to those nox.living would pass; its new length.
    keepLiving: (ids: { number }) -> number,
    --- Sets the things evenly round (cx, cy) from an angle in radians, each stopped; how many placed.
    circle: (ids: { number }, cx: number, cy: number, radius: number, angle: number, count: number) -> number,
    --- nox.damageAt about each thing in the list, in order, each its own source.
    damageAtEach: (ids: { number }, reach: number, full: number, damage: number, damageType: number, except: number?, spareSource: boolean?) -> (),
    --- # Players' numbers and book
    --- A player's unit's gold.
    gold: (unit: number) -> number,
    --- Sets a player's unit's gold; with report, its client told at once.
    setGold: (unit: number, n: number, report: boolean?) -> (),
    --- A player's unit's mana and most mana.
    mana: (unit: number) -> (number, number),
    --- Sets a player's unit's mana and most; report "total" (or nil) or "plain".
    setMana: (unit: number, mana: number, most: number, report: string?) -> (),
    --- Sets a player's unit's health and most health, its client told both.
    setPlayerHealth: (unit: number, health: number, most: number) -> (),
    --- A player's unit's strength.
    strength: (unit: number) -> number,
    --- Sets a player's unit's strength, its client told.
    setStrength: (unit: number, n: number) -> (),
    --- A player's level of a spell of the book (name or number 1-136; 0: unknown).
    spellLevel: (unit: number, spell: string | number) -> number,
    --- A player's book: the level of each spell 1-136 (0: unknown).
    spells: (unit: number) -> { number },
    --- Raises a spell the player knows, below 5, a level, its client told; whether it rose.
    raiseSpell: (unit: number, spell: string | number) -> boolean,
    --- The engine's spell grant: up a level, or to `level` when not 0; whether it was granted.
    grantSpell: (unit: number, spell: string | number, level: number, notify: boolean?, show: boolean?) -> boolean,
    --- A player's level of an ability 1-5.
    abilityLevel: (unit: number, ability: number) -> number,
    --- Sets a player's level (0-5) of an ability 1-5; with report, its client told, and with show it flies to its bar.
    setAbility: (unit: number, ability: number, level: number, report: boolean?, show: boolean?) -> (),
    --- A player's field guide: for each creature 1-40, whether it has it.
    fieldGuide: (unit: number) -> { boolean },
    --- Puts a creature (its guide number 1-40 or its type's name) in a player's field guide as a found guide does; with notify, the player and the others told. Whether it was put in (false: already known, or no such creature).
    grantCreature: (unit: number, creature: string | number, notify: boolean?) -> boolean,
    --- A spell's number (1-136 in a player's book) from its name (SPELL_...); nil when there's no such spell.
    spellId: (name: string) -> number?,
    --- A creature's number in the field guide (1-40) from its type's name (as grantCreature reads it); nil when it has none.
    guideNumber: (name: string) -> number?,
    --- Clears a player's spells, abilities and field guide (no messages: nox.revoke tells its client).
    clearBook: (unit: number) -> (),
    --- Queues a spell (0x11), ability (0x12) or field guide creature (0x13) taken off a player's client.
    revoke: (playerName: string, kind: number, id: number) -> (),
    --- Sends up to 30 queued revokes to each player, once a frame; how many are still owed.
    sendRevokes: () -> number,
    --- # Quests
    --- A player's status flags (1 observing, 0x20 eliminated, 0x400 poisoned...).
    status: (unit: number) -> number,
    --- The frame a player's quest lives ran out (0: never).
    gameOverAt: (unit: number) -> number,
    --- Sets the frame a player's quest lives ran out (0: never).
    setGameOverAt: (unit: number, frame: number) -> (),
    --- The quest exit a player went out by (an id; 0 none).
    exitGate: (unit: number) -> number,
    --- A boss's loot: every reward kind made `count` times at `stage`, each item into the holder.
    rewards: (holder: number, stage: number, count: number) -> (),
    --- What a fresh reward marker (kinds: 8 bits, nil all) gives at `stage`, made at (x, y); its id or nil.
    rewardAt: (x: number, y: number, stage: number, kinds: number?) -> number?,
    --- Switches a thing on or off as the engine does, its clients told.
    setOnOff: (id: number, on: boolean?) -> (),
    --- A speech bubble over a thing every client draws, shown that many frames.
    unitSay: (id: number, text: string, frames: number) -> (),
    --- The quest's stage now.
    questStage: () -> number,
    --- # The game
    --- Sets the game's listed name (15 letters show).
    setServerName: (name: string) -> (),
    --- Sets the next map the game goes to ("Name.map").
    exitTo: (map: string) -> (),
    --- Closes the game to everyone not in it now (once; nox.unlock undoes it).
    lock: () -> (),
    --- Gives the game back the host's own access settings (nothing when not locked).
    unlock: () -> (),
    --- Adds a name to the game's allowed list (on), or takes every entry of it off.
    allow: (name: string, on: boolean?) -> (),
    --- Whether thing a is hostile to thing b.
    isEnemy: (a: number, b: number) -> boolean,
    --- The ids of the empty chests on the map: a whole-world walk, for set-up.
    chests: () -> { number },
    --- A new item of a type into a player's inventory (with wear, put on), with a spec's modifiers; its id, nil for no such type.
    giveItem: (unit: number, thingType: string, wear: boolean?, spec: NoxItemSpec?) -> number?,
    --- Makes a team with a name and colour 1-16, every client told; its id, nil when there's no room.
    createTeam: (name: string, colour: number) -> number?,
    --- Puts a thing onto a team, every client told (not moved to its base).
    joinTeam: (id: number, team: number) -> (),
    --- Removes a team, its members left with none, every client told.
    removeTeam: (team: number) -> (),
    --- Plays a sound (its number 0-1023 or name) at a point, from no thing.
    soundAt: (x: number, y: number, sound: string | number) -> (),
    --- # Streams and maths
    --- The mode's own random stream: 0-1 with no arguments, 1-n with one, lo-hi with two.
    random: (lo: number?, hi: number?) -> number,
    --- A number rounded to the server's float as its maths makes it.
    f32: (x: number) -> number,
    --- cos of a number as a float, as the server works it out.
    cosf: (x: number) -> number,
    --- sin of a number as a float, as the server works it out.
    sinf: (x: number) -> number,
    --- One draw from the server's own game stream, 0 to n - 1 (it shares the engine's draws).
    gameRandom: (n: number) -> number,
    --- A seeded std::mt19937 stream; stream:next() gives its next number.
    mt19937: (seed: number) -> NoxStream,
    --- Shuffles a list in place as the C++ shuffles one from that stream; the list.
    shuffle: (stream: NoxStream, list: { any }) -> { any },
    --- a * b wrapped as a 32-bit unsigned product.
    mul32: (a: number, b: number) -> number,
    --- # Teams and scores
    --- The teams now, as { id, name, score, colour, players } tables.
    teams: () -> { NoxTeam },
    --- Sets a team's score, told to everyone.
    teamScore: (team: number, score: number) -> (),
    --- Moves a player's lessons up or down, told to everyone; its lessons now.
    changeLessons: (unit: number, by: number) -> number,
    --- Counts a death for a player, told to everyone.
    addDeath: (unit: number) -> (),
    --- Whether more than one player (in team play, team) is in play: the game's test before scoring.
    anyonePlaying: () -> boolean,
    --- The host's lesson limit and minutes for the stock mode the mode looks like (0: none).
    limits: () -> (number, number),
    --- # The stock messages
    --- Sends MSG_INFORM to everyone: a notice's code and exactly the numbers it carries.
    inform: (code: number, ...number) -> (),
    --- Sends one of the game's lines by its string id (e.g. "objcoll.c:FlagNoTeam") to one player.
    informString: (unit: number, id: string) -> (),
    --- Reports a team's flag (0 home, 1 carried, 2 dropped), its colour and carrier's net code to everyone.
    flagStatus: (team: number, state: number, colour: number, carrier: number?) -> (),
    --- Whether the match timer runs and ms left; given seconds, starts it; given false, stops it.
    timer: (seconds: (number | boolean)?) -> (boolean, number),
    --- Ends the match with its winner: kind "team", "player", "flagTeam" or "ballTeam"; who 0: a tie.
    endMatch: (kind: string, who: number?, byTime: boolean?) -> (),
    --- Whether the match is over (till the next map loads).
    gameOver: () -> boolean,
    --- # Items and modifiers
    --- An item's facts (type, modifiers by slot, durability, charges, worn, holder); nil when gone or not an item.
    item: (id: number) -> NoxItemFacts?,
    --- A modifier.bin entry by name as { name, section, position, color, worth, desc }; nil when none.
    modifier: (name: string) -> NoxModifier?,
    --- Every modifier's name in a section ("effectiveness", "material" or "enchantment"), in modifier.bin's order.
    modifiers: (section: string) -> { string },
    --- Whether a modifier fits a slot ("effectiveness", "material", "primary", "secondary") of an item type.
    modifierFits: (thingType: string, slot: string, name: string) -> boolean,
    --- Puts on an item the player's unit carries; true, or false and why.
    equip: (unit: number, item: number) -> (boolean, string?),
    --- Takes off an item the player's unit wears; whether it came off.
    unequip: (unit: number, item: number) -> boolean,
    --- # Player levels
    --- Sets a player's level (1-10), its numbers worked out again.
    setLevel: (unit: number, level: number) -> (),
    --- Sets a player's experience, its client told.
    setExperience: (unit: number, experience: number) -> (),
    --- Gives a player experience as a kill does (levels gained with it).
    giveExperience: (unit: number, amount: number) -> (),
    --- # Events and timers
    --- The event bits for nox.watchEvents: death, destroyed, damage, damaged, drop, equip.
    events: NoxEvents,
    --- Sets which events a thing sends the mode's hooks (a sum of nox.events); its old bits.
    watchEvents: (id: number, mask: number) -> number,
    --- Calls fn(...) once, that many frames (1 or more) from now; its timer id.
    after: (frames: number, fn: (...any) -> (), ...any) -> number,
    --- Calls fn(...) every that many frames (1 or more), first after one beat; its timer id.
    every: (frames: number, fn: (...any) -> (), ...any) -> number,
    --- Cancels a timer of nox.after or nox.every; whether it was still waiting.
    cancelTimer: (id: number) -> boolean,
    --- # Weather
    --- NoX Core players' weather (peer nil: everyone, and who comes later); wind.angle in degrees clockwise on screen, 0 blowing toward the top; strength, wind.speed 0-1; fade in frames.
    weather: (peer: number?, weather: NoxWeather) -> (),
    --- # Storage
    --- The mod's own key-value store on the host (nil when this host keeps none).
    store: NoxStore?,
    --- # Generated maps
    --- Builds one of the mod's own maps: ahead (default) on a worker, onGenerated tells; or now, its result back.
    generate: (request: NoxGenerate) -> (boolean, any),
    -- (In a map generator module, the map writer `w` also places teleport pentagrams and lights:
    -- w:thing(type, x, y, spec) with spec.transporter = { target = id } for a pentagram (the id of
    -- the pentagram it sends to, 0 for none; w:nextExtent() is the id the next thing written gets)
    -- and spec.light = { intensity (0-63), color = { r, g, b }, ... } for a light such as
    -- ColorLight; every other light field as the map editor names it.)
    --- The map name a tag (1-3 of a-z and 0-9, a letter first) gets: <mapPrefix>_<slot><TAG>.
    mapName: (tag: string) -> string,
    --- A tag's map's state ("building", queued ones too; "ready"; "failed"); nil when none.
    generating: (tag: string) -> string?,
    --- Cancels a map still queued or building; whether there was one.
    cancelGenerate: (tag: string) -> boolean,
    --- The loaded map's name (its file's, without the folder and extension); "" before the first map.
    currentMap: () -> string,
    --- The script API version this host runs.
    api: number,
}

--- Loads another of the mode's server scripts, server/<name>.luau, and returns what it returns.
declare function require(name: string): any

-- The hooks the script's table may have (the server calls each one it has), for the Mod
-- Reference: "--- @hook <call> | <doc>".
--- @hook configure(ctx) | Before the game (ctx.options, seed, slot, maxPlayers): may return { map, maxPlayers }, a map to generate (generate) and host options: quest, questScreens, questMaps, forceClass, multiclass, monstersAlert, keepThingsOnDeath, serverName, and bookIsTheMod (true: outside a quest a player starts with no spells, abilities or field guide creatures and may use only those the mode grants (nox.grantSpell, nox.raiseSpell, nox.setAbility, creatures with nox.grantCreature), at their levels, as in a quest; the books of NoX Core clients of level 2 list only those; reward books, guides and pedestals give nothing).
--- @hook start() | The game is made.
--- @hook onJoin(player) | A player came in ({ peer, name, unit, identified }).
--- @hook playerReady(player) | A player's game has the map loaded and is in play: after its join, and again after each map change.
--- @hook onLeave(player) | A player left.
--- @hook onChat(player, text) | A chat line: true takes it (not relayed).
--- @hook onChoice(player, index) | An answer to nox.choice.
--- @hook tick(frame) | Once per game frame.
--- @hook console(line) | A host console line: return a list of reply lines to take it.
--- @hook save() | A live reload with state kept: return the state.
--- @hook restore(state) | The new code after a live reload: save()'s state.
--- @hook stop() | The game is about to go.
--- @hook onUiMessage(player, value) | A value the player's UI sent (nox.send there).
--- @hook mapLoaded(name) | Each map's load (the map's name, as nox.currentMap gives it).
--- @hook onKill(victim, killer, assist) | A player's death (ids; nil: none).
--- @hook respawnPoint(unit) | Where a unit comes back: return x, y (nil: the game's start).
--- @hook collide(thing, other) | A thing it watches (nox.watchCollide) was touched by another (ids; nil: no thing).
--- @hook onObjectKilled(e) | A watched thing died ({ id, type, x, y, by }); told the next frame's start.
--- @hook onDestroyed(e) | A watched thing was destroyed ({ id, type, x, y }).
--- @hook onDamage(target, source, weapon, amount, kind) | Before a watched thing takes a hit: return a new amount (0 cancels) or nil; changes nothing in the world.
--- @hook onDamaged(target, amount) | A watched thing took a hit.
--- @hook onPickup(taker, item, x, y) | Something picked an item up from (x, y).
--- @hook onDrop(holder, item) | A holder dropped a watched item.
--- @hook onEquip(unit, item, worn) | A watched item was put on (true) or taken off.
--- @hook onSpellCast(caster, spell, target, x, y) | A spell was cast (its name; target nil: none).
--- @hook onExit(exit, player) | A player touched an exit: return false to hold them; changes nothing in the world.
--- @hook onGenerated(tag, ok, result) | A map nox.generate built ahead is written (ok), or failed (result: why).
