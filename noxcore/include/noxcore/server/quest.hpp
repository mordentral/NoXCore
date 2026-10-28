// The quest (game flag 0x1000, the multiplayer gauntlet): its start, its players and difficulty,
// and the awards it begins with.
//   - Started (from mapReadSetFlags when a quest map follows a chat
//     game): stage 0 (a saved quest's kept: not yet), the map list's plays cleared
//     (not yet), MSG_GAUNTLET 0 (level start) to everyone; each player's tallies
//     cleared, in the quest while five or fewer are (questActive;
//     MSG_GAUNTLET 1 to everyone for each one in), made anew (unitInitPlayer:
//     its gold gone, its level 10 in a network game, its spells: none but a quest's own,
//     QuestGameStartingExtraLives lives, its outfit), its gold gone again, its armour but the
//     0x405 kinds and its weapons of subclass 0x8200 gone, its class's item (a Sword; a
//     SulphorousFlareWand with Replenishment1; the conjurer's), observing when not in; team play
//     (gameplay flag 4, at once cleared again by Team_RemoveAll: an original bug), one team
//     (colour 9, its colour's title) with every player in; the stat
//     multipliers kept and the quest's set (health 3, a warrior's mana
//     1 and the others' 3, strength and speed 1), each player's numbers anew and MSG_STAT_MULTIPLIERS.
//   - Difficulty (every 5 s in the server's tick after the respawn pass):
//     ((players in - 1) x PlayerDifficultyDelta + 1) x the stage; changed,
//     the damage players take ((it - 1) x PlayerDamageDiffCoeff +
//     PlayerDamageDiffInit, at most PlayerDamageCap: World::questDamage) and monsters' health
//     (SystemHealth's: World::questHealth), then each monster and generator at full health made
//     its type's health times that (a generator at most GeneratorMaxHealth; a monster a player
//     controls kept).
#pragma once

#include "noxcore/server/combat.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::server {

/// The players in the quest (player: quest-active 1).
int questPlayers(const World& world);
/// the difficulty anew from the players in and the stage.
void questSetDifficulty(World& world);
/// the damage and health multipliers by the difficulty, and the monsters and
/// generators at full health rescaled; not those in `notYet` (made this frame: the exe's new
/// objects aren't on its list till the next frame's unitsNewAddToList).
void questRescale(World& world, std::span<const std::uint32_t> notYet = {});
/// (a quest's, each frame): every 5 s the difficulty anew; changed, rescaled.
void questDifficultyTick(Fight& fight);
/// the quest begun.
void startQuest(Fight& fight);
/// the multipliers kept at the quest's start put back, each player's numbers anew
/// (and with `tell`, MSG_STAT_MULTIPLIERS).
void endQuestStats(World& world, bool tell);
/// MSG_STAT_MULTIPLIERS (SrvNet_SendStatMultipliers: 0xEF, its class's health, mana,
/// strength and speed multipliers as floats) to a player.
void sendStatMultipliers(World& world, const ServerObject& unit);

/// The quest's maps (the exe's list, 0x20 bytes each): every map of the game's map
/// list with the quest type (0x1000), at most 0x80, in its order; each with its group (maps whose
/// names' first 6 letters match, case aside, share one: 1, 2... in order of first), its plays and
/// the play it was last on (the plays' count, from 1000).
struct QuestMaps {
    struct Entry {
        std::string name; ///< "<map>.map"
        int group = 0;
        int plays = 0;
        int last = 0;
    };
    std::vector<Entry> maps;
    int playCount = 1000;
    int current = 0; ///< the map last played's index

    /// mapSelectFirst: from the map list (names, without ".map", with their map
    /// types' game flags), the quest's maps.
    void build(const std::vector<std::pair<std::string, std::uint32_t>>& mapList);
    /// (the quest's start): every map unplayed, the count from 1000.
    void clearPlays();
    /// (a quest map's load): the map (by name, case aside) played, and each other map
    /// of its group; the count up one.
    void played(std::string_view map);
    /// getQuestMapFile: the next quest map. None without maps, the one when one; else
    /// at random among those played fewer than the most (or, every map played as often, all of
    /// them), not the last one or its group, not played in the last 4 plays.
    std::optional<std::string> pick(std::minstd_rand& rng) const;
};

