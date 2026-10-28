# Bugs in the original game (GAME.EXE 1.2b)

NoX Core (the noxcore library and the noxsdl client) is a modern client for Nox 1.2b, written from
scratch from our own analysis of how the game behaves; no code from the original game is
included. It is not affiliated with or endorsed by Electronic Arts or Westwood Studios, and it
needs your own copy of the game.

NoX Core aims to play as the original does. The bugs bad actors can use (crashes, cheats,
exploits against other players or the host) are fixed; the bugs and quirks that have become part
of how the game is played are kept as GAME.EXE has them. None is changed in the game's logic
without a deliberate decision, and the list is reviewed as a whole. This file lists the bugs of
the original found while studying it, fixed or kept. Each entry says where the bug is in the original, what it
does, and where NoX Core keeps it ("Ours"). Where NoX Core can't or won't reproduce a bug (a crash,
a hang, memory that isn't ours to read or write), "Ours" says what it does instead; the deliberate
differences from the original are in [vanilla_deviations.md](vanilla_deviations.md).

## Conventions for entries

- Each entry heading has a stable id, `OB-nnn`. A new entry takes the next unused number,
  wherever it goes in the file; ids are never renumbered or reused. Other documents and source
  comments cite entries by id.
- Each entry has **Where** (the original's code and what it does wrong), **Effect** (what a player
  or host sees) and **Ours** (where NoX Core keeps it, or what it does instead and why).
- Name the original's code by logical name only: the game's own function name
  (`SrvNet_OnPacket`, `Spell_CastMagicMissile`), an OpenNox name without its prefix and address
  (`plrReadVals`), or a few words ("the host's ping test"). No addresses or offsets.
- Say where our version lives (paths under `noxcore/` and `noxsdl/`, or class and function
  names), and refer only to public material: the source and these two documents.

## Transport

### OB-001: An ack that jumps past 0x20 across the sequence wrap leaves packets queued for good
- **Where:** the reliable stream's ack handler, mode 1 (a changed ack drops the queued packets
  before it): with the ack under 0x20 or over 0xE0 it compares signed, else unsigned
  (`seq < ack`). An ack that moves from near 0xFF to 0x20 or past it in one step (the peer took
  many packets before acking, e.g. while it loaded a map) compares unsigned: the packets
  0xE0-0xFF it just acknowledged count as after it and stay queued.
- **Effect:** those stale packets are resent for a whole lap of the sequence; when the peer next
  expects their numbers it takes the stale ones (old messages again) and drops the real ones,
  which the next ack then removes unsent: their messages are lost (seen: a map's UseMap, the
  client never loading the map). Needs more than ~32 packets in flight across the wrap; the exe's
  game sends few reliable packets (one netlist a frame), so it rarely gets there.
- **Ours:** Stream::applyAck compares the same; our server packs a frame's reliable messages into
  one packet (vanilla_deviations.md, VD-02 "Reliable messages packed per frame"), which keeps it
  out of reach as the exe's netlist does.

### OB-002: The join's ping test says JOIN_OK after refusing
- **Where:** the host's ping test, a join's ten pings done: too many lost, or the average under
  the host's lowest ping or over its highest, sends SERVER_ERROR (13 00 / 13 01), then falls
  through to sending JOIN_OK (0x14) all the same.
- **Effect:** none that shows: the client took the refusal first and ignores the 0x14.
- **Ours:** the same (ServerTransport::pollPingTests).

### OB-003: The join's ping test divides by no answers
- **Where:** the host's ping test: the average is the answered pings' times (those over 0) over
  their count (IDIV); with every ping answered in 0 ms (a LAN's, timeGetTime's resolution) the
  count is 0: a divide by zero, the host crashed. Only with a ping limit set (Access: Min / Max
  Ping).
- **Ours:** an average of 0 there instead (not the crash).

### OB-004: The ping test's later pings carry stack bytes as their header
- **Where:** the ping sender writes only bytes 2-7 of a SERVER_PING (0x10, its number, the time).
  The first (the lobby handler's case 0x0E / 0x11) has its 00 <the TRY_JOIN's seq> set there;
  the next ones from case 0x12 go out in the buffer the server's receive function filled with
  the sender's sockaddr (02 00: AF_INET), and one after a lost ping in a stack buffer never set.
- **Effect:** none: the client echoes the two bytes in its SERVER_PONG and the server's receive
  function takes 0x0E-0x14 by the op alone.
- **Ours:** every ping 00 <the TRY_JOIN's seq> (stack bytes not reproduced); the lobby takes a
  pong whatever its header.

### OB-225: A new client's latency isn't reset: it starts with the last one's on its stream
- **Where:** the connection handler keeps each stream's latency record (its last CLIENT_PING's time,
  five round trips, their average). A connect and a client's close each clear a record, but the one
  of the stream the packet came in on (the server's listening stream), not the new or closing
  client's.
- **Effect:** a player joining on a stream someone left reports that player's latency on the
  scoreboard till its own first five round trips (about ten seconds) are in, and its first average
  mixes in the old samples left in the ring.
- **Ours:** the same (ServerTransport::LatencyRecord, by stream id, never reset).

### OB-226: The latency report's search never ends with no host's player and no latencies
- **Where:** SrvNet_BroadcastNextPlayerLatency (every 500 ms) walks the players round and round for
  the next one that is the host's own (player 0x1F) or has a latency over 0, with no end if there
  is none.
- **Effect:** only without a player 0x1F (a dedicated server): with every player's latency 0 (as
  each is till its first five round trips are in) the server hangs.
- **Ours:** gives up after one lap and reports nothing that time (GameHost::reportNextLatency).
  NoX Core deviates here, by an approved decision (vanilla_deviations.md, VD-36): a hang is not
  kept.

## Server

### OB-005: The lasting spells' walk goes on through records a cast in it freed
- **Where:** spellCastByPlayer walks the lasting spells' list with each record's next taken before
  its update. An update that damages (Lightning, Chain Lightning, Plasma) runs a generator's
  onHurt script there and then; a CastSpell* in it starts a lasting spell, whose start frees
  every cancelled record first: the next one the walk holds, and the one being updated when the
  cast cancelled it (the same spell by the same caster).
- **Effect:** the walk reads, writes and cancels freed records (whatever the pool put there since).
  Needs a map script that casts a lasting spell from a generator's onHurt; no stock map's is
  known to.
- **Ours:** a memory fix, not a logic change: the record being updated, ended meanwhile, stays in
  the list (its end run) till its update returns, and the next isn't held across the update
  (Spells::updateLasting, DurationSpell::walked / finished; 2026-10-16).

### OB-006: A summon refused for lack of room still sounds as if it began
- **Where:** a lasting spell's start (the summons' 0x4B-0x72 among them) plays the spell's cast
  sound (spellGetAud44) before running its start; Spell_SummonStart then refuses past the
  creature limit (checkSummonedCreaturesLimit) with only Summon.c:CreatureControlFailed. No
  NoCanDo / 0xE7 (the instant spells' failure sound in spellAccept) is played for it.
- **Effect:** casting a summon with the cage full sounds like a summon starting (noticed in play,
  2026-10-07).
- **Ours:** the same (spells.cpp's summon start).

### OB-007: Picking things up has no reach check
- **Where:** SrvNet_OnPacket's MSG_TRY_GET (0x73) calls the pickup with no distance check.
- **Effect:** a modified client can pick up items anywhere on the map.
- **Ours:** game_host.cpp's TryGet, the same.

### OB-008: A gold pile made with no players about divides 0 by 0
- **Where:** GoldInit averages the players' experience (read from each one's unit) as
  sum / count with no check for count 0.
- **Effect:** with no players the mean is NaN; each __ftol of it gives 0 (the low half of the
  64-bit integer indefinite), so the amount is randInt(0, 0) - 0 + randInt(15, 30): 15-30 by
  luck, not by design (the first randInt still takes a random draw).
- **Ours:** world.cpp's applyInit gives the same 15-30, with the same two draws.

### OB-009: Working a lever or button has no reach check
- **Where:** SrvNet_OnPacket's MSG_TRY_COLLIDE (0x7B) calls the object's COLLIDE with the
  player's unit, checking only that the player isn't an observer or in a dialog or trade.
- **Effect:** a modified client can throw levers, press buttons and pull chains anywhere on the
  map (the stock client sends it only within 75).
- **Ours:** game_host.cpp's TryCollide, the same.

### OB-010: A trigger's once-only deciding handler, after its run
- **Where:** TriggerCollide reads the result of scriptCallByEventBlock for its touch handler
  without a null check; that returns NULL for a handler flagged to run once (2) that has run (1).
- **Effect:** the next touch of such a trigger reads through NULL: the server crashes.
- **Ours:** collide.cpp's triggerCollide can't follow it there; the touch doesn't count (as a
  handler returning 0 would).

### OB-011: Magic Missile with more of your own missiles flying than the spell allows
- **Where:** Spell_CastMagicMissile fires MagicMissileCount less those of yours still flying;
  only exactly none left gives "mmissile.c:TooManyMissiles".
- **Effect:** below zero (more flying than the count: possible after the count drops, e.g. a
  lower level), nothing is fired, yet the spell counts as cast: the mana is taken and the sound
  plays, with no message. A player's cast never gets there: the can't-cast check
  (checkPlrCantCastSpell) refuses with error 3 once its own missiles reach the count; only other
  casters (wands, scripts, monsters) could.
- **Ours:** spells.cpp's kMagicMissile, the same.

### OB-012: Magic Missile's sixth missile
- **Where:** Spell_CastMagicMissile's spread is a switch with five cases (0, -16, +16, -32, +32
  from the facing) inside a loop with no cap.
