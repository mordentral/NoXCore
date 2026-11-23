-- The server toolbox: the global `nox` a mode's server scripts see (modes/script/src/scripted_mode.cpp
-- registers it; a test checks that every function there is declared here). Read by the map
-- editor's Luau diagnostics and its Mod Reference panel. Each function: one "--- " doc line, then
-- its declaration on one line.

-- language: its client's UI language ("en", "ru", "ko"...: a NoX Core client of level 14), else the host's own.
type NoxPlayer = { peer: number, name: string, unit: number, identified: boolean, clientKey: string?, language: string, account: string?, class: string?, level: number?, experience: number? }
type NoxItemSpec = { effectiveness: string?, material: string?, primary: string?, secondary: string?, durability: number?, charges: number? }
type NoxItemFacts = { type: string, effectiveness: string?, material: string?, primary: string?, secondary: string?, durability: number, maxDurability: number, charges: number, maxCharges: number, worn: boolean, holder: number? }
-- text and identify: desc's and IDENTIFYDESC's words from the string table (none without nox.csf); effects: each effect's number by its function's name.
type NoxModifier = { name: string, section: string, position: string, color: { r: number, g: number, b: number }, worth: number, desc: string, text: string?, identify: string?, effects: { [string]: number } }
-- name: the game's name for it (UTF-8; none without nox.csf); durability, charges: its most (0: none); damageMin,
-- damageCoefficient, requiredStrength, range: a weapon record's (a hit: damageMin + (strength - requiredStrength) x
-- damageCoefficient); damageType: a melee swing's; armor: an armour record's ARMOR_VALUE (the fraction it takes off);
-- classes: who may use or wear it (a multiclass player: anything).
type NoxItemStats = { type: string, name: string?, durability: number, charges: number, damageMin: number?, damageCoefficient: number?, requiredStrength: number?, range: number?, damageType: string?, armor: number?, classes: { warrior: boolean, wizard: boolean, conjurer: boolean } }
type NoxEvents = { death: number, destroyed: number, damage: number, damaged: number, drop: number, equip: number, use: number, pickup: number }
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
type NoxRecords = {
    put: (key: string, row: { [string]: number | string }) -> (boolean, string?),
    remove: (key: string) -> boolean,
}
type NoxObject = { id: number, type: string, x: number, y: number, team: number, holder: number, health: number, maxHealth: number, radius: number, mass: number, direction: number, owner: number, pendingDelete: boolean, netCode: number, alive: boolean, player: boolean, observing: boolean, lessons: number, deaths: number, peer: number, flag: boolean, colour: number, home: { x: number, y: number }, dropped: number }
type NoxItem = { id: number, type: string, flag: boolean }
type NoxUnitFilter = { monsters: boolean?, players: boolean?, alive: boolean?, notDead: boolean?, notGone: boolean?, enemiesOf: number? }
type NoxAttack = { damage: number?, type: number?, range: number?, area: boolean?, sides: number?, x: number?, y: number? }
type NoxMark = { frame: number, n: number, last: number }
type NoxMonsterInfo = { experience: number, health: number, questHealth: number, speed: number }
type NoxSpellInfo = { flags: number, onceOnly: boolean, manaCost: number, trapCost: number }
type NoxTeam = { id: number, name: string, score: number, colour: number, players: number }
type NoxStream = { next: (self: NoxStream) -> number }
-- A map script's LuauCall(name, a, b, c, d) as its handler gets it: call[1] to call[4] the four values as signed 32-bit ints (0: left out),
-- caller and trigger the script's caller and trigger now (ids; nil: none); its methods read value n (1-4) as an int, a float (its bits),
-- a thing (the map's handle: -1 caller, -2 trigger, else a script id; its id, nil none) or a string (the map's string of that id; nil none).
-- object and string only while the handler runs.
type NoxMapCall = { name: string, caller: number?, trigger: number?, [number]: number,
    int: (self: NoxMapCall, n: number) -> number, float: (self: NoxMapCall, n: number) -> number,
    object: (self: NoxMapCall, n: number) -> number?, string: (self: NoxMapCall, n: number) -> string? }