/// The map load's quest part (a quest map loaded): the map's difficulty (0-2 at random), the stage
/// up one, its generators and bosses, the difficulty anew,
/// the map's rewards (not yet), the map played, the warp gates on
/// (off from WarpGateCutoffStage); the warp state cleared.
void questMapLoaded(Fight& fight, std::string_view mapName);
/// a quest map's generators by the difficulty `difficulty` (one with no monsters for
/// it deleted; its most by gamedata, doubled from QuestHardcoreStage but a singular one's; its type
/// the generator of its first monster's kind), one quest exit (class Exit, subclass 1) kept of the
/// map's at random, and from stage 5 the bosses (Hecubah at one of the
/// HecubahMarkers, Necromancers at half the NecromancerMarkers) on stage 5, from
/// MinionsAlwaysStage, or on odd stages one time in two; the markers deleted. The bosses made.
std::vector<std::uint32_t> questGenerators(Fight& fight, int difficulty);

/// server_questNextStageThreshold: the stage a warp gate leads to (5, 10, 15, 20; from
/// 20 the stage itself).
int questNextStageThreshold(int stage);
/// server_questMaybeWarp: every player in the quest waits in a warp gate and one of them
/// has reached the next threshold's stage before.
bool questMaybeWarp(const World& world);
/// every player in the quest has gone out by a quest exit.
bool questAllExited(const World& world);
/// (a player out by a quest exit, the others not yet): the exit's countdown, its
/// seconds QuestExitTimerStart (or what's left of the timer) less the share already out
/// (servStartCountdown with "objcoll.c:ExitCountdown"; MSG_GAUNTLET 0x14, the HUD);
/// not again while one runs at the full time.
void questExitCountdown(Fight& fight);
/// a player waiting in a warp gate put out (its observing over, at the gate's way
/// out, sound 0x138, point FX 0x81): by Game_QuestCheckWarpGate, by the warp gates going off
/// (a soul gate taken, a map past WarpGateCutoffStage) and by its own Action or
/// Jump (Plr_UpdateObserver).
void questLeaveWarpGate(Fight& fight, ServerObject& unit);
/// collideExit's quest part (a player at an exit it may use): the next quest map
/// picked (and kept for the time limit), its abilities ended; a quest exit (subclass 1): the
/// stage reached (MSG_GAUNTLET 0x1D), out as an observer, MSG_INFORM 0x12 to everyone; a warp gate
/// (2): waiting in it as an observer, "objcoll.c:PlayerEntersWarp" (the exe's MSG_INFORM 0x13 to the
/// others is dropped by SrvNet_SendInform: never sent);
/// sound 0x3EB; the countdown when not everyone's out. Everyone out (or the warp taken): the next
/// map (a warp's at the threshold's stage, the tallies cleared).
void questExitCollide(Fight& fight, ServerObject& exit, ServerObject& unit);
/// A quest's frame (updateServer's quest end): the rejoin after a map's load
/// (those observing but ready, in the quest and in no exit, back in play and their
/// team) (5 minutes from the first soul gate taken: the warp window), Game_QuestCheckWarpGate
/// (30 frames after the last went in, everyone waiting in a gate: the warp, else each put out,
/// "Gauntlet.c:WarpRestrictedSolo / Multi") (questForgetSlots). (And a
/// step with the save transfer.)
void questFrame(Fight& fight);
/// Game_OnTimeLimitExpired's quest part: the kept next map ("chklimit.c:AutoExitToNextMap"), those
/// still in tallied, the timer stopped.
void questTimeUp(Fight& fight);
/// Plr_OnDisconnect's quest part (a player in the quest leaving): 0x3F3 at it, out of the quest;
/// nobody left in it, stage 0 and the next quest map (getQuestMapFile). (Its keys
/// given away, Plr_GiveAwayKeysOnLeave, and with the shared keys.)
void questPlayerLeaves(Fight& fight, ServerObject& unit);

