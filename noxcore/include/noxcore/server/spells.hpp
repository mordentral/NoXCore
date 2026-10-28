// Spells on the server: a player's MSG_TRY_SPELL to the spell going off (Server/Magic).
//
//   - Asked (Spell_StartCast, from MSG_TRY_SPELL: up to five spell ids, the last byte
//     "on the caster"): not while trading or already speaking one; every spell in its book
//     (PlayerControl::spellLevels: a versus game's all, a quest's or solo game's what it has
//     found) and castable by it (below); then the caster's casting (state 2, CAST_SPELL) and
//     the spell is spoken.
//   - The game mode is the world's (World::gameFlags, World::number): the spells' power is 3 in
//     a versus game and the book's level otherwise, and their numbers are gamedata's SOLO or
//     ARENA ones as the game is solo or networked; nothing else here knows the mode.
//   - Spoken (Spell_UpdatePendingCasts): a phoneme every 3 frames (the server's
//     interval), MSG_REPORT_SPELL_START (0x70) to the caster with the first; once all its
//     phonemes are out, cast (or the next spell of a glyph's).
//   - Cast (Plr_FinishSpellCast): castable (checkPlrCantCastSpell and
//     the class: a wizard's spells need spell flag 0x2000000, a conjurer's 0x4000000, either's
//     0x1000000; warriors none; error 9), its mana (the spell's cost; short: MSG_INFORM 0 0xB),
//     at the caster (on itself) or what's under its cursor (an enemy only, for a hostile spell:
//     flag 0x20), at the cursor (castSpellByUser -> spellAccept).
//     It worked: MSG_INFORM 1 (the spell); it didn't: the mana back. MSG_REPORT_SPELL_STAT
//     (0xDF: the spell, 15, or 0 when it failed) either way; the caster's idle again.
//   - Mana: its class's and level's (makePlayer: an arena wizard's 150, a conjurer's 125), back a
//     point every 300 seconds' worth / its most; its client told (MSG_REPORT_MANA
//     0x45, all of it 0xDE when new).
//   - The spells so far: Fireball (0x1B: a Fireball, StrongFireball or TitanFireball (3-5) by its
//     power; twice the caster's radius ahead, FireballSpeedCoeff x its speed plus the caster's),
//     Lesser Heal (0x29: LesserHealAmount to the target; at full health on itself, the mana
//     back), Magic Missile (0x32: MagicMissileCount at its power (an arena's 4 at 3, solo 1-5),
//     less its own still flying, "mmissile.c:TooManyMissiles" at none; fanned 0, -16, +16, -32,
//     +32 from its facing, its radius + 4 ahead, at 0.8-1.2 x their speed, a tenth of it to start
//     plus the caster's; each after the picked enemy or the nearest within 600 of the cursor;
//     missiles.hpp flies them).
//   - Traps: a list holding Glyph (0x22) builds one (glyphs.cpp).
//   - By hand (player.hpp's gestures): the phonemes go down the phoneme tree (walkPhoneme), and
//     the pattern's end casts the spell at the node reached (finishCast); a book's spell walks
//     the same tree, and Plr_FinishSpellCast reads its spell there too.
// Not yet: the other spells, a conjurer's Bomber, spells that fly to their target (flag 4), the
// counterspell's and the anti-magic's checks, the flag / ball / crown holders' limits on what
// they may cast, the summons' limits, quest games' spell levels.
// Sounds: the phonemes (to everyone else), the spells' own sounds, the "can't" sound
// (0xE7 / 0xE8).
#pragma once

#include "noxcore/server/world.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <random>
#include <vector>

namespace nox::server {

struct Spells {
    World& world; ///< its game flags and gamedata: the mode the spells play by
    const things::ThingDb& things;
    const things::ModifierDb* modifiers;
    std::minstd_rand& rng;
    std::uint32_t frame;
    /// The spell's own function called by itself (an item's effect: item_effects.hpp), not
    /// through spellAccept: no "can't" sound when it fails.
    bool direct = false;