declare nox: {
    --- # Players and messages
    --- Writes a line to the host's log.
    log: (text: string) -> (),
    --- The players in the game now, as { peer, name, unit, identified, language } tables.
    players: () -> { NoxPlayer },
    --- One player by its peer as { peer, name, unit, identified, language }; nil when no such player is in.
    player: (peer: number) -> NoxPlayer?,
    --- A mode option's value as text (mode.json's options, as the host set them); nil when unset.
    option: (key: string) -> string?,
    --- The game frame now (30 a second); 0 before the game is made.
    frame: () -> number,
    --- Sends a value to a player's UI (its onMessage), in order; over 900 bytes (to 256 KB) packed and sent in pieces to a client of level 6 or above; false when it has no UI ready, its link is behind, 1 MB is already queued for it, or a big value's client is older (nothing sent: send it again later).
    send: (peer: number, value: any) -> boolean,
    --- Whether a player's client runs the mode's UI (and so draws views as data).
    hasUi: (peer: number) -> boolean,
    --- The key a player's NoX Core client proved for this host (mode.json's "clientKeys": 32 hex digits, the same for that PC every time); nil: none.
    clientKey: (peer: number) -> string?,
    --- A text line to one player's unit alone, as the native modes tell one (250 characters at most); colour { r, g, b } or { r = , g = , b = } (0-255): drawn in it by NoX Core clients of level 8, plain to others.
    tell: (unit: number, text: string, colour: { [any]: number }?) -> (),
    --- A text line to everyone as the engine broadcasts one (250 characters at most); colour as nox.tell's.
    textToAll: (text: string, colour: { [any]: number }?) -> (),
    --- # The text overlay
    --- A string-table key's text ("Journal:<name>", "thing.db:<name>PrettyName", an ability's title, a key of the mode's own), one for everyone or { en = , ru = , ko = ... } each NoX Core client of level 14 gets in its language (English, else the first, for one it hasn't; with others = "own" in the table, { ru = "...", others = "own" }, a player whose language isn't given keeps its own text for the key instead, as if the mode hadn't set it): its journal, story dialogs, item names and descriptions, ability titles and tooltips show it in place of nox.csf's, in order with the game's messages; nil takes it away. Stock and older clients keep nox.csf's. A story dialog's speaker name and a shop window's title ("NPC:<script name>", which the server looks up itself) are the key's text in each player's language (its first 31 letters, the shop's 24), for every client (stock and older ones in the host's language). 64 bytes a key, 4096 a text; false when 512 keys or 64 KB are kept already.
    setText: (key: string, text: (string | { [string]: string })?) -> boolean,
    --- The overlay's keys starting with prefix (no case) gone, nox.csf's shown again; nil or "": every one.
    clearText: (prefix: string?) -> (),
    --- A story dialog as the map's TellStory opens one, from npc (a creature) to a player's unit, its text a key (31 bytes; nox.setText's or nox.csf's), the speaker's name given (nil: the NPC's own, nox.setText's "NPC:<name>" first); false, nothing told, when the key is nox.setText's and the player's client can't show it (stock, or below level 14: tell it a line instead).
    tellStory: (unit: number, npc: number, key: string, speaker: string?) -> boolean,
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
    --- Sends the thing's touches to mode.collide from now on, instead of its own collide (a missile's too: what it hits, a wall, its running out).
    watchCollide: (id: number) -> (),
    --- Puts a thing from the ground into a player's unit's inventory, as a pickup does.
    take: (unit: number, item: number) -> (),
    --- A player picks a thing on the ground up as the game's own pickup does (gold into the purse, a potion drunk, class and three-of-a-kind refusals with their lines and sounds, onPickup; past the carry capacity "too much"), wherever it is: check reach yourself (the game's click pickup: 75). Whether it was taken.
    pickUp: (unit: number, item: number) -> boolean,
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
    --- An effect every client draws: op a MSG_FX_* message of a point (x, y), of two points (x, y, x2, y2: lightning, rays) or the green bolt (0x98: x, y, x2, y2, its frames); or a quest effect by name: "GENERATOR_BREAK" (x, y, size 0-255, 200 a generator's), "GENERATOR_HURT" (x, y), "GREEN_ZAP" (x, y, x2, y2, frames).
    effect: (op: number | string, x: number, y: number, x2: number?, y2: number?, frames: number?) -> (),
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
    --- In a co-op campaign the party's health scale goes on top: at its next pass (at once when
    --- the party changes, else within 5 s) the director takes the most set here as the creature's
    --- own base, whatever the value (the scaled number it had too), and keeps the health's share of
    --- it. A health passed as the creature has it, with only the most changed, stays the party's
    --- points (set the most every frame and the health doesn't grow).
    setHealth: (id: number, health: number, most: number) -> (),
    --- Marks every part of a thing to be sent to the clients again.
    touch: (id: number) -> (),
    --- A creature's sight range; given a range, sets it instead.
    sightRange: (id: number, range: number?) -> number,
    --- A creature reads `profile` as its monster.bin record in place of its type's (nil: its type's again): monster.bin's keys with their values, a whole record (a key left out is 0 or none, as in the file), `name` its name; STATUS a number of bits or '+' names, MELEE_ATTACK_DAMAGE_TYPE a number or name, a missile or function "" for none. Its AI's retreat and resume levels, status flags and flee range are taken from it as for one made with it, unless keepAi; its health, experience and speed stay. Not kept in a saved map.
    setCreatureProfile: (id: number, profile: { [string]: number | string | boolean }?, keepAi: boolean?) -> (),
    --- The monster.bin record a creature reads (its own, else its type's) as nox.setCreatureProfile takes one; nil when it has none.
    creatureProfile: (id: number) -> { [string]: number | string }?,
    --- A creature's or player's base speed and this frame's.
    speed: (id: number) -> (number, number),
    --- A creature's or player's base speed (0-100: what each frame's is worked out from), and this frame's when given.
    setSpeed: (id: number, base: number, current: number?) -> (),
    --- A thing's mass (how hard it is to push), above 0.
    setMass: (id: number, mass: number) -> (),
    --- Whether a creature is immune to "poison", "fire" or "electricity" (its SUBCLASS's IMMUNE_*); given on, sets it so first.
    immune: (id: number, kind: "poison" | "fire" | "electricity", on: boolean?) -> boolean,
    --- The sound set (soundset.bin's, by name) a creature's cries come from; nil: its type's again.
    setSoundSet: (id: number, name: string?) -> (),
    --- An NPC's (subclass NPC or FEMALE_NPC) six colours, each { r, g, b } 0-255 (nil: kept); the players who know it told.
    setNpcColors: (id: number, colors: { { number } }) -> (),
    --- A creature's AI told to cast a spell (its number 0-136, or name) at a point on the map (0-5888), as its own CAST_SPELL_ON_LOCATION; whether it was pushed.
    creatureCast: (id: number, spell: string | number, x: number, y: number) -> boolean,
    --- A creature's spellbook and casting rhythm (each field left out kept): spells[name or number] = its ways ("reaction", "defensive", "disabling", "offensive", "escape"; one, a list, or false: not cast); delays[way] = { least, most } frames; power 0-5; aim 0-1.
    setCaster: (id: number, caster: { spells: { [string | number]: string | { string } | boolean }?, delays: { [string]: { number } }?, power: number?, aim: number? }) -> (),
    --- false: a creature or a player holding the item can't let it go (every drop of it by one fails: by hand, a Fumble, the holder's death); a chest, or a thrown chakram in flight, lets it out as any item; true or nil: as the game has it.
    setDroppable: (id: number, droppable: boolean?) -> (),
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
    --- A spell's thing.bin record as { flags, onceOnly, manaCost, trapCost } (a player's prices as the game has them); nil when none.
    spellInfo: (spell: string | number) -> NoxSpellInfo?,
    --- false: Charm refuses the creature as one not charmable (a charm on it now ends uncontrolled); true or nil: as the game has it.
    setCharmable: (id: number, charmable: boolean?) -> (),
    --- A mod image drawn flat on the floor at (x, y) under the things, for NoX Core clients of level 7 running the mode's UI; opts { size (world units, 64), colour { r, g, b, a }, glow, spin (degrees a second), pulse { amount 0-1, period (s), scale }, follow (a thing's id) }; the map's own (gone at its load); its id, nil past 64 a map.
    decal: (asset: string, x: number, y: number, opts: { size: number?, colour: { number }?, glow: boolean?, spin: number?, pulse: { amount: number?, period: number?, scale: boolean? }?, follow: number? }?) -> number?,
    --- A decal (nox.decal's id) gone.
    removeDecal: (id: number) -> (),
    --- Every decal of the map gone.
    clearDecals: () -> (),
    --- An enchantment's frames left on a thing (0: on for good, or not on).
    enchantTime: (id: number, enchantment: string | number) -> number,
    --- An enchantment that's on given that many frames left (0: for good), silently; whether it was on.
    setEnchantTime: (id: number, enchantment: string | number, frames: number) -> boolean,
    --- A creature's or player's poison level (0: none) and the frames till it falls a step.
    poison: (unit: number) -> (number, number),
    --- Sets a creature's or player's poison level as the game does (0 cures it quietly); frames: till its first step down.
    setPoison: (unit: number, level: number, frames: number?) -> (),
    --- A gamedata.bin number as this game reads it (index from 0); nil for a name it hasn't.
    gameNumber: (name: string, index: number?) -> number?,
    --- Sets a gamedata.bin number (the line this game reads; index from 0, within its table) for this map alone, read from then on wherever the game reads it (one read as a thing is made or at the map's load keeps what it was); each map loads with the install's numbers, and nothing is saved or sent (clients keep their own: a stock ability's cooldown shading, say). Whether there was such a number.
    setGameNumber: (name: string, value: number, index: number?) -> boolean,
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
    --- A thing's object flags as the engine has them (0x10 SHORT, 0x40 NO_COLLIDE, 0x2000 NO_PUSH_CHARACTERS, 0x4000 AIRBORNE, 0x8000 DEAD, 0x1000000 ENABLED...), read only.
    flags: (id: number) -> number,
    --- Sets (on or nil) or clears flags among 0x10 SHORT (missiles and flyers pass over it), 0x40 NO_COLLIDE, 0x2000 NO_PUSH_CHARACTERS (creatures don't push it) and 0x8000 DEAD (not a creature's or a player's: a chest with it stays shut), as fields; nothing sent.
    setFlag: (id: number, flags: number, on: boolean?) -> (),
    --- A creature's status bits as it plays (monster.bin's STATUS and the AI's own: 0x4000 running...); given bits among the run ones (0x2000 CAN_RUN, 0x4000 RUNNING, 0x8000 ALWAYS_RUN, 0x10000 NEVER_RUN), sets (on or nil) or clears them first; the bits after.
    monsterStatus: (id: number, bits: number?, on: boolean?) -> number,
    --- Where a player's cursor is in the world, as its client last told the server (what aims its swings and spells).
    cursor: (unit: number) -> (number, number),
    --- What a wall type's (thing.bin's WALL name) breakable walls throw when hit or broken, for this map alone: up to 8 thing types in place of thing.bin's ({} none); nil, thing.bin's again. Each map loads with thing.bin's.
    setWallDebris: (wallType: string, names: { string }?) -> (),
    --- A missile's hit shape: a point made a circle of 1-32, a circle's radius up to 4x its type's (its owner spared by the growth until it's clear; only nil gives the stock hit back).
    setHitRadius: (id: number, radius: number?) -> (),
    --- A missile's burst reach and push reach times 0.5-3, read as it bursts (nil: 1).
    setBurstScale: (id: number, scale: number?) -> (),
    --- A thing drawn 0.5-3 x its size by NoX Core clients of client level 5 (its sprite and, a missile's, its burst); nil: as its type; nothing changes on the server.
    setDrawScale: (id: number, scale: number?) -> (),
    --- Keeps a creature or player from acting for that many frames, till it dies (a player's attacks, jumps, spells and abilities refused, a creature's AI paused); 0 frees it; false when 256 units are busy.
    setBusy: (unit: number, frames: number) -> boolean,
    --- The frames a unit stays busy (nox.setBusy) or still (in a leap); 0: free.
    busy: (unit: number) -> number,
    --- Carries a creature or player to (x, y) over time frames (20), over bodies, short of a wall; still the while, hittable; NoX Core clients of level 8 draw it height (40) high; onLand when it's down. Where it lands and whether a wall cut it short; nil when it can't (dead, held, frozen, jumping, charging, leaping).
    leap: (unit: number, x: number, y: number, opts: { time: number?, height: number? }?) -> (number?, number?, boolean?),
    --- NoX Core clients of level 8 draw a thing's body turning through its facings, turnsPerSecond (-8 to 8; negative the other way) for frames (0-1800); 0 stops it; nothing changes on the server.
    setSpin: (id: number, turnsPerSecond: number, frames: number) -> (),
    --- NoX Core clients of level 9 draw a creature's or player's body in its own animation name (a player's: "HAMMER_STRIKE", "WARCRY"..., or "ATTACK", the strike with what it holds; a creature's: "ATTACK", "CAST_SPELL"...), its frames spread over frames (0-1800), then as before; 0 stops it; opts.hold ("first", "last" or a frame number from 1) holds that one frame the while instead; opts.weapon (a player's: a weapon layer, "HAMMER", "OGRE_AXE"...) drawn in place of what it holds the while; every layer follows the body's frame and facing (a spin's too); none for a dead unit, one that dies stops; nothing changes on the server.
    playAnim: (unit: number, name: string, frames: number, opts: { weapon: string?, hold: (string | number)? }?) -> (),
    --- Defines (or changes) an ability the mode can grant: a button on a NoX Core client's strip beside the quickbar (client level 8); spec { title, icon (a mod asset's name, or a game icon's: "ABILITY_...", "SPELL_..."), cooldown (frames), target "point" | "none" | "hold", hold (a held one's most frames, 60) }.
    defineAbility: (id: string, spec: { title: string?, icon: string?, cooldown: number?, target: string?, hold: number? }) -> (),
    --- Gives a player an ability: its button 1-4; nil when the strip's full or the player's client can't show one (stock, or a NoX Core client before level 8: nothing granted); onModAbility when it's used.
    grantAbility: (unit: number, id: string) -> number?,
    --- Takes an ability from a player; whether it had it.
    revokeAbility: (unit: number, id: string) -> boolean,
    --- The frames till a player's ability can be used again (0: ready, or not granted); a warrior's stock one by its name ("BerserkerCharge", "WarCry", "Harpoon", "TreadLightly", "EyeOfTheWolf") too.
    abilityCooldown: (unit: number, id: string) -> number,
    --- Sets a player's ability's cooldown in frames (0: ready now); whether it's granted. A stock one's: its client told ready (0) or not, as the game tells it (the bar's shading runs the game's own delay); one under way goes on.
    setAbilityCooldown: (unit: number, id: string, frames: number) -> boolean,
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
    --- Sets a thing's collide by name: "DefaultCollide" or "ModeCollide"; nil gives it its type's own back (thing.bin's, with the data it was made with).
    setCollide: (id: number, name: string?) -> (),
    --- Brings a watching (observing, not dead) player's unit back into play as the quest does.
    backIntoPlay: (unit: number) -> (),
    --- on: puts a player out of play watching (unseen, out of reach), held out (its Action never brings it in), its camera the mode's (nox.camera; where it is till then) and no "observation mode" told; off: back into play as nox.backIntoPlay brings it, the hold let go (as it is when anything else brings it into play: a later watch is a free one).
    observe: (unit: number, on: boolean) -> (),
    --- Holds a watching player's camera (the view it's sent and where it hears from) at (x, y), or on a thing (thingOrX alone: following it while it's in the world), so its own controls no longer move it; nil lets it go (the stock observer's controls again). A player in play (alive, not watching) watches a thing as a creature's watcher does (status 2: its camera on it, it stands where it is; its own Move, Action or Jump or a hit bring it back, and so does a map change), the mode's abilities still heard from it; nil brings it back.
    camera: (unit: number, thingOrX: number?, y: number?) -> (),
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
    --- Puts a spell ("spell": its name or number) or an ability ("ability": 1-5) the player knows on its client's spell bars quietly: the first empty slot, only where no slot holds it (nothing moved or doubled), no book, sound, sparks or line. Sent once the player is in play and every revoke owed to it (nox.revoke) has gone. Whether it will be sent (false: not known, or a client below client level 10 or without the mode UI: nothing sent).
    putOnBars: (unit: number, kind: "spell" | "ability", what: string | number) -> boolean,
    --- A line about the player's character ("level 12, act 3"), which its NoX Core client keeps with the character it joined with (in its .plr) and shows next to it on its character screens. Plain text, cut to 80 bytes, control characters dropped; "" takes it away; the same line again is sent once. Whether it was sent (false: a stock client, one below client level 15 or without the mode UI: nothing sent).
    characterNote: (unit: number, text: string) -> boolean,
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
    --- Queues a spell (0x11: 0-136), ability (0x12: 0-5) or field guide creature (0x13: 0-40) taken off a player's client; a number past the game's tables is an error.
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
    --- The host's team options as they stand (gameplay flags 1, 2, 4); mode.json's "gameplay" pins them.
    gameplay: () -> { teamDamage: boolean, autoAssign: boolean, teamPlay: boolean },
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
    --- Sends one of the game's lines by its string id (e.g. "objcoll.c:FlagNoTeam", 1-48 characters as the game sends; longer or empty is an error) to one player.
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
    --- A modifier.bin entry by name as { name, section, position, color, worth, desc, text, identify, effects }; nil when none.
    modifier: (name: string) -> NoxModifier?,
    --- A type's (name or number) base stats: its name, most durability and charges, weapon or armour record, damage type, classes; nil for no such type.
    itemStats: (thingType: string | number) -> NoxItemStats?,
    --- Every modifier's name in a section ("effectiveness", "material" or "enchantment"), in modifier.bin's order.
    modifiers: (section: string) -> { string },
    --- Whether a modifier fits a slot ("effectiveness", "material", "primary", "secondary") of an item type: the game's reward-table rule, and the grade below its rows its own drops give (WeaponPower1, ArmorQuality1, Material1-2) where their lists allow it (never on a bow, a crossbow or a wand staff for a power or material).
    modifierFits: (thingType: string, slot: string, name: string) -> boolean,
    --- Puts on an item the player's unit (or a creature) carries; true, or false and why.
    equip: (unit: number, item: number) -> (boolean, string?),
    --- Takes off an item the player's unit (or a creature) wears; whether it came off.
    unequip: (unit: number, item: number) -> boolean,
    --- The item the player's client holds as its secondary weapon; nil for none, one no longer carried or one in hand now.
    secondaryWeapon: (unit: number) -> number?,
    --- The game's shop repair price for an item (durability and a staff's charges; a stock keeper's); nil when it needs none.
    repairCost: (item: number) -> number?,
    --- The game's shop repair without gold: durability and a staff's charges to their most, its holder's client told; whether it needed one.
    repair: (item: number) -> boolean,
    --- Sets an item's durability, 1 to its most (more is its most), its holder's client told; false for a thing without durability.
    setDurability: (item: number, durability: number) -> boolean,
    --- Sets an existing item's modifiers by slot (a name, false for none; a slot left out stays), the clients that know it told; an arrow or bolt in flight takes any (its hits read them). true, or false and why (a thing that takes none, one worn, a set that doesn't fit: nox.modifierFits).
    setItemSpec: (item: number, spec: { effectiveness: (string | boolean)?, material: (string | boolean)?, primary: (string | boolean)?, secondary: (string | boolean)? }) -> (boolean, string?),
    --- A thing type's (name or number) worth for this map's world (shop prices, repair costs, a quest's sale) in place of thing.bin's; nil gives thing.bin's back. Nothing is sent.
    setWorth: (thingType: string | number, worth: number?) -> (),
    --- The item types (at most 64) a player can't drop or sell in this map's world in place of the game's own list (the quest items: BookOfOblivion, the Oblivion staffs, the keys...; its two Lich orb keys can be dropped, never sold); nil gives the game's back.
    setKeptItems: (types: { string | number }?) -> (),
    --- Sets a thing's pickup by name: "DefaultPickup", "WeaponPickup" or "ArmorPickup" (an Oblivion staff's without its own line, OblivionUp and wear); nil gives it its type's own back.
    setPickup: (id: number, name: string?) -> (),
    --- # Player levels
    --- Sets a player's level (1-10), its numbers worked out again.
    setLevel: (unit: number, level: number) -> (),
    --- Sets a player's experience, its client told.
    setExperience: (unit: number, experience: number) -> (),
    --- Gives a player experience as a kill does (levels gained with it).
    giveExperience: (unit: number, amount: number) -> (),
    --- # Events and timers
    --- The event bits for nox.watchEvents: death, destroyed, damage, damaged, drop, equip, use (onUse), pickup (onTryPickup).
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
    --- The mode's records on a host that keeps them (rows of at most 16 named numbers or texts, stamped with the time they were put; nil when this host keeps none).
    records: NoxRecords?,
    --- # The map's script (NoX Core maps a mod ships)
    --- Runs fn(call) for the map script's LuauCall(name, ...) and LuauCallReturn(name, ...), at once inside the map's call (what fn returns, a number, true or false, or nox.mapObject's, is LuauCallReturn's result; else 0); nil takes it away. Register at the script's top level: a reload's code registers its own.
    onMapCall: (name: string, fn: ((call: NoxMapCall) -> any)?) -> (),
    --- Calls the map script's own function by name with whole numbers, the first for its first parameter (true and false: 1 and 0; those left out 0); what it returns as an int, nil when it returns nothing. While the map's script runs (inside a LuauCall's handler, or a hook the engine calls from inside one of its builtins: onKill for a kill its Damage made) it runs nested with that call's caller and trigger (8 LuauCalls deep at most); else as a timer's, with none. An error for a name the map hasn't, too many values, or inside an asked hook.
    callMapFunction: (name: string, ...(number | boolean)) -> number?,
    --- Calls the map script's function (its name, or its value as the script holds a function: its index) as the engine calls an object's script handler: self (GetTrigger) and other (GetCaller) the things given (ids; nil: none), its parameters 0; what it returns as an int, nil when it returns nothing. Nested as callMapFunction's while the map's script runs (that call's own self and other back after it). An error for a function the map hasn't, a thing that's gone, or inside an asked hook.
    callMapHandler: (fn: string | number, self: number?, other: number?) -> number?,
    --- A thing as the map script names it (its script id, what Object("name") gives there): for a LuauCallReturn's result or a map function's value; nil when it's gone.
    mapObject: (id: number) -> number?,
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
--- @hook onDamage(target, source, weapon, amount, kind, cause) | Before a watched thing takes a hit (cause: "blow", "missile", "spell" or "other"): return a new amount (0 cancels) or nil, and optionally a second number, the amount its worn armour, raised shield or blocking weapon wear by instead (the hit before the mode's own scaling; nil: by the hit as it lands); changes nothing in the world.
--- @hook onReflect(target, missile, front) | A missile about to hit a watched thing (front: met at its front): return true to turn it as a reflective shield does (back, the target's now) instead of the hit; changes nothing in the world.
--- @hook onStrike(weapon, victim, attacker, modifiers) | A weapon strikes (weapon: the swung one, a thrown chakram, a shot arrow or bolt; attacker nil: none), at the moment its enchantments' pre-hit effects would run (past their checks: not a friend in a game without friendly hits, not one invulnerable or behind a reflective shield), before them and the blow's own damage, at once: the world may change (nox.damage, nox.effect, nox.sound...). modifiers: the weapon's by slot ({ effectiveness, material, primary, secondary }, names; a slot without one left out).
--- @hook onDamaged(target, amount) | A watched thing took a hit.
--- @hook onPickup(taker, item, x, y) | Something picked an item up from (x, y).
--- @hook onDrop(holder, item) | A holder dropped a watched item.
--- @hook onEquip(unit, item, worn) | A watched item was put on (true) or taken off.
--- @hook onUse(user, item) | An item with nox.events.use used (a held item's use, a book read as it's picked up, food a creature eats), at once, in place of its own USE: return true to have that run as well, after.
--- @hook onTryPickup(taker, item) | An item with nox.events.pickup about to be picked up (past the game's checks), at once, in place of its own pickup: return true to have it picked up as the game would; else it stays where it is.
--- @hook onSpellCast(caster, spell, target, x, y, made) | A spell was cast (its name; target nil: none; made: what was made while it went off, its missiles or creature and anything its kills made at once, not what comes on later frames, oldest first, nil: none; normally heard before its missiles first fly, but after a host stall they may have flown a frame at their type's size).
--- @hook onExit(exit, player) | A player touched an exit: return false to hold them; changes nothing in the world.
--- @hook castCost(caster, spell, cost, how) | A player's spell's price before it's paid (cost: the game's mana; how "cast", or "trap": into a glyph's trap, at its check and as it's paid): return mana, health (health a plain loss, never to below 1; short of either: refused, as short of mana; given back where the game gives a cast's price back) or nil for the game's; changes nothing in the world.
--- @hook onAbility(unit, ability, info) | A warrior's stock ability ("BerserkerCharge", "WarCry", "Harpoon", "TreadLightly", "EyeOfTheWolf"): info { event = "start" (accepted) | "end" | "hooked" (a harpoon's hold: its pull runs from the next frame till the end), reason (an end's: "time", "wall", "hit" (a charge into a creature; a harpoon's pull done), "broken", "missed", "walk", "cancel" (a reset; a charge's kill readying it), "death", "other"; one end a harpoon's throw), target (what a charge hit, what a harpoon holds), x, y (the warrior's place then) }; told the next frame's start.
--- @hook onModAbility(unit, id, x, y, phase) | A player used a granted ability (x, y: its cursor's world point; phase "press", or for a "hold" one "release" when it's let go and "end" when its hold ran out, x and y nil then): return false to refuse a press (no cooldown starts); a press is never heard during its cooldown or while the player is busy.
--- @hook onLand(unit, x, y, cut) | A leap (nox.leap) came down at (x, y) (cut: a wall or ground no one stands on stopped it short); none for one that died or went mid-leap.
--- @hook onGenerated(tag, ok, result) | A map nox.generate built ahead is written (ok), or failed (result: why).