- **Effect:** with six missiles (a quest game at level 5: the ARENA table's 6), the sixth has no
  case and keeps the fifth's heading, so two fly together.
- **Ours:** spells.cpp's kMagicMissile, the same.

### OB-013: Shock's discharge on a blow ignores its power
- **Where:** DefaultDamage takes the enchantment off, then hurts the striker by `ShockDamage[4]`
  (a constant 4): the fifth value whatever its power. The touch discharge (the player's collide
  handler) reads the power first and uses `ShockDamage[power - 1]`.
- **Effect:** a level-1 Shock does level-5 damage (75 by the shipped table) to a melee striker.
  (In the arena every spell is level 3, so there it's 75 instead of 45.)
- **Ours:** combat.cpp's defaultDamage, the same.

### OB-014: Punching doesn't discharge Shock
- **Where:** DefaultDamage's discharge needs the hit's weapon object (obj3) non-null, and a
  bare-handed punch passes none (its hit record's weapon left empty), though elsewhere the game
  has a case for "no weapon: a player's hand".
- **Effect:** only a weapon blow (or touching, through the collide) sets off a Shock; a punch
  never does.
- **Ours:** combat.cpp's defaultDamage, the same.

### OB-015: Cancel, Detonate and Phantom do nothing
- **Where:** the dispatcher sends Cancel (6), Detonate (0x12) and Phantom (0x39) each to a
  function of its own that only returns 1.
- **Effect:** cast, mana spent, "spell OK", and nothing happens.
- **Ours:** spells.cpp's cases 6 / 0x12 / 0x39, the same.

### OB-016: Protection from Magic does nothing
- **Where:** the dispatcher (spellAccept) sends spell 0x3F to a function that only returns 1.
- **Effect:** the spell is cast (mana spent, "spell OK"), but no enchantment is put on and
  nothing changes; ENCHANT_PROTECT_FROM_MAGIC is never given by it.
- **Ours:** spells.cpp's case 0x3F, the same.

### OB-017: Wink is Restore Health
- **Where:** the dispatcher sends both 0x44 (Restore Health) and 0x85 (Wink) to
  castSpellWinkORrestoreHealth: the target's health to full.
- **Effect:** Wink heals fully instead of whatever it was meant to do (it's not a player spell
  in the shipped spell book, so mostly scripts and wands would reach it).
- **Ours:** spells.cpp's cases 0x44 / 0x85, the same.

### OB-018: The harpoon's rope messages with a missing end
- **Where:** the rope-on broadcast (MSG_FX_DURATION_SPELL 7) and SrvNet_BroadcastHarpoonBreak
  (off: 14) fill the 7-byte message only when both the warrior and its bolt are there, but send
  it either way.
- **Effect:** with either missing, every client gets a message of whatever the stack held
  (often a stray effect or nothing).
- **Ours:** sends nothing in that case (an uninitialised buffer can't be reproduced); listed
  here in case a client relied on it.

### OB-019: A hostile spell with the cursor on a friend
- **Where:** Plr_FinishSpellCast, out of a quest: a hostile spell (flag 0x20) with a non-enemy
  under the cursor returns at once.
- **Effect:** nothing happens: no mana, no "can't" message, no spell report, and the caster is
  left in its casting state (2) until something else changes it.
- **Ours:** spells.cpp's finish, the same.

### OB-020: Chain Lightning's bolts carry a stray power byte
- **Where:** lightningSpellDuration makes each bolt record (spell 7) from the duration-spell pool
  without clearing it, and sets only its spell, ends and links; SrvNet_BroadcastStartRaySpell /
  StopRaySpell send the record's power as the beam's byte.
- **Effect:** the byte is whatever the pool slot last held (some older spell's power), so the
  clients get an arbitrary 0-5 for the bolt.
- **Ours:** sends the chain's own power (the old value can't be reproduced).

### OB-021: A five-spell list reads past its end
- **Where:** Spell_UpdatePendingCasts takes the next spell as the dword after the current one in
  the magicEnt's spell list. After the fifth (index 4) that's the dword past the list: the index
  and trap-flag bytes, never 0.
- **Effect:** a list of five where the fifth isn't Glyph goes on to "speak" index 5 (spell 5 or
  0x105, read from those bytes), then index 6 (the phoneme-tree pointer), and so on through the
  record. The client never sends five (one spell, or at most three and Glyph); only a modified
  one could.
- **Ours:** spells.cpp's update stops at the fifth (the garbage can't be reproduced).

### OB-022: A trap's mana is lost when its glyph isn't laid
- **Where:** Spell_UpdatePendingCasts takes each trap spell's mana as it's spoken (at the trap
  prices); once Glyph is cast, worked or not, it clears TrapSpellsCnt. Plr_FinishSpellCast gives
  back only the Glyph's own cost (0) when Spell_CastGlyph fails (MaxTrapCount reached meanwhile,
  a conjurer at MaxBomberCount or its summon limit).
- **Effect:** the trap's spells are paid for and gone, with no glyph.
- **Ours:** spells.cpp's update and glyphs.cpp's castGlyph, the same.

### OB-023: A glyph's Fireball, Burn, Fist and Cleansing Flame lose their owner
- **Where:** a glyph going off casts its spells with the glyph as their source; Fireball, Burn,
  Fist of Vengeance and Cleansing Flame make their object with createAt(obj, source): owned by
  the glyph, which is deleted the same moment. At the frame's end the glyph is freed and
  unitRemoveChild leaves what it owned with no owner. (Meteor, Meteor Shower, Toxic Cloud, Magic
  Missile and Pixie Swarm are owned by the caster and keep it.)
- **Effect:** after the first frame those four's harm comes from no one: whatever looks up their
  owner (the kill credit, the owner's side) finds none, so the wizard isn't credited.
- **Ours:** the same (their owner the glyph's extent, gone with it).

### OB-024: A map glyph with more than five spells
- **Where:** GlyphXfer reads a u8 count and that many spell names straight into the glyph's
  InitData, which holds five (then the count, the target, the point).
- **Effect:** a sixth spell overwrites the count, a seventh the target, and so on (the editor
  doesn't make such maps).
- **Ours:** world.cpp keeps the first five and the count at most 5.

### OB-025: A flying spell cast into a wall
- **Where:** castSpellByUser sends a spell of flag 4 at another to createSpellFly and returns 1
  whatever it does; the projectile isn't made when a wall (ray flags 5) stands within the
  caster's radius + 4 ahead.
- **Effect:** facing a wall, the spell is "cast" (mana spent, MSG_INFORM 1) and nothing flies.
- **Ours:** spells.cpp's flySpell / finish, the same.

### OB-026: A spell projectile's point
- **Where:** SpellProjectileCollide casts its spell with a one-word argument (the target, a
  local) where the spells read a target and a point after it.
- **Effect:** the point is whatever follows on the stack; the flying spells (enchantments, the
  teleports, Swap, Tag, Telekinesis, Moonglow, Firewalk) don't seem to use it.
- **Ours:** the target's place (the garbage can't be reproduced).

### OB-027: One "door locked" notice for the whole server
- **Where:** DoorCollide keeps the time of the last "locked" notice in one global and sends none
  within 1501 ms of it.
- **Effect:** two players at different locked doors within a second and a half: only the first
  is told (and hears the rattle); the other gets nothing.
- **Ours:** collide.cpp's doorCollide, the same (World::lockNoticeAt, in frames).

### OB-028: Moonglow reads a duration gamedata hasn't
- **Where:** spellCreateMoonglow reads "MoonglowEnchantmentDuration"; gamedata.bin has
  "MoonglowEnchantDuration" (9000). The missing name reads 0, and a 0-frame enchantment lasts
  for good.
- **Effect:** a player's MOONGLOW stays until its Moonglow object goes (MoonglowUpdate: 300 s, or
  its owner destroyed) or the spell is cancelled; on anything that isn't a player the start puts
  LIGHT on and fails at once, so the spell's end takes it off again: Moonglow on a monster does
  nothing.
- **Ours:** the same.

### OB-029: Cleansing Flame at power 5 reads past its colour
- **Where:** Spell_CastCleansingFlame picks each flame from a 10-entry table, index size (power,
  or power - 1) + 5 for the blue (mana) one. Outside a solo game the size is always 4; in a solo
  game it's the spell's power.
- **Effect:** at power 5 a red ring's size-5 flames are the table's sixth entry, SmallBlueFlameCleanse
  (blue ones in a red ring); a blue ring's are the eleventh, past the table (whatever thing type the
  next global holds).
- **Ours:** clamps the index to the table (the last, LargeBlueFlameCleanse) for the blue case; the
  red one does as the game does.

### OB-030: Steam pipes pointing diagonally never blow (all four stock pipes)
- **Where:** the push of BlowUpdate. Its angle test for a pipe's diagonal ways wants |dy| / |dx|
  at least 0.5773 and at most 0.1732 at once; for north and south it wants at most 0.3732. The
  numbers look like tan 60 and tan 75 with the decimal point one place off (1.732, 3.732).
- **Effect:** SteamPipe1-4 (DirectionInit: all diagonal) push nothing at all; a pipe turned
  north or south would push only what's nearly level with it, not what's in front.
- **Ours:** objects.cpp's blowUpdate, the same.

### OB-031: A mover on an axis-aligned path never sees it passed its waypoint
- **Where:** MoverUpdate checks for having passed its waypoint only when both parts of its
  velocity are non-zero.
- **Effect:** heading exactly along x or y (a waypoint level with it), it never takes the next
  waypoint: it creeps up to this one and stays there, its load with it.
- **Ours:** objects.cpp's moverUpdate, the same.

### OB-032: A mover at a fork whose every link leads back hangs the server
- **Where:** the same function picks a random link that isn't the waypoint it came from, in a
  loop with no way out.
- **Effect:** a waypoint with two or more links that all lead back where the mover came from
  loops forever.
- **Ours:** the same.

### OB-033: A flagball game tied at its time limit crashes the server
- **Where:** Game_OnTimeLimitExpired's flagball check calls SrvNet_BroadcastFlagBallWinner(NULL)
  when no team is ahead; that reads the winner's id through the null.
- **Effect:** a flagball match that runs out of time with the best teams level (or no teams)
  crashes the host. (CTF sends 0xFFFF for "no winner" instead.)
- **Ours:** modes.cpp's checkTimeLimit sends team 0 there.

### OB-034: Fumble on a flagball carrier doesn't push the ball or tell anyone
- **Where:** castFumble pushes the carrier's ball (force 100) and clears its owner, but the
  carried ball is NO_COLLIDE, which objectApplyForce skips; and it sends no
  MSG_REPORT_BALL_STATUS.
- **Effect:** the ball drops where it is, loose, while every client still shows it carried by
  the fumbled player until someone else takes it.
- **Ours:** spells.cpp's Fumble, the same.

### OB-035: King of the Realm brings the map's flags back 30 s in
- **Where:** mapInfoSetKotr deletes the map's flags (delayedDeleteObject) but, unlike the crowns
  it deletes, doesn't take them off the respawn list; the respawn pass sees them gone and makes
  new ones.
- **Effect:** (from the code; not seen live) a KotR game on a map with flags gets them back at
  their spots 30 s after the start.
- **Ours:** modes.cpp's setupMode and respawnPass, the same.

### OB-036: Team elimination counts every team as in play when anyone is
- **Where:** the players-in-play count, in team play, counts a team for each team once any
  player at all is in play, without looking at who is in which team.
- **Effect:** where it's used with teams the count is the number of teams, not of teams with
  players left (the sudden death checks use the team count and countNonEliminatedPlayersInTeam
  instead, so it barely shows).
- **Ours:** modes.cpp's playersInPlay, the same.

### OB-037: Any client can put anyone in any team
- **Where:** SrvNet_OnPacket 0xC4 subs 10 (join) and 11 (switch) take a team and a net code
  from the client and act on whatever the code names, unchecked.
- **Effect:** a modified client can move other players (or monsters) between teams, and
  (switching, outside chat) back to a player start.
- **Ours:** game_host.cpp, the same.

### OB-038: An NPC's blow takes its stamina the wrong way round
- **Where:** mobActionMelee1, for an NPC (subclass 0x10): when the weapon's stamina
  (weaponGetStaminaByType) is more than it has, it's taken off (the byte wrapping below 0); when
  it has enough, its stamina is set to 0.
- **Effect:** an NPC with the stamina for a blow is left with none; one short of it is left
  with a great deal (256 less the shortfall).
- **Ours:** monsters.cpp's MELEE_ATTACK start, the same.

### OB-039: A monster hurt by nothing it knows roams for no time
- **Where:** a monster answering a blow with no creature behind it pushes DEPENDENCY_TIME with
  gameFPS x 5 (150) as its argument, where TIME holds while its argument (a frame) is still to
  come.
- **Effect:** the TIME condition has long passed, so the ROAM over it is popped the frame it's
  pushed (the "look about for 5 s" never happens).
- **Ours:** monsters.cpp's reactToHurt, the same.

### OB-040: A tile's wall check never tests the row's range
- **Where:** the path finder's wall-in-a-tile check tests `y < 0 && y > 255`, which is never
  true, so a row off the grid is looked up as it is.
- **Effect:** none seen (the wall lookup finds nothing there).
- **Ours:** pathfind.cpp's wallBlocking, the same.

### OB-041: A waypoint route over 16 writes a 17th
- **Where:** BuildWaypointPath copies up to `most` + 1 waypoints into the monster's 16-waypoint
  route, the 17th over the monster's waypoint index.
- **Effect:** none (the index is set to 0 just after); the route is cut to 16 ("too long").
- **Ours:** pathfind.cpp's waypointRoute stops at 16 with the same status.

### OB-042: Any client can order any creature
- **Where:** SrvNet_OnPacket's MSG_TRY_CREATURE_COMMAND (0x78) hands the creature named by the
  client's net code to AI_OrderCreatures, and enactUnitOrder checks the owner only for a banishing
  (order 0).
- **Effect:** a modified client can set anyone's creatures, or a map's monsters, idle, guarding,
  escorting it or hunting, and watch any of them (order 1: the camera on a monster anywhere,
  playerObserveMonster).
- **Ours:** game_host.cpp's 0x78 and monsters.cpp's orderCreatures, the same.

### OB-043: A monster drinks a health potion twice
- **Where:** mobActionPickupObject (and the main AI's eating, monsterMainAIFn) calls
  Obj_TryPickup, whose PotionPickup drinks a health potion that won't overfill and deletes it,
  then Obj_UseItem on the same (deleted, still in memory) potion when it's a HEALTH_POTION (0x10;
  the main AI: 0x90 with MUSHROOM).
- **Effect:** a monster still short of health after the first drink gets the potion's health
  twice (PotionUse heals again and deletes it again).
- **Ours:** monsters.cpp's PICKUP_OBJECT and think, the same.

### OB-044: The bomber's glyph from a map copies the wrong spells when the first is empty
- **Where:** MonsterXfer counts the bomber's set spells (Field511-513) and copies that many from
  the first slot, not the set ones.
- **Effect:** a map bomber with its first spell slot empty carries a glyph with a 0 spell in
  place of a set one (the generator's copies all three).
- **Ours:** monsters.cpp's bomberGlyph (allThree false for a map's), the same.

### OB-045: A solo warrior's level up repeats its abilities
- **Where:** Plr_CheckLevelUp calls plrReadVals, which for a solo game's warrior grants every
  ability up to its level again (abilGivePlayerAll -> Abil_GrantToPlayer); each one already known
  answers "use.c:HadAbility".
- **Effect:** each level up past the second sends the warrior one "HadAbility" notice per ability
  it already has (unchecked in play: the notices may be hidden by the client).
- **Ours:** player.cpp's readVals / grantAbility, the same.

### OB-046: A quest warrior's ability slots clear the spell book by level
- **Where:** abilGivePlayerAll in a quest writes 0 to the spell book's slot of the level index,
  not the ability's.
- **Effect:** the level slots holding an ability (1-5) are the ability slots themselves, so it
  takes the warrior's abilities away; reached only while a quest's setup flags are set (readVals
  skips the grant in a quest otherwise).
- **Ours:** player.cpp's giveLevelAbilities, the same.

### OB-047: The console's respawn switches can't be turned off
- **Where:** the console's "set monsters respawn on|off" and "set weapon respawn on|off" set the
  server option bit (8, 2) for both "on" and "off"; only the printed line differs.
- **Effect:** once on, monster and weapon respawning stay on whatever the host types.
- **Ours:** host_console.cpp, the same.

### OB-048: Remote sysop commands answer on the host's screen
- **Where:** SrvNet_OnServerCmd's case 2 runs the sysop's line through server_parseCmdText, whose
  lines go to the host's console (Gui_Console_Printf).
- **Effect:** a sysop typing a command sees nothing back (the host's screen gets "RemoteSysop" and
  the result).
- **Ours:** the same (GameHost::takeConsole); with the server's test commands on, the lines are
  also sent to the sysop as text messages, for testing.

### OB-049: A point on a polygon's corner-to-corner line can read as outside
- **Where:** the point-in-polygon test (polygon) counts the crossings of a ray from the point to
  one of (0, 0), (0, 0x1700), (0x1700, 0x1700), (0x1700, 0) (a counter's bit 2 before and after
  each test), the edges' ends inclusive.
- **Effect:** a ray through a corner meets both edges there and counts two, so a point lined up
  with corners (a square's diagonal) is outside for that test; the next test may say inside.
- **Ours:** World::polygonAt, the same.
- **Also the client's ambient light** (polygonDrawColor tests the view's centre with it): a view
  that stops on such a line inside a polygon takes the map's ambient instead of the polygon's, and
  keeps it until the view moves (it tests again only then). noxsdl's PolygonAmbient, the same.

### OB-050: Blue flames hiss every frame after the 18th minute
- **Where:** ManaDrainCollide plays 0xE4 when gameFrame - (short)Field542 > gameFPS / 2, then
  stores the frame in Field542, a u16.
- **Effect:** from frame 0x8000 (about 18 minutes at 30 frames a second) the stored frame no longer
  matches the frame (negative, then wrapped), the difference is always large, and a player standing
  in blue flames hears the hiss every frame instead of twice a second, for the rest of the game.
- **Ours:** the same (ServerObject::soundFrame, the ManaDrainCollide in collide.cpp).

### OB-051: A blow on a player sounds like the armour in its pack
- **Where:** PlayerDamageSound goes through the whole inventory for body armour (class 0x2000000,
  subclass 4) and takes the highest material (0x2000 for a Material7 piece); it never asks whether
  the piece is worn.
- **Effect:** a player carrying a breastplate it isn't wearing is hit with that metal's sound.
- **Ours:** the same (damageSound in sound.cpp).

### OB-052: Generic armour breaks without a sound
- **Where:** ArmorDie picks the breaking sound by material; for armour that's neither metal, wood,
  leather nor cloth it leaves the sound variable holding the position pointer and passes that to
  audCreate as the sound's number.
- **Effect:** no sound (an id past the table plays nothing); the message still goes.
- **Ours:** no sound for it either (Items::breakNotice).

### OB-053: ProjectileSpeed on anything but a bow writes past the blow's record
- **Where:** effectProjectileSpeed multiplies the current speed (speedCur, a float) of its fifth
  argument, meant for the arrow shootApplyEffects passes. As an ATTACKEFFECT,
  itemApplyAttackEffect passes it the blow's record on its caller's stack (playerAttack's,
  ChakramInMotionCollide's, ArrowCollide's).
- **Effect:** a sword, staff, chakram or quiver with a ProjectileSpeed enchantment scales a float
  past the record (as far in as an object's speed lies), in the caller's frame or beyond it.
  modifier.bin allows it only on bows and crossbows, whose shots don't go through
  itemApplyAttackEffect; only a map that puts it elsewhere reaches it.
- **Ours:** nothing (attackEffects in item_effects.cpp); the bow's own is reproduced (shootEffects).

### OB-054: Speed4's DISENGAGEEFFECT has no ';'
- **Where:** modifier.bin: "DISENGAGEEFFECT = SpeedDisengage 0.2" ends without a ';', and the
  modifier reader reads a value up to the next one.
- **Effect:** the value runs on through the next line, "ALLOWED_WEAPONS = NO_WEAPONS", which is
  lost: Speed4 isn't kept off weapons. (Its number is still read: the next token, 0.2.)
- **Ours:** parseModifierDb reads it the same way (nothing here uses ALLOWED_WEAPONS yet).

### OB-055: The script's green bolt effect comes from nowhere
- **Where:** the script builtin Effect for "GREEN_BOLT" calls netSendFxGreenBolt with its local
  int array for the bolt's ends, which only the ray effects fill; on that path they're never set.
- **Effect:** the bolt's ends are whatever was on the stack: a stray bolt somewhere, or nothing
  seen.
- **Ours:** not sent at all (ScriptHost::effect).

### OB-056: Death's end counts an elimination death twice
- **Where:** updateUnitBuffs, when a player's DEATH enchantment runs out, kills it
  (unitDamageClear, whose PlayerDie scores the death) and then calls playerIncrementElimDeath and
  SrvNet_BroadcastReportLesson itself.
- **Effect:** in elimination (playerHandleElimDeath counts every death) the player loses two lives
  to it. (In an arena PlayerDie's scoring, with no killer, takes a lesson as for a suicide and
  counts no death, so the one there is the only one; in KotR, none either.)
- **Ours:** the same (stepEnchantments in enchant.cpp).

### OB-057: A script timer that cancels the next one sends the timer walk through freed timers
- **Where:** the script timers' pass frees each due timer (onto the activators' free list) and
  keeps its next before calling its function; CancelTimer frees the one it names the same way.
  When the function cancels the timer after it, the walk goes on from that freed one, whose next
  is now the free list.
- **Effect:** the walk runs on through the free list: the timer that just ran, and every timer
  freed before it, run again (their records keep their frame and function), and the free list is
  left looping, so later timers can be handed the same record.
- **Ours:** the cancelled timer simply doesn't run and the pass goes on (ScriptHost::runTimers
  looks for the next due timer afresh after each call); the exe's re-runs aren't reproduced.

### OB-058: A great sword's parry of a spell projectile reflects it twice
- **Where:** SpellProjectileCollide: the great sword's parry (state 0xD, weapon bit 0x400, in
  front: 0x37A, projectileReflect, changeOwner, a parry state 0x12-0x14) doesn't return, unlike
  the raised shield's. It falls through to the inversion and reflective-shield checks and then,
  the projectile no longer being after the parrier (changeOwner made its caster the target), to
  projectileReflect a second time.
- **Effect:** the two reflections cancel: the projectile, now the parrier's and after its caster,
  flies on into the parrier as before; its next touch (the parry state no longer 0xD) reflects it
  once and it goes back. The parry works a frame late, with 0x37A and 0x40.
- **Ours:** the same (Missiles::spellFlyHits).