    /// MSG_TRY_SPELL as the packet handler takes it (SrvNet_OnPacket): a lone hostile spell with
    /// a friend under the cursor (out of a quest) isn't begun at all; a lone spell that can't
    /// begin is reported failed (MSG_REPORT_SPELL_STAT 0).
    void request(ServerObject& caster, const std::array<std::uint32_t, 5>& spells, bool targetSelf);
    /// Spell_StartCast: starts speaking `spells` (the non-zero ones counted; that many from the
    /// first); whether it began.
    bool start(ServerObject& caster, const std::array<std::uint32_t, 5>& spells, bool targetSelf);
    /// The frame's speaking, for every player speaking a spell.
    void update();
    /// The spell going off (spellAccept's spells): at `target` (may be null) and
    /// (x, y), from `source` (the exe's a4: a glyph that went off, a spell projectile; null, the
    /// caster itself); `caster` is its a3 and, unless `firstCaster` is given (a turned spell
    /// projectile's first caster, a2: a lasting spell's record source), its a2. Whether it worked.
    bool cast(std::uint32_t spell, ServerObject& caster, ServerObject* target, float x, float y, int power = 0,
              ServerObject* source = nullptr, ServerObject* firstCaster = nullptr);
    /// A spell of flag 4 cast at another (createSpellFly): a Magic projectile,
    /// the caster's radius + 4 ahead (if no wall's between, ray flags 5) along its facing at its
    /// speed plus the caster's, carrying the spell at its caster's power to `target` (none: the
    /// nearest for the spell's flags within 600 of a player's cursor); the caster's INFRAVISION
    /// on it.
    void flySpell(ServerObject& caster, ServerObject* target, std::uint32_t spell);
    /// Why it can't cast `spell` (checkPlrCantCastSpell and the class check):
    /// 0 it can, else the MSG_INFORM error. For a trap's spells (`trap`) the flag,
    /// ball and crown holders' limits don't count.
    int cantCast(const ServerObject& caster, std::uint32_t spell, bool trap = false) const;
    /// checkPlrCantCastSpell alone (cantCast without the class check).
    int cantCastSpell(const ServerObject& caster, std::uint32_t spell, bool trap = false) const;
    /// Plr_FinishSpellCast: a spoken spell (by book or by hand) cast. The spell is the
    /// one at its player's node of the phoneme tree (PlayerControl::phonemeLeaf); at the root
    /// nothing's cast; off the tree, at a node of no spell or one not in its book (but Glyph), it
    /// hears "SpellUnknown". Else castable (the class check, then cantCastSpell: MSG_INFORM 0 why,
    /// 0xE7), its mana (short: MSG_INFORM 0 0xB, 0xE8), at its spellTarget and its
    /// cursor (castByUser): it worked, MSG_INFORM 1; it didn't, the mana back. A casting player
    /// idle again; MSG_REPORT_SPELL_STAT (15 when it went off, none then for spell flag
    /// 0x100000; 0 when not).
    void finishCast(ServerObject& caster);
    /// castSpellByUser: `spell` by `caster` at `target` and (x, y): a hostile one
    /// shows the caster (invisibility, invulnerability and an oval shield end); one of flag 4 at
    /// another (or nothing) flies there (true); the rest go off (cast). Whether it worked.
    bool castByUser(std::uint32_t spell, ServerObject& caster, ServerObject* target, float x, float y);
    /// Plr_CastQueuedSpellFirst / Last (the hand's codes 0x1C-0x1E): the
    /// first (or last) of its trap's spells cast at `target` and its cast point
    /// (PlayerControl::castX / castY), or told why not (cantCastSpell, 0xE7); out of the queue
    /// either way. False with none queued.
    bool castQueued(ServerObject& caster, ServerObject* target, bool last);
    /// What `spell` costs it (the exe's price): a summoning its table's (a
    /// player's only), in a trap (`trap`) Lightning, Chain Lightning and Mana Bomb their
    /// gamedata TrapCost, else the spell's.
    int manaCost(const ServerObject& caster, std::uint32_t spell, bool trap) const;

    /// A staff that casts used (WandCastUse, useWandCastSpell): not spent (a
    /// click), not within its wait (less twice its READINESS); its casting flag set, its spell at
    /// power 4 from the user, at the creature under a player's cursor (else itself) and the
    /// cursor; worked: a charge less (but Chain Lightning's and Plasma's staves: theirs as they
    /// go), MSG_REPORT_CHARGES. Whether it was used.
    bool wandCast(ServerObject& user, ServerObject& wand);