/// a quest score, (generators x 10 + secrets x 35 + kills x 0.1) x the stage to the
/// power 1.9, chopped at each float step (the FPU's mode).
std::uint32_t questScoreOf(std::uint32_t generators, std::uint32_t secrets, std::uint32_t kills, std::uint32_t stage);
/// a player's quest score (0 out of the quest; at most 999999999). Alone in the quest
/// its own; else its own times the group's (every player's generators and secrets, no kills, at the
/// stage of the last by index) over the best one's (generators and secrets, no kills).
std::uint32_t questScore(World& world, const ServerObject& unit);
/// MSG_GAUNTLET 0x0C, level won (SrvNet_SendGauntletLevelWon, 90 bytes) for `to`: the last map's
/// secret areas, `to`'s stage (with any entry), then up to 6 players in the quest by index: code,
/// generators, secrets, secrets credited, kills, score.
std::vector<std::uint8_t> questLevelWonMessage(World& world, const ServerObject& to);
/// MSG_GAUNTLET 0x0D (briefing; with `lockScreen` flag 1) or 0x0E (the quest's start), 69 bytes:
/// the stage, flags (1 lock screen, 2 bosses on this map), the map's quest picture and text id.
std::vector<std::uint8_t> questBriefingMessage(const World& world, std::uint8_t sub, bool lockScreen);
/// The map start's quest part (the map's start with players in, instead of the fade): stage 1, the
/// quest's start (0x0E); after a warp the briefing locked (0x0D, 1; the warp flag cleared); else
/// each player its level won (0x0C) and everyone the briefing (0x0D, 0). (Each player's save sent
/// its client first, MSG_GAUNTLET 0x1C: not yet.) Then every player's tallies cleared.
void questMapStart(Fight& fight);
/// a quest's shared silver key, the first SilverKey of the last player (by index)
/// holding one (null: none).
ServerObject* questSharedSilverKey(World& world);
/// a door unlocked in a quest: its marker cleared (World: doorQuestOpened),
/// MSG_GAUNTLET 0x0F to everyone.
void questOpenDoor(World& world, ServerObject& door);
/// Plr_GiveAwayKeysOnLeave: a leaving quest player's keys, with anyone left in the
/// quest, each to the one holding fewest ("GeneralPrint:GainedKey", 0x334); the rest on the ground
/// within 60 of it.
void questGiveAwayKeys(Fight& fight, ServerObject& unit);
/// Plr_RemoveFromGame's quest end (a player gone): everyone left out by a quest exit, the kept next
/// map; everyone left waiting in a warp gate with one past the threshold, the warp (tallies
/// cleared, the warp flag, the stage the threshold's less one, the next map); else, someone out by
/// a quest exit, the exit's countdown anew.
void questAfterLeave(Fight& fight);
/// a player's quest tallies cleared, the stage it joined at the quest's.
void questClearTallies(const World& world, ServerObject& unit);
/// SrvNet_SendTeamHealthPercent (MSG_TEAM_MSG 0xC4 sub 0x0C: its code, health x 100 / its most).
std::vector<std::uint8_t> questTeamHealthMessage(const ServerObject& unit);
/// room in the quest (five or fewer in).
bool questRoomFor(const World& world);
/// What every player was told of the player at slot `index` (its lives, its keys: livesSent /
/// silverKeySent / goldKeySent by index) forgotten (questPlayerJoined's, questForgetSlots', Plr_RemoveFromGame's walk).
void questForgetSlot(World& world, std::uint8_t index);
/// a player in the quest: MSG_GAUNTLET 1 to everyone, its tallies cleared, its slot
/// forgotten.
void questPlayerJoined(Fight& fight, ServerObject& unit);
/// (a quest's frame, last): each slot whose client came in (World::questSlotSeenAt)
/// more than 30 s ago and isn't in the quest forgotten; in it, the mark cleared.
void questForgetSlots(Fight& fight);
/// a map ended with it in (not being deleted, in the quest): the time run out
/// (Game_OnTimeLimitExpired) or its own way out by a quest exit (collideExit).
void questTimeUpTally(ServerObject& unit);
/// a kill to a player's tallies (from monsterCallDieFn, a quest's).
void questKill(ServerObject& unit);
/// a secret area found, to its tallies (and the map's: credited at its change).
void questSecretFound(ServerObject& unit);

