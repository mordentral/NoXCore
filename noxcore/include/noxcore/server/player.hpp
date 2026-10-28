// A player's unit on the server: its class's numbers, its control events, and the player update
// (updatePlayer -> Plr_UpdateState) as far as walking and running.
//
//   - MSG_PLAYER_INPUT's events go into its control buffer (SrvNet_OnPlayerInput: dropped whole past 128),
//     then all but the newest MoveForward, MoveLeft and MoveRight are dropped (SrvNet_DedupPlayerCtrlEvents);
//     MSG_MOUSE sets where its cursor is.
//   - Each frame, walking (state 0) or running (5) it pushes itself along its facing at its
//     speed, twice that running; it runs when its cursor is past 100 units, and once running
//     until it stops. Then its events, if any came: a walker stops (idle, 0xD) unless a move
//     keeps it walking or running (from this frame's choice), and Orientation turns it. A frame
//     with no events at all changes nothing (clients send their facing every frame, so a late
//     packet doesn't stop it). Its force comes before the frame's turn, so it sets off a frame
//     after the key.
//   - An arena player's numbers are its class's best (plrReadVals under game
//     flag 0x2000): speed (gamedata <Class>MaxSpeed x 1e-4) and mass (strength / the warrior's
//     best x 20 + 10).
//   - Jump (a frame's events stop at it): 90 stamina (it regains 100 / 30 a frame, up to 100;
//     its client is told what's left, MSG_REPORT_STAMINA), AIRBORNE, and twice its speed along
//     its facing each frame for the JUMP animation's length, deaf to its keys till it lands.
//   - Action: an attack (state 1). Bare hands (no stamina): a jab, or for a warrior the hook
//     one time in four, landing as its animation ends (combat.hpp), no other till then.
//     Dying (3) for a second, then dead (4): the Action key respawns it. Hit for more than 19,
//     it recoils (0x1E) for the RECOIL animation.
//   - The emotes (MSG_SOCIAL: social()): taunt 0x19, laugh 0x1A, each its animation through;
//     point 0x1B to its animation's middle, held there 20 frames (0x1C), then the rest (0x1D).
//   - Hand-typed spells (control codes 0x14-0x1B: the gestures Up, Down, Left, Right, UpperRight,
//     UpperLeft, LowerRight, LowerLeft, as the stock client's actions number them, two past the
//     1.2b control table's names; spells.hpp), 0x1C the pattern's end, 0x1D / 0x1E the first /
//     last queued spell.
#pragma once

#include "noxcore/net/player_input.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/game_data.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace nox::server {

/// How long each player animation runs: frames and the frames each lasts less one, by
/// kPlayerStates index (the server's table at which the thing reader fills from
/// PlayerDraw; animPlayerGetFrameRange reads it).
struct PlayerAnimations {
    std::array<std::pair<int, int>, 55> ranges{};

    /// From NewPlayer's DRAW in thing.bin (all zero if it isn't there).
    static PlayerAnimations load(const things::ThingDb& things);
    std::pair<int, int> operator[](std::size_t i) const { return i < ranges.size() ? ranges[i] : std::pair{0, 0}; }
};

/// Makes `unit` a player's of class `classId` (0 warrior, 1 wizard, 2 conjurer) as a game with
/// `gameFlags` does (plrReadVals, gamedata from `gameData` or the shipped
/// numbers): a network game's at the class's maximums (its MaxHealth, MaxMana, MaxStrength,
/// MaxSpeed), a solo game's from the Base numbers to them by `level` (1-10, a ninth of the way a
/// level). Its spells (spellAwardAll2): a versus game's all at level 3, a quest's
/// the class's first at 1 (a wizard's Fireball, a conjurer's Charm and Lesser Heal), a solo
/// game's none (its saved book, the mode layer's to fill). A solo game's warrior has the
/// abilities of its level (Berserker at 2, War Cry 3, Tread Lightly 4, Eye of the Wolf 5,
/// Harpoon 6).
void makePlayer(ServerObject& unit, std::uint8_t classId, const things::GameData* gameData,
                std::uint32_t gameFlags = GameFlag::Network | GameFlag::Arena, std::uint8_t level = 10);

