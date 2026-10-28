// The game modes' common ground, as GAME.EXE's server keeps it: teams, players' lessons
// (scores) and deaths, and the scoring of a player's death by mode.
//
//   - Teams (the exe's Team table, team.c: 16 slots). A team is made with an id (1-16, or the
//     lowest free) in the first free slot, its colour that slot (Team_Create).
//     Team_AddMember puts an object in a team (making the team if it's missing;
//     nothing if it's in it already), newest first; on a network host a player (or the flagball's
//     ball) added with sendNet is told to everyone (MSG_TEAM_MSG 0xC4 sub 1 {u32 team, u16 net
//     code, u16 type}), and in a chat game (game flag 0x80) without gameplay flag 2 one asked to
//     is put within 50 of its team's base. Team_RemoveMember (sub 2
//     {u32 net code}; an empty team goes, sub 6, in clan play or with game flag 0x8000, but in
//     CTF, flagball or team KotR, which only lose its clan and name), Team_MoveMember
//     (sub 3 {u32 team, u16 net code}), Team_SetScore (sub 8 {u32 team, u32 score}),
//     Team_Remove (sub 6), Team_RemoveAll (sub 7), Team_RemoveAllMembers
//     (sub 5). A team's description (SrvNet_SendTeamCreate, sub 0): u32 id,
//     u32 clan, u32 score, the coop flag (game flag 0x200), its name's length, colour, use-title,
//     the UTF-16 name.
//   - Lessons and deaths (PlayerControl::lessons / deaths): changeScore /
//     playerSubLessons / playerIncrementElimDeath; each change
//     goes to everyone (SrvNet_BroadcastReportLesson: MSG_REPORT_LESSON 0x4E, u16 net
//     code, u32 lessons, u32 deaths).
//   - A player's death scored (Plr_Die, while anyone plays): an arena
//     (playerUpdateScore): its killer +1 (a team-mate -1, itself -1 and its team
//     the same), the assist +1 unless on the victim's or killer's team; the victim's death
//     counted. King of the Realm: points by who wore the crown (KotRKingKillsPawn /
//     PawnKillsKing / KingKillsKing; a suicide or team kill by the king costs 1). Elimination
//     (playerHandleElimDeath): a suicide -1, a kill +1, the victim's death and its team's count +1; past the
//     lives the victim's spawned things go (with the observers).
// Not yet: the WOL kill matrix, the sudden death countdown (playerIncrementElimDeath's), the
// friends lists and minimap marks refreshed on a team change (SrvNet_SendResetFriends).
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/world.hpp"

#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