/// Plr_Die's quest part: a life gone (a death counted, flag 2); out of lives, game
/// over (MSG_GAUNTLET 2 with its tallies), its tallies cleared, the death's losses, its lives
/// QuestGameStartingExtraLives again.
void questDeath(Fight& fight, ServerObject& unit);
/// a quest death's losses: half its gold; its odd diamond, emerald and ruby each
/// sold for half its worth and half the rest of each gone; its weapon in hand when
/// it has another its class may use (but its class's own plain one); an armour piece
/// worn at random (a warrior's two: not the 0x405 kinds); two spells (a caster's:
/// MSG_GAUNTLET 0x11), two field guide creatures (a conjurer's: 0x13), an ability (a warrior's:
/// 0x12), each at random of those it may lose.
void questDeathLosses(Fight& fight, ServerObject& unit);
/// AnkhCollide (collideAnkhQuest): a player not yet given a life by this ankh (on
/// this map, nor by name, class and serial in its last 64 within 240 s) and under MaxExtraLives
/// gets one (an AnkhTradable picked up; 0x3EC, point FX 0x82, "objcoll.c:AwardExtraLife");
/// else "objcoll.c:ExtraLifeAlreadyAwarded" / "pickup.c:MaxTradableAnkhsReached" with 0x39D, at most
/// once in 1.5 s.
void questAnkhCollide(Fight& fight, ServerObject& ankh, ServerObject* other);

/// SoulGateCollide (a quest's): the warp gates off; the first soul gate
/// any player in the quest takes starts the warp window's 5 minutes; the player's
/// checkpoint this gate (a new one, or a second on: 0x3ED, point FX 0x82,
/// "objcoll.c:SoulGateCollide").
void questSoulGateCollide(Fight& fight, ServerObject& gate, ServerObject* other);
/// a place within 60 of `gate` a teleport may reach (up to 32 tries; else the last).
std::pair<float, float> nearSoulGate(Fight& fight, const ServerObject& gate);
/// (a player joining a quest): the soul gate a player touched last of all of theirs its
/// checkpoint, and a place within 60 of it (false: none).
bool joinAtSoulGate(Fight& fight, ServerObject& unit, float& x, float& y);

/// Spell_GrantToPlayer: a player's spell (1-0x88) up a level (at most 5, a quest's 3;
/// `setLevel` instead when not 0), its group with it (spell flags 0x1000 / 0x4000 / 0x10000: every
/// spell flagged 0x2000 / 0x8000 / 0x20000); refused with "MaxSpellLevel" at the most (3 in a solo
/// or quest game, else 5; a quest's once-only spells past 0), "AwardSpellError" out of range.
/// With `notify`: 0xE2 at it and MSG_GAUNTLET 0x1E (the award's text) to it and the others; then
/// MSG_REPORT_SPELL_AWARD (0x6F: spell, level, `notify` | 0x80 with `show`). False when refused.
bool grantSpell(World& world, const things::ThingDb& things, ServerObject& unit, int spell, bool notify, bool show, int setLevel);
/// A quest's once-only spells (Spell_GrantToPlayer's list: granted once, never a level past 1).
bool questOnceOnlySpell(int spell);

/// noxcpp: the class every player plays here (World::forceClass), if any: a quest's without
/// quest saves only (nothing of it is then written back to a stock client's .plr).
std::optional<std::uint8_t> forcedClass(const World& world);

/// noxcpp: makes a player one of `classId`: its class and its info's (what its MSG_NEW_PLAYER
/// tells: GameHost sends it again), its guides none and its spells the class's start (a quest's,
/// startQuest's; a network game's every one at 3), its numbers the class's, what the class can't
/// use taken off (kept). Nothing when it's of that class already. Where it's done: a run's start
/// (startQuest, before its own resets), a .plr loaded in a forced quest, and back in a game
/// without one (Server's load: its character's own class, PlayerControl::ownClass).
void forceClass(Fight& fight, ServerObject& unit, std::uint8_t classId);

} // namespace nox::server