### OB-059: The hand's spell controls are two codes off their table
- **Where:** the 1.2b control table (ctrlevent_code_infos: {name, has data, size} per code) has
  no gamma pair, while the stock client's actions (Cli_ProcessControlEvents) and the server's
  switch (Plr_UpdateState) number from the gestures on as if it had: the gestures are 0x14-0x1B,
  the pattern's end 0x1C, the queued spells 0x1D / 0x1E. Both ends size an entry's data by the
  table at the code sent (the client's input packing, SrvNet_DecodePlayerInputEntries).
- **Effect:** CastQueuedSpell and CastMostRecentSpell (0x1D / 0x1E) land on the table's
  CastSpell1 / 2, which carry no data: the creature the client names as their target never
  arrives, so a queued spell is never cast at one (the pattern's end, 0x1C, carries 4 bytes, the
  last two gestures a byte and 4 bytes of nothing).
- **Ours:** the same (net/player_input.cpp sizes by the table; player.cpp's switch takes the
  exe's codes).

### OB-060: A woman's hand-typed phonemes sound like a man's
- **Where:** Plr_UpdateState's gestures (0x14-0x1B) play 0xBA-0xC1 (SpellPhonemeUp...) for
  everyone, where a book's spell picks the speaker's voice (spellGetPhoneme: 0xC2-0xC9 for a
  PlayerFemale).
- **Effect:** the others hear a man's voice from a woman typing a spell by hand.
- **Ours:** the same (player.cpp's events).

### OB-061: A hand-typed spell goes at the last book spell's target
- **Where:** Plr_FinishSpellCast casts at the player's spell target, which only
  Spell_UpdatePendingCasts sets, as a book's spell ends (the caster for "on itself", else what its
  cursor was on).
- **Effect:** a spell typed by hand (the pattern's end, or half a second on) is aimed at whatever
  the last book spell was: the caster itself, a creature long since out of reach, or one that's
  gone (a pointer to a freed object).
- **Ours:** the same, by the target's extent (PlayerControl::spellTarget): one gone is none.

### OB-062: The pattern's end in the middle of a book's spell
- **Where:** Plr_UpdateState's 0x1C (and 0x1D / 0x1E) call Plr_FinishSpellCast whenever
  SpellCastStart is set, a book's spell as well (Spell_StartCast sets it), and clear it; the
  book's entry stays on the pending list.
- **Effect:** the spell at the node spoken so far is cast at once (before the book's first
  phoneme, the last spell's node: the last spell again); the book's spell then goes on and is
  cast too at its end, and with SpellCastStart clear a second book spell can begin beside it.
- **Ours:** the same, but one book spell at a time (Spells::start refuses while one's spoken).

### OB-063: BecomePet on a creature a player commands undoes itself
- **Where:** unitBecomePet sets the creature's subclass 0x80, reports it watched and marks it
  before unitSetOwner, whose unitClearOwner finds the old owner (a player that controls it) and
  takes 0x80 off again, reports it lost and unmarks it.
- **Effect:** a summoned or charmed creature made a pet (the script's BecomePet) ends unmarked and
  'lost' on its player's screen, though it's the host's; deleted later it isn't unwatched.
- **Ours:** the same (monsters.cpp's becomePet).

### OB-064: The script's BecomePet / BecomeEnemy / MakeFriendly with no host player
- **Where:** the script builtins BecomePet, BecomeEnemy and MakeFriendly read
  `Plr_GetByIndex(0x1F)->PlayerUnit` without checking the player: Plr_GetByIndex returns NULL for
  a slot that isn't active (a dedicated server has no player 0x1F). MakeFriendly sets the
  creature's subclass 0x100 first.
- **Effect:** on a server with no local player, a map script calling one of them (on an object
  that exists) reads through NULL: the server crashes.
- **Ours:** nothing happens without the host (script_host.cpp; MakeFriendly still sets 0x100).

### OB-065: MSG_SOCIAL with an unknown emote hangs the server
- **Where:** SrvNet_OnPacket's MSG_SOCIAL (0xE3): values 1, 2 and 4 call Plr_SetState and jump to
  the `ADD ESI, 2` (the message's length) before the loop's test; any other value jumps past it
  to the loop's test, with ESI still on the 0xE3.
- **Effect:** the same message is read again forever: a client sending MSG_SOCIAL 0, 3 or 5-255
  freezes the server's frame (an endless loop).
- **Ours:** the message is skipped (game_host.cpp's dispatch moves on by its two bytes; social()
  does nothing).

### OB-066: A quest's team play set and at once cleared
- **Where:** the quest's start sets gameplay flag 4 (team play), then calls Team_RemoveAll, whose
  first act is to clear flag 4; the quest's one team is made after.
- **Effect:** a quest runs with every player in one team but without team play (whatever checks
  flag 4, friendly fire and the team rules, plays it as no teams).
- **Ours:** the same (quest.cpp's startQuest).

### OB-067: Spell_GrantToPlayer's quest cap of 3 checks the granted spell, not the group's
- **Where:** Spell_GrantToPlayer: granting a group's head (spell flag 0x1000 / 0x4000 / 0x10000)
  raises each member (0x2000 / 0x8000 / 0x20000) a level, at most 5, and in a quest caps at 3 the
  granted spell's own level, not the member's.
- **Effect:** in a quest a group's members can rise past 3 (to 5) while the head stays at 3.
- **Ours:** the same (quest.cpp's grantSpell).

### OB-068: The next quest map read from before the list when none may be picked
- **Where:** getQuestMapFile counts the maps it may pick (played fewer than the most, not the last
  map or its group, not in the last 4 plays) and draws randInt(0, count - 1); with none,
  randInt(0, -1) draws nothing and returns -1, and the function returns entry -1 (the 32 bytes
  before the list).
- **Effect:** with few quest maps (two of one group, say), the next quest map is whatever text lies
  before the list.
- **Ours:** no map (QuestMaps::pick returns none; the caller keeps its map).

### OB-069: Quest rewards: weapons' enchantments drawn by a number, not a slot bit
- **Where:** rewardMakeWeapon (both enchantments) and rewardMakeArmor (the second) draw
  randInt(stage / 2 - 1, stage / 2 + 1) within 1-4 and test the enchantment table's slot bits
  against that number itself (1, 2, 3 or 4), where everywhere else a slot is one bit (1, 2, 4, 8,
  0x10).
- **Effect:** those enchantments come only from the tables' first three levels' rows (bits 1-4:
  never a level 4, slots 0x18), whatever the stage; armour's first enchantment, by the true slot,
  can be level 4.
- **Ours:** the same (rewards.cpp's enchantSlot).

### OB-070: Quest rewards: a second enchantment of the stun kind refused without a first
- **Where:** the same two functions compare the second enchantment's group with the first's by
  the first's table row, a local that starts at 0 (Stun1's row) and stays so when there was no
  first.
- **Effect:** with no first enchantment, a second of the Stun group is dropped.
- **Ours:** the same.

### OB-071: Quest rewards: the share of markers unset with no players or more than six
- **Where:** the reward markers' placement sets its share (0.5 on stage 1, else 0.4 / 0.7 / 1.0
  for 1-2 / 3-4 / 5-6 players in) only in those cases; otherwise it multiplies by an
  uninitialised stack float.
- **Effect:** with no player in the quest (or more than six) the number of reward markers placed
  and red potions kept is whatever was on the stack.
- **Ours:** a share of 0 (only the always-placed markers; every red potion gone).

### OB-072: Quest rewards: the marker made an Ankh can give its item too
- **Where:** the Ankh's placement replaces one kind-0x80 marker with an Ankh and deletes it with
  delayedDeleteObject, which leaves it on the object list till the frame's end; the reward
  markers' placement (OB-071) then may choose it and activate it.
- **Effect:** that spot can hold both the Ankh and an item.
- **Ours:** the same.

### OB-406: Quest rewards: the gems asked for by names thing.bin doesn't have
- **Where:** createGem (the reward of kinds 0x20 and 0x80, "gold or a gem") makes its gem by name
  with newObjectByTypeID: "RubyGem", "EmeraldGem" or "DiamondGem". thing.bin's gems are Ruby,
  Emerald and Diamond (the shops' stock asks for those), and the type lookup by name matches a
  name exactly, case aside, against thing.bin's types only (no aliases, no suffix taken off), so
  each call finds no type and returns no object. createGem passes that on; every caller takes no
  object as no reward.
- **Effect:** a quest reward never is a gem. Where one is drawn (10 in 100 of the gold-or-gem
  draws once the stage's slot is 4 or more: some draws from stage 3, every one from stage 6; a
  stage or two sooner in a chest, from a RewardMarkerPlus or a boss) there is no reward at all: a reward marker on the ground or in a chest leaves nothing,
  and the reward items Hecubah and the Necromancer carry are one fewer. A marker of every kind
  draws gold-or-gem 24 times in 100, so about 2 of its 100 rewards are lost that way; a gold-only
  marker loses 10. Shops (kinds 1, 2, 4, 8 and 0x10 only) aren't affected.
- **Ours:** kept as the original has it (1:1), by decision, until reviewed: makeGold asks for the
  same three names and `make` (both in `noxcore/src/server/rewards.cpp`) returns nothing for a
  type thing.bin doesn't have.

### OB-073: A quest exit with no quest maps copies from a null name
- **Where:** ExitCollide copies getQuestMapFile's pick into the next map's name with an inline
  strcpy; with no quest maps in the list the pick is null.
- **Effect:** the server crashes when a player uses a quest exit or warp gate with no quest maps.
- **Ours:** nothing kept, no map loaded (quest.cpp's questExitCollide). The same for the last
  player leaving a quest (Plr_OnDisconnect loads the pick with mapLoad): ours keeps the map and
  the stage (quest.cpp's questPlayerLeaves).

### OB-074: A quest's shared-out score counts players out of the quest
- **Where:** a player's quest score (more than one player in the quest): its walk of every
  player's tallies tests the asked player's quest flag, not each one's.
- **Effect:** the group's generators and secrets, the best score it is divided by and the stage
  taken (the last player's by index) count every player, those observing outside the quest too.
- **Ours:** the same (quest.cpp's questScore).

### OB-075: A quest's generators destroyed are never counted
- **Where:** the tally that adds one to a player's generators destroyed (flag 8) is never called
  (no call or reference in the exe; MonsterGeneratorDie doesn't).
- **Effect:** the generators term of a quest score (x 10) is always 0, and MSG_GAUNTLET 0x0C's and
  0x02's generators always 0.
- **Ours:** the same (no tally on a generator's death).

### OB-076: The others are never told a player went into a warp gate
- **Where:** ExitCollide's warp gate branch calls SrvNet_SendInform with code 0x13 and the
  player's net code for each other player; SrvNet_SendInform's jump table sends only codes 0, 1,
  2, 0xC, 0xD, 0x10, 0x14, 0x15 (6 bytes) and 0x11 (2 bytes): 0x13 falls through to its end and
  nothing is queued. (SrvNet_BroadcastInform takes 0x13, but isn't the one called.)
- **Effect:** the client's notice of a player going into a warp gate never shows; only the player
  who went in is told ("objcoll.c:PlayerEntersWarp").
- **Ours:** the same: nothing sent.

### OB-077: MSG_GAUNTLET with a sub other than 3 or 0x1B hangs the server
- **Where:** SrvNet_OnPacket's MSG_GAUNTLET (0xF0): subs 3 and 0x1B end with `ADD ESI, 2`; any
  other sub jumps straight to the loop's test with ESI still on the 0xF0, as MSG_SOCIAL's above.
- **Effect:** a client sending MSG_GAUNTLET with another sub freezes the server's frame (an endless
  loop).
- **Ours:** the message is skipped (game_host.cpp's Gauntlet case). Not reproduced.

### OB-078: File transfers: streams by id alone, a never-accepted send kept for good, no size check
- **Where:** the MSG_XFER code. Net_Xfer_OnSendAccept, Net_Xfer_OnRecvCancel and
  Net_Xfer_OnRecvData take the stream by its id without checking the connection (OnRecvData then
  sends its done to the stream's connection but hands the file over as the data message's
  sender's); Net_Xfer_OnRecvData appends a chunk without checking the file's size, and
  SrvNet_OnPacket takes a data message's size from it without checking the packet's; the cleanup
  for a connection gone gives up only the accepted sends (state 2); Net_Xfer_OnSendAbort doesn't
  count the send down (the count of sends in progress).
- **Effect:** any client can open, feed, finish or cancel another's transfer by its id (the file
  then counted as its own); a chunk past the size writes over the heap, and a size past the packet
  copies whatever follows it in the receive buffer; a send never accepted holds its stream (one of
  0x20) for good; the frame's send walk runs on after an abort.
- **Ours:** the same (xfer.cpp), but a chunk past the size isn't written (the file never ends
  whole and times out, as the exe's would) and a message past its packet isn't read (framing.cpp).

### OB-365: A file transfer's start sets aside whatever size it names
- **Where:** Net_Xfer_OnRecvStart allocates the receive buffer at the start message's u32 size,
  unchecked (only a size of 0 is refused).
- **Effect:** any client can have the host set aside up to 4 GB for each of the 0x20 receive
  streams with a few start messages it never follows with data: the host runs out of memory.
- **Ours:** a start naming more than 1 MB (`Xfers::kMaxReceive`; a client's only transfer, its
  saved character, is a few KB) is refused unanswered, so its sender's transfer times out
  (`noxcore/src/server/xfer.cpp`). A stream's chunks that come early are held only while they fit
  the file, and one already in, or held, isn't held again.

### OB-079: MSG_XFER with a sub past 6 hangs the server
- **Where:** SrvNet_OnPacket's MSG_XFER (0xC2): a sub above 6 jumps (`JA`) to the loop's test
  without advancing, as MSG_SOCIAL's and MSG_GAUNTLET's above.
- **Effect:** a client sending MSG_XFER with sub 7-255 freezes the server's frame (an endless loop).
- **Ours:** the rest of the packet is dropped (framing.cpp's unknown sub). Not reproduced.

### OB-080: A chat line without its NUL runs over SrvNet_OnPacket's stack; one past its packet relays what follows
- **Where:** SrvNet_OnPacket's MSG_TEXT_MESSAGE (0xA8): the text from the message's byte 11 on is
  copied into a 512-byte stack buffer, wide by a wcscpy or narrow by a swprintf(L"%S") with no
  limit (a count of INT_MAX), both to the first NUL whatever the message's length byte (its byte
  8) says; the copy is then never used. The relay sends the length byte x (1 or 2) + 11 bytes from
  the message without checking the packet's length.
- **Effect:** a client's line with no NUL in its first 256 characters (an edited client: the stock
  one sends len + 1 with the NUL, CliNet_SendChatText) writes past the buffer over the frame's
  other locals and, with a long enough run of non-zero bytes after it, the return address: a
  remote crash at least. A length byte past the packet's end relays whatever follows the packet in
  the receive buffer (an earlier packet's bytes) to every player (or the team).
- **Ours:** the text is read only for the map vote, within the message (game_host.cpp's
  TextMessage case: to the NUL or the message's end); a message past its packet isn't read (the
  rest of the packet dropped, framing.cpp). Not reproduced.

### OB-081: A .plr's map name is written twice its length, and read into 32 bytes
- **Where:** plr_rwGameData copies the map's name into the player's saved map name (32 bytes),
  then writes u16 its length and 2 x that many bytes from there; reading, it reads 2 x the length
  into the same 32 bytes.
- **Effect:** a save carries whatever followed the name in the buffer (a longer earlier name's
  end); a name past 15 letters would read over the fields after it (the quest's in-game flag, the
  ankhs taken). The stock maps' names are short.
- **Ours:** player_save.cpp writes the same bytes from PlayerControl::savedMapName (zeros past
  its 32) and reads at most the 32.

### OB-082: A for-good enchantment comes back from a .plr lasting a second
- **Where:** guiEnchantment writes each enchantment's BuffsDur (0: for good); its reader turns a
  0 into gameFPS.
- **Effect:** a solo save's enchantment on for good (a potion's, a shield's) lasts 30 frames
  after the load.
- **Ours:** the same (player_save.cpp's readEnchantment).

### OB-083: An unknown enchantment in a .plr writes past the table
- **Where:** guiEnchantment's reader: enchantByName returns 32 for a name it doesn't know;
  BuffsDur[32] is written (past the 32) and getEnchantSpell(32) reads past its table (the first
  ENCHANT name's pointer taken as a spell).
- **Effect:** a damaged or edited file overwrites the field after BuffsDur and casts nonsense.
- **Ours:** the name is passed over (nothing cast, nothing written).

### OB-084: A long journal entry in a .plr runs over the reader's stack
- **Where:** plr_rwJournal reads each entry's name (its u8 length, up to 255) into a 64-byte
  stack buffer.
- **Effect:** a name over 63 letters (an edited file) overwrites the stack.
- **Ours:** read whole, kept to 63 letters (Plr_AddJournalEntry's own cut).

### OB-085: A remote player's secondary weapon and quiver aren't saved
- **Where:** plr_rwInventory writes the secondary weapon and last quiver from the host's own
  inventory window's picks, matched by net code against the saved player's items; a solo host
  also writes its items in its window's order when the window shows them all.
- **Effect:** in a quest every player's file has none (or, on a listen host, the host's own pick
  should it match); a dedicated host's save is in its list's order, which a load turns round
  (each item loaded at the list's head).
- **Ours:** the same since 2026-10-07: the host's window through HostClient::inventoryWindow
  (noxsdl's cells, secondary weapon and last quiver), matched by net code against the saved
  player's things in any game; without a host's client (a dedicated server) none, and the list's
  order.

### OB-086: Saving a .plr tells its client its inventory loaded
- **Where:** plr_rwInventory sends netMsgInventoryLoaded (0x71) on writing as well as on reading.
- **Effect:** each save scrolls a closed inventory window back to its top
  (Gui_Inv_OnInventoryLoaded).
- **Ours:** the same.

### OB-087: An old warrior's .plr gives its abilities as spells
- **Where:** guiSpellbook's reader grants by Spell_GrantToPlayer when the section's version is
  under 3, whatever the class.
- **Effect:** a version 1-2 file's ABILITY_ names aren't spells: "AwardSpellError", no abilities.
- **Ours:** the same.

### OB-088: A quest's vote settings only switch the votes on
- **Where:** a quest's restart and kick votes, as cast, test nox.cfg's ResetQuestMinVotes /
  KickQuestPlayerMinVotes (6 and 5) only for not 0; the votes' threshold gives both kinds a fixed
  6, the count passes a restart when every player in the quest has voted, a kick at 6 or all in
  the quest but one (two or more).
- **Effect:** the settings' numbers do nothing.
- **Ours:** the same (GameHost::Options resetQuestVotes / kickQuestPlayerVotes).

### OB-089: A quest save of 64 KiB or more reaches its client cut short
- **Where:** SrvNet_SendSaveDataToClient keeps computeServerPlayerDataBufferSize's count as a u16
  (tested as 16 bits and masked to 0xFFFF) for its buffer and the transfer's size.
- **Effect:** a player whose server sections come to 64 KiB or more (a very full inventory) gets
  "SAVEDATA" with only the count's low 16 bits of them, which won't load; at a multiple of 64 KiB
  nothing is sent (and the follow-up sends MSG_GAUNTLET 0x1C {0} after its {1}).
- **Ours:** the same (player_save.cpp `saveDataSent`).

### OB-090: A map change's pixie branch tests the player instead of the pixie
- **Where:** a map loaded, come by an exit: for each player's owned thing not isUnit, the test
  "ObjClass & 1 (a missile) and a further check" is made on the player (class 4, never a missile)
  where the owned thing was meant.
- **Effect:** that branch (a player's pixie's target forgotten) never runs.
- **Ours:** the branch isn't run either. A carried pixie's target is forgotten as it's carried
  (Server::loadMapKeeping: our extents are renumbered, the old map's target is gone with it).

### OB-091: A creature carried through a map change never decays
- **Where:** mapSwitchLevel calls decayDestroy, which runs decay over the whole decay list (the
  decay flag, ObjFlags bit 22, off, out of the list), the kept things' entries with the rest's;
  nothing puts them back.
- **Effect:** a migrating creature (isUnit) whose decay was running as the map changed (its corpse,
  a script's DeleteObjectTimer) stays on the next map for good: a corpse never goes.
- **Ours:** the same (Server::loadMapKeeping: a kept thing's decayAt cleared, its timer the decay's
  end).

### OB-092: A missile placed by Obj_SetPosition stays in the motion queue when freed
- **Where:** Obj_SetPosition calls unitHasCollideOrUpdateFn for anything with a Collide, a
  missile too (the new things' pass skips class 1); its way out (from servFinalizeDelObject)
  skips class 1.
- **Effect:** the queue is drained every frame before a deleted thing is freed (at the next tick's
  end), so it's harmless in play; but a missile placed so and freed before a drain (at a map change,
  whose loop frees the swept at once, or with the game paused) leaves a freed node in the list,
  which the next frame's steps walk.
- **Ours:** never: ours doesn't queue a missile at all (physics.cpp joinsMotion).

### OB-093: GetHost without a host player crashes
- **Where:** the script builtin GetHost reads `Plr_GetByIndex(0x1F)->PlayerUnit` with no check:
  Plr_GetByIndex gives NULL for an inactive slot.
- **Effect:** a map script calling GetHost on a server with no player in slot 0x1F (a dedicated
  server) reads through NULL and crashes.
- **Ours:** 0 pushed (solo.hpp `hostPlayer`: none).

### OB-094: A "map:waypoint" exit reads the wrong map's types
- **Where:** mapLoad cuts ".map" off by copying the name less its last four letters, but from the
  whole exit string: "Wiz02C.map:ReturnTo2BWP" gives "Wiz02C.map:ReturnTo", so checkMapFile finds
  no file and the map info it would read stays the current map's; those are the types the
  keep-the-things check reads.
- **Effect:** after a waypoint exit that check decides whether the players keep their things
  (the next map a quest's, bit 2) by the map they're leaving. In a solo game 0x200 skips the
  outfit anyway; a network game's waypoint exit to or from a quest map would keep or lose them
  wrongly.
- **Ours:** the same (Server::loadMapKeeping: the old map's types when a waypoint is given).

### OB-095: wiz02b's exit back to wiz02c names a waypoint wiz02c hasn't (map data)
- **Where:** wiz02b.map's exit to "Wiz02C.map:ReturnTo2BWP"; wiz02c.map's waypoint is
  "Wiz02C.map:ReturnFrom2BWP" (the waypoint lookups compare the names past the scope exactly).
- **Effect:** no waypoint matches; the player stays at wiz02c's player start.
- **Ours:** the same (World::waypointMatching finds none).

### OB-096: con08b's exits back to Con08A and wiz03c's second way back to Wiz03b name no waypoint (map data)
- **Where:** con08b.map's three exits to "Con08A.map:FromTempleWP" (con08a.map has no such
  waypoint; war08a and wiz08a, the same map for the other classes, have theirs); wiz03c.map's exit
  at (1035, 4899) to "Wiz03b.map:HideoutRear" (wiz03b's waypoint is "HideoutRearWP", which
  wiz03c's two other exits there name).
- **Effect:** as wiz02b's: no waypoint matches, the player comes in at the map's player start.
- **Ours:** the same; a test lists every solo exit that leads nowhere (these three; every other
  of the 240 names a map there is and a waypoint it has).

### OB-097: Exits to another class's maps, and a map no exit names (map data)
- **Where:** War03d.map (no exit anywhere names it; its own leads back to war03b); exits to
  another class's maps left in the shared layouts: con05b's to War06a.map, wiz07e's teleporter to
  Con07A.map, war08b's to Con08b.map, and war08a's / wiz08a's / con08a's at (5440, 3876) to
  Con03a.map (Con02a's exit spot copied).
- **Effect:** a player who reaches one goes to the other class's map (with its scripts, and
  their quest flags, for the wrong class). Whether any is reachable in play (behind walls, off)
  wasn't checked.
- **Ours:** the same (the exits go where their strings say).

### OB-098: Buying a stack reads the gold once (1.2b)
- **Where:** Shop_OnBuyStack (MSG_TRADE 0x17) reads the player's gold (playerGetGold) before its
  loop and checks each item's price against that, never the gold left; playerSubGold stops at 0.
- **Effect:** a player who can pay for one of a stack gets as many as it asks (up to the stock), its
  gold down to 0.
- **Ours:** the same (trade.cpp's 0x17).

### OB-099: A shop's sale ignores its pickup's refusal
- **Where:** Shop_OnBuyItem and Shop_OnBuyStack call the item's PICKUP function and don't look at
  its answer; the stock entry, the keeper's record and the gold go regardless.
- **Effect:** an item a pickup refuses (carrying too much: DefaultPickup's
  "pickup.c:CarryingTooMuch") is paid for and lost: it was never in the world, and isn't in the
  inventory.
- **Ours:** the same (the thing deleted: trade.cpp's `buy`).

### OB-100: Selling a stack sells anything of the type
- **Where:** Shop_OnSellStack (MSG_TRADE 0x19) takes the first `count` inventory things of the
  type with none of Shop_OnSellItem's refusals (a quest item, "CantSellQuestItem"; a glyph,
  "CantSellItem"), worn or not; its sound only when it found all `count`.
- **Effect:** a client asking for a stack sale can sell quest items (keys, the orbs, Oblivion) and
  glyphs, and equipped things.
- **Ours:** the same (trade.cpp's 0x19).

### OB-101: A repair checks neither the need nor the gold
- **Where:** Shop_OnRepairItem (MSG_TRADE 0x1A) charges shopGetItemCost's repair price (at least
  1) and mends the item whatever its state, with no look at the player's gold (playerSubGold stops
  at 0).
- **Effect:** a whole item costs 1 to "repair"; a player without the gold gets the repair free
  (its gold to 0). The stock client asks the cost first (0x1E: 0x39D for a whole thing), so only a
  modified client does either.
- **Ours:** the same.

### OB-102: A repair of a thing without health data crashes the server
- **Where:** Shop_OnRepairItem passes `obj->HealthData->Max` to unitSetHP (it reads HealthData
  and its Max itself) before that function's own null check.
- **Effect:** a client asking a repair (MSG_TRADE 0x1A) of a carried thing with no HEALTH in
  thing.bin (a key, a glyph, a spell book or field guide, a quiver, a gem) reads through NULL. The
  stock client only offers repairs where 0x1E said there was one (0x39D for these), so only a
  modified client does it.
- **Ours:** not reproduced: the rest of the repair runs (its price taken, the gold reported, 0x323)
  with nothing to mend (trade.cpp's 0x1A: health 0 of 0).

### OB-103: MSG_TRADE 0x0E at a shop ends it as a trade
- **Where:** SrvNet_OnPacket's MSG_TRADE 0x0E (cancel) calls Trade_CloseP2P on the player's session
  whatever it is; at a shop that gives the shopkeeper its "gold put up" through playerAddGold and
  sends it TRADE 0x01, both through its monster data (a field of it read as the Player a player's
  data holds there), and never thaws the player.
- **Effect:** a modified client's 0x0E at a solo shop leaves its player frozen till the next map
  (mapSwitchLevel thaws everyone) and writes into the shopkeeper's monster data.
- **Ours:** the player stays frozen (the same); the gold and the message go to players alone
  (trade.cpp's `closeTrade`).

### OB-104: Players can never trade with each other
- **Where:** Trade_StartSession is called only from SrvNet_OnPacket's MSG_TRADE 0x15, which wants
  the other object's subclass 8 (a shopkeeper's); players (NewPlayer, PlayerFemale) have no
  SUBCLASS.
- **Effect:** the trading half of the code (offers, accepts, the P2P window: the server's trade
  handlers and the client's GUITrade) can't be reached in 1.2b.
- **Ours:** the same: 0x15 refuses a player; the trade itself is implemented and works once started
  (trade.hpp's `startTrade`; tested between two network players).

### OB-105: Anyone in a trade can take back either side's offer
- **Where:** Trade_WithdrawOffer (MSG_TRADE 0x10) finds the code in either offer list and gives
  the thing back to its owner, whoever asked.
- **Effect:** a trader can pull the other's offer out of the window (back to its owner): no gain,
  only a nuisance (and unreachable: players can't trade, above).
- **Ours:** the same.

### OB-106: A trade started with the sessions' pool used up crashes the server
- **Where:** Trade_StartSession writes the two sides into createShopStruct's answer without a null
  check when neither or both are players (a shop's start checks it).
- **Effect:** with the 0x40 TradeSessions all held, a trade between two players (or two non-players)
  writes through NULL. Unreachable in 1.2b (players can't trade, above).
- **Ours:** not reproduced: no trade starts.

### OB-107: Things given to a shopkeeper with the trade items' pool used up are lost
- **Where:** an accepted offer's hand-over to a non-player side and Trade_WithdrawOffer (a
  non-player side's offer taken back) put the thing into the shop's stock through
  addItemToShopSession while the offer's own entry is still held, and drop the answer;
  loadShopItems drops it too. A session's free frees its offer entries but not their things.
- **Effect:** with the 500 TradeItems held, a thing a player's offer hands to a shopkeeper vanishes
  (in no list, never freed) and a stock thing made for a shop is left the same. Whatever the pool,
  a player who leaves a shop (Trade_ExitShop, outside a quest) with things offered loses them.
  Players offer at a shop only with a modified client (the stock GUIShop has no offer), and the
  pool fills only with many quest shops kept (about 64 things each) or many open shops.
- **Ours:** the same (trade.cpp's `lose`: gone, its extent not reused).

### OB-108: A solo spell book bought by the stack goes on with a freed shop
- **Where:** Shop_OnBuyStack gives each item through its PICKUP; a spell book's (SpellBookPickup)
  learns it in a solo game, and Spell_GrantToPlayer (notify and award flag 1) shuts the shop:
  Trade_ExitShop frees the session (the pool's memory filled with 0xAC). The buy then calls two
  more of the shop's functions on it and goes on with the rest of the stack.
- **Effect:** reads through the freed session's sides (0xACACACAC): a crash, when a client asks a
  stack of a spell book it hasn't learnt (a single buy puts books straight into the inventory).
- **Ours:** the buy ends where the shop shut, the book paid for (trade.cpp's `buy`).

### OB-109: A solo save that fails writing its map leaves the player's things with the SaveGameLocation
- **Where:** the solo save: saveMakePlayerLocation moves the player's owned things in the world to
  a new SaveGameLocation; monstersAllBelongToHost gives them back only when mapSaveMap succeeded.
- **Effect:** a map write that fails (a full disk, a read-only file) leaves the location in the
  world owning the player's creatures and things; the next save makes another.
- **Ours:** the same (solo_save.cpp `saveSoloGame`).

### OB-110: NPCXfer writes an unset word
- **Where:** NPCXfer in write mode writes a stack word after the script handlers (MonsterXfer
  writes a 0 there) without setting it: whatever the stack held.
- **Effect:** a saved NPC's record carries a stray u32 (the stock Save\WORKING\Con01a\Con01a.map
  has 48079008 and 6104500); the load reads it into the same unused local. Harmless.
- **Ours:** 0 (object_save.cpp: the stack's garbage can't be had).

### OB-111: A save counts a destroyed thing among what its holder carries but doesn't write it
- **Where:** mapReadWriteObjData writes the number of things in the holder's inventory chain,
  destroyed ones (flag 0x20, waiting to be freed) included; xfer_saveObj then writes nothing for a
  destroyed one.
- **Effect:** a holder saved while one of its things is being deleted claims more things than
  follow it: the load takes the next records in the file as its things.
- **Ours:** the same (object_save.cpp).

### OB-112: A save loses a thrown chakram, and things held two deep of a type seen nowhere else
- **Where:** the save's TOC pass gives TOC ids to the object list's things and what they hold (one
  level), a generator's templates and theirs, the updatable list's things (not what they hold);
  xfer_saveObj writes nothing for a thing whose type has no id (its 0 return is ignored by its
  callers).
- **Effect:** a chakram in flight (ChakramInMotion carries it) is written by server_mapRWObjectData
  only when its type has an id from something else; a thing held by a held thing likewise.
  Otherwise it's missing from the save, and its holder's count (above) is off.
- **Ours:** the same (object_save.cpp).

### OB-113: A map save puts a wall two Wall spells changed back to the first spell's shape
- **Where:** the solo save's map write: mapSetWallInGlobalDir0pr1 gives each wall a Wall spell
  changed (its record's `replaced` set) its old shape (`oldDirection`) for the write, and its
  counterpart gives it the spell's shape (`direction`) after. Both walk the records newest first,
  so for a wall two spells changed the last one applied is the oldest record's.
- **Effect:** the save itself writes the wall's original shape (right), but afterwards the live
  wall has the first spell's shape, not the second's, until the spells end.
- **Ours:** the same (map_state.cpp saveMapSections).

### OB-114: A map save with no floor writes only FloorMap's version
- **Where:** server_mapRWFloorMap's write side returns after the version when the floor grid is
  empty (no box to write).
- **Effect:** reading such a map back takes the next section's bytes for the floor's box and
  tiles. Only a map without floor tiles (no shipped solo map).
- **Ours:** the same (map_state.cpp: the section is the version alone).

### OB-115: A map save writes the last script loaded when the map has none
- **Where:** server_mapRWScriptObject writes the save's ScriptObject from "<work dir>\nc.obj",
  which only a map's ScriptObject read writes.
- **Effect:** a map without a ScriptObject section is saved with the script of the last map
  that had one. (Every shipped solo map has a script.)
- **Ours:** the same within a run (Server::lastScriptCode, solo_save.cpp `saveMap`); the exe's
  nc.obj also outlives the run, ours doesn't (none read yet: an empty script).

### OB-116: GoBackHome faces where its home facing points from where the monster stands
- **Where:** the script builtin GoBackHome pushes FACE_LOCATION at its current position + 10
  along its home direction, under MOVE_TO_HOME; the point isn't taken from its home.
- **Effect:** back home, the monster turns towards a point 10 from where it was when the script
  called, not to its home facing (right only when it was called at home).
- **Ours:** the same (script_builtins_creatures.cpp).

### OB-117: PushObject adds the prefab's offset to the difference
- **Where:** the script builtin PushObject takes (object - point) + the prefab's offset (the
  script's prefab x / y) as the way, where the point should have had it; and it divides by that
  way's length unchecked.
- **Effect:** in a prefab's function with an offset the push goes the wrong way (outside prefabs
  the offset is 0); an object exactly at the point gets a NaN force.
- **Ours:** the same.

### OB-118: HasClass and HasSubclass with an unknown name
- **Where:** the script builtins HasClass and HasSubclass push their result only when the name is
  found: HasClass returns at the end of its CLASS table without a push; HasSubclass walks its
  subclass tables 32 entries at a time with no end but a pointer wrapping to 0, so past the last
  table it reads on through whatever memory follows.
- **Effect:** HasClass with a name not in the table leaves the stack a word short (the script's
  next pops are off); HasSubclass with one reads past its tables (a crash or a garbage match).
- **Ours:** both push nothing for an unknown name (HasSubclass after the nine tables).

### OB-119: LookAtDirection past 0-8, SetHalberd past 0-3 or without a host
- **Where:** LookAtDirection (through mathDirection4ToAngle) indexes the 3 x 3 grid's table
  unchecked; SetHalberd (script_OblivionGive) indexes the four Oblivion staffs' names unchecked
  and reads Plr_GetByIndex(0x1F)->PlayerUnit with no check.
- **Effect:** a way outside 0-8 faces by whatever follows the table; a kind outside 0-3 gives
  whatever thing the next pointer names; SetHalberd on a server without player 0x1F crashes.
- **Ours:** those do nothing.

### OB-120: The group owner tests push nothing without their group
- **Where:** the script builtins GroupIsOwnedBy, IsOwnedByAny and GroupIsOwnedByAny return
  without a push when a group they name doesn't exist. (Their arguments also read as (object,
  group), (group, object) and (group, group) whatever OpenNox's names suggest.)
- **Effect:** the stack a word short after the call.
- **Ours:** the same.

### OB-121: A secret wall's open / shut goes to every player, but counts as told only for one
- **Where:** SrvNet_UpdatePlayer's secret-wall pass (the secret walls about a player's view):
  when the wall's state isn't what that player was last told (its bit in the wall record's
  per-player bits), it calls SrvNet_BroadcastWallOpen / Close (MSG_OPEN_WALL 0x3B /
  MSG_CLOSE_WALL 0x3C), which go to every player, and sets or clears that one player's bit.
- **Effect:** each other player gets it again when the wall comes into its own view (a
  duplicate), and players far away hear of walls they can't see. Harmless to the clients.
- **Ours:** the same (game_host.cpp updatePlayer).

### OB-122: A broken secret wall crashes the server when it opens or shuts in view
- **Where:** the secret-wall pass takes the wall by Map_GetWallAtGrid, which passes over a broken
  one (flag 0x20), and hands it to SrvNet_BroadcastWallOpen / Close unchecked: they read its id
  through NULL. A wall both secret and breakable (the editor allows it) that's broken keeps its
  record in the secret list.
- **Effect:** once broken, its state changing (or differing from what a player was told) while
  a player has it in view crashes the server.
- **Ours:** can't follow it there: no message, the player's bit left as it was.

### OB-123: A secret wall's record with a state past 4 is told open every frame
- **Where:** the secret-wall pass sets the value it compares with the player's bit only for
  states 1-4; any other leaves it the player unit's pointer, never equal to 0 or 1.
- **Effect:** MSG_OPEN_WALL to every player each frame for as long as a player has it in view.
  Only a map (or save) with such a state (the editor writes 1-4, or 0, which the load makes 1
  or 3).
- **Ours:** the same (game_host.cpp updatePlayer).

### OB-124: A save moves a monster's AI timers
- **Where:** XFer_ActionData calls AssignIfGreater on the monster's AI timer fields (ours:
  sightedSoundAt, blockedAt, nextMelee, actionFrame and the rest) in write mode too, with the
  time since the save (0): after writing them it sets each one under 1 to 1.
- **Effect:** a monster whose "never" (0) frames were saved has them at frame 1 afterwards (a
  timer meant unset now set long ago). Harmless where they're compared against the frame.
- **Ours:** the same (object_save.cpp).

### OB-125: An unknown MSG_DIALOG sub-op hangs the server
- **Where:** SrvNet_OnPacket's MSG_DIALOG (0xD0) handles subs 1 and 2; any other jumps back to
  the loop's test with its read pointer where it was, so the same message is read again and
  again.
- **Effect:** a client sending MSG_DIALOG with sub 0 or 3 and up makes the server loop for ever.
- **Ours:** the message has no size (net/framing.cpp), so GameHost::onPayload stops reading that
  packet: the rest of it is lost, the server goes on.

### OB-126: TellStory with no caller or no trigger reads through NULL
- **Where:** SrvNet_OpenNpcDialog (from TellStory with the script's caller and trigger) reads
  both objects' UpdateData before its null check.
- **Effect:** TellStory run by a timer or a map handler without a caller / trigger crashes the
  server.
- **Ours:** nothing happens (dialog.cpp's openNpcDialog).

### OB-127: MSG_DIALOG 3's strings are copied unbounded
- **Where:** SrvNet_OpenNpcDialog strcpys the text id into its 32 bytes at byte 2 and the NPC's
  portrait (itself strcpyd into the NPC by StoryPic) into its 32 bytes at byte 102, after
  writing the dialog's type at byte 134.
- **Effect:** a text id of 32 letters or more runs on under the name, the sound and the portrait
  (written after it: what they leave of it is sent); a portrait of 32 letters or more overwrites
  the type byte with its 33rd letter (or its end), and a longer one the exe's stack.
- **Ours:** the message is built in the exe's order over a zeroed 135 bytes (dialog.cpp's
  npcDialogMessage), so the same bytes are sent; what would go past the message is dropped.

### OB-128: A YESNONEXT dialog's answer is always 0
- **Where:** SrvNet_OnDialogAnswer keeps the client's answer in the NPC's dialog answer only
  when its dialog type is 1 (YESNO); else 0, YESNONEXT (3) too.
- **Effect:** GetAnswer after a YESNONEXT dialog reads 0 whatever was clicked.
- **Ours:** the same (dialog.cpp's dialogAnswer).

### OB-129: A dialog cancelled (or its NPC gone) while a player talks leaves the player talking
- **Where:** SrvNet_OnDialogAnswer unfreezes the player, but sends MSG_DIALOG 4 and clears its
  dialogWith only while the NPC it talks to still has both functions; CancelDialog sets them to
  -1 regardless of anyone talking to it. (An NPC deleted meanwhile: the exe reads its freed
  record.)
- **Effect:** a script that cancels an NPC's dialog while its dialog is open leaves the player
  "talking" for good: no exits (the exit's check), no picking up, dropping or working things
  (SrvNet_OnPacket 0x72 / 0x73 / 0x7B), IsTalking 1; its dialog window open.
- **Ours:** the same; an NPC no longer there counts as one without functions (dialog.cpp). A map
  change carries the player's dialogWith to the NPC carried with it or, left behind, to none
  there: still talking (Server::loadMapKeeping).

### OB-130: One freeze holder for every player
- **Where:** unitFreeze / unitUnFreeze keep who froze a player in one global (the script's
  Frozen gives 1, a dialog's TellStory 0): while it's set, an unfreeze by 0 (a dialog's answer)
  does nothing, for every player.
- **Effect:** in a game with several players (a quest's or a network game's scripts), a script's
  Frozen on one player keeps another frozen after its dialog or shop ends: that player's thaw is
  swallowed while the holder is set, and the script's later thaw of the first clears the holder
  but doesn't thaw the second. It stays frozen till something unfreezes it itself, or a map
  switch thaws every player.
- **Ours:** the same (World::freezeLock, monsters.hpp freezeUnit / unfreezeUnit).

### OB-131: A field guide of no known creature is used up
- **Where:** Obj_FieldGuideUse calls Plr_AwardFieldGuide with the guide's creature index
  (Game_GetCreatureGuideIndex: 0 for an unknown name) and deletes the guide without looking at
  the result.
- **Effect:** such a guide prints "AwardGuideError" and is gone.
- **Ours:** the same (books.cpp).

### OB-132: The journal builtins take any object for a player
- **Where:** JournalEntry / JournalDelete / JournalEdit with an object (not 0) pass it to
  comJournalEntryAdd / comJournalEntryRemove / comJournalEntryUpdate, which read its
  UpdateData's player record as a player's.
- **Effect:** a script giving a monster or an item reads garbage as a player (a crash, or a
  journal written in some other memory); JournalEntry plays its 0x387 at the object regardless.
- **Ours:** nothing for an object that isn't a player's unit (the sound still played).

### OB-133: A creature carried into a visited map's saved copy forgets whom it fights
- **Where:** the saved map's after-load pass looks every monster's references (the list of what
  it saw, its enemy, its focus, what it heard, its last enemy and its transits list) up as script
  ids, carried creatures (ObjFlags 0x80000000) too, whose fields hold pointers and net codes,
  not script ids.
- **Effect:** none is found: a creature that came along (the player's escort) loses what it saw,
  its enemy, its focus, what it heard, its last enemy and its transits (its stack is spared:
  the stack's pass skips it).
- **Ours:** the same (solo_save.cpp: a live field looked up as an id finds nothing; a 0 is looked
  up as the exe does).

### OB-134: An immobile thing made in play has no net code of its own
- **Where:** netGetUnitCodeServ gives a thing of class 0x20400000 (IMMOBILE or 0x20000000: the
  ones the clients load from their own map) its Extent | 0x8000; only a map record's read sets
  Extent (newObjectWithTypeInd zeroes it, createAt doesn't set it).
- **Effect:** every such thing made in play (a script's CreateObject of an immobile type) is 0x8000
  to the clients: none of them can be told apart, or found in the client's map.
- **Ours:** the same (netCodeOf by ServerObject::fileExtent).

### OB-135: The script's Pickup tests the taker's type for gold, not the item's
- **Where:** the Pickup builtin: in a solo game, before Obj_TryPickup, a player taker gets
  Plr_DropCheapestItemIfFull unless `player->TypeInd` is Gold, QuestGoldPile or QuestGoldChest:
  the taker's type, never one of those, where the item's was meant.
- **Effect:** a script giving a solo player gold with a full inventory drops its cheapest item
  for it (the gold needs no room).
- **Ours:** the same: the taker's type tested (Plr_DropCheapestItemIfFull through
  HostClient::inventoryRoom).

### OB-136: The walls' message ids count on from map to map
- **Where:** server_mapRWSecretWalls / server_mapRWDestructableWalls give each secret / breakable
  wall found its id from a counter of its own (one per kind), each only ever incremented:
  nothing resets them (the walls' counts are reset, the ids aren't). MSG_DESTROY_WALL /
  OPEN_WALL / CLOSE_WALL carry the server's id; the client finds the wall by the id its own
  reads gave (Map_BreakWallById, Map_GetSecretWallById).
- **Effect:** the ids of a process's second and later maps go on from the first's: a client
  whose process has read other maps (or the same ones in another order) finds no wall or the
  wrong one, so a broken wall or an opened secret one isn't shown to it. Only a server and
  client that started together and loaded the same maps agree.
- **Ours:** differs: each map's ids from 0 (World::load), as noxsdl's client numbers them; a
  deliberate change.

### OB-137: The units update's wake-up near a secret wall reads a stale flag
- **Where:** updateUnits's secret-wall walk: states 2 and 4 (moving) set a local flag and wake
  what's about the wall (getUnitsInRect with a wake-up for each), 1 and 3 clear it; any other
  state wakes by the flag as the record before it left it (the first record in the list: an
  unset stack byte).
- **Effect:** a secret wall with a state past 4 (only a map or save with one) wakes what's about it
  whenever the record before it in the list is moving (or at random as the list's head).
- **Ours:** the wake-up isn't reproduced.

### OB-138: A wall group's walk goes on as a group of groups
- **Where:** server_scriptExecuteFnForEachGroupObj's case 2 (a wall group, for the WallGroup*
  builtins) has no break: after its walls it falls into case 3 and walks the same list again,
  taking each entry's first word (the wall's x) as a group's id (server_scriptGetGroup) and
  running itself on that group.
- **Effect:** WallGroupOpen / Close / Toggle / Break on a group also act on the walls of any wall
  group whose id is one of its walls' x (or of groups of groups by that id); a wall group whose
  wall's x is its own id (or a cycle of such) recurses till the stack runs out (a crash).
- **Ours:** the same (script_host.cpp forGroup), but a group met again on the same walk is passed
  over (no crash).

### OB-139: Abil_GrantToPlayer's error line is MISSING
- **Where:** Abil_GrantToPlayer asks Str_GetString for "AwardAbilityError" from
  Server\Ability\Ability.c, so the table's label is "Ability.c:AwardAbilityError"; nox.csf has it as
  "ComAblty.c:AwardAbilityerror".
- **Effect:** an ability out of 1-5 (an ability book with a bad entry, a .plr's unknown ability
  name) tells the player "MISSING:'Ability.c:AwardAbilityError'".
- **Ours:** the same (player.cpp's grantPlayerAbility).

### OB-140: A monster-owned creature's poison is told through its owner's data as a player's
- **Where:** Obj_ReducePoison, removePoison and setSomePoisonData send
  MSG_REPORT_OBJECT_POISON to a subclass 0x80 creature's ObjOwner whatever it is;
  SrvNet_SendReportObjectPoison reads its UpdateData's player record as a Player.
- **Effect:** a creature of subclass 0x80 owned by a monster queues the message to a garbage
  player index (or reads through a bad pointer).
- **Ours:** told to a player owner only.

### OB-141: A map creature whose health is kept has none on the host
- **Where:** NPCXfer and MonsterXfer's read: a record with the map's health kept (Field361 + 1,
  the byte after the status-from-monster.bin one) has its HealthData's most and current health
  set to 0 when Game_HasFlags(1) (the host), after both were read from the record;
  monsterOnSpawnSpellcaster then keeps the map's (that is, none).
- **Effect:** on the host the campaign's creatures with it (Con01A's four NPCs, its maiden and the
  airship captain, for one) have no health: no blow or spell can hurt them (the stock Save\WORKING\Con01a
  has them at 0 / 0). It may be meant (story characters that mustn't die), but the flag's other use
  says "the map's health": kept as in the original.
- **Ours:** the same (world.cpp's NPCXfer and MonsterXfer reads).

### OB-142: A map's startup screen doesn't pause the game
- **Where:** the client's MSG_FADE_BEGIN {1} (CliNet_OnPacket 0xE4: Game_SetPaused(1),
  Draw_FadeOutScreen(25, 1, with an unpausing callback)) and Gui_Brief_ShowLockScreen, whose own
  Draw_FadeOutScreen ends the running fade from black first (ending a fade runs its callback).
- **Effect:** a map whose MapInitialize calls StartupScreen (War01A's chapter 1, for one): the
  map's start sends MSG_FADE_BEGIN {1, 1} and then the script's StartupScreen pauses and, the
  map not drawn since the client's map change (gameChangeMap), sends MSG_CHAPTER_END at once;
  the client takes both in one frame, and the briefing's fade ends the map's fade in, whose
  callback finds no fade to black and unpauses. The game runs under the chapter's screen (the
  other callers and the load menu care for the briefing; this order doesn't).
- **Ours:** the same (noxsdl's cinematics.cpp).

### OB-143: MSG_CHAPTER_END without a host player goes to another player
- **Where:** StartupScreen / DeathScreen's fade callback: the search for player 0x1F leaves its
  record pointer at the last player it looked at (ECX, garbage with no players) and
  SrvNet_SendChapterEnd sends to that one's index.
- **Effect:** only without a player 0x1F (a dedicated server running a solo map's script): the
  last player in the list gets the chapter's screen (or a garbage index).
- **Ours:** nothing is sent without a host player; the map's still set not drawn. Not
  reproduced.

### OB-144: WideScreen's windows remembered by the last call only
- **Where:** the windows' hide / show routine: (1) stores what was open (book, inventory, server
  options, map) and hides them; (0) shows what's stored again, and never clears it.
  WideScreen(1) calls it each time it gets a bars record (also while the bars are in), EndGame
  calls (0).
- **Effect:** WideScreen(1) twice forgets the windows the first hid (the second found none open);
  WideScreen(0) twice, or EndGame after a widescreen, opens again what a past widescreen hid.
- **Ours:** the same (cinematics.cpp setHud, main.cpp's hideHud).

### OB-145: The client's NPC table fills up
- **Where:** Cli_NewNpc: 64 records (g_cliNpcs, 0x524 bytes each), emptied only at a game's
  start. MSG_REPORT_NPC (CliNet_OnPacket 0x69) for a code without a record takes a free one;
  with none free it gets none.
- **Effect:** after 64 different NPC codes in a session (net codes change from map to map), a new
  NPC / NPCWizard / Maiden isn't drawn at all (NPCDraw and MaidenDraw draw nothing without a
  record) and its worn things and poison aren't kept.
- **Ours:** the same (client::World, kNpcSlots).

### OB-146: NPCDraw and MaidenDraw leave the drawing colours set
- **Where:** NPCDraw sets materials 1-6 and at its end sets 0-5 to white, so 6 keeps the
  NPC's colour; it returns before that when the state has no frames or a helper it calls fails,
  leaving all six. MaidenDraw sets 1-6 before MonsterDraw and never resets them.
- **Effect:** whatever is drawn next with material colours it doesn't set itself takes the NPC's or
  maiden's.
- **Ours:** not reproduced: noxsdl gives each sprite its own colours; which art would show it
  isn't checked.

### OB-147: The save menu's failed load shows no error
- **Where:** the save menu's Load: the load failing, it makes the error box
  (Gui_Dialog_Create(0, SaveErrorTitle, SaveErrorTitle, 0x21)) and then calls Gui_Dialog_Close,
  which closes the box it just made. The same for the in-game menu's AutoLoad answered Yes (the
  question's OK handler): the new box takes the question's window and flags (0x21 has the
  auto-close 0x20), and windowDlgProc closes it after the OK.
- **Effect:** a slot whose load fails (Player.plr missing or unreadable after the copy) leaves the
  menu shut and the game as it was, without a word. Only AutoLoad while dying or dead (no question
  asked) shows the box.
- **Ours:** the same (cinematics.cpp loadSlot / autoLoadNow and its one message box).

### OB-148: With no saved game the save menu can't be left
- **Where:** the save menu's fill: with no slot read, Cancel (0x1F7) and Load (0x1F6) are turned
  off (and Save left as another of the menu's routines sets it).
- **Effect:** after a death screen (Save off) with no saved game at all, the menu's Quit is off:
  only Escape could close it (consoleEsc's close of the menu, if the key gets past the menu's
  window procedure, which takes everything), onto a dead game. From the in-game menu Save still
  works (and shuts it).
- **Ours:** the same (cinematics.cpp fillLoadMenu); noxsdl formerly kept Cancel on after a
  briefing.

### OB-149: AutoSave stays off for the rest of the game
- **Where:** menuGameOnButton's 0x232A: our drawable flagged DEAD turns the button off (the
  window's enable set to 0); nothing turns it back on but the window made anew at the next
  game's GUI set-up (game_guiInit).
- **Effect:** pressing AutoSave while dead leaves it greyed after a load too, for the game.
- **Ours:** the same (quit_menu.cpp; QuitMenu::newGame at each game).

### OB-150: A .plr's Music section can hold more saved tracks than the level's six
- **Where:** plr_rwMusic's read: the count (an i32 from the file) is the loop's bound with no
  cap, each entry written at the music stack's entry i, g_musicStack + (i + level x 6) x 16,
  then the stack's size is set to the count.
- **Effect:** a Music section with more than six entries (none the game writes: the push stops at
  six) writes past the level's six into the next level's, and past the table's three levels into
  what follows; a later pop reads from past the six too. A negative count reads none and leaves a
  negative size (the pop's own test puts it back to 0).
- **Ours:** not reproduced: noxsdl's loadClientSections takes the first six and keeps the size to
  0-6 (MusicPlayer::setStackSize).

### OB-151: Solo counts only AUTOSAVE as a saved game
- **Where:** client_countSaveFiles (the main menu's Solo, windowMainMenuProc) checks
  "%s\Save\SAVE%04d\Player.plr" 13 times but passes only the folder: the %04d reads the caller's
  EDI from the stack, the same number each time, never a slot's (verified in the disassembly:
  the format's call pushes only the buffer, the format and the folder, so the %04d takes the
  EDI the function saved on entry; neither windowMainMenuProc, the button's wndButtonProc nor the routine between them touch EDI: it is
  cursorUpdate's, a window pointer or 0 at the click, never 1-13).
- **Effect:** with saves in SAVE0001-SAVE0013 but no AUTOSAVE, Solo goes straight to a new
  character (the class screen) and the saved games can't be reached from the main menu.
- **Ours:** client/solo_menu.cpp countSaveFiles counts AUTOSAVE only.

### OB-152: Delete on an empty slot after AUTOSAVE says AUTOSAVE can't be deleted
- **Where:** windowSelCharProc's 0x1F7 (Delete) in a solo game: the folder-name helper takes the
  slot's folder name from its path into the remembered save name, but leaves it as it was when
  the path has no '\' (an empty slot's is empty); the name is then compared with AUTOSAVE.
- **Effect:** Delete on AUTOSAVE, then on an empty slot, shows "You cannot delete the AutoSave
  game" again instead of the NoCanDo sound.
- **Ours:** client/solo_menu.cpp soloDelete, the same (Menus' remembered name).

### OB-153: The character counts can count a file twice
- **Where:** client_countPlayerFiles04 (and client_countPlayerFiles02, the same for flag 2) reads
  each Save\*.plr's File Info into one record that isn't cleared; a file the File Info read
  can't read, or one with no File Info section, leaves the last one's flags there (the first
  file's: the stack's).
- **Effect:** an unreadable .plr after a quest character counts as another quest character
  (New is off at 14 when joining a quest game); after a network character, it sends the Join to
  the character screen.
- **Ours:** client/solo_menu.cpp countQuestCharacters / countNetworkCharacters, the same (the
  first record's flags 0).

### OB-154: The join's character list shows an unreadable .plr as a nameless Warrior
- **Where:** findAutosaves out of a solo game reads each Save\*.plr into the next record of a
  zeroed array with the File Info read and keeps it when the list's filter lists it; the read's
  result isn't tested. A file it can't read (no open, a File Info past 12) leaves the record as
  it was: zeroed, flags 0, which the filter lists for a game that isn't a quest. (After a file
  turned down the record still holds that one, turned down again.)
- **Effect:** in a network game's list, a broken .plr shows as " the Warrior" with no date; Load
  and Delete on it give NoCanDo (its path is empty). A .plr with no File Info section shows the
  same with its path (Delete removes the file; Load holds what the stack had).
- **Ours:** client/solo_menu.cpp listCharacters, the same.

### OB-155: The Join's character screen and its list disagree
- **Where:** windowMultiplayerSub's Join (0x2711) and the Host's handler go to the character
  screen when client_countPlayerFiles02 (a quest game: client_countPlayerFiles04) finds a file
  flagged 2 (4); the screen's list (findAutosaves and its filter) then shows every .plr not
  flagged 4 (or every one flagged 4).
- **Effect:** with only .plr files flagged 1 or 0 in Save\ (a solo File Info, none), the Join goes
  straight to a new character though the list would show them; with one network character among
  them, the list shows them all.
- **Ours:** noxsdl menus.cpp chooseCharacter by the counts, the list by listCharacters, the same.

### OB-156: Moving the secondary weapon in the inventory loses it
- **Where:** cliInventorySpriteUpd (a drag's start) takes the cell's first out with the cell's
  take-out, which on a cell marked the secondary weapon's calls clientSetAltWeapon(0) and clears
  the mark. The inventory's mouse handler's in-grid drop then tests the click cell's mark to
  carry it to the new cell, a mark the take-out has already cleared: dead code. The quick click's
  test of the mark (the secondary unset instead of worn) is dead the same way.
- **Effect:** taking the secondary weapon from its cell (to move it; a quick click wears it)
  makes it no secondary weapon, the server told (MSG_REPORT_SECONDARY_WEAPON 0); only dropping it
  on the secondary weapon's box makes it one again. A save meanwhile writes none.
- **Ours:** the same (noxsdl Inventory::mouseDown).

### OB-157: SAVE_SERVER of a .plr it can't read sends 4 bytes never written
- **Where:** CliNet_SendSaveServerXfer: computeServerPlayerDataBufferSize returns 0 for a file
  it can't open, the buffer is malloc(0 + 4), the buffer's fill fails at its own open without
  writing it, and the 4 bytes go as the transfer; the transfer's dispatch (case 3) writes them
  out and cliPlrInfoLoadFromFile reads their value as the first section's id. The fill also
  never tests the file's end: a .plr without its closing 0 id has it read the last id again past
  the end.
- **Effect:** none seen (the game's start sends clientCurSave, which the menus have just written
  or read); a missing or cut .plr would load whatever the heap held onto the player.
- **Ours:** server::saveServerSent sends 4 zero bytes for an unreadable file (the load then finds
  the 0 id: cliPlrInfoLoadFromFile's resets and nothing more) and stops at a file's end.

### OB-158: A group of groups with no members, or naming no group, crashes the script's group lookups
- **Where:** the group-kind lookup (the kind ObjectGroup / WaypointGroup / WallGroup test for): a
  group of groups (kind 3) takes the kind of the group its list's head names
  (server_scriptGetGroup of its member list's first value), down while that's a group of groups
  too. An empty one reads [0] (its member list is null); a member naming no group reads the kind
  byte through the lookup's null (before the null test can see it). Groups of groups naming
  each other in a loop never end.
- **Effect:** a script's ObjectGroup / WaypointGroup / WallGroup of any name, met in the list
  (newest first) before the group it finds, crashes the game on such a group. No stock map has a
  group of groups (an editor's map can).
- **Ours:** World::groupKind gives such a group no kind: never matched, the walk goes on.

### OB-159: The script's name lookups copy the name into fixed buffers
- **Where:** Waypoint, ObjectGroup / WaypointGroup / WallGroup strcpy the script's string into a
  0x4C-byte stack buffer and strcat the prefab's "%Prefix" (a global buffer) after it,
  unbounded; the name match copies both names into two 0x100-byte globals, unbounded too.
- **Effect:** a name of 76 or more characters with its prefix overruns the builtin's stack frame
  (its saved registers and return address); past 255, the match's buffers overrun each other and
  what follows. No stock map's script has such a name.
- **Ours:** the names as long as they are, matched whole.

### OB-160: The map sent for download: its "same map" check compares with a name nothing writes
- **Where:** SrvNet_MapSendLoadMap means to keep the loaded .nxz when the map is the one loaded:
  REPE CMPSB with ECX = 1 (EBP, set to 1 just before) between the map's file name (g_mapPath)
  and another global, the first letters only. But the name it keeps is copied to
  g_mapSendName; the global compared is read there and written nowhere (0 in the image), so the
  check holds only for an empty file name.
- **Effect:** none to see: every map load (not a solo game's) reads maps\<map>\<map>.nxz again,
  the same map too (and the old buffer leaks). (This entry first said the letters were compared
  with the last map's, which would have sent a map in S... after So_Druid the old file; the exe
  doesn't.)
- **Ours:** GameHost::mapSendLoadMap reads the file at every load whose file name isn't empty.

### OB-161: A map request turned down still counts as a send under way
- **Where:** SrvNet_OnRequestMap adds to g_mapSendsInProgress before it checks the player's
  sender; a request while one is under way prints "Sending" and adds anyway, and nothing takes it
  off again (MSG_CANCEL_MAP takes one off whatever the sender's state).
- **Effect:** the count stays above 0, so the map send pass keeps looking at the senders every
  even frame and every later map load aborts "under way" sends (none) for nothing. On a map
  without its .nxz (g_mapSendSize 0) the map send pass also calls SrvNet_MapSendLoadMap each even
  frame: a "CompressFail" console line 15 times a second for good.
- **Ours:** GameHost::onRequestMap counts it the same way.

### OB-162: The map download's window for player 30 reads another stream's queue
- **Where:** the map send pass calls its window count with index + 1, which takes the stream to
  count from g_mapSenders[index + 1]'s first byte: for players 0-29 the next sender's index,
  index + 1, so their own stream (Net_Send's index + 1) by luck. There are 31 senders: for
  player 30 it reads the byte past them, g_mapSendsStarted's low byte.
- **Effect:** player 30's download waits on the unacknowledged map packets of stream number
  "downloads started so far" (mod 256; 0x80 and over none): usually none, so it isn't held back
  by its own acknowledgements and is sent a block every even frame, or it waits on another
  player's download.
- **Ours:** GameHost::mapSendFrame takes that stream for slot 30 (mapSendsStarted_).

### OB-163: A player leaving mid-download is still sent the map
- **Where:** Plr_OnDisconnect doesn't reset the player's map sender (the sender's reset is
  called only by the cancel, the abort and the last block), so it stays sending.
- **Effect:** the map send pass goes on sending its blocks to the player's stream index every
  even frame (Net_Send fails on a closed stream) until the file is done, then counts it down (no
  "downloaddone": no player). A player who joins the slot meanwhile is sent the rest of the old
  download as MSG_MAP_SEND_PACKETs it never asked for (Cli_MapDownload_OnBlock takes them,
  ungated: buffered, or one written to its download file when its number is the next).
- **Ours:** GameHost::leave leaves the sender as it is too.

### OB-164: A 15-letter game name typed in the Server Settings overwrites slot 1's first spells byte
- **Where:** guiServerOptionsProcPre's 0x4003 for 0x2775 (the name field left): the field's
  text (up to 15 letters) is strcpy'd to the current settings slot's name, a 15-byte field; with
  15 letters its NUL lands on the byte after it, the first byte of the spells-allowed bitmap.
- **Effect:** slot 1's spells 1-7 (bit 0 isn't a spell) read as off till the slot is refreshed
  (the settings window's read at Go or a rule set's Save, a map's load, the window's opening): a
  player joining meanwhile is told so in MSG_UPDATE_GUI_GAME_SETTINGS (0xB1), and Go's read of
  the window rewrites the byte before it matters to the game.
- **Ours:** the same (GameHost::editSettings' name).

### OB-165: The Server Settings window's MSG_UPDATE_GUI_GAME_SETTINGS carries stack bytes
- **Where:** Plr_CreateFromJoinData (the window open) and the main loop's settings check read
  the window (the settings window's read) into an uninitialised stack record: it writes the
  name (strncpy, padded), the type, the spells, weapons and armour, "Form a new game", and the
  map's first word with its NUL; the limits only when their fields aren't empty, and nothing past
  the map name's NUL.
- **Effect:** those bytes (the rest of the map field, the limits of an emptied field) go to the
  clients as whatever the stack held; the main loop's check also compares them with what it last
  sent, so they can make it send again for nothing.
- **Ours:** not reproduced: the record starts zeroed (GameHost::fillFromWindow on a fresh record).

### OB-166: A hosted game's settings slot 1 is Arena's till the next map
- **Where:** the start-up calls the posting of the hosted game after the first load; it ends
  with slot 1's game flags (as the load's copy left them) `& 0xE90F | 0x100`: the type bits but
  Solo's cleared, Arena's set.
- **Effect:** till the next map's load (not a chat map's) or the Server Settings' opening, a
  joining player is told (0xB1) the game's type is Arena; its client copies that into its slot 0
  (the first 0xB1 since its join), whose game flags its player list compares (bit 0x20) for a
  redraw. What else a stock client shows from it isn't checked.
- **Ours:** the same (GameHost's constructor).

### OB-167: A map's load copies the settings to slot 1 before its rules apply
- **Where:** server_loadMapFile refreshes slot 0 and copies it to slot 1; the map change
  turned every spell, weapon and armour on first (spellEnableAll and its weapons and armour
  counterpart) and reads the map's user.rul into slot 0 only after the load.
- **Effect:** slot 1, which a joining player is told (0xB1), says every spell, weapon and armour is
  allowed whatever the rules; the clients' spells allowed come from MSG_GAME_SETTINGS_2 (slot 0),
  so what's played isn't touched.
- **Ours:** the same (GameHost::loadNextMap's refreshSettingsSlot0(true)).

### OB-168: A stream awaiting its CLIENT_ACCEPT reads a 4-byte packet's fifth byte
- **Where:** the transport's packet receive, for a stream awaiting its CLIENT_ACCEPT:
  `3 < len && packet[4] == 0x20` takes the packet as the CLIENT_ACCEPT; a 4-byte packet (a
  reliable "1F <ack>" alone, or a header with a new ack and two bytes) has no byte 4, so the
  receive buffer's byte from an earlier, longer datagram is read.
- **Effect:** with a 0x20 left there, the game is handed a CLIENT_ACCEPT with nothing past its op
  (Plr_CreateFromJoinData on whatever the buffer held); a stock client's CLIENT_ACCEPT is always
  longer, so only a crafted packet gets there.
- **Ours:** not reproduced: ServerTransport::takeAccept wants a byte 4.

### OB-169: A map's load tells the match timer before setting it
- **Where:** the map change (a network game, not a chat map): Game_SetTimerRunning(a player in
  play && the timer's check) sends SrvNet_SendTimerStatus(0x9F, running), whose remaining time is
  the time left (g_gameTimerEndTicks less now); the new map's end is set only after it.
- **Effect:** the load's MSG_TIMER_STATUS (held for each CLIENT_READY) carries the old match's time
  left: after a match lost on time none or less, after a load in mid-match what was left of it. A
  client takes a 0xD3 only within 30 frames of its own frame (CliNet_OnPacket 0xD3), so one that
  loads the map in under a second (a local one) shows that time until a later 0xD3; a slower one
  drops it.
- **Ours:** not reproduced: loadMapKeeping sets timerEnd before setTimerRunning, so the new limit's
  time.

### OB-224: An obelisk feeding a staff and its holder at once skips its charge steps
- **Where:** updateObelisk, for each player in reach: a staff in hand under full charge takes 1 of
  the obelisk's mana (a network game), then the player takes 1 if not full; the charge is looked at
  once, after both, and the clients are told (unitNeedSync, MSG_OBELISK_CHARGE) only when it is a
  multiple of 8. Taking 2 a frame from an odd charge, every value looked at is odd.
- **Effect:** a wizard or conjurer short of mana with a staff recharging, standing at an obelisk
  that isn't at an even charge (one refilled to an odd value, or another player's draw), drains it
  with no change to its look until the last point, which the staff takes alone: it then shows empty
  at once. From an even charge every step shows.
- **Ours:** the same (`noxcore/src/server/objects.cpp`, Objects::obeliskUpdate).

### OB-230: A host quitting waits for every player's MSG_SERVER_QUIT_ACK with no limit
- **Where:** the host's quit (Game_BeginQuit): the acks awaited are every player but the host's own,
  those not yet in the game too (they are never sent the message: it goes only to players in the
  game); each MSG_SERVER_QUIT_ACK counts one down (from anyone, quitting or not), and only a count
  reaching exactly 0 finishes the quit. A player leaving or timing out meanwhile doesn't count.
- **Effect:** a player still loading or downloading the map, or one that drops before it answers,
  leaves the host in its "Shutting down server..." box (no buttons) for good; an unasked ack sent
  earlier makes the count end at -1 and never finish either.
- **Ours:** the count is the same (noxcore/src/server/game_host.cpp, GameHost::beginQuit and
  the 0xC8 case), but noxsdl keeps the leaving host in the background and closes it after 5 s
  whatever the count (vanilla_deviations.md, VD-33), so the host's player is never held.

### OB-273: The path finder's obstacle marks have no bounds check
- **Where:** the monsters' path finder marks the obstacles on its 256 x 256 tile grid (each
  obstacle's tiles, each hole, transporter and elevator's tile, each roaming waypoint's tile).
  The grid is one block, column after column (a tile's entry is 256 times its column plus its
  row), and nothing checks the tile is on the map.
- **Effect:** a mark for a tile just past the grid's top or bottom edge lands at the other end of
  the next or the previous column (row 256 marks the tile (x + 1, 0); row -1 the tile (x - 1,
  255)), so a path there is blocked where nothing stands. Further out it writes past the grid
  into whatever memory follows. Only an object at the very edge of the map, or off it, reaches
  either.
- **Ours:** the first kept (the same tile marked); a mark past the grid's ends is not written
  (`markedTile` in `noxcore/src/server/pathfind.cpp`).

### OB-344: A frozen player vanishes from the other players' screens (fixed, by decision, in games with more than one player)
- **Where:** a player is frozen (unitFreeze: the object's NO_UPDATE flag) while it talks to an NPC
  (the script's StartDialog, SrvNet_OpenNpcDialog), while it trades at a shop (Trade_StartSession)
  and while a script holds it (Frozen, a cinematic's). The frame's update of the units skips a
  NO_UPDATE object's update function; a player is marked as needing to be sent to the others
  (unitNeedSync) only by its own update (updatePlayer), every frame, so a frozen player is sent to
  nobody. (Monsters are marked by the frame itself, frozen or not.) Every client stops drawing
  another player or a live creature it hasn't been sent for 5 frames (it takes it that the server
  lost sight of it).
- **Effect:** in a game with several players, the others see a player disappear about a sixth of
  a second after it starts talking to an NPC, opens a shop or is held by a script, and come back
  when it's free; in a cinematic that holds everyone, each player sees only itself. Its own screen
  is unaffected. The dialogs and cinematics were made for the single-player game, where nobody
  else is there to see it.
- **Ours:** fixed by decision (2026-10-26) for games with more than one player in
  them (a quest, the co-op campaign): a frozen player is sent each frame as a standing one is, so
  the others, stock clients too, keep drawing it (`Server::updateWorld` in
  `noxcore/src/server/server.cpp`). A single-player game, and any game with one player in it, is
  unchanged, as are the arena modes.

## Client

### OB-170: The quest's screens: a wrap from the wrong corner, a column from two axes, a line measured in another font
- **Where:** the quest's start screen wraps QuestSplash4b ("and you will reappear there...") at
  (the screen's top corner - x) plus 630, the left corner's place taken by the top's
  ((height - 480) / 2 for (width - 640) / 2); it measures "Click to Continue" in the default font
  and draws it in the window's. A level's results screen takes a column's width as the second
  place's x less the first's y (235), where the name's limit uses the two x's (170).
- **Effect:** above 640 x 480 the soul gate's line wraps (width - height) / 2 sooner (64 at
  1024 x 768); "Click to Continue" on the start screen sits right of centre; the results' names
  and numbers may run 65 past their column before they wrap (names are cut short of it anyway).
- **Ours:** Cinematics::drawQuestStart / drawQuestResults keep the font and the column; noxsdl
  draws the 640 x 480 layout scaled (vanilla_deviations.md, VD-08), where the corners agree.

### OB-171: A solo warrior's sword as the clients dress it: Material1 over ArmorQuality1
- **Where:** Cli_ResetPlayerDefaultOutfit, a solo game's warrior: the sword's modifiers start
  0xFFFFFFFF, ArmorQuality1's id goes into the first byte and then Material1's into the same
  first byte, where the server's sword has ArmorQuality1 first and Material1 second (the
  server's outfit table).
- **Effect:** a client's picture of a solo warrior's starting sword has its material in its
  quality's place and no material, till an equip report replaces it.
- **Ours:** client::World::resetOutfit writes the same {Material1, 0xFF, 0xFF, 0xFF}.

### OB-172: The Advanced window's weapon and armour boxes: bits counted, not lines
- **Where:** objlst.wnd's checkboxes: box k shows bit (first line shown + k), stepping past bits
  0, 1, 4 and 5 (0x33) and hiding the box once the bit passes the lines' count; a click changes
  the bit of the line beside the box, by its label.
- **Effect:** the weapons from the top are right (their lines are bits 2, 3, 6...), but scrolled
  each box shows the state of a bit a line or more on; the last weapons (bits 23-26) never get a
  box; the armour's boxes (its lines are bits 0-25) show the wrong items' states from the first line.
- **Ours:** ServerOptions::objectChecks counts the same way; clicks change the line's own bit.

### OB-173: The Advanced window's rule sets by their short names
- **Where:** the rule sets' list takes maps\<map>\*.rul by cAlternateFileName, cutting 4 off its
  end.
- **Effect:** where Windows keeps no short name (a name already 8.3, or 8.3 names off) the line is
  blank and Save / Load / Delete on it use ".rul".
- **Ours:** noxsdl lists the long names (vanilla_deviations.md, VD-10).

### OB-174: Line of sight: who hides whom is by the occluders' middles
- **Where:** drawBlack's sweep: two occluders' angle intervals that overlap are resolved by the
  squared distance from the eye to each one's middle (a wall arm's middle 5 off its cell's
  centre), not by where they actually are along each ray.
- **Effect:** a long occluder behind a short one whose middle is further away can win the angles
  and hide it (or the reverse); at junctions it decides which arm is seen. Mostly invisible at
  the walls' scale.
- **Ours:** client::SightRegion resolves overlaps the same way.

### OB-175: A wall seen as a direction whose variant it lacks isn't drawn
- **Where:** drawWalls draws a wall seen by only some of its arms as the direction they make (by
  a table), with the wall's own variant (byte 2); the client's wall images
  (client_thing_read_WALL) are zero-filled past a direction's variants.
- **Effect:** a junction with a variant the straight direction doesn't have draws nothing while
  it's seen that way (if any map has one).
- **Ours:** MapView's drawWall draws nothing then too.

### OB-176: A flat wall base wholly before a span is drawn from that span on
- **Where:** the wall column test (a wall's columns in the spans), its one-row case (direction
  2's base): the first span the base starts left of moves the left clip to that span's start and
  returns seen, even when the base ends before the span.
- **Effect:** such a wall is drawn from that span's start rightwards though its base isn't in sight.
- **Ours:** client::SightRegion::columns keeps it with `noxsdl walls classic on`; noxsdl's improved
  wall culling (the default, vanilla_deviations.md, VD-05) walks a flat base column by column.

### OB-177: A wall drawWalls skips keeps its sight bits into the next frame
- **Where:** drawWalls clears a wall's seen arms (byte 3) and its seen / front bits (byte 4 &
  0xFC) after drawing it, or when its columns come out empty, but not on its early returns: a
  direction 7 / 9 wall with neither half's base in the spans, an 8 / 10 with neither half more
  than 2 columns in them, any other whose base isn't in them. drawBlack only ORs those bits in,
  so the next frame takes the wall as seen even if the sweep no longer reaches it, with last
  frame's arms added to this frame's (and a stale front bit puts it in the front walls' list).
- **Effect:** a wall reached one frame but cut by the column test can be drawn the next from the
  old bits (a crossing as the direction both frames' arms make), for as long as it keeps failing.
  Rare: a reached wall's base is nearly always in the spans.
- **Ours:** client::SightRegion starts each frame clean (not reproduced, by decision).

### OB-178: A corner seen by one arm is drawn cut through at its hidden half
- **Where:** drawBlack sweeps a corner's arms one by one; seen from in front, an arm that turns
  away lies within the front arm's angles and further off (by the middles), so only the front
  arm makes it in. drawWalls draws one arm seen as the wall's own direction (the direction
  table: -1), and a direction 7 / 9 wall in the columns of its two base halves: the first half
  in the spans, the second behind the front arm, so the image is cut at the second half's start
  (the cell's x + 13).
- **Effect:** the end of a wall run that turns away behind itself, past a door say, shows a pillar
  cut straight down through the middle and no wall after it (the walls going on behind are never
  reached); the floor behind is black. Stock Oasis has one: the DunMirDoor at (3312, 3036), its
  corner (146, 134), seen from (3100, 3150).
- **Ours:** the same with `noxsdl walls classic on` (client::SightRegion). noxsdl's improved wall
  culling (the default, vanilla_deviations.md, VD-05) draws the corner whole: a base hidden only by the
  wall's own arms isn't cut.

### OB-179: A door in a see-over wall line blacks out what's behind it, walls included
- **Where:** drawBlack: a wall whose WALL def has flag 1 off is seen wherever it's in view, but
  drawWalls draws it only in the columns where its base runs inside the spans (the column test);
  a door that isn't a gate blocks sight by its leaf like any wall.
- **Effect:** in a short, see-over wall line (FieldStoneShort and the like) with a wooden door,
  the door casts a black wedge over the floor beyond it, and the see-over walls going on behind
  the door are cut where their base enters it or not drawn at all. Generated Delve levels in the
  Grasslands (FieldStoneShort walls, WoodenDoor doors) show it at every locked door.
- **Ours:** the same with `noxsdl walls classic on`. noxsdl's improved wall culling (the default,
  vanilla_deviations.md) draws the see-over walls behind a sight-blocking object whole; the floor
  behind the door stays dark.

### OB-180: Walls at the screen's bottom are cut where their base leaves it
- **Where:** drawBlack's cells end at the row holding the view's bottom ((top + height) / 23), and
  only its cells' walls are marked seen. The scene draw lists walls 4 rows (and 2 columns) past
  the view, but drawWalls returns at once for a wall not marked. Its column test (against the
  window's height) cuts a base whose lower end is past the bottom at the column of its last row
  on the screen, and fails one wholly below it.
- **Effect:** a wall line running off the bottom of the screen ends in a piece cut straight down
  where its base leaves the screen, and the walls whose base is just below aren't drawn, though
  their images (about 70 units tall) would reach well up into the view: walls pop in at the
  bottom as their base comes onto the screen. The viewport is the window (the Options' screen
  size, draw_setCutSize, only shrinks it), and the HUD covers only its own panels.
- **Ours:** the same with `noxsdl walls classic on` (client::SightRegion's cells and columns).
  noxsdl's improved wall culling (the default, vanilla_deviations.md, VD-05) works the walls out over
  the view grown by the images' reach (4 rows, 2 columns), so they're drawn whole when seen.

### OB-181: The floor's last two screen rows are always black
- **Where:** the spans give each polygon edge a crossing on the rows from its upper end to the
  row before its lower one, and the screen's bottom side is flat: the last row (height - 1) gets
  no crossings. The black fill fills the rows between the first and the last row with crossings
  (both kept) by the spans, but that loop stops before the last such row, and the fill below
  starts at it: rows height - 2 and height - 1 are blacked whole.
- **Effect:** a two-pixel black line under the floor along the screen's bottom (walls and
  sprites are drawn over it).
- **Ours:** not reproduced: noxsdl's mask is drawn over the polygon (vanilla_deviations.md, VD-05, the line of
  sight's drawing), which covers that row.

### OB-182: A wall wholly past a corner of the view blacks the corner off, with a soft edge
- **Where:** drawBlack sweeps every sight-blocking wall cell its grid holds (left / 23 to
  (left + width) / 23, and the rows likewise), and the clip drops an interval whose two points
  both lie off the view. A cell can straddle the view's edge while its arms (say the north-east
  and south-east ones, east of its centre) lie wholly outside it.
- **Effect:** such arms near a corner of the view are nearer than the screen's sides, so they take
  the angles across the corner but leave no points: the polygon runs straight from the one side's
  last point to the next side's first, a shadow edge (two different kinds). The floor in the
  corner past that chord is black though nothing hides it, and with Soft Shadow Edge the ten lines
  run along the chord. Up to a cell's size (about 20 units) at the very corner of the screen, while
  a wall stands just past it.
- **Ours:** the same (client::SightRegion). Related: a wall crossing a screen side has its first
  point clipped onto that side a few units past the side's last point, and the edge between
  them (wall and side: a shadow edge) gets the ten lines along the side, a small dark patch at
  the screen's edge where the wall leaves it.

### OB-183: A BELOW thing or wall hanging told into shadows stays faded out after it comes back
- **Where:** MSG_OBJECT_IN_SHADOWS (CliNet_OnPacket) sets the drawable's lingering mark with its
  seen and told marks; an update (Cli_CreateOrUpdateDrawable) clears only the told mark. Only the
  scene's sort clears lingering, and only for the general list; the BELOW things drawn by their
  DRAW function (the third list) and the wall hangings (the fourth) never pass there. Their draws
  stamp the last in-sight frame only while lingering is clear, and the generic draw fades by it
  while it's set.
- **Effect:** such a drawable, kept after the word (Fade Objects on; or a map object, which the
  word only hides, either way), and updated again, is drawn at the fade's alpha from the frame it
  was last seen: a second later alpha 0, invisible till it's made anew. Needs a non-IMMOBILE
  BELOW or hanging thing that's sent in the update stream and goes behind a wall.
- **Ours:** the same with Fade Objects on (noxsdl's drawScene: the non-general sprites fade while
  lingering, client/fade.hpp). With it off, a map object told into shadows is dropped and made
  anew by its next update (World::remove), so it comes back whole: not reproduced there. (A stock
  server never tells a map object, the client's own CLIENT_PERSIST / IMMOBILE ones: its word of
  shadows, netSendObjects2Plr, skips class 0x20400000.)

### OB-184: The drawable sight test boxes a raised drawable below where it's drawn
- **Where:** the drawable sight test, its box when the eye's line to the drawable is cut: the
  bottom is its position's y plus its z plus the shape's bottom (or a circle's radius less
  ZSIZE's bottom), where the generic draw takes z off.
- **Effect:** a flying or jumping drawable partly off the screen is judged by a box below its
  image; one sunk into the floor by a box above it.
- **Ours:** the same (client::SightRegion::screenBox).

### OB-185: A drawable across a corner of the screen is never seen by its box
- **Where:** the drawable sight test's partly-on-screen case: a box off the left or right gives
  two points down its other side, but one off the top or bottom then replaces them with two
  points along its other side, from its left to its right (inset 3 when drawn).
- **Effect:** for a box over a corner one of those points is off the screen, so its line crosses
  the screen's side: not seen however much of it shows (a tree whose base is beyond a corner pops
  in only once its box crosses one side alone, or its position comes into sight).
- **Ours:** the same.

### OB-186: An inside-out box leaves the sight test's second point unset
- **Where:** the drawable sight test sets its two points only for a side off the screen the way
  it faces (the left past the left, the right past the right, the top above, the bottom below); a
  box whose bottom is above its top (an extreme ZSIZE, or a drawable sunk deep) can count sides
  off without that.
- **Effect:** the first point stays the drawable's position and the second is whatever the stack
  held. Rare: it needs such a box partly on the screen with its line cut (a sinking drawable never
  drawn by the generic draw, say).
- **Ours:** both points at its position.

### OB-187: The particle engine's particles show in the black
- **Where:** the pass over the scene (from the scene draw, after the sorted drawables) draws
  every particle without a drawable of its own (flag 8 clear) with its draw: no sight test, no
  polygon. Only the converge and smoke-ember particles have a drawable ("Particle",
  SIGHT_DESTROY) and go by the drawable sight test.
- **Effect:** a MSG_FX_PARTICLEFX effect on a drawable behind a wall (image fire, sparkles, top
  sparks, pixel rise) is drawn over the black where the drawable itself isn't.
- **Ours:** improved (2026-10-14): a free particle is drawn only where the eye sees its ground
  point (Effects::drawParticles; vanilla_deviations.md, VD-05). It had first been reproduced as it was.

### OB-188: A lasting beam is seen by where its middle was when it began
- **Where:** Cli_AddDurationRay makes the ray's drawable midway between its two drawables as they
  are then; nothing moves it after, and its DRAW draws the beam between the drawables as they are
  now, whole, if the scene's sort keeps the drawable (the drawable sight test at that first
  middle).
- **Effect:** a chain lightning, energy bolt or harpoon rope whose ends move far from where they
  were shows or hides by that old point: drawn through walls while it's in sight, gone while
  both ends are in plain view but it isn't.
- **Ours:** the same (Effects::Ray::mid).

### OB-218: A directional light ignores its anti flag: a cone always brightens
- **Where:** cliLight16, the client's light grid: an all-round light of negative intensity (an
  anti-light) subtracts its colour from the grid, but the cone path (a light whose LightOmni isn't
  all-round) adds its colour whatever the light's anti flag says.
- **Effect:** an anti-light given a cone (in the map or by its thing type) lights its kite instead
  of darkening it. No stock map has one.
- **Ours:** the same (`noxsdl/src/lighting.cpp`, `light_cone.hpp`).

### OB-219: The light grid's triangle filler: thin halves skipped, a doubled row, a top clip that doesn't move on
- **Where:** cliLight16 lights a cone's grid points by filling its kite as two triangles with the
  game's scanline filler. A half of a triangle (above or below its middle point) that covers
  fewer than 2 rows is skipped; the row where the two halves meet is lit twice; each edge's x step
  is divided by its rows plus one (both ends counted); a triangle cut off at the grid's top starts
  its spans at the top edge's first x, not where the cut rows would have taken it; the grid's last
  row is never filled and spans stop before its last column. Triangles wound the other way aren't
  filled, so a half width of 0, or of 180 degrees or more, lights nothing.
- **Effect:** a cone's lit area is a little ragged and uneven: a thin sliver of a narrow or
  far-turned cone isn't lit, one row along its middle is lit twice as bright, and a cone reaching
  past the view's top edge is lit slanted there; a light set to an all-round cone of 180 degrees
  (or a width of 0) lights nothing.
- **Ours:** the same filler, quirks kept (`noxsdl/src/light_cone.cpp`); the per-pixel lighting
  fills the kite exactly (its edges aside).

### OB-220: A light aimed straight up faces 269 degrees
- **Where:** colorLightAimAtTarget turns an aiming light toward its target by acos(dx / distance)
  stored to a float (chopped), times the float 57.2959 (a little more than 180 / pi), 360 less
  that when the target is above, truncated to whole degrees. For a target straight above the
  product is a hair over 90 (90.00018), so 360 less it is a hair under 270 and the light faces
  269 degrees.
- **Effect:** a cone aimed at something directly above it is turned one degree off; more
  generally a target above whose true direction is on a whole degree, or a few ten-thousandths
  of a degree past one, comes out a degree short.
- **Ours:** the same (`noxsdl/src/light_cone.cpp`).

### OB-221: A light set to rotate, sweep or aim never moves without keys
- **Where:** ColorLightUpdateDraw, a light's per-frame update, drops itself for a light none of
  whose colour, intensity or cone keys is non-zero, before it reaches the rotation, sweep and aim
  steps.
- **Effect:** an InvisibleLight with the rotate (0x80), sweep (0x100) or aim (0x40) flag but no
  keys stays as it was loaded. No stock map uses those flags.
- **Ours:** the same (`noxsdl/src/light_cone.cpp`); the map editor's light panel warns of it.

### OB-223: A summoning's spell-bar slot never darkens for want of mana
- **Where:** quickBarWarriorDraw darkens a slot's top by the share its spell's cost exceeds the
  client's mana, the cost read by spellManaCost's ordinary mode: the spell's byte in thing.bin's
  spell list. The summonings (the creature guide's spells) all have 0 there; the server charges
  each from its own per-creature price table instead (15 for a Bat up to 85 for an Ogre Warlord),
  which the client never reads. In the trap tray the slot's own cost is the ordinary one too,
  though the slots before it are taken at their trap prices: Lightning and Energy Bolt (0 in
  thing.bin) never darken there, Mana Bomb darkens by its ordinary cost rather than
  ManaBombTrapCost.
- **Effect:** a conjurer's summon slots stay bright however little mana is left, while casting one
  is refused with "You don't have enough mana to cast that spell."; other spells darken as mana
  runs short.
- **Ours:** NoX Core deviates here, as an approved deviation (2026-10-20; vanilla_deviations.md,
  VD-34): the spell bar reads the summonings' prices from the same per-creature table the server
  charges from (`noxsdl/src/quickbar_mana.hpp`, used by `noxsdl/src/hud.cpp`), so a summon's slot
  darkens and refills as an ordinary spell's does. The trap tray reads the same prices (only
  Summon Bomber can be put in a trap); the rest of the tray is as described.

### OB-343: A warrior's mana key keeps the blue potion from an earlier caster game
- **Where:** guiHealthManaInit sets the mana slot's type (BluePotion) only when it makes the mana
  slot, for a wizard or conjurer, and nothing clears it; the mana key (quickManaPotion) uses
  whatever type is left there.
- **Effect:** in a session that played a caster first, a warrior's mana key uses a blue potion
  from the inventory; the server then does nothing with it (a warrior has no mana to restore, so
  the potion is kept). In a fresh session the key does nothing.
- **Ours:** the warrior's mana key does nothing, as in a fresh session (`drinkPotion`,
  `noxsdl/src/quickbar.cpp`): the stale type has no outcome to keep.

### OB-345: MonsterGeneratorDraw takes an address as a one-shot animation's frame
- **Where:** MonsterGeneratorDraw (the monster generators' draw) picks the frame of the animation its xStatus chooses by
  that animation's kind; for kind 0 (OneShot) it takes the address of the animation's delay byte
  in the draw data, not a frame number, and draws image[that].
- **Effect:** none with the retail thing.bin, whose 45 generators' animations all loop; a
  generator given a one-shot animation would draw from far past its image list (garbage or a
  crash).
- **Ours:** draws nothing for that kind, as for the kinds the exe draws nothing for
  (`pickImage`, `noxsdl/src/thing_art.cpp`).

### OB-346: GlyphDraw takes a glyph's alpha from the low byte of its drawable's address
- **Where:** GlyphDraw (the trap glyph's draw) works out the glyph's alpha only with game flag 2
  set and our own player's drawable known; otherwise it keeps the low byte of its drawable
  argument (a pointer) and draws the glyph at that alpha.
- **Effect:** none in GAME.EXE: game flag 2 (the game has a client of its own) is set as the
  game starts and on every map load, and nothing clears it; and the world is drawn only once our
  player's drawable is known (the screen is left black otherwise), so GlyphDraw never runs
  without either. Reached, a glyph would be drawn anywhere at an alpha from wherever its drawable
  sits in memory.
- **Ours:** without our own drawable (or the flag) the glyph is drawn at full alpha, the alpha
  GlyphDraw gives a GUI drawable and a player under INFRAVISION (`glyphAlpha`,
  `noxcore/include/noxcore/client/state_draw.hpp`).

## Menus

### OB-189: The game list's description box takes a LAN game's last weapons from its map's name
- **Where:** the game list's entry from a SERVER_INFO reply (a LAN / server-list game) copies the
  weapons' u32 from the reply's byte 5 on, where the server put only three bytes (the low ones
  of its weapons word), the map name starting at byte 8 (server_makeServerInfoPacket).
  client_gui_serverInfoBlock walks 27 weapon bits: the last three are the map name's first
  letter's low bits.
- **Effect:** whether the box lists the weapons of bits 24-26 as disabled depends on the map's
  name (an "E" says some are, though they're allowed). Games from XWIS read 0xFF there.
- **Ours:** the same (lobby::listingFromServerInfo, Menus::showGameInfo).

### OB-190: The chat never says a whispered-to user is gone
- **Where:** the chat's handler for a user leaving its lobby means to warn when the last user
  picked for private messages goes (WolUser.c:LostSelection, sound 0xE2), but compares what the
  users list's 0x4014 returns with -1. For that list (it takes several,
  wndListboxProcWithData10) 0x4014 returns a pointer to the picked indices, never -1, where the
  helper beside it rightly reads its first.
- **Effect:** the warning never shows; what's typed next goes to the lobby instead, unannounced.
- **Ours:** Menus::removeUserLine (menus_wolchat.cpp) doesn't warn either.

### OB-191: Clicking a lobby while in none joins nothing and leaves "Updating channel list..." up
- **Where:** the chat window's 0x4010 for the lobby list: with no current lobby (the current
  lobby read as 0) it calls the menu's lobby join with that 0 instead of the lobby clicked; the
  chat's join under it does nothing with 0, but the menu's join still puts up the buttonless
  "Updating channel list..." box.
- **Effect:** after leaving or being kicked from a lobby, clicking one only shows the box (Escape
  closes it); Refresh, which joins one by itself, is the way back in.
- **Ours:** Menus::onChat calls joinLobby(-1) the same way.

### OB-192: A thing over more than four of the map grid's cells is left out of the others
- **Where:** the map grid's link puts an object's collision box in each grid cell (85 units) it
  covers, row by row, through a per-cell link whose node array on the object has four entries
  (with a count of those used): from the fifth cell on, the box isn't linked.
- **Effect:** a box over more than four cells (a thing wider than two cells either way: a big
  elevator, a large monster near cell corners) isn't found by getUnitsInRect or the motion's
  touches in its other cells: a search there misses it, a mover there passes into it.
- **Ours:** the same (MapGrid::link).

### OB-193: A search inside two others finds nothing
- **Where:** getUnitsInRect runs only while its depth count (from -1) is below 1, keeping a stamp
  per depth on each object: a search made in a search's callback works, one more inside that
  does nothing (no callback at all).
- **Effect:** e.g. mapCheck's bodies in the way, asked inside two nested searches, sees none: the
  line counts as clear of bodies.
- **Ours:** the same (MapGrid::inRect's depth).

### OB-194: What a search's callback moves takes the search with it
- **Where:** getUnitsInRect reads a node's next after the callback; a callback that moves the
  object (Obj_SetPosition -> moveUpdateSpecial: unlinked and linked at its new place's list
  heads) leaves that node's next pointing into the new cell's list.
- **Effect:** the elevators, shafts and pentagrams (elevatorFn, fnElevatorShaft, PentagramUpdate)
  that send what they find elsewhere: the rest of that cell's list may be skipped (or another
  cell's walked) that frame.
- **Ours:** the same (the walk reads the next node's index after the callback).

### OB-195: A thing put past the map grid's edge is linked out of the grid
- **Where:** the grid's coordinate-to-cell conversion gives a coordinate below 0 its own value
  (Math_FloatToInt of it, not divided by 85), and the map grid's link links the point node in
  that cell unclamped (the extent cells are clamped): a thing at a negative coordinate or past
  70 x 85 writes past the grid's arrays. moveUpdateSpecial deletes what it relinks there, but the
  new things' pass (unitsNewAddToList) links without that test.
- **Effect:** memory past the grid overwritten; no stock map places anything there.
- **Ours:** the point cell clamped (vanilla_deviations.md, VD-28 "The map grid's point cell is
  clamped"); the rest the same.

### OB-196: A deleted thing is still found by searches that don't test 0x20, this tick and the next
- **Where:** delayedDeleteObject only marks it (0x20); it stays linked in the map grid till the
  deferred deletes are freed at the next tick's end. Several search callbacks have no 0x20 test:
  the death balls' meeting (the met flag set before its own 0x20 test), the hammer and the
  single blow (0x8049 only), damageAround, the earthquake, Inversion, the monsters' circles, the
  clouds, the sentry, the pixies' fellows, the observer's jump and more.
- **Effect:** a death ball meeting one deleted the frame before scorches and goes; a hammer's blow
  lands on a deleted thing (its wear, no miss sound) and the single blow is wasted on one when no
  creature is in reach; a deleted thing is struck, shaken, poisoned, turned or chosen as food.
- **Ours:** the same (World::dying_, the callbacks' tests as the exe's).

### OB-197: A script's CreateMover / GroupCreateMover mover moves nothing
- **Where:** the mover's making (builtins 53 / 54) sets the new Mover's UpdateData object to the
  object, but not its carried extent, which MoverXfer reads and MoverUpdate tests first (zero:
  straight off the updatable list); Move finds a thing's movers by the carried extent too.
- **Effect:** the mover is made, on, its script id returned, but at its first turn it leaves the
  updatable list without moving its object; Move on it or on the object does nothing more. No
  stock map's script calls either builtin.
- **Ours:** the same (createMover leaves `carried` 0).

### OB-198: The ray walk never ends heading below 0 from cell 0
- **Where:** mapTraceRay's banded walks: modf under control word 0x1F3F (rounding chop) gives
  -0.1 / 23 the whole part -0, so heading towards negative coordinates from column (or row) 0 the
  band's leave is 0 and the next x is -0.1 again. A ray from column 0 to x below -23, steeper
  than 45 degrees (or from row 0 to y below -23, shallower), that meets no wall in that column /
  row loops for ever.
- **Effect:** the game hangs. No stock map has a ray's end there (its edges are walled).
- **Ours:** a guard (vanilla_deviations.md, VD-30 "The ray walk ends when a band repeats").

### OB-199: A waypoint outside the map grid is filed past its columns
- **Where:** waypointMapRegister takes the waypoint's cell by the grid's coordinate-to-cell
  conversion unclamped (a coordinate below 0 its own value; 5950 or more past the 70th column)
  and links it into that cell's waypoint list: memory before or past the grid's arrays.
- **Effect:** memory overwritten; the nearest-waypoint search (it clamps its cells) never finds
  it. No stock map places one there.
- **Ours:** filed in no cell (vanilla_deviations.md, VD-31 "A waypoint outside the map grid",
  with VD-28's point cell clamp).

### OB-200: A script with more than 1024 string constants overflows the string table
- **Where:** the script loader reads the ScriptObject's string count and stores each constant's
  text into the script string table, a fixed array of 1024 entries, with no check on the count.
- **Effect:** past 1024 constants the entries are written over what follows the table in memory.
  No stock map comes near it (442 constants at most), but a compiled map could; a map whose
  source is kept in string 0 (the community compiler's habit) has one constant fewer to spare.
- **Ours:** no overwrite: the VM keeps every constant (`script::Vm`'s string table grows), and
  the compiler (`noxcore/include/noxcore/script/compiler.hpp`) refuses a script with more than
  1024 constants.

### OB-201: A string made at run time and kept in a variable names a freed slot after the next handler call
- **Where:** the object handlers' call (scriptCallByEventBlock: triggers, monster events,
  polygons, pickups, generators) frees every script string made at run time at its end and sets
  the string count back to the constants'; the next string made takes the first freed id again.
  A variable still holding such an id (a global, a function's locals, which persist, an object
  variable, a timer's argument) isn't told. Strings made in a timer, a dialog or an entry point
  (MapInitialize and the like) last until the next handler call's end too.
- **Effect:** read after that, the id names an empty slot (a null pointer: a print or comparison
  of it reads address 0) or, once reused, whatever string was made next. No stock map keeps a
  built string (IntToString, FloatToString, a concatenation) in a variable; a custom map could.
- **Ours:** the same ids and reuse (`ScriptHost::callBlock`, `script::Vm::freeRuntimeStrings`);
  an empty slot reads as the empty string.

### OB-202: A once-only or pickup handler put off while a script runs jams the waiting list
- **Where:** a handler set off while a script is mid-way (the script stack not empty: a builtin
  that picks up an item or hurts a generator, say) is put on a waiting list of 32, run after the
  next handler call made with the stack empty. A once-only handler is marked run before that
  test, and an item's pickup handler loses its function right after the call (Obj_TryPickup), so
  when its turn comes the call refuses it and returns without taking it off the list.
- **Effect:** it never runs, and it stays first: every handler waiting behind it never runs
  either, more are added until the 32 are full, and the rest are dropped. A waiting handler's
  object freed meanwhile is still on the list, read through freed memory.
- **Ours:** the same jam (`ScriptHost::callBlock`); a deleted object's waiting handlers are taken
  off the list (`ScriptHost::forgetObject`) rather than read freed.

### OB-203: A handler's result is overwritten by the next waiting handler's call
- **Where:** the handler call returns a pointer to one shared result word, set to 0 at the start
  of every call and to the handler's return value. After its own handler it runs the first one
  waiting (OB-202) through the same call, which sets the word again before the first call's
  caller reads it.
- **Effect:** with handlers waiting, a trigger's deciding handler (the one that says whether a
  touch counts) reads the waiting handler's value or 0 instead of its own: a pressure plate that
  should go down doesn't, or one that should stay up goes down.
- **Ours:** the same (`ScriptHost::callBlock`'s shared result).

### OB-204: A timer, dialog or entry-point function that returns a value stops every handler
- **Where:** a function declared with a return value leaves that value on the script stack when
  it ends. Only the handler call empties the stack; timers, dialogs, entry points and the
  console's script call don't. Left by one of those, the word makes every handler call find the
  stack not empty and put itself off (OB-202), and none runs to empty it (only a dialog or
  entry-point function declared with arguments, called with none pushed, takes words off).
- **Effect:** from then on no handler of the map runs (triggers, monster events, polygons...):
  the first 32 wait, the rest are dropped. No stock map's timer, dialog or entry-point function
  returns a value (1765 timer, 1558 dialog and 294 entry-point uses checked).
- **Ours:** the same (`ScriptHost::call` leaves the word, `ScriptHost::callBlock` waits).

### OB-205: A group name stored without its terminator reads whatever follows it
- **Where:** the map loader's GroupData reader copies a group name's stored bytes into a
  76-byte buffer on the stack and passes that buffer on as the name, never terminating it.
- **Effect:** a name stored without its NUL (the community map editor wrote groups that way) is
  read with whatever the buffer held after it (the last group's name, or older stack bytes), so
  scripts looking the group up by its name may not find it. A stored name longer than 76 bytes
  writes past the buffer. No stock map has either.
- **Ours:** the stored bytes are the name (as if terminated); the map editor's validator warns
  about an unterminated name and refuses one longer than 75 bytes, and renaming a group in the
  editor stores it terminated.

### OB-222: A floor tile stored twice drops the earlier record's edges without freeing them
- **Where:** the map loader's FloorMap reader (version 4) reads each pair record over the pair's
  slots: tileReadOne starts the tile's edge list afresh for every record, so when a later record
  stores a tile again, the edges the earlier record read stay allocated with nothing pointing at
  them.
- **Effect:** a few bytes lost per edge each time such a map loads; the tile itself is the later
  record's, edges and all, as intended. The community map editor stores many pairs twice (a full
  pair, then its even tile again); no stock map stores a pair twice.
- **Ours:** the same reading (each tile from the last record that has it: `FloorMap::readPairs`,
  `World::load`, the map editor's document), with nothing lost.

### OB-206: A monster generator's spawn rate past 4 takes an address as its interval
- **Where:** the monster generator's update looks its slot's spawn rate (0-4) up in a table of the
  gamedata values SpawnRateHighValue ... SpawnRateVeryVeryLowValue; a stored rate past 4 reads the
  table's neighbour, a pointer, as the number of frames between spawns.
- **Effect:** such a generator effectively never spawns. No stock map has a rate past 4.
- **Ours:** the same (the rate kept as the map has it); the map editor offers only 0-4 and its
  validator warns about a rate past 4.

### OB-207: An exit's map name of 80 bytes or more runs over its point
- **Where:** ExitXfer reads the map name's length (u32) and that many bytes into the exit's
  80-byte name buffer with no check; the return point's x and y follow the buffer.
- **Effect:** a longer name overwrites the point (and more past it). No stock map's name is near
  that long.
- **Ours:** our reader keeps the name whole; the map editor's validator refuses a name longer than
  79 bytes.

### OB-208: A quest intro's title or picture of 32 bytes or more runs over the next buffer
- **Where:** the MapInfo reader (version 3) reads the quest intro's title and picture names, each
  a u8 length and that many bytes, into 32-byte buffers with no check.
- **Effect:** a longer name writes past its buffer into the next field. No stock map's is long.
- **Ours:** our reader keeps them whole; the map editor's Map Info panel and validator hold each
  to 31 bytes.

### OB-209: A waypoint stored with flag bit 1 is never found by the nearest-waypoint search
- **Where:** the WayPoints reader keeps the stored flags word whole. Bit 1 is the waypoint's own
  "registered in the map grid" mark (waypointMapRegister sets it and skips a waypoint that has
  it), so a waypoint whose map record has it set is never put in the grid's lists.
- **Effect:** the searches that walk the grid (the nearest waypoint to a point) never find it;
  links and scripts by id still do. The game's own save writes only bit 0, and no stock map has
  bit 1.
- **Ours:** our loader takes bit 0 (on or off) and registers every waypoint; the map editor shows
  bit 1 as runtime-only and its validator warns about it.

### OB-210: Object variables have no storage: a script that uses one crashes the game
- **Where:** a compiled script's function 0 lays out "object variables", words each object is
  meant to carry for the script, and the interpreter reads and writes them through the trigger
  object's script-variable pointer (the object whose handler runs: the script's `self`). Object
  creation sets that pointer to null and the object's free releases it if set, but nothing ever
  allocates it, nor does any save or load touch it. A load or store of an object variable
  therefore reads or writes near address zero; so does any call with no trigger (timers, entry
  points, polygons), where the object itself is missing.
- **Also:** were the words there, two more slips in the interpreter: the first object variable
  sits at offset 0, which a store negates to 0, so its stores go to the running function's own
  first local instead; and reading an object array's element through the index instructions
  reads below the object's words (at minus the offset minus the index), where a store through
  the element's address writes the right word.
- **Effect:** the game faults the moment such a script runs. No stock map has an object
  variable, so none is affected; a community map compiled with them could never have worked.
- **Ours:** the interpreter's addressing is the same (offset 0 included), and the host keeps each
  object's words as that addressing implies (0 until written, gone with the object, never
  saved) instead of faulting; with no trigger, reads give 0 and stores are lost. Our compiler
  supports object variables for NoX Core maps, warns that the game crashes on them, lays them out
  past a one-word pad so none sits at offset 0, and reads array elements through the element's
  address. They are off in the compiler and the map editor unless enabled (vanilla_deviations.md,
  VD-32).

### OB-211: An array store one past the end is let through
- **Where:** the interpreter's index instructions check the index against the array's length:
  the integer read ends the function at an index equal to the length, but the float read and
  the element-address instruction (every array store) only past it.
- **Effect:** `a[N] = v` on an array of N writes the word after the array (the next variable, or
  past the function's words for its last one) and the function goes on; `f[N]` on a float array
  reads that word. None of the stock scripts' 1772 constant indexes is past its array (indexes
  computed at run time weren't traced).
- **Ours:** the same (the element-address instruction took the integer read's check until now).

### OB-212: A script string join longer than 255 characters overruns the interpreter's stack
- **Where:** the script interpreter joins two strings (`+` and `+=` on strings) with sprintf
  "%s%s" into a 256-byte buffer on its own stack, with no length limit. A string id with no
  string behind it (a run-time string freed at a handler call's end, OB-201) is a null pointer,
  which the C library prints as "(null)".
- **Effect:** a joined text of 256 characters or more writes over the interpreter's saved
  registers and return address: the game crashes when the script function returns. No stock map
  joins anything near that long.
- **Ours:** the joined text is cut to 255 characters (`noxcore/src/script/vm.cpp`); a missing
  string joins as "(null)" too.

### OB-400: A map script that loops for ever hangs the server
- **Where:** the script interpreter (script_callByIndex) runs a function's instructions until it
  returns, with no count of what it has run: a loop that never ends never gives the server's frame
  back.
- **Effect:** the server hangs, and every player in the game with it, as soon as the looping
  function is called (at the map's start, from a timer or from a handler). A custom map carries
  its own script, so a host that loads a broken or hostile one hangs. No stock map loops for
  ever: their largest call runs about 2,100 instructions.
- **Ours:** NoX Core deviates here, by decision (vanilla_deviations.md, VD-41): a call is stopped
  after 1,000,000 instructions, and a frame's calls after 2,000,000 between them
  (script::Vm::kMaxSteps, kMaxFrameSteps), and the stop is logged.

### OB-401: Script calls nest without a limit, a handler a builtin sets off included
- **Where:** a script function's call to another (and to itself) is the interpreter calling
  itself, with no depth limit. A builtin that sets off an object's handler there and then (Damage
  on a generator runs its onHurt at once) calls the interpreter again from inside the running
  call when the script's value stack is empty, again with no limit.
- **Effect:** a function that calls itself without end nests until the server's stack runs out: a
  crash. So can a handler that sets itself off again (a generator's onHurt that damages its own
  generator nests once per hit while its health lasts). No stock map does either.
- **Ours:** fixed, by decision (a crash a hostile map could cause; vanilla_deviations.md, VD-41).
  The whole top-level call is stopped when its script functions nest more than 256 deep
  (script::Vm::kMaxDepth) or the handlers its builtins set off nest more than 32 deep
  (script::Vm::kMaxNested); a handler goes on from the depth it was set off at, so the count is
  the whole call's. At both limits the nesting takes about 128 KB of stack in a release build, well
  inside a server thread's 1 MB. The stop is logged once per function and reason on a map.


### OB-213: Random draws only 0 - 32767 above its low bound, and from above it when the bounds are reversed
- **Where:** the script builtin Random(lo, hi) is the game's random-table draw: lo plus a table
  entry (0 - 32767) modulo hi - lo + 1, in 32-bit arithmetic; when that span is 0 (hi = lo - 1)
  it gives hi without drawing.
- **Effect:** a range wider than 32768 never yields more than lo + 32767 (Random(1, 100000) stays
  at or below 32768). With lo above hi the span is negative and the remainder of the
  non-negative entry is positive: the result runs from lo up to lo + (lo - hi) - 2, outside the
  two bounds (Random(5, 1) gives 5, 6 or 7). Every stock call has lo below hi and a span under
  32768.
- **Ours:** the same results (`ScriptHost::builtin`, Random).

### OB-214: A script's float comparison with a NaN is true for ==, < and <=
- **Where:** the interpreter's six float comparisons test the FPU's compare flags with patterns
  that don't separate "unordered" (a NaN on either side) from "equal" and "below": ==, < and <=
  test flags a NaN sets, !=, > and >= test flags it clears.
- **Effect:** with a NaN (0.0 / 0.0, or a float variable holding a NaN's bits) `x == y`, `x < y`
  and `x <= y` are true and `x != y`, `x > y` and `x >= y` false, whatever the other side. No
  stock script makes a NaN.
- **Ours:** the same verdicts (`noxcore/src/script/vm.cpp`).

### OB-215: GetHolder on an object nobody holds reads through a null pointer
- **Where:** the script builtin GetHolder(object) reads its holder's script id without checking
  that it has a holder.
- **Effect:** called on an object lying on the ground (or a player, a monster...), the game reads
  address 0 and crashes. No stock map calls it.
- **Ours:** 0 for an object nobody holds (`ScriptHost::builtin`, GetHolder).

### OB-216: ResetQuestStatus's wildcards: a '*' in the middle only works right after the ':'; long names overflow
- **Where:** ResetQuestStatus(name) scopes a name without a ':' to the map ("Map:name"), then:
  without a '*' it removes that quest variable; "*:*" removes all; a '*' last removes every name
  starting with what comes before it (case ignored); a '*' first removes every name in which
  the rest is found, at its first appearance (case kept), at the name's end. Any other '*' is
  read as if the name were "Scope:*Suffix": it compares up to the first ':' (case ignored), then
  skips the one character after the ':' and looks for everything after that character (the '*'
  included if it wasn't there) in the variable's name from the same place. The quest variables
  themselves keep their names in 132-byte records, copied with no length check.
- **Effect:** "Map:Door*Open" (a '*' that isn't right after the ':') removes nothing, unless a
  name holds that text with the '*' literally. "*:Talked" misses a variable whose name has
  "Talked" earlier as well as at its end. A quest variable's full name of 132 characters or more
  writes past its record. No stock map resets a quest variable.
- **Ours:** the same matching (`noxcore/src/server/quest_status.cpp`); a name of any length is
  kept whole.

### OB-239: A harpoon flying at a target exactly where it is divides 0 by 0
- **Where:** a harpoon in flight with a target turns its velocity straight at the target each
  frame: the difference to the target divided by its length, times its speed. Nothing checks the
  length first.
- **Effect:** with the harpoon exactly at the target's centre (both differences 0) the division is
  0 / 0: the velocity becomes not-a-number, and so does the harpoon's place from then on. It
  needs the harpoon to reach the very centre without touching the target first: rare.
- **Ours:** the velocity is left as it was for that frame (`Missiles::harpoonUpdate` in
  `noxcore/src/server/missiles.cpp`).

### OB-265: A box against a wall's half line is pushed by it half a cell past the wall's end
- **Where:** the motion pushes a box (a crate, a block, a table...) off the walls in the cells
  under it, each wall taken as up to two lines along its cell's diagonals. A wall that ends in
  the middle of its cell (a corner or a T piece) has a half line there; the game's table gives the
  half lines that start at the cell's middle the length of a whole diagonal, so they run on half
  a cell past the cell's edge. (The half lines that start at the cell's edge are right.)
- **Effect:** a box over that cell and the next is pushed off a line that isn't there, half a
  cell past the end of the wall's arm, where the next cell holds no wall carrying on: it stops
  short of the opening beside the arm's end or is turned aside there.
- **Ours:** the same (the game's table, `kBoxWalls` in `noxcore/src/server/physics.cpp`).

### OB-266: A box pressed against a wall from one side keeps its speed into the wall
- **Where:** a box that overlaps a wall line is pushed out of it and has its speed into the line
  taken off. The normal it uses is the line's positive one whichever side the box is on (the
  depth it tests for the side is never negative there).
- **Effect:** from the line's other side (north-west of a / wall, north-east of a back-slash
  wall) the speed taken off is the box's speed away from the wall; its speed into the wall stays,
  and only the push (100 x its depth, damped) holds it out. A crate shoved at such a wall sinks
  about twice as far into it as from the other side, and slides along it differently.
- **Ours:** the same (`Mover::boxWallLine` in `noxcore/src/server/physics.cpp`).

### OB-260: A monster generator with an unknown spawn rate takes its own address as the wait
- **Where:** a generator's spawn rate for the map's difficulty is a kind from 0 to 4 (High,
  Normal, Low, Very Low, Very Very Low), looked up in the game data's numbers. A kind past 4
  falls through the lookup with the generator object's own memory address left where the wait
  belongs.
- **Effect:** the wait becomes a number in the hundreds of millions of frames: the generator never
  spawns in any real game. Only a map saved with a bad rate byte (an editor's) has one.
- **Ours:** a kind past 4 waits the largest wait there is (`generatorUpdate` in
  `noxcore/src/server/monsters.cpp`): never, as the game in practice.

### OB-340: nox.csf's header counts one variant fewer than the file holds
- **Where:** the string table's loader sizes its tables from the header's two counts (labels,
  texts) and then reads every label in the file. Stock nox.csf's header says 5750 texts; the file
  holds 5751.
- **Effect:** none in practice: the allocation is rounded up to 16 entries, so the one text past
  the count lands in the slack. A file with more texts past the header's count than that writes
  past the table.
- **Ours:** NoX Core's reader keeps every text whatever the header says. The Content Editor's
  writer keeps the stock header as it is and counts every text it adds or takes away
  (`CsfDocument::write`), so an exported nox.csf never says fewer than it holds beyond stock's one.

### OB-341: The game folder's loose audio files play as raw PCM whatever their format
- **Where:** a sample whose `audio\<name>.wav` exists in the game folder is read from it instead
  of Audio.bag, its data taken as plain 16-bit PCM.
- **Effect:** a .wav in IMA ADPCM or MP3 plays as noise.
- **Ours:** decoded by its own format (PCM, IMA ADPCM or MP3); one that won't decode is passed over
  for the bag's sample (`AudioBag::decode`). The Content Editor says only PCM ones work in a stock
  player's audio folder.

### OB-342: A text message of 256 characters or more overflows the client's stack
- **Where:** the text message (MSG_TEXT_MESSAGE) carries its length, with the end, in one byte; a
  client copies the text into a fixed buffer on its stack.
- **Effect:** a host that sends a text of 256 or more characters (a map script's Print or Chat, a
  long server line) crashes every client; at 255 the length byte wraps and the rest of the packet
  is misread.
- **Ours:** the server cuts every text message at 254 characters (`World::textMessage`; VD-40).

### OB-370: A message-of-the-day line of 256 bytes or more overflows the client's stack
- **Where:** the client shows the message of the day line by line: it copies each line, up to its
  newline, into a fixed 256-byte buffer on its stack without checking the line's length, then
  widens it into the window's list.
- **Effect:** a host whose motd.txt has a line of 256 bytes or more (no newline in it) crashes every
  client that joins its game, as the message of the day arrives.
- **Ours:** the host's own motd.txt is sent as it is, as the original does. The notice a NoX
  Core-only mod sends a stock player in the message of the day's place is broken into lines of
  at most 255 bytes, on whole UTF-8 characters (`GameHost::onIncomingClient` in
  `noxcore/src/server/game_host.cpp`).

### OB-407: A pentagram keeps its partner after the partner is deleted
- **Where:** the map load links each teleport pentagram to its partner once, as a pointer;
  deleting the partner frees its update data and returns the object to the pool, and nothing
  clears the link.
- **Effect:** touched, the pentagram writes the arrival's state into the freed data and sends what
  stands on it to where the deleted partner was, or to the object that reused it. No stock map
  deletes a pentagram.
- **Ours:** the partner is looked up by its number each time, and only a teleporter is taken
  (`partnerOf` in `noxcore/src/server/objects.cpp`); a deleted one is not found and nothing is
  sent.

### OB-408: A map thing outside the client's drawable grid corrupts the client's memory
- **Where:** the client links each drawable it makes, the map's own included, into a 47 x 47 grid
  of 128-unit cells by its position, with no clamp.
- **Effect:** a thing the client loads from the map (an immobile or client-only one: pentagrams and
  lights among them) at a negative coordinate or at 6016 or more reads or writes outside the
  grid's arrays. No stock map places one there.
- **Ours:** fixed by decision (a bad actor could use it). The server's grid is clamped (VD-28).
  The map generator's writer refuses anything placed outside the map's square, 0 to 5888: every
  object and waypoint, an exit's way out, a glyph's target and a hole's fall point. Walls and floor
  tiles must be in cells 0 to 255 (`MapWriter`). Stock maps place nothing there, so they are unchanged.