namespace nox::server {

struct Items;

/// The mode index of the game's flags (arena, elimination, CTF, KotR, flagball,
/// quest; none: 0).
int modeIndex(std::uint32_t gameFlags);
/// mapToGameFlags: a map's types (MapInfo) as game flags (1 coop 0x200, 2 quest 0x1000,
/// 4 arena 0x100, 0x20 elimination 0x400, 8 CTF 0x20, 0x10 KotR, 0x40 flagball, 0x80000000 chat 0x80).
std::uint32_t mapToGameFlags(std::uint32_t mapTypes);
/// This mode's lesson limit (servGamedataGet): as the host set it and clients are told.
std::uint16_t lessonLimit(const World& world);
/// The lesson limit the game's own checks act on: lessonLimit, or 0 for a mode that keeps its
/// limits itself (World::modeOwnsLimits).
std::uint16_t enforcedLessonLimit(const World& world);

/// The active team of that id (Team_GetByID), or null.
ServerTeam* teamById(World& world, std::uint8_t id);
/// The teams there are (g_teamCount).
int teamCount(const World& world);
/// The players in it.
int teamPlayers(World& world, const ServerTeam& team);
/// Whether `o` is a member of team `id` (Team_IsObjectInTeam).
bool inTeam(const World& world, const ServerObject& o, std::uint8_t id);
/// A new team (Team_Create): id 0 takes the lowest free one; none past 16.
ServerTeam* createTeam(World& world, std::uint8_t id);
/// Team_SetName: at most 20 characters.
void setTeamName(ServerTeam& team, std::u16string_view name, bool useTitle);
/// The team with the fewest players, or null.
ServerTeam* emptiestTeam(World& world);

/// Team_AddMember (see above). `rng` and `frame` for the move to its base.
void addTeamMember(World& world, std::uint8_t id, ServerObject& o, bool sendNet, bool moveToBase, std::minstd_rand& rng, std::uint32_t frame);
void removeTeamMember(World& world, ServerObject& o);
bool moveTeamMember(World& world, ServerObject& o, ServerTeam& to, bool moveToBase, std::minstd_rand& rng, std::uint32_t frame);
void setTeamScore(World& world, ServerTeam& team, int score);
void removeTeam(World& world, ServerTeam& team, bool sendNet);
void removeAllTeams(World& world, bool sendNet);
void removeAllTeamMembers(World& world, ServerTeam& team);
/// MSG_TEAM_MSG sub 0 (SrvNet_SendTeamCreate / SrvNet_SendTeamInfo) and sub 1 for a member
/// (SrvNet_SendTeamAddMember): a joining client's view of the teams.
std::vector<std::uint8_t> teamMessage(const World& world, const ServerTeam& team);
std::vector<std::uint8_t> teamMemberMessage(const ServerTeam& team, const ServerObject& member);

/// A player's lessons up / down (no message: reportLesson).
void changeLessons(ServerObject& o, int by);
/// Its deaths up (playerIncrementElimDeath; no message), and in elimination:
/// once sudden death may come (20 s since it was reset) the observers are shut out (status
/// 0x100), and with fewer players in play than SuddenDeathPlayerThreshold (in team play fewer
/// teams, and a team down to one) its countdown starts (SuddenDeathCountdown seconds,
/// "Settings.c:SuddenDeathImminent").
void addDeath(Fight& fight, ServerObject& o);

/// Elimination's sudden death. Its reset (at the map's start, a player leaving, nobody
/// left in play), status 0x100 cleared. Each frame (updateServer's part): the
/// observers shut out once it may come and nobody may join, and the countdown
/// started as on a death. The countdown run out (checkTimeLimit): game flag 0x4000000, MSG 0x9A
/// {u16 x, u16 y} at each player's camera, 0x246 to each. In it nothing heals
/// (adjustHealth) and players lose a point of health every SuddenDeathLifeTime x 30 / its most
/// frames.
void resetSuddenDeath(Fight& fight);
void suddenDeathCheck(Fight& fight);
void suddenDeathDrain(Fight& fight, ServerObject& unit);
/// The join check for no player: whether a player may join (outside chat, in elimination: before
/// sudden death may come, or with no deaths yet).
bool mayJoin(World& world, std::uint32_t frame);
/// MSG_REPORT_LESSON to everyone.
void reportLesson(World& world, const ServerObject& o);
/// The match's winner to everyone: `op` MSG_REPORT_FLAG_BALL_WINNER 0x56, _FLAG_WINNER 0x57,
/// _DEATHMATCH_WINNER 0x58 or _DEATHMATCH_TEAM_WINNER 0x59; `who` a team id or a net code; won by
/// the time limit or not.
void winnerMessage(World& world, std::uint8_t op, std::uint16_t who, bool byTime, std::uint32_t frame);
/// The kill feed's MSG_INFORM 0x0E: killer, assist, victim (net codes, 0 none), what killed it (a
/// type, or the player's own note) and its kind (1 a monster's).
std::vector<std::uint8_t> killFeedMessage(std::uint16_t killer, std::uint16_t assist, std::uint16_t victim, std::uint16_t weapon, std::uint8_t kind);

/// The scoring of a player's death (Plr_Die's mode part): `killer` its killer's top owner (the
/// player, or a monster), `assist` the assisting player.
void scoreDeath(Fight& fight, ServerObject& victim, ServerObject* killer, ServerObject* assist);

/// More than one player (or, with gameplay flag 4, team) in play, not observing
/// (gamePlayIsAnyPlayers).
bool anyonePlaying(World& world);

/// The match timer: this mode's time limit (minutes); MSG_TIMER_STATUS 0xD3 (u32
/// running, i32 ms left, u32 frame: SrvNet_SendTimerStatus); running or not, told to
/// everyone (Game_SetTimerRunning); started at its limit; run out.
std::uint8_t timeLimit(const World& world);
/// The time limit the game's own timer acts on: timeLimit, or 0 for a mode that keeps its limits
/// itself (World::modeOwnsLimits).
std::uint8_t enforcedTimeLimit(const World& world);
std::vector<std::uint8_t> timerStatus(const World& world, std::uint32_t frame);
void setTimerRunning(World& world, bool running, std::uint32_t frame);
void startMatchTimer(World& world, std::uint32_t frame);
bool timerRunOut(const World& world, std::uint32_t frame);

/// Observers (Plr_GoObserver, playerLeaveObserver_0,
/// Plr_UpdateObserver): status 1, invisible and NO_COLLIDE, a camera of its own that follows a player
/// (Move: the next) or pans to the cursor (past 30, a tenth of the rest a frame); Jump follows
/// the nearest of its creatures within 100 of the camera (none: pans); Action rejoins (its team,
/// a player start; in a quest Action or Jump: out of its warp gate, else one in the quest and out
/// by no exit rejoins where it is). With nobody left in play the timer stops and every score resets. The first
/// returns false when refused (an ability under way, without `force`).
bool goObserver(Fight& fight, ServerObject& unit, bool canJoin, bool force);
/// Nobody left in play (Plr_GoObserver's and Plr_RemoveFromGame's end): the timer stops, every
/// score is reset and told, sudden death reset.
void nobodyLeftCheck(Fight& fight);
/// Team_ResetAllScores: every team's score 0, the clients told (MSG_TEAM_MSG 9).
void resetTeamScores(World& world);
/// A player joining or rejoining play: with teams, one without a team joins the
/// one with the fewest players, told to everyone (moved to its base in a chat game). (Clan play:
/// by its clan, not kept.)
void joinTeam(Fight& fight, ServerObject& unit);
/// Each frame in a chat game with teams: each player in play joins the team whose
/// base (its flag) is nearest, told to everyone.
void chatBases(Fight& fight);
void leaveObserver(Fight& fight, ServerObject& unit);
void updateObserver(Fight& fight, ServerObject& unit);
/// A map's load tick before the map switch: every player's flags dropped
/// near it, every player's status 0x10 cleared, and with gameplay
/// flags 2 and 4, not game flags 0xC000 nor chat, the teams emptied and the players shared out
/// afresh (shareOutPlayers(true)). (The client and WOL have their own.)
void beforeMapLoad(Fight& fight);
/// Sharing out: with `emptyFirst` every team emptied (Team_RemoveAllMembers), then the players in
/// play without a team shuffled (50 random swaps), each to the team with the fewest,
/// told to everyone and put at its base.
void shareOutPlayers(Fight& fight, bool emptyFirst);
/// wndGuiTeamCreate (the host's Use Teams): team play (gameplay flag 4), two
/// teams, each named by the colour of the map's flag of its team (Team_GetColorTitle), its colour,
/// told to everyone (SrvNet_SendTeamCreate), the flag its base.
void createFlagTeams(Fight& fight);
/// toggleAllTeamFlags: every team's base flag on or off.
void setTeamBases(World& world, bool on);
/// The teams of the host's Go (Capture the Flag or Flag Ball chosen): made till there
/// are two, coloured 1 and 2 and named by their colours' titles, told; team play on.
void flagGameTeams(World& world);
/// Team_Rename: its own name now (not its colour's title), MSG_TEAM_MSG 4 {u32 id,
/// wchar16 name[20]} to everyone.
void renameTeam(World& world, ServerTeam& team, std::u16string_view name);
/// Watching a creature (a creature order 1: playerObserveMonster): out of
/// observing (status 1) or watching another first; status 2, its camera on the creature, the
/// observer's update (its unit stands where it is). Move goes on to its next creature
/// (playerObserverFindGoodSlave0: the next alive in its list, else the first;
/// none, back), Action or Jump back to itself; hurt, back too.
void observeCreature(Fight& fight, ServerObject& player, ServerObject& creature);
/// playerObserveClear: watching a creature, back to itself (status 2 off, the
/// camera let go, the player's update).
void observeClear(Fight& fight, ServerObject& player);
/// The creature it's watching (playerGetPossess: status 2's camera), or none.
ServerObject* possessed(World& world, const ServerObject& player);
/// Plr_ClearStatusFlags' host part for status 1 (out of observing): outside a chat game, with
/// others in play and the timer stopped, it starts at the mode's limit.
void observingEnded(World& world, std::uint32_t frame);
/// Elimination's lives (its lesson limit) used up.
bool outOfLives(const World& world, const ServerObject& unit);
/// A dead player out of lives (Plr_UpdateState's dead state, past half a second): an observer
/// already follows the last player in play; else it goes observing (status 0x20, Plr_GoObserver),
/// those following it move on (playerLeaveObsByObserved), and it follows its killer
/// (its last attacker's player, in play) or the last player in play.
void eliminated(Fight& fight, ServerObject& unit);

/// The map-loaded objects of a team join it (createAt's Team_AddMember, in a solo
/// game or with gameplay flag 4; a chat map's flags aside): a team game's teams come of its flags.
void placeTeamMembers(World& world, std::uint32_t mapFlags, std::minstd_rand& rng, std::uint32_t frame);
/// The game's mode for this map (mapReadSetFlags, a network game's part): the
/// map's types (MapInfo: 4 arena, 8 CTF, 0x10 KotR, 0x20 elimination, 0x40 flagball, 0x80000000
/// chat) against the host's choice (`chosen`, mode bits; one the map lacks: its first).
/// CTF wants flags (unteamed players then shared out),
/// flagball the same and its ball, KotR crowns
/// (mapInfoSetKotr: the flags go; crowns by team play); a mode its map can't
/// hold sets none. Not KotR, the crowns go (mapFindCrown). A chat map: chat mode,
/// the timer off, its flags its teams' bases (teamAssignFlags).
void setupMode(Fight& fight, std::uint32_t mapFlags, std::uint32_t chosen);

/// server_checkVictory (each frame, after the units): a team's or a player's lessons at
/// this mode's limit wins (MSG_REPORT_DEATHMATCH_TEAM_WINNER 0x59 / _WINNER 0x58); elimination's
/// check: the last player or team with lives left. Either ends the match (GameOver).
void checkVictory(Fight& fight);
/// Game_OnTimeLimitExpired (each frame): the timer run out ends the match by the mode:
/// CTF / flagball the best team (MSG_REPORT_FLAG_WINNER 0x57 / FLAG_BALL_WINNER 0x56), arena and
/// KotR the best team or player, elimination the fewest deaths; a tie: no winner.
void checkTimeLimit(Fight& fight);
/// Game_OnTimeLimitExpired's winner by the mode: the match
/// over (GameOver), its winner told.
void matchWinner(Fight& fight);
/// After a player goes observing by MSG_SERVER_CMD: elimination down to one in play
/// ends.
void observerLeftPlay(Fight& fight);
/// Plr_RemoveFromGame's world side (a player's unit gone, from GameHost::leave): its slot forgotten
/// (quest.hpp's questForgetSlot); fewer than two in play, in every mode, the match's reset
/// (nobodyLeftCheck); else elimination down to one in play over (observerLeftPlay); a quest's end
/// of it (questAfterLeave).
void playerRemoved(Fight& fight, ServerObject& unit);

/// Capture the flag. A map's Flag (TeamXfer: its team, its material modifier the colour, home
/// where it stands):
///   - FlagCollide: touched by a live player in CTF (pickupFlagCtf).
///     Another team's flag on the ground, its team with players: the toucher drops a flag it
///     carries (within 50), takes this one (out of the world, into its inventory, weapon bit 1,
///     its class told to everyone), whose Blink-like spells end (those of spell flag
///     0x80000); INFORM 6 {u32 net code, u32 colour}, flag status 1. A team without players:
///     "objcoll.c:FlagNoTeam". Its own team's flag away from home (past 5 either way): home,
///     INFORM 4 {u32 net code}, status 0. At home: each flag it carries is captured: its lessons
///     +1 (and its team's), the flag home, status 0, INFORM 5 {u32 net code, u32 colour}; at the
///     CTF lesson limit a team there wins (game over, MSG_REPORT_FLAG_WINNER 0x57).
///   - Dropped (DefaultDrop): INFORM 7 {u32 net code, u32 colour}, status 2, the frame kept;
///     FlagUpdate: 30 s on the ground and it goes home (status 0, INFORM 8 {u32
///     colour}), 0x131 where it lay.
///   - Flag status: kept by team (World::flagStatus) and told to everyone,
///     MSG_REPORT_FLAG_STATUS 0xD8 {u8 state, u8 team, u8 colour, u16 carrier}; a client coming in
///     in CTF is sent each team's.
/// The flags' minimap marks: with the minimap marks.
/// A flag's colour, by its material modifier (MaterialTeamRed 1, Blue 2, Green 3,
/// Cyan 4, Yellow 5, Violet 6, Black 7, White 8, Orange 9); else 0.
int flagColor(const ServerObject& flag, const things::ModifierDb* modifiers);
void flagStatus(World& world, std::uint8_t team, std::uint8_t state, std::uint8_t color, std::uint16_t carrier);
std::vector<std::uint8_t> flagStatusMessage(const World& world, std::uint8_t team);
/// createAt for a thing already made (a captured flag, home): into the world at (x, y).
void placeAt(World& world, ServerObject& o, float x, float y, std::uint32_t frame);
void flagUpdate(Fight& fight, ServerObject& flag);
void flagCollide(Fight& fight, ServerObject& flag, ServerObject* other);
void flagDropped(World& world, const ServerObject& holder, ServerObject& flag, const things::ModifierDb* modifiers, std::uint32_t frame);

/// Flagball. One GameBall, made at a random GameBallStart (at the mode's setup,
/// and again for one lost; the old one deleted, observers following it moved on), its
/// possession time and reset speed from gamedata (FlagballPossDuration, FlagballResetVel).
///   - BallUpdate: drag 0.01. Its last holder gone, nobody's (status 1). Not held
///     for 20 s (the exe's clock: 600 frames), a new one. Loose: collides, and slower than the
///     reset speed it's nobody's. Held: no collisions, held now; past the possession time the
///     holder loses it, pushed (force 30) within 32 either side of its facing (status 1); a dead
///     holder drops it (status 1); else it's carried in front of the holder (its radius, the
///     ball's and 10 along its facing) when the way there is clear (flags 5).
///   - BallCollide: a player touching it loose takes it (the ball in its team,
///     status 2, 4 for a blue team, with its net code), its Haste-like enchantments end, but the
///     one who last had it while its team has two players ("objcoll.c:CantPickupBall", at most
///     every 45 frames). BallBounce 0x3A0 refused (or not a player's), BallGrab 0x39F taken.
///   - Thrown (checkWinkFlags, its Action outside a chat game): pushed away from
///     the holder (force 100), status 1, no attack, BallThrow 0x39E (at the ball when held too long). Knocked loose
///     (gameballOnPlayerDamage) by 30 damage or more: force 30, its last holder
///     the victim, the ball into the attacker's team. Fumble (castFumble) knocks
///     it loose (force 100). Going observer lets it go (status 1).
///   - At a flag (the ball, or a player holding it): its last holder scores when
///     it's of the team after the flag's (the first after the last: the other team's flag): +1 lesson, its team +1,
///     INFORM 9 {u32 net code, u16 team}, MSG_FX 0x9A; the flagball limit wins (0x56); the ball
///     back to a random start (status 0, MSG_FX 0x81). HomeBaseCollide: the ball into
///     a base: its last holder +1 when of the base's team, the base's team +1 (MSG_FX 0x9A), the
///     ball to a random start (MSG_FX 0x81). BallHitGoal 0x3A1 a score (at the toucher, at the base).
///   - MSG_REPORT_BALL_STATUS 0xD9 {u8 state, u16 carrier} to everyone, and to a
///     client coming in. Wall (0x84) can't be cast. The WOL kill record: no.
bool spawnBall(Fight& fight, ServerObject* old);
void ballStatus(World& world, std::uint8_t state, std::uint16_t carrier);
std::vector<std::uint8_t> ballStatusMessage(const World& world);
void ballUpdate(Fight& fight, ServerObject& ball);
void ballCollide(Fight& fight, ServerObject& ball, ServerObject* other);
void ballAtFlag(Fight& fight, const ServerObject& flag, ServerObject& toucher);
void homeBaseCollide(Fight& fight, ServerObject& base, ServerObject* other);
/// The ball it holds (owned), or null.
ServerObject* heldBall(World& world, const ServerObject& unit);
bool throwBall(Fight& fight, ServerObject& unit);
void ballHitCarrier(Fight& fight, ServerObject* attacker, ServerObject& victim, int damage);

/// The respawns (respawnAdd for each thing made while the list is open: the
/// map's; the respawn update each frame, but in a quest or coop game, closing the list). A thing gone
/// (broken, used up), a creature dead, an item (not a weapon, armour, wand or the crown) taken,
/// or with the weapons-respawn option a weapon or armour taken: a new one of its type 30 s on at
/// its spot (MSG_FX 0x81 and 0x11B; a dead creature's body goes then). A weapon, armour, wand or
/// crown lying 5 s since it was put down more than 50 from its spot goes back there (MSG_FX 0x81
/// and 0x11B at both ends; a wand recharged, ammunition and health restored). (Every
/// type may respawn: the host's allowed weapons and armour, the types' dd10, aren't kept.)
/// King of the Realm: the crowns (setupMode keeps them by team play: one each team's crown,
/// or the loose ones).
///   - Taken (Obj_PickupCrown: touched by a live player, CrownCollide; or handed back,
///     below): DefaultPickup's rules (another team's refused), then its frame noted (crownAt),
///     the CROWN enchantment for good, INFORM 0x0A {u32 net code, u32 team} to everyone; who it
///     was going back to forgotten. 0x139 at the taker.
///   - Dropped (Obj_DropCrown): in team KotR a team's crown is to go back to the team's
///     player who took one longest ago (not the dropper); DefaultDrop, the enchantment off,
///     INFORM 0x0B {u32 net code, u32 team}.
///   - CrownUpdate: one to go back to a live player is taken by it; one owned in the
///     world is carried in front of its owner (a dead owner lets go).
///   - A king killed outside team KotR (from the KotR scoring): the crown dropped
///     where it fell, for its killer. Going observer drops it. Respawning or rejoining in team
///     KotR takes its team's crown when nobody holds it; at a map's start each
///     team's crown goes to a random player of the team.
bool pickupCrown(Items& items, ServerObject& who, ServerObject& crown);
bool dropCrown(Items& items, ServerObject& holder, ServerObject& crown, float x, float y);
void crownUpdate(Fight& fight, ServerObject& crown);
void crownCollide(Fight& fight, ServerObject& crown, ServerObject* other);
void dropKingsCrown(Fight& fight, ServerObject& victim, ServerObject* killer);
void takeTeamCrown(Fight& fight, ServerObject& unit);
void crownsToTeams(Fight& fight);

void addRespawn(World& world, const ServerObject& o);
void forgetRespawn(World& world, std::uint32_t extent);
void respawnPass(Fight& fight);

} // namespace nox::server