/// Map_UpdatePlayerPolygon: the map polygon it's in (World::polygonAt) and that polygon's group;
/// leaving one runs its leave script, entering one its enter script (caller: the player); a
/// quest's secret area found the first time is told.
void updatePolygon(World& world, ServerObject& unit);
/// plrReadVals: its numbers again by its class and level (health and mana full),
/// a warrior's abilities by its level (a solo game's; told with `notify`), its client sent them.
void readVals(World& world, ServerObject& unit, bool notify);
/// Plr_CheckLevelUp: at XPTable's next level, a level up (a solo game's moment: the LevelUp,
/// MSG_FX 0x9A, the level-up state; else "LevelUP").
void checkLevelUp(Fight& fight, ServerObject& unit);
/// unitGiveXP: worth more than it has, a hundredth of the difference and one
/// (MSG_REPORT_EXPERIENCE, a level up); the gain.
float giveXP(Fight& fight, ServerObject& unit, float worth);
/// Plr_GiveExperience (the script's GiveXp): `xp` more, "health.c:gainpoints", a level up.
void giveExperience(Fight& fight, ServerObject& unit, float xp);
/// SrvNet_SendReportExperience: MSG_REPORT_EXPERIENCE 0x6E to the player's client, its experience
/// (u32, truncated), reliable.
void reportExperience(World& world, const ServerObject& unit);
/// Obj_AwardSoloKillXP: in a solo game the player behind whoever killed it (its creature only
/// when it controls it) gets its worth (giveXP), "gainpoints".
void awardKillXP(Fight& fight, ServerObject& victim);

/// Its minimap's marks (PlayerControl::minimapMarks): what it's sent wherever it is (the update stream skips
/// the view and the walls for them; one a while out of view). Marking
/// (netMarkMinimapObject) adds the reasons' bits (1 a creature of its, a player,
/// the ball, a crown, a flag; 2 a ray's end), a new mark at the list's head with the object's
/// xStatus 1 set; unmarking (netUnmarkMinimapObj) takes bits off and, with none left, the mark (xStatus 1
/// cleared, untold).
void markMinimap(World& world, ServerObject& player, ServerObject& o, std::uint32_t bits);
void unmarkMinimap(World& world, ServerObject& player, ServerObject& o, std::uint32_t bits);
/// For every player (netMarkMinimapForAll / netUnmarkMinimapSpec).
void markMinimapForAll(World& world, ServerObject& o, std::uint32_t bits);
void unmarkMinimapForAll(World& world, ServerObject& o, std::uint32_t bits);

/// Plr_SetState: `unit` (a player's) into `state` as the exe allows it; false when
/// refused. Dead, only dying or dead (3, 4); jumping (AIRBORNE) out of a solo game, no recoil
/// (0x1E); the emotes (0x18-0x1D) not while charging (Berserker, ability 1) nor jumping (0xC);
/// out of an attack (1) only into dying or dead while War Cry's shout is on (its flag 2); an
/// attack not before the last one's done (`attackReady`). A taunt (0x19) plays 0x12D, a laugh
/// (0x1A) 300, a point's middle (0x1C) 0x12E as it begins; anything but an attack forgets the
/// punch. A new state starts its frame count and animation.
bool setPlayerState(World& world, ServerObject& unit, std::uint8_t state, std::uint32_t frame);
/// playerMakeDefItems: with `heal`, its poison gone and its health and mana whole;
/// its abilities cancelled, out of any quest exit or warp gate, flags 0x18 / 0x4000 / 0x8000 /
/// 0x140000 off, state 0xD, no enchantments, no spell being cast nor trap being built, no harpoon,
/// nothing that last hurt it; its whole health and mana told it; its outfit anew (Items::
/// defaultItems), or with `keep` what it has kept (MSG_PLAYER_RESPAWN's flag 0).
void makeDefItems(Fight& fight, ServerObject& unit, bool heal, bool keep);
/// each player at a map's change: its control buffer emptied, a quest's secrets of
/// the map credited, state 0xD and (refused, or not co-op) makeDefItems (healed; `keep`: the next
/// map a quest's bit 2); an observer told to leave (status 0x20) back in play.
void mapChangePlayer(Fight& fight, ServerObject& unit, bool keep);
/// MSG_SOCIAL (SrvNet_OnPacket 0xE3): an emote, 1 the laugh (0x1A), 2 the taunt (0x19), 4 the
/// point (0x1B, then 0x1C held a moment, 0x1D); anything else nothing.
void social(World& world, ServerObject& unit, std::uint8_t gesture, std::uint32_t frame);