    // Glyphs (spells.cpp's glyph part; the client builds a trap as its spells then Glyph).
    /// Spell_CastGlyph: `owner`'s queued trap spells into a Glyph at `source`
    /// (a conjurer's: a Bomber carrying it, with the summons), aimed at (x, y).
    bool castGlyph(ServerObject& owner, ServerObject& source, float x, float y);
    /// A glyph going off (GlyphCollide's, GlyphDie's, the chain's): gone, its
    /// spells cast from it at what set it off (`by`; null: the nearest within GlyphRange), the
    /// glyphs about it set off next frame.
    void glyphGoesOff(ServerObject& glyph, ServerObject* by);
    /// Whether `by` touching `glyph` sets it off.
    bool setsOff(const ServerObject& glyph, const ServerObject& by) const;
    /// What's under its cursor (PlayerControl::cursorObject: combat.hpp's objectAtCursor, each frame).
    ServerObject* cursorTarget(const ServerObject& caster);
    /// The power it casts `spell` at (spellGetPower): 3 in a versus game, else a
    /// player's level of it in its book (an ImaginaryCaster's 1; a monster's own power: not
    /// yet, 3).
    int power(const ServerObject& caster, std::uint32_t spell) const;

    /// A spell that lasts begun: one of the same spell by the same caster ends
    /// (for Chain Lightning and Plasma, one still running is kept instead); its record at the list's
    /// head, its end `frames` on (0: none), its start's sound; its start run (failing: it's
    /// cancelled). Whether it began. `caster` is the record's caster (Shield's: its target),
    /// `source` its source. `noFunctions`: begun with no create, update nor destroy (the
    /// Wall a .plr's walls are raised with, DurationSpell::noFunctions).
    bool startLasting(std::uint32_t spell, ServerObject* source, ServerObject* caster, ServerObject* glyph, ServerObject* target, float x,
                      float y, int power, std::uint32_t frames, bool noFunctions = false);
    /// The frame's lasting spells (spellCastByPlayer): the cancelled finished,
    /// the gone forgotten, each updated, ended at its end or when its update says so.
    void updateLasting();
    /// The lasting spells `caster` cast of `spell` (any summoning for a summoning) cancelled
    /// (spellCancelDurSpell).
    void cancelLasting(std::uint32_t spell, const ServerObject& caster);
    /// The ground shakes (earthquakeSend): MSG_FX_JIGGLE to each player whose
    /// camera (PlayerControl::camX / camY) is within 300 of (x, y), less with its distance.
    void jiggle(float x, float y, int strength);

private:
    /// Whose spell it is: 10 not enabled, 9 not its class's (a player's).
    int whoseSpell(const ServerObject& caster, std::uint32_t spell) const;
    /// checkPlrCantCastSpell as the game has it (cantCastSpell: then a mode's say,
    /// ModeHooks::cantCast).
    int gameCantCastSpell(const ServerObject& caster, std::uint32_t spell, bool trap) const;
    /// Detonate Glyphs and Trigger Glyph.
    bool detonateGlyphs(ServerObject& owner, ServerObject& source);
    bool triggerGlyph(ServerObject& owner);
    void tell(const ServerObject& player, std::vector<std::uint8_t> bytes);
    /// Somewhere to drop a thing within `reach` of `o` (a random way, nearer each
    /// of 64 tries, the first with a clear ray (flags 1); else on it).
    std::pair<float, float> dropSpot(const ServerObject& o, float reach);
    /// The lasting spells' own start (0: begun), update (non-zero: it ends) and end.
    int lastingStart(DurationSpell& s);
    int lastingUpdate(DurationSpell& s);
    void lastingEnd(DurationSpell& s);
    /// A cancelled one gone: its end, its caster idle again.
    void finishLasting(std::list<DurationSpell>::iterator it);
    /// Health up to its most (unitAdjustHP).
    void addHealth(ServerObject& t, int amount);
    /// MSG_FX at a point (netSendPointFx).
    void pointFx(std::uint8_t op, float x, float y);
    /// Drain Mana's target near (x, y).
    ServerObject* drainTarget(float x, float y, ServerObject* caster);
    /// `amount` of `from`'s mana to `caster`; whether any came.
    bool drainMana(ServerObject& caster, ServerObject& from, int amount);
    /// Lightning's target within `range` of (x, y) (spellEnergyBoltSetTarget;
    /// `glyph`: no facing or sight needed).
    ServerObject* boltTarget(float x, float y, float range, ServerObject* caster, bool glyph);
    /// Chain Lightning's next one within `range` of (x, y) from `from`
    /// (lightningCanAttackCheck).
    ServerObject* chainTarget(float x, float y, float range, ServerObject& from, const ServerObject* owner,
                              const std::vector<ServerObject*>& struck);
    /// Chain Lightning's frame (onFrameLightning).
    int chainLightning(DurationSpell& s, ServerObject* caster);
    /// The Wall spell's pieces: a line's shape,
    /// the next cell, and one wall raised (whether the line goes on).
    static int wallShapeFor(int code);
    static int wallCornerShape(int code, int cx, int cy, int x, int y);
    static int wallStepX(int code, int cx, int x, bool back);
    static int wallStepY(int code, int cy, int y, bool back);
    bool raiseWall(DurationSpell& s, int x, int y, int shape);
    /// Its hostile lasting spells end.
    void endHostileLasting(const ServerObject& o);
    /// A teleport's wait (TeleportDelay in a solo game, else the next pass).
    void teleportDelay(DurationSpell& s);
    /// Blink, Swap, Teleport to Mark and to Target going (their updates).
    int teleportSpell(DurationSpell& s, ServerObject* caster, ServerObject* target);
};

/// The phoneme tree (the exe builds it from thing.bin's spells in order: each spell's
/// phonemes a path from the root, the spell at its end, a later spell of the same phonemes
/// taking the node). A node is the phonemes from the root; one step down
/// (updateSpellRelated): off the tree (nullopt) where no spell's phonemes go, or
/// for a phoneme past 0-8; off the tree it stays.
void walkPhoneme(const things::ThingDb& things, std::optional<std::vector<std::uint8_t>>& node, int phoneme);
/// The spell at a node of the tree (0: none).
std::uint32_t phonemeSpell(const things::ThingDb& things, const std::vector<std::uint8_t>& node);

/// A lasting spell cancelled (spellCancelSpellDo): its player told
/// (MSG_REPORT_SPELL_STAT, 15; Chain Lightning's 0), a ray's beam stopped for everyone
/// (MSG_FX_DURATION_SPELL 0x9E: Plasma 8, Charm 9, Chain Lightning's bolts 10, Lightning 11,
/// Drain Mana 12, Greater Heal 13), and it goes at the next pass.
void cancelDurationSpell(World& world, DurationSpell& s);
/// A ray spell's beam shown (`start`) or taken away between its caster and `to`
/// (MSG_FX_DURATION_SPELL 0x9E: Plasma 1, Charm 2, a chain bolt 3, Lightning 4, Drain Mana 5,
/// Greater Heal 6 (from its target); +7 to stop).
void rayFx(World& world, const DurationSpell& s, bool start, std::uint32_t to);
/// One of Chain Lightning's bolts shown or taken away (0x9E 3 / 10).
void boltFx(World& world, const DurationSpell& chain, bool start, std::uint32_t from, std::uint32_t to);
/// The Wall spell's walls `which` picks put back as they were (one by one: the players
/// sent one told, 0x3D or 0x3E; the record gone).
void revertMagicWalls(World& world, const std::function<bool(const MagicWall&)>& which);
/// Each tick, for the host's unit: a .plr's walls (World::pendingMagicWalls) put
/// back on `unit`'s Wall begun without functions: it gets the Wall's and loses its
/// target (so the next map change ends it); each wall made (destructible, the saved
/// material, look, shape and health) or, there, given the saved shape; the list emptied. None
/// found: false, the walls wait.
bool restoreMagicWalls(World& world, const ServerObject& unit);
/// Spells::cancelLasting without the rest (spellCancelDurSpell).
void cancelLastingSpells(World& world, std::uint32_t spell, const ServerObject& caster);
/// Its channelled spells end (cancelAllSpells): Lightning, Chain Lightning,
/// Greater Heal, Channel Life, Drain Mana, Plasma, Oval Shield.
void cancelChannelledSpells(World& world, const ServerObject& caster);
/// The spell an enchantment comes from (getEnchantSpell: the exe's table;
/// 0 for none).
std::uint32_t enchantmentSpell(int enchantment);
/// What a player leaving the game (Plr_OnDisconnect; also going observer and an elimination
/// game's last death: with the game modes) leaves behind goes
/// (playerRemoveSpawnedStuff): a wizard's glyphs (MSG_FX 0x81 at
/// each), a conjurer's creatures with what they carry (MSG_FX 0x81 at each), its
/// missiles. (The other things it owns whose type the exe's table marks, its type
/// record's mark: not yet.)
void removeSpawned(World& world, ServerObject& player);

} // namespace nox::server