/// playerAddGold / playerSubGold: a player's gold up, or down (not
/// below 0); its client told on the frame's report (MSG_REPORT_GOLD).
void addGold(ServerObject& unit, std::int32_t amount);
void subGold(ServerObject& unit, std::uint32_t amount);

/// Abil_GrantToPlayer: a warrior's ability 1-5 at level 5 ("AwardAbilityError" out of
/// range, "use.c:HadAbility" known already), MSG_REPORT_ABILITY_AWARD (0x80 with `notify`); in a
/// quest MSG_GAUNTLET 0x20 to it and (not while its .plr is applied) the others. False when not.
bool grantPlayerAbility(World& world, ServerObject& unit, int ability, bool notify);
/// Plr_AwardFieldGuide: creature 1-40 into its field guide ("AwardGuideError" out of
/// range; nothing when known), its group with it (the Spider's spiders); with `notify` 0xE3 and
/// MSG_GAUNTLET 0x1F to it and the others; MSG_REPORT_GUIDE_AWARD (0xD1). False when not.
bool awardFieldGuide(World& world, ServerObject& unit, int creature, bool notify);
/// comJournalEntryAdd: a journal entry at the head of its list (Plr_AddJournalEntry,
/// at most 63 letters), its client told (MSG_JOURNAL_MSG 0xD5, 1).
bool addJournalEntry(World& world, ServerObject& unit, std::string_view name, std::uint16_t type);
/// its journal's entries of the `types` bits gone (the host's client told).
void removeJournalEntries(World& world, ServerObject& unit, std::uint16_t types);

/// A MSG_PLAYER_INPUT's events into its control buffer.
void addControls(ServerObject& unit, std::span<const net::ControlEntry> entries);

/// The camper alarm (server option 0x2000; updateServer runs it in a network
/// game's update, not in a chat game nor once the match is over):. Every
/// CamperStartTime frames (gamedata) each player in play not observing is looked at: within
/// CamperRadiusSq of where the last look had it, it's found still (`checkedAt`: the last look's
/// frame). One found still at the last look (within CamperFadeTime of it) is a camper: marked for
/// all (MSG_INTERESTING_ID kind 2, SrvNet_BroadcastInterestingIdAdd) with the
/// CamperAlarm sound (in CTF only one carrying the flag sounds it); one that isn't is unmarked
/// (SrvNet_BroadcastInterestingIdRemove).
void camperWatch(Fight& fight, std::uint32_t& checkedAt);
/// its watch begun anew (going to observe, made whole again, the option turned on),
/// its mark taken off.
void camperReset(World& world, ServerObject& unit, std::uint32_t frame);
/// everyone's (the server option 0x2000 turned on in a game).
void camperResetAll(World& world, std::uint32_t frame);
/// MSG_INTERESTING_ID kind 2 for a player: {0xD2, its code, its type, 1 add / 2 take off, 2}.
std::vector<std::uint8_t> camperMarker(const ServerObject& unit, bool add);

/// noxcpp's (no stock counterpart; the update rate's substeps, clock.hpp): how far the next frame
/// will move a walking, running or standing player from its frame's place and motion, as
/// Plr_UpdateState's switch pushes it before the frame's control events (which only set the
/// frame after's state and facing): along its facing at its SpeedCur, doubled running (state 5 or
/// the cursor past 100), or not at all standing; through the frame's five physics steps. Nothing
/// for one observing or in another state (attacking, jumping...) or confused: its motion goes on.
/// Walls and other objects aren't met; the frame decides. It only reads.
std::optional<std::pair<float, float>> predictedFrameMove(const World& world, const ServerObject& unit);

/// Its frame (updatePlayer and Plr_UpdateState as far as walking, running and
/// jumping); its control events are used up.
void updatePlayer(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight);

/// What the update stream says it's doing (playerMapAnimState), and the frame
/// of it (0xFF: none).
std::uint8_t playerAnimation(const ServerObject& unit, std::uint8_t& frame, const World* world = nullptr);

} // namespace nox::server
