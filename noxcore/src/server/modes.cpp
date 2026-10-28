// The classic game modes, as the game plays them (modes.hpp describes each in full): teams and their
// MSG_TEAM_MSG news, lessons (scores) and deaths, the scoring of a death by mode, elimination's
// sudden death, the match timer and the winner, observers, the mode's setup for a map, capture
// the flag, flagball, King of the Realm's crowns, and the respawning of a map's things.
#include "noxcore/server/modes.hpp"

#include "noxcore/net/player_input.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

// A 16-bit value's low and high bytes.
using util::hi;
using util::lo;

// A u32, little-endian.
using util::putU32;

/// To every client, reliably (Net_QueueImportantSeq 0x9F).
void toAll(World& world, std::vector<std::uint8_t> m)
{
    world.broadcasts().push_back({std::move(m), true, 0});
}

/// Unlinks it: out of the team's members, its team id cleared.
void unlink(ServerTeam& team, ServerObject& o)
{
    std::erase(team.members, o.extent);
    o.team = 0;
}

} // namespace

int modeIndex(std::uint32_t gameFlags)
{
    // The mode bits in mode-index order: arena, elimination, CTF, KotR, flagball, quest. The flags'
    // mode bits (0x17F0, coop 0x200 and chat 0x80 among them) must equal one exactly; else 0.
    static constexpr std::uint32_t kModes[6] = {0x100, 0x400, 0x20, 0x10, 0x40, 0x1000};
    for (int i = 0; i < 6; ++i) {
        if ((gameFlags & 0x17F0u) == kModes[i]) {
            return i;
        }
    }
    return 0;
}

std::uint32_t mapToGameFlags(std::uint32_t mapTypes)
{
    std::uint32_t f = 0;
    f |= (mapTypes & 1u) ? 0x200u : 0;
    f |= (mapTypes & 2u) ? 0x1000u : 0;
    f |= (mapTypes & 4u) ? 0x100u : 0;
    f |= (mapTypes & 0x20u) ? 0x400u : 0;
    f |= (mapTypes & 8u) ? 0x20u : 0;
    f |= (mapTypes & 0x10u) ? 0x10u : 0;
    f |= (mapTypes & 0x40u) ? 0x40u : 0;
    f |= (mapTypes & 0x80000000u) ? 0x80u : 0;
    return f;
}

std::uint16_t lessonLimit(const World& world)
{
    return world.lessonLimits[static_cast<std::size_t>(modeIndex(world.gameFlags))];
}

std::uint16_t enforcedLessonLimit(const World& world)
{
    return world.modeOwnsLimits ? 0 : lessonLimit(world); // (noxcpp: told clients, the mode's to act on)
}

ServerTeam* teamById(World& world, std::uint8_t id)
{
    for (ServerTeam& t : world.teams) {
        if (t.active && t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

int teamCount(const World& world)
{
    return static_cast<int>(std::ranges::count_if(world.teams, &ServerTeam::active));
}

bool inTeam(const World& world, const ServerObject& o, std::uint8_t id)
{
    if (o.team != id || id == 0) {
        return false;
    }
    for (const ServerTeam& t : world.teams) {
        if (t.active && t.id == id) {
            return std::ranges::find(t.members, o.extent) != t.members.end();
        }
    }
    return false;
}

int teamPlayers(World& world, const ServerTeam& team)
{
    int n = 0;
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (o.player && !o.gone() && inTeam(world, o, team.id)) {
            ++n;
        }
    }
    return n;
}

ServerTeam* createTeam(World& world, std::uint8_t id)
{
    if (15 < teamCount(world)) {
        return nullptr; // ("teamexceed" on the host's console)
    }
    std::size_t slot = 0;
    while (slot < world.teams.size() && world.teams[slot].active) {
        ++slot;
    }
    if (slot == world.teams.size()) {
        return nullptr;
    }
    if (id == 0) { // (the lowest id no team has, 1-16; none, 0)
        std::uint8_t free = 1;
        while (free <= 16 && std::ranges::any_of(world.teams, [&](const ServerTeam& t) { return t.active && t.id == free; })) {
            ++free;
        }
        id = free <= 16 ? free : 0;
    }
    ServerTeam& t = world.teams[slot];
    t = ServerTeam{};
    t.active = true;
    t.id = id;
    t.color = static_cast<std::uint8_t>(slot + 1);
    return &t; // ("teamcreate" on the host's console, but in a coop game)
}

void setTeamName(ServerTeam& team, std::u16string_view name, bool useTitle)
{
    team.name = std::u16string(name.substr(0, 20));
    team.useTitle = useTitle;
}

ServerTeam* emptiestTeam(World& world)
{
    ServerTeam* best = nullptr;
    // (0x20, more than any team holds: the first active team wins a tie for the fewest.)
    int fewest = 0x20;
    for (ServerTeam& t : world.teams) {
        if (!t.active) {
            continue;
        }
        const int n = teamPlayers(world, t);
        if (n < fewest) {
            best = &t;
            fewest = n;
        }
    }
    return best;
}

void addTeamMember(World& world, std::uint8_t id, ServerObject& o, bool sendNet, bool moveToBase, std::minstd_rand& rng, std::uint32_t frame)
{
    ServerTeam* team = teamById(world, id);
    if (!team) {
        team = createTeam(world, id);
        if (!team) {
            return;
        }
    } else if (inTeam(world, o, id)) {
        return;
    }
    team->members.insert(team->members.begin(), o.extent);
    o.team = team->id;
    if (world.network()) {
        // (A player's: its "NewMember" line on the host's own screen.) In a chat game without
        // gameplay flag 2, asked to, to within 50 of its base.
        if (o.player && moveToBase && !(world.gameplayFlags & 2u) && (world.gameFlags & GameFlag::Chat)) {
            if (const ServerObject* base = team->base ? world.byExtent(team->base) : nullptr) {
                const auto [x, y] = clearPointNear(world, rng, base->x, base->y, 50.0f);
                setPosition(world, o, x, y, frame);
            }
        }
        if (sendNet && (o.player || o.typeName == "GameBall")) {
            const std::uint16_t code = netCodeOf(o);
            // MSG_TEAM_MSG 0xC4 sub 1: {u32 team, u16 net code, u16 type}.
            std::vector<std::uint8_t> m = {0xC4, 0x01};
            putU32(m, team->id);
            m.insert(m.end(), {lo(code), hi(code), lo(o.type), hi(o.type)});
            toAll(world, std::move(m));
        }
    }
    if (o.player) {
        refriend(world, o); // (Team_AddMember's end, after MSG_TEAM_MSG: its friends afresh)
    }
}

void removeTeamMember(World& world, ServerObject& o)
{
    ServerTeam* team = teamById(world, o.team);
    if (!team || !inTeam(world, o, o.team)) {
        return;
    }
    if (world.network()) {
        // Sub 2: {u32 net code}.
        std::vector<std::uint8_t> m = {0xC4, 0x02};
        putU32(m, netCodeOf(o));
        toAll(world, std::move(m));
    }
    unlink(*team, o);
    if (o.player) {
        refriend(world, o); // (its friends afresh)
    }
    // An emptied team goes only in clan play or with game flag 0x8000; else it stays as it is.
    if (!world.clanTeams && !(world.gameFlags & 0x8000u)) {
        return;
    }
    if (teamPlayers(world, *team) == 0) {
        const bool keep = (world.gameFlags & (GameFlag::CaptureTheFlag | GameFlag::FlagBall)) ||
                          ((world.gameFlags & GameFlag::KingOfTheRealm) && (world.gameplayFlags & 4u));
        if (!keep) {
            removeTeam(world, *team, true);
            return;
        }
        team->clan = 0;
        setTeamName(*team, u"", false);
    }
}

bool moveTeamMember(World& world, ServerObject& o, ServerTeam& to, bool moveToBase, std::minstd_rand& rng, std::uint32_t frame)
{
    if (!inTeam(world, o, o.team)) {
        return false;
    }
    if (world.network()) {
        // Sub 3: {u32 team, u16 net code, u16 0}.
        std::vector<std::uint8_t> m = {0xC4, 0x03};
        putU32(m, to.id);
        const std::uint16_t code = netCodeOf(o);
        m.insert(m.end(), {lo(code), hi(code), 0, 0});
        toAll(world, std::move(m));
    }
    if (ServerTeam* from = teamById(world, o.team)) {
        unlink(*from, o);
    }
    addTeamMember(world, to.id, o, false, moveToBase, rng, frame);
    return true;
}

void setTeamScore(World& world, ServerTeam& team, int score)
{
    team.lessons = score;
    if (world.network()) {
        // Sub 8: {u32 team, u32 score}.
        std::vector<std::uint8_t> m = {0xC4, 0x08};
        putU32(m, team.id);
        putU32(m, static_cast<std::uint32_t>(team.lessons));
        toAll(world, std::move(m));
    }
}

void removeTeam(World& world, ServerTeam& team, bool sendNet)
{
    if (!team.active) {
        return;
    }
    if (world.network() && sendNet) {
        // Sub 6: {u32 team}.
        std::vector<std::uint8_t> m = {0xC4, 0x06};
        putU32(m, team.id);
        toAll(world, std::move(m));
    }
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player && o.team == team.id) {
            unlink(team, o);
        }
    }
    team = ServerTeam{};
}

void removeAllTeams(World& world, bool sendNet)
{
    world.gameplayFlags &= ~4u;
    for (ServerTeam& t : world.teams) {
        if (t.active) {
            removeTeam(world, t, false);
        }
    }
    if (sendNet) {
        toAll(world, {0xC4, 0x07});
    }
}

void removeAllTeamMembers(World& world, ServerTeam& team)
{
    if (teamPlayers(world, team) <= 0) {
        return;
    }
    if (world.network()) {
        // Sub 5: {u32 team}.
        std::vector<std::uint8_t> m = {0xC4, 0x05};
        putU32(m, team.id);
        toAll(world, std::move(m));
    }
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player && o.team == team.id) {
            unlink(team, o);
        }
    }
}

std::vector<std::uint8_t> teamMessage(const World& world, const ServerTeam& team)
{
    // Sub 0: {u32 id, u32 clan, u32 score, u8 coop, u8 name length, u8 colour, u8 use-title,
    // the UTF-16 name}.
    std::vector<std::uint8_t> m = {0xC4, 0x00};
    putU32(m, team.id);
    putU32(m, team.clan);
    putU32(m, static_cast<std::uint32_t>(team.lessons));
    m.push_back((world.gameFlags & GameFlag::Coop) ? 1 : 0);
    m.push_back(static_cast<std::uint8_t>(team.name.size()));
    m.push_back(team.color);
    m.push_back(team.useTitle ? 1 : 0);
    for (const char16_t c : team.name) {
        m.push_back(static_cast<std::uint8_t>(c));
        m.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    return m;
}

std::vector<std::uint8_t> teamMemberMessage(const ServerTeam& team, const ServerObject& member)
{
    std::vector<std::uint8_t> m = {0xC4, 0x01};
    putU32(m, team.id);
    const std::uint16_t code = netCodeOf(member);
    m.insert(m.end(), {lo(code), hi(code), lo(member.type), hi(member.type)});
    return m;
}

void changeLessons(ServerObject& o, int by)
{
    if (o.player) {
        o.player->lessons += by;
    }
}

namespace {

// Sudden death may come 20 s (600 frames) after its last reset.
bool suddenDeathMayCome(const World& world, std::uint32_t frame)
{
    return 30u * 20u < frame - world.suddenDeathFrom;
}

/// The players in play (not observing). In team play, the teams, each counted when
/// anyone at all is in play (the exe doesn't look at who's in which).
int playersInPlay(World& world)
{
    int n = 0;
    const auto inPlay = [](const ServerObject& o) { return o.player && !o.gone() && !(o.player->statusFlags & 1u); };
    if (!(world.gameplayFlags & 4u)) {
        for (const ServerObject* o : world.playerUnits()) {
            n += inPlay(*o) ? 1 : 0;
        }
        return n;
    }
    const bool anyone = std::ranges::any_of(world.playerUnits(), [&](const ServerObject* o) { return inPlay(*o); });
    for (const ServerTeam& t : world.teams) {
        n += t.active && anyone ? 1 : 0;
    }
    return n;
}

/// countNonEliminatedPlayersInTeam.
int inPlayInTeam(World& world, const ServerTeam& team)
{
    int n = 0;
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (o.player && !o.gone() && inTeam(world, o, team.id) && !(o.player->statusFlags & 1u)) {
            ++n;
        }
    }
    return n;
}

/// The observers shut out (status 0x100).
void shutOutObservers(Fight& fight)
{
    for (ServerObject* pu : fight.world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player && (o.player->statusFlags & 1u)) {
            fight.setStatus(o, 0x100, true);
        }
    }
}

/// The countdown to sudden death, when it's due: fewer in play than the threshold (team play:
/// fewer teams, one of them down to one player). servStartCountdown: the timer to
/// its seconds, running, its line to everyone.
void maybeStartCountdown(Fight& fight)
{
    World& world = fight.world;
    if (!(world.gameplayFlags & 4u)) {
        if (world.suddenDeathThreshold <= playersInPlay(world)) {
            return;
        }
    } else {
        if (world.suddenDeathThreshold <= teamCount(world)) {
            return;
        }
        if (!std::ranges::any_of(world.teams, [&](const ServerTeam& t) { return t.active && inPlayInTeam(world, t) == 1; })) {
            return;
        }
    }
    const int seconds = static_cast<int>(world.number("SuddenDeathCountdown"));
    world.timerEnd = fight.frame + static_cast<std::uint32_t>(seconds) * 30u;
    setTimerRunning(world, true, fight.frame);
    // MSG_INFORM 0xA9 sub 0x0F: {u8 0, the string table id, NUL}.
    toAll(world, informStringBytes("Settings.c:SuddenDeathImminent")); // (SrvNet_BroadcastInformString)
    world.countdown = true;
}

} // namespace

void addDeath(Fight& fight, ServerObject& o)
{
    World& world = fight.world;
    if (!o.player) {
        return;
    }
    ++o.player->deaths;
    if (!(world.gameFlags & GameFlag::Elimination)) {
        return;
    }
    if (suddenDeathMayCome(world, fight.frame) && !world.suddenDeathShutOut) {
        shutOutObservers(fight);
        world.suddenDeathShutOut = true;
    }
    if (!(world.gameFlags & 0x4000000u) && !world.countdown && suddenDeathMayCome(world, fight.frame)) {
        maybeStartCountdown(fight);
    }
}

void resetSuddenDeath(Fight& fight)
{
    World& world = fight.world;
    world.suddenDeathFrom = fight.frame;
    world.suddenDeathShutOut = false;
    world.suddenDeathThreshold = static_cast<int>(world.number("SuddenDeathPlayerThreshold"));
    world.suddenDeathLifeTime = static_cast<int>(world.number("SuddenDeathLifeTime"));
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player && (o.player->statusFlags & 0x100u)) {
            fight.setStatus(o, 0x100, false);
        }
    }
    world.gameFlags &= ~0x4000000u; // (Game_ClearFlags: the countdown with it)
    world.countdown = false;
}

bool mayJoin(World& world, std::uint32_t frame)
{
    if (!(world.gameFlags & GameFlag::Chat) && (world.gameFlags & GameFlag::Elimination)) {
        for (const ServerObject* pu : world.playerUnits()) {
            const ServerObject& o = *pu;
            if (o.player && 0 < o.player->deaths) {
                return !suddenDeathMayCome(world, frame);
            }
        }
    }
    return true;
}

void suddenDeathCheck(Fight& fight)
{
    World& world = fight.world;
    if ((world.gameFlags & GameFlag::GameOver) || !(world.gameFlags & GameFlag::Elimination)) {
        return;
    }
    if (!suddenDeathMayCome(world, fight.frame)) {
        world.suddenDeathChecked = false;
    } else if (!world.suddenDeathChecked) {
        if (!mayJoin(world, fight.frame)) {
            shutOutObservers(fight);
        }
        world.suddenDeathChecked = true;
    }
    if ((world.gameFlags & 0x4000000u) || world.countdown) {
        return;
    }
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (o.player && 0 < o.player->deaths) {
            if (suddenDeathMayCome(world, fight.frame)) {
                maybeStartCountdown(fight);
            }
            break;
        }
    }
}

void suddenDeathDrain(Fight& fight, ServerObject& unit)
{
    // A point every lifetime x FPS / its most frames.
    if ((unit.flags & ObjFlag::Dead) || unit.maxHealth == 0 || unit.health == 0) {
        return;
    }
    const std::uint32_t every = static_cast<std::uint32_t>(30 * fight.world.suddenDeathLifeTime) / unit.maxHealth;
    if (every != 0 && fight.frame % every == 0) {
        fight.takeHealth(unit, 1);
    }
}

void reportLesson(World& world, const ServerObject& o)
{
    if (!o.player) {
        return;
    }
    // MSG_REPORT_LESSON: {u16 net code, u32 lessons, u32 deaths}.
    const auto code = static_cast<std::uint16_t>(o.extent);
    std::vector<std::uint8_t> m = {0x4E, lo(code), hi(code)};
    putU32(m, static_cast<std::uint32_t>(o.player->lessons));
    putU32(m, static_cast<std::uint32_t>(o.player->deaths));
    toAll(world, std::move(m));
}

namespace {

bool wearsCrown(World& world, const ServerObject& o)
{
    // unitIsCrown: a Crown in its inventory.
    return std::ranges::any_of(o.inventory, [&](std::uint32_t e) {
        const ServerObject* i = world.byExtent(e);
        return i && i->typeName == NameId::Crown;
    });
}

/// playerUpdateScore (an arena).
void arenaDeath(Fight& fight, ServerObject& victim, ServerObject* killer, ServerObject* assist)
{
    World& world = fight.world;
    ServerTeam* victimTeam = victim.team ? teamById(world, victim.team) : nullptr;
    ServerTeam* killerTeam = killer && killer->team ? teamById(world, killer->team) : nullptr;
    ServerTeam* assistTeam = assist && assist->team ? teamById(world, assist->team) : nullptr;
    const auto suicide = [&] {
        changeLessons(victim, -1);
        reportLesson(world, victim);
        if (victimTeam) {
            setTeamScore(world, *victimTeam, victimTeam->lessons - 1);
        }
    };
    if (killer == &victim || (!killer && !assist)) {
        suicide();
    } else {
        if (killer && (killer->classes & oc::Player)) {
            if (killerTeam && killerTeam == victimTeam) { // a team-mate
                changeLessons(*killer, -1);
                reportLesson(world, *killer);
                setTeamScore(world, *killerTeam, killerTeam->lessons - 1);
            } else {
                changeLessons(*killer, 1);
                reportLesson(world, *killer);
                if (killerTeam) {
                    setTeamScore(world, *killerTeam, killerTeam->lessons + 1);
                }
            }
        }
        addDeath(fight, victim);
        reportLesson(world, victim);
    }
    if (!assist) {
        return;
    }
    // The assist: a point unless it shares a team with the killer or the victim.
    if (killerTeam ? killerTeam == assistTeam : false) {
        return;
    }
    if (assistTeam && (assistTeam == victimTeam)) {
        return;
    }
    changeLessons(*assist, 1);
    reportLesson(world, *assist);
    if (assistTeam) {
        setTeamScore(world, *assistTeam, assistTeam->lessons + 1);
    }
}

/// playerHandleKotrDeath.
void kotrDeath(Fight& fight, ServerObject& victim, ServerObject* killer)
{
    World& world = fight.world;
    ServerTeam* victimTeam = victim.team ? teamById(world, victim.team) : nullptr;
    if (!killer) {
        return; // (the exe's: not even the victim's death counted)
    }
    ServerTeam* killerTeam = killer->team ? teamById(world, killer->team) : nullptr;
    if (!(killer->classes & oc::Player)) {
        return;
    }
    const auto points = [&](const char* name) { return static_cast<int>(world.number(name)); };
    // Its own death or a team-mate's: a king loses a point.
    if (killer == &victim || (victimTeam == killerTeam && victimTeam)) {
        if (wearsCrown(world, *killer)) {
            changeLessons(*killer, -1);
            reportLesson(world, *killer);
            if (killerTeam) {
                setTeamScore(world, *killerTeam, killerTeam->lessons - 1);
            }
        }
    } else if (!killerTeam || killerTeam == victimTeam) {
        // The killer in no team (free-for-all KotR): points when either wore a crown.
        const bool killerKing = wearsCrown(world, *killer);
        if (killerKing || wearsCrown(world, victim)) {
            changeLessons(*killer, points(killerKing ? "KotRKingKillsPawnPoints" : "KotRPawnKillsKingPoints"));
            reportLesson(world, *killer);
            // Not team KotR (gameplay flag 4): a dead king's crown drops, for its killer.
            if (!(world.gameplayFlags & 4u) && wearsCrown(world, victim)) {
                dropKingsCrown(fight, victim, killer);
            }
        }
    } else if (wearsCrown(world, *killer)) {
        // Team KotR, a foe slain by a king: the points to it and its team.
        const int p = points(wearsCrown(world, victim) ? "KotRKingKillsKingPoints" : "KotRKingKillsPawnPoints");
        changeLessons(*killer, p);
        setTeamScore(world, *killerTeam, killerTeam->lessons + p);
        reportLesson(world, *killer);
    } else if (wearsCrown(world, victim)) {
        // A pawn slaying another team's king: the same.
        const int p = points("KotRPawnKillsKingPoints");
        changeLessons(*killer, p);
        setTeamScore(world, *killerTeam, killerTeam->lessons + p);
        reportLesson(world, *killer);
    }
    addDeath(fight, victim);
    reportLesson(world, victim);
}

/// playerHandleElimDeath.
void elimDeath(Fight& fight, ServerObject& victim, ServerObject* killer)
{
    World& world = fight.world;
    ServerTeam* victimTeam = victim.team ? teamById(world, victim.team) : nullptr;
    ServerTeam* killerTeam = killer && killer->team ? teamById(world, killer->team) : nullptr;
    if (killer == &victim) {
        changeLessons(victim, -1);
        addDeath(fight, victim);
        reportLesson(world, victim);
        if (victimTeam) {
            setTeamScore(world, *victimTeam, victimTeam->lessons + 1);
        }
        return;
    }
    if (killer && (killer->classes & oc::Player)) {
        if (killerTeam && killerTeam == victimTeam) {
            changeLessons(*killer, -1);
        } else {
            changeLessons(*killer, 1);
        }
        reportLesson(world, *killer);
    }
    addDeath(fight, victim);
    reportLesson(world, victim);
    // (In elimination a team's score counts its deaths: the fewest wins at the time limit.)
    if (victimTeam) {
        setTeamScore(world, *victimTeam, victimTeam->lessons + 1);
    }
}

} // namespace

void scoreDeath(Fight& fight, ServerObject& victim, ServerObject* killer, ServerObject* assist)
{
    World& world = fight.world;
    if (!anyonePlaying(world)) {
        return; // (gamePlayIsAnyPlayers: one alone scores nothing)
    }
    if (world.sandboxNoScoring) {
        return; // (NoX Core's map editor session sandbox: kills and deaths don't count; never set elsewhere)
    }
    if (world.gameFlags & GameFlag::Arena) {
        arenaDeath(fight, victim, killer, assist);
    } else if (world.gameFlags & GameFlag::KingOfTheRealm) {
        kotrDeath(fight, victim, killer);
    } else if (world.gameFlags & GameFlag::Elimination) {
        elimDeath(fight, victim, killer);
    }
}

bool anyonePlaying(World& world)
{
    // Playing: not observing, or observing only as one eliminated (status 0x20).
    // Status 1 observing, 0x20 eliminated (set by eliminated()): an eliminated player still counts.
    const auto playing = [](const ServerObject& o) { return o.player && !o.gone() && (!(o.player->statusFlags & 1u) || (o.player->statusFlags & 0x20u)); };
    int n = 0;
    if (!(world.gameplayFlags & 4u)) {
        for (const ServerObject* o : world.playerUnits()) {
            if (playing(*o)) {
                ++n;
            }
        }
    } else {
        const auto units = world.playerUnits();
        for (const ServerTeam& t : world.teams) {
            if (!t.active) {
                continue;
            }
            for (const ServerObject* o : units) {
                if (playing(*o) && inTeam(world, *o, t.id)) {
                    ++n;
                    break;
                }
            }
        }
    }
    return 1 < n;
}

std::uint8_t timeLimit(const World& world)
{
    return world.timeLimits[static_cast<std::size_t>(modeIndex(world.gameFlags))];
}

std::uint8_t enforcedTimeLimit(const World& world)
{
    return world.modeOwnsLimits ? 0 : timeLimit(world); // (noxcpp: told clients, the mode's to act on)
}

std::vector<std::uint8_t> timerStatus(const World& world, std::uint32_t frame)
{
    std::vector<std::uint8_t> m = {0xD3};
    putU32(m, world.timerRunning ? 1u : 0u);
    // (Frames to ms, at 30 frames a second.)
    const auto ms = static_cast<std::int32_t>((static_cast<std::int64_t>(world.timerEnd) - static_cast<std::int64_t>(frame)) * 1000 / 30);
    putU32(m, static_cast<std::uint32_t>(ms));
    putU32(m, frame);
    return m;
}

void setTimerRunning(World& world, bool running, std::uint32_t frame)
{
    world.timerRunning = running;
    toAll(world, timerStatus(world, frame));
}

void startMatchTimer(World& world, std::uint32_t frame)
{
    world.timerEnd = frame + static_cast<std::uint32_t>(enforcedTimeLimit(world)) * 60u * 30u;
    setTimerRunning(world, true, frame);
}

bool timerRunOut(const World& world, std::uint32_t frame)
{
    return (enforcedTimeLimit(world) != 0 || world.countdown) && world.timerRunning && world.timerEnd < frame;
}

void observingEnded(World& world, std::uint32_t frame)
{
    if (!(world.gameFlags & GameFlag::Chat) && anyonePlaying(world) && !world.timerRunning && enforcedTimeLimit(world) != 0) {
        startMatchTimer(world, frame);
    }
}

namespace {

/// The camera onto `target` (playerCameraFollow: following it already, it lets go).
void follow(PlayerControl& p, std::uint32_t target)
{
    p.cameraFollow = p.cameraFollow == target ? 0 : target;
}

/// The next to follow: the player after the one followed (none: a flagball game's
/// ball, else the first), the first of them in play; none, the ball or the first in play.
std::uint32_t nextToFollow(World& world, const PlayerControl& p)
{
    // (The players' units in the objects' order; the first GameBall that isn't one looked for
    // only when it's wanted.)
    const UnitList<ServerObject> players = world.playerUnits();
    const auto ballExtent = [&]() -> std::uint32_t {
        for (auto& [extent, o] : world.objects()) {
            if (!o.player && o.typeName == "GameBall") {
                return o.extent;
            }
        }
        return 0;
    };
    const auto inPlay = [](const ServerObject* o) { return !o->gone() && !(o->player->statusFlags & 1u); };
    std::size_t from = 0;
    const ServerObject* current = p.cameraFollow ? world.byExtent(p.cameraFollow) : nullptr;
    if (!current && (world.gameFlags & GameFlag::FlagBall)) {
        if (const std::uint32_t ball = ballExtent()) {
            return ball;
        }
    }
    if (current && current->player) { // (the next player; else from the first)
        const auto at = std::find(players.begin(), players.end(), current);
        from = at == players.end() ? players.size() : static_cast<std::size_t>(at - players.begin()) + 1;
    }
    for (std::size_t i = from; i < players.size(); ++i) {
        if (inPlay(players.begin()[i])) {
            return players.begin()[i]->extent;
        }
    }
    if (const std::uint32_t ball = ballExtent()) {
        return ball;
    }
    for (ServerObject* o : players) {
        if (inPlay(o)) {
            return o->extent;
        }
    }
    return 0;
}

/// playerObserverFindGoodSlave0: after the creature watched in its owner's list
/// (none watched: from the player's first), the first alive; none, the first alive of the
/// player's own.
ServerObject* nextCreature(World& world, const ServerObject& unit)
{
    const auto alive = [](const ServerObject* o) { return !(o->flags & ObjFlag::Dead) && !o->gone(); };
    const ServerObject* followed = unit.player->cameraFollow ? world.byExtent(unit.player->cameraFollow) : nullptr;
    std::vector<ServerObject*> from;
    if (!followed) {
        from = ownedCreatures(world, unit);
    } else if (const ServerObject* owner = followed->owner ? world.byExtent(followed->owner) : nullptr) {
        // (playerObserverFindGoodSlave: those after it in its owner's list)
        const auto list = ownedCreatures(world, *owner);
        const auto at = std::ranges::find(list, followed);
        if (at != list.end()) {
            from.assign(at + 1, list.end());
        }
    }
    for (ServerObject* o : from) {
        if (alive(o)) {
            return o;
        }
    }
    for (ServerObject* o : ownedCreatures(world, unit)) {
        if (alive(o)) {
            return o;
        }
    }
    return nullptr;
}

/// On to the next, the camera let go first: watching a creature, its next creature
/// (none: back to itself); else the next player (nextToFollow).
void followNext(Fight& fight, ServerObject& unit)
{
    PlayerControl& p = *unit.player;
    std::uint32_t next = 0;
    if (possessed(fight.world, unit)) {
        ServerObject* c = nextCreature(fight.world, unit);
        if (!c) {
            observeClear(fight, unit);
            return;
        }
        next = c->extent;
    } else {
        next = nextToFollow(fight.world, p);
    }
    p.cameraFollow = 0;
    if (next) {
        follow(p, next);
    }
}

} // namespace

ServerObject* possessed(World& world, const ServerObject& player)
{
    if (!player.player || !(player.player->statusFlags & 2u) || player.player->cameraFollow == 0) {
        return nullptr;
    }
    return world.byExtent(player.player->cameraFollow);
}

void observeClear(Fight& fight, ServerObject& player)
{
    // playerObserveClear.
    if (!player.player || !(player.player->statusFlags & 2u)) {
        return;
    }
    fight.setStatus(player, 2, false);
    player.player->cameraFollow = 0;
    player.player->observing = false; // (Update: the player's again)
}

void observeCreature(Fight& fight, ServerObject& player, ServerObject& creature)
{
    // playerObserveMonster.
    PlayerControl& p = *player.player;
    if (p.statusFlags & 1u) {
        leaveObserver(fight, player); // (playerLeaveObserver_0)
    }
    if (p.statusFlags & 2u) {
        observeClear(fight, player);
    }
    fight.setStatus(player, 2, true);
    p.cameraFollow = 0;
    follow(p, creature.extent);
    p.observing = true; // (Update: Plr_UpdateObserver)
}

void nobodyLeftCheck(Fight& fight)
{
    World& world = fight.world;
    if (anyonePlaying(world)) {
        return;
    }
    setTimerRunning(world, false, fight.frame);
    for (ServerObject* pu : world.playerUnits()) { // (playerForceSendLessons)
        ServerObject& o = *pu;
        if (o.player) {
            o.player->lessons = 0;
            o.player->deaths = 0;
            reportLesson(world, o);
        }
    }
    resetTeamScores(world);
    resetSuddenDeath(fight);
}
void resetTeamScores(World& world)
{
    for (ServerTeam& t : world.teams) { // (Team_ResetAllScores)
        t.lessons = 0;
    }
    toAll(world, {0xC4, 0x09});
}
void joinTeam(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    if (world.clanTeams || (world.gameFlags & 0x8000u)) {
        return; // (by its clan: the clans aren't kept; one without a clan joins none)
    }
    if (teamCount(world) == 0 || unit.team != 0) {
        return;
    }
    if (ServerTeam* t = emptiestTeam(world)) {
        addTeamMember(world, t->id, unit, true, true, fight.rng, fight.frame);
    }
}
void chatBases(Fight& fight)
{
    World& world = fight.world;
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        // Observers are skipped, but not the eliminated (status 0x20).
        if (!o.player || o.gone() || ((o.player->statusFlags & 1u) && !(o.player->statusFlags & 0x20u))) {
            continue;
        }
        ServerTeam* nearest = nullptr;
        float best = 1e9f;
        for (ServerTeam& t : world.teams) {
            const ServerObject* base = t.active && t.base ? world.byExtent(t.base) : nullptr;
            if (!base) {
                continue;
            }
            // (On the x87's stack, 53 bits, compared there; the best kept stored.)
            const util::X87 dy = util::X87(base->y) - o.y;
            const util::X87 dx = util::X87(base->x) - o.x;
            const util::X87 d = dy * dy + dx * dx;
            if (d < util::X87(best)) {
                nearest = &t;
                best = d.f();
            }
        }
        if (!nearest) {
            continue;
        }
        ServerTeam* now = o.team ? teamById(world, o.team) : nullptr;
        if (!now) {
            addTeamMember(world, nearest->id, o, true, false, fight.rng, fight.frame);
        } else if (now != nearest) {
            moveTeamMember(world, o, *nearest, false, fight.rng, fight.frame);
        }
    }
}
bool goObserver(Fight& fight, ServerObject& unit, bool canJoin, bool force)
{
    World& world = fight.world;
    if (!unit.player) {
        return false;
    }
    PlayerControl& p = *unit.player;
    if (!force && std::ranges::any_of(world.activeAbilities(), [&](const ActiveAbility& a) { return a.unit == unit.extent; })) {
        return false; // (one of its abilities under way)
    }
    // In CTF, flagball or KotR what it owns of theirs goes: the ball let go (no longer NO_COLLIDE,
    // nobody's; status 1), its crown (Obj_DropCrown) and flags (Obj_DropAt) dropped where it
    // stands.
    if ((world.gameFlags & 0x70u) && fight.things) {
        if (ServerObject* ball = heldBall(world, unit)) {
            ball->flags &= ~ObjFlag::NoCollide;
            clearOwner(world, *ball);
            ballStatus(world, 1, 0);
        }
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        for (const std::uint32_t e : std::vector<std::uint32_t>(unit.inventory)) {
            ServerObject* o = world.byExtent(e);
            if (o && o->owner == unit.extent && (o->typeName == NameId::Crown || (o->classes & oc::Flag))) {
                items.drop(unit, *o, unit.x, unit.y);
            }
        }
    }
    if (possessed(world, unit)) { // (watching a creature: back first)
        observeClear(fight, unit);
    }
    fight.setStatus(unit, 1, true);
    if (!world.quest()) {
        nobodyLeftCheck(fight);
    }
    std::vector<std::uint8_t> inform = {0xA9, 0x0C}; // (SrvNet_SendInform 0x0C: it may join)
    putU32(inform, canJoin ? 1u : 0u);
    world.broadcasts().push_back({std::move(inform), true, unit.extent});
    enchant(world, unit, Enchant::Invisible, 0, 5);
    unit.flags |= ObjFlag::NoCollide;
    p.camX = unit.x;
    p.camY = unit.y;
    p.cameraFollow = 0;
    if (world.gameFlags & GameFlag::Solo) {
        p.freeCam = true;
    } else if ((world.gameFlags & GameFlag::FlagBall) && !force) {
        followNext(fight, unit);
    }
    removeSpawned(world, unit);
    p.curTraps = 0; // (its glyphs out, gone with removeSpawned)
    p.observing = true;
    camperReset(world, unit, fight.frame); // (the last)
    return true;
}

void leaveObserver(Fight& fight, ServerObject& unit)
{
    if (!unit.player) {
        return;
    }
    // (0x121: status 1 observing, 0x20 eliminated, 0x100 shut out.)
    fight.setStatus(unit, 0x121, false);
    observingEnded(fight.world, fight.frame); // (Plr_ClearStatusFlags' part for status 1)
    disenchant(fight.world, unit, Enchant::Invisible);
    unit.player->observing = false;
    markFriends(fight.world, unit); // (monsterMarkUpdate)
    unit.flags &= ~ObjFlag::NoCollide;
    takeTeamCrown(fight, unit); // (team KotR: its team's crown, when nobody holds it)
    // (A quest's enchantments sent: with the quests.)
}

bool outOfLives(const World& world, const ServerObject& unit)
{
    const std::uint16_t lives = world.lessonLimits[1]; // (servGamedataGet(0x400))
    return (world.gameFlags & GameFlag::Elimination) && 0 < lives && unit.player && lives <= unit.player->deaths;
}

void eliminated(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    const auto followLastInPlay = [&] {
        for (ServerObject* pu : world.playerUnits()) {
            ServerObject& o = *pu;
            if (o.player && !(o.flags & ObjFlag::Dead) && !(o.player->statusFlags & 1u)) {
                p.cameraFollow = o.extent; // (playerCameraFollow on each: the last)
            }
        }
    };
    if (p.statusFlags & 1u) {
        p.cameraFollow = 0;
        followLastInPlay();
        return;
    }
    fight.setStatus(unit, 0x20, true);
    goObserver(fight, unit, false, false);
    p.cameraFollow = 0;
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player && o.player->cameraFollow == unit.extent) {
            followNext(fight, o);
        }
    }
    // The player behind its last attacker, still about and in play.
    ServerObject* by = unit.hurtBy ? fight.parentPlayer(world.byExtent(unit.hurtBy)) : nullptr;
    if (by && !by->gone() && !(by->classes & oc::Monster) && (!by->player || !(by->player->statusFlags & 1u))) {
        p.cameraFollow = by->extent;
        return;
    }
    followLastInPlay();
}

void updateObserver(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    for (auto& mark : p.marks) { // (its teleport marks gone with their objects)
        const ServerObject* m = mark ? world.byExtent(mark) : nullptr;
        if (m && m->gone()) {
            mark = 0;
        }
    }
    unit.touch();
    // The camera kept on whatever it follows.
    if (const ServerObject* f = p.cameraFollow ? world.byExtent(p.cameraFollow) : nullptr) {
        p.camX = f->x;
        p.camY = f->y;
    }
    bool any = false;
    // The frame's active controls; the first one clears moveHeld, so it reads true only while forward is held.
    for (const PlayerControl::Event& e : p.controls) {
        if (!e.active) {
            continue;
        }
        if (!any) {
            any = true;
            p.moveHeld = false;
        }
        const auto code = static_cast<net::ControlCode>(e.code);
        if (code == net::ControlCode::MoveForward) {
            if (!p.freeCam) {
                p.moveHeld = true;
                if (!p.moveHeldBefore) {  // forward newly pressed: on to the next player
                    followNext(fight, unit);
                }
            } else {
                // Free camera: pans toward the cursor when more than 30 off it, a tenth of the rest a frame.
                // (On the x87: the cursor by FILD; x's difference kept on the stack, y's stored;
                // each new place stored.)
                using util::X87;
                const auto pan = [](float cam, X87 d) {
                    if (X87(30.0f) < d) {
                        return (X87(cam) - (d - 30.0f) * 0.1f).f();
                    }
                    if (d < X87(-30.0f)) {
                        return (X87(cam) - (X87(30.0f) + d) * 0.1f).f();
                    }
                    return cam;
                };
                const float x = pan(p.camX, X87(p.camX) - p.cursorX);
                const float y = pan(p.camY, X87((X87(p.camY) - p.cursorY).f()));
                if (0 <= x && MapGrid::cellOf(x) < 70 && 0 <= y && MapGrid::cellOf(y) < 70) {  // kept on the map: x / 85 and y / 85 under 70, as the exe bounds it
                    p.camX = x;
                    p.camY = y;
                }
            }
        } else if (code == net::ControlCode::Action || (code == net::ControlCode::Jump && (possessed(world, unit) || world.quest()))) {
            // Rejoin; watching a creature, back to itself. (The exe's check decides in those cases:
            // shut out (0x100) or without a clan, no.) In a quest: one waiting in a warp gate is put
            // out of it (nothing else); one out by a quest exit or not in the quest
            // follows the next; one in the quest rejoins where it is. One not in the quest first
            // tries to come in: "MainBG.wnd:Loading" while its saved character isn't loaded
            // yet, else room and in; none, "GeneralPrint:QuestGameFull".
            bool mayRejoin = false;
            const bool out = (p.statusFlags & 0x20u) || p.heldOut; // (noxcpp: heldOut, a NoX Core-only game's stock player)
            if (!out && world.quest()) {
                if (p.questActive == 0) {
                    std::string_view id;
                    if (p.questAwaitingSave) {
                        id = "MainBG.wnd:Loading";
                    } else {
                        p.questActive = questRoomFor(world) ? 1u : 0u;
                        if (p.questActive == 1) {
                            questPlayerJoined(fight, unit);
                        } else {
                            id = "GeneralPrint:QuestGameFull";
                        }
                    }
                    if (!id.empty()) {
                        world.broadcasts().push_back({informStringBytes(id), true, unit.extent});
                    }
                }
                if (p.warpGate != 0) {
                    questLeaveWarpGate(fight, unit);
                    continue;
                }
                mayRejoin = p.exitGate == 0 && p.questActive != 0;
            } else if (!out) {  // out of a quest: no rejoining in clan games, under game flag 0x8000, or shut out (0x100) while anyone plays
                mayRejoin = !(world.clanTeams || (world.gameFlags & 0x8000u) || ((p.statusFlags & 0x100u) && anyonePlaying(world)));
            }
            if (mayRejoin && possessed(world, unit)) {
                observeClear(fight, unit);
            } else if (mayRejoin) {
                joinTeam(fight, unit);
                leaveObserver(fight, unit);
                p.cameraFollow = 0;
                if (!world.quest()) { // (a quest's stays where it is)
                    const auto [x, y] = findPlayerStart(world, &unit, fight.rng);
                    setPosition(world, unit, x, y, fight.frame);
                    p.camX = x;
                    p.camY = y;
                }
            } else {
                followNext(fight, unit);
            }
        } else if (code == net::ControlCode::Jump) {
            // Its own creature nearest the camera within 100 (its square), else a free camera.
            ServerObject* best = nullptr;
            float bestD = 1e8f; // (0x4CBEBC20)
            // (getUnitsInRect over the camera's 100 square, by the grid)
            // (Each edge stored from the x87: chopped.)
            world.unitsInRect((util::X87(p.camX) - 100.0f).f(), (util::X87(p.camY) - 100.0f).f(), (util::X87(p.camX) + 100.0f).f(),
                              (util::X87(p.camY) + 100.0f).f(), [&](ServerObject& o) {
                if (!(o.classes & oc::Monster) || o.owner != unit.extent) { // (no 0x20 test)
                    return;
                }
                const util::X87 dx = util::X87(o.x) - p.camX; // (on the x87's stack; the best stored)
                const util::X87 dy = util::X87(o.y) - p.camY;
                if (dx * dx + dy * dy < util::X87(bestD)) {
                    best = &o;
                    bestD = (dx * dx + dy * dy).f();
                }
            });
            if (!best || best->extent == p.cameraFollow) {
                p.cameraFollow = 0;
                p.freeCam = true;
            } else {
                follow(p, best->extent);
                p.freeCam = false;
            }
        }
    }
    if (any) {
        p.moveHeldBefore = p.moveHeld;
    }
}

void winnerMessage(World& world, std::uint8_t op, std::uint16_t who, bool byTime, std::uint32_t frame)
{
    // {op, u16 the winner (a team id or a net code), u8 by the time limit, u32 frame}
    std::vector<std::uint8_t> m = {op, lo(who), hi(who), static_cast<std::uint8_t>(byTime ? 1 : 0)};
    putU32(m, frame);
    toAll(world, std::move(m));
}

std::vector<std::uint8_t> killFeedMessage(std::uint16_t killer, std::uint16_t assist, std::uint16_t victim, std::uint16_t weapon, std::uint8_t kind)
{
    return {0xA9, 0x0E, lo(killer), hi(killer), lo(assist), hi(assist), lo(victim), hi(victim), lo(weapon), hi(weapon), kind};
}

void checkVictory(Fight& fight)
{
    World& world = fight.world;
    const std::uint16_t limit = enforcedLessonLimit(world);
    if (world.gameFlags & GameFlag::Elimination) {
        // (limit: the lessons limit, here the lives each player has.)
        // The players in play with lives left all on one team, or one alone.
        if (limit == 0) {
            return;
        }
        ServerObject* winner = nullptr;
        ServerTeam* team = nullptr;
        for (ServerObject* pu : world.playerUnits()) {
            ServerObject& o = *pu;
            if (!o.player || o.gone() || (o.player->statusFlags & 1u) || limit <= o.player->deaths) {
                continue;
            }
            if (o.team == 0) {
                // (A second side with lives left: not over yet.)
                if (winner || team) {
                    return;
                }
                winner = &o;
            } else if (!team) {
                team = teamById(world, o.team);
            } else if (team != teamById(world, o.team)) {
                return;
            }
        }
        if (!anyonePlaying(world)) {
            return;
        }
        world.gameFlags |= GameFlag::GameOver;
        if (team) {
            winnerMessage(world, 0x59, team->id, false, fight.frame);  // MSG_REPORT_DEATHMATCH_TEAM_WINNER
        } else {
            winnerMessage(world, 0x58, winner ? netCodeOf(*winner) : 0, false, fight.frame);  // MSG_REPORT_DEATHMATCH_WINNER
        }
        return;
    }
    if (world.gameFlags & GameFlag::Coop) {
        return;
    }
    if (limit == 0) {
        return;
    }
    // A team at the limit first, then a player in play.
    for (ServerTeam& t : world.teams) {
        if (t.active && limit <= t.lessons) {
            world.gameFlags |= GameFlag::GameOver;
            winnerMessage(world, 0x59, t.id, false, fight.frame);
            return;
        }
    }
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player && !o.gone() && !(o.player->statusFlags & 1u) && limit <= o.player->lessons) {
            world.gameFlags |= GameFlag::GameOver;
            winnerMessage(world, 0x58, netCodeOf(o), false, fight.frame);
            return;
        }
    }
}

void checkTimeLimit(Fight& fight)
{
    World& world = fight.world;
    if (!timerRunOut(world, fight.frame)) {
        return;
    }
    if (world.quest()) {
        questTimeUp(fight); // (the exits' next map, "chklimit.c:AutoExitToNextMap")
        return;
    }
    if (world.countdown) {
        // (0x9A is MSG_FX_WHITE_FLASH.)
        // Elimination's sudden death: game flag 0x4000000, MSG 0x9A at each player's camera and
        // 0x246 to each.
        world.gameFlags |= 0x4000000u;
        std::vector<const ServerObject*> units;
        for (const ServerObject* pu : world.playerUnits()) {
            const ServerObject& o = *pu;
            if (o.player) {
                const auto x = static_cast<std::uint16_t>(static_cast<int>(o.player->camX));
                const auto y = static_cast<std::uint16_t>(static_cast<int>(o.player->camY));
                world.broadcasts().push_back({{0x9A, lo(x), hi(x), lo(y), hi(y)}, true, o.extent});
                units.push_back(&o);
            }
        }
        for (const ServerObject* o : units) {
            playSound(world, 0x246, *o, 2, netCodeOf(*o));
        }
        setTimerRunning(world, false, fight.frame);
        return;
    }
    matchWinner(fight);
}

void observerLeftPlay(Fight& fight)
{
    // SrvNet_OnServerCmd 0's end: elimination down to one in play is over.
    if ((fight.world.gameFlags & GameFlag::Elimination) && playersInPlay(fight.world) == 1) {
        matchWinner(fight);
    }
}

void playerRemoved(Fight& fight, ServerObject& unit)
{
    // Plr_RemoveFromGame, after its unit's deletion: what every player was told of its
    // slot forgotten; in every mode (Plr_GoObserver's test skips a quest, this one doesn't) fewer
    // than two in play, the match's reset; else elimination down to one in play is over
    // (playersInPlay == 1); then a quest's end of it (the exits, the warp, the
    // countdown).
    World& world = fight.world;
    if (!unit.player) {
        return;
    }
    // Its trade cancelled, a quest's shop kept for its slot freed.
    cancelTrade(fight, unit);
    dropQuestShop(world, unit.player->index);
    questForgetSlot(world, unit.player->index);
    unit.player->statusFlags = (unit.player->statusFlags | 1u) & ~0x20u; // (its unit gone: counted by neither)
    if (!anyonePlaying(world)) {
        nobodyLeftCheck(fight);
    } else {
        observerLeftPlay(fight);
    }
    if (world.quest()) {
        questAfterLeave(fight);
    }
}

void matchWinner(Fight& fight)
{
    World& world = fight.world;
    // The best team (and in an arena or KotR the best player without a team, not observing): a
    // tie between the best, none.
    // Team scores: `most` picks the highest (lessons), else the lowest; a tie only when a second team equals the best.
    const auto best = [&](bool most, ServerTeam*& bestTeam, ServerObject*& bestPlayer) {
        bool tie = false;
        int score = most ? INT32_MIN : INT32_MAX;
        bestTeam = nullptr;
        bestPlayer = nullptr;
        for (ServerTeam& t : world.teams) {
            if (!t.active) {
                continue;
            }
            if (most ? score <= t.lessons : t.lessons <= score) {
                tie = t.lessons == score && bestTeam;
                score = t.lessons;
                bestTeam = &t;
            }
        }
        return std::pair{tie, score};
    };
    ServerTeam* team = nullptr;
    ServerObject* player = nullptr;
    if (world.gameFlags & (GameFlag::CaptureTheFlag | GameFlag::FlagBall)) {
        // The best team's score (from -1).
        bool tie = false;
        int score = -1;
        for (ServerTeam& t : world.teams) {
            if (t.active && score <= t.lessons) {
                tie = t.lessons == score && team;
                score = t.lessons;
                team = &t;
            }
        }
        world.gameFlags |= GameFlag::GameOver;
        if (world.gameFlags & GameFlag::FlagBall) {
            // (A tie: the exe's SrvNet_BroadcastFlagBallWinner(NULL) reads through the null:
            // an original bug. Here team 0.)
            winnerMessage(world, 0x56, team && !tie ? team->id : 0, false, fight.frame);  // MSG_REPORT_FLAG_BALL_WINNER
        } else {
            winnerMessage(world, 0x57, team && !tie ? team->id : 0xFFFF, true, fight.frame);  // MSG_REPORT_FLAG_WINNER (0xFFFF: a tie, nobody)
        }
        setTimerRunning(world, false, fight.frame);
        return;
    }
    if (world.gameFlags & (GameFlag::Arena | GameFlag::KingOfTheRealm | GameFlag::Elimination)) {
        // The best (the most lessons; elimination: the fewest deaths).
        const bool elim = (world.gameFlags & GameFlag::Elimination) && !(world.gameFlags & (GameFlag::Arena | GameFlag::KingOfTheRealm));
        auto [tie, score] = best(!elim, team, player);
        // Then the players without a team, by the same rule, against the best team's score.
        for (ServerObject* pu : world.playerUnits()) {
            ServerObject& o = *pu;
            if (!o.player || o.gone() || o.team != 0 || (o.player->statusFlags & 1u)) {
                continue;
            }
            const int v = elim ? o.player->deaths : o.player->lessons;
            if (elim ? v <= score : score <= v) {
                tie = v == score && player;
                score = v;
                player = &o;
            }
        }
        world.gameFlags |= GameFlag::GameOver;
        if (tie) {
            winnerMessage(world, 0x59, 0, true, fight.frame);
        } else if (player) {
            winnerMessage(world, 0x58, netCodeOf(*player), true, fight.frame);
        } else if (team) {
            winnerMessage(world, 0x59, team->id, true, fight.frame);
        }
    }
    setTimerRunning(world, false, fight.frame);
}

void placeTeamMembers(World& world, std::uint32_t mapFlags, std::minstd_rand& rng, std::uint32_t frame)
{
    if (!(world.gameFlags & GameFlag::Solo) && !(world.gameplayFlags & 4u)) {
        return;
    }
    for (auto& [extent, o] : world.objects()) {
        if (o.team == 0 || o.holder != 0 || ((o.classes & oc::Flag) && (mapFlags & 0x80000000u))) {
            continue;
        }
        addTeamMember(world, o.team, o, false, false, rng, frame);
    }
}

namespace {

/// The share-out(0): the players in play without a team, shuffled (50 random swaps), each to the
/// team with the fewest, told to everyone and put at its base. (Clan play: by
/// clan, with the clans.)
void shareOutLoose(Fight& fight)
{
    World& world = fight.world;
    std::vector<ServerObject*> loose;
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player && !o.gone() && (!(o.player->statusFlags & 1u) || (o.player->statusFlags & 0x20u)) && o.team == 0) {
            loose.push_back(&o);
        }
    }
    if (1 < loose.size()) {
        nox::util::uniform_int_distribution<int> pick(0, static_cast<int>(loose.size()) - 1);
        for (int i = 0; i < 50; ++i) {
            const int a = pick(fight.rng);
            const int b = pick(fight.rng);
            std::swap(loose[static_cast<std::size_t>(a)], loose[static_cast<std::size_t>(b)]);
        }
    }
    for (ServerObject* o : loose) {
        if (!world.clanTeams) {
            if (ServerTeam* t = emptiestTeam(world)) {
                addTeamMember(world, t->id, *o, true, true, fight.rng, fight.frame);
            }
        }
    }
}

/// The flags on the map, counted; with any (and not game
/// flag 0x8000), the loose players shared out.
bool flagsOnMap(Fight& fight)
{
    World& world = fight.world;
    world.flagCount = 0;
    for (const auto& [extent, o] : world.objects()) {
        if ((o.classes & oc::Flag) && !o.gone()) {
            ++world.flagCount;
        }
    }
    if (0 < world.flagCount && !(world.gameFlags & 0x8000u)) {
        shareOutLoose(fight);
    }
    return 0 < world.flagCount;
}

void deleteObject(ServerObject& o)
{
    o.pendingDelete = true; // (delayedDeleteObject; forgetRespawn's minimap: with the marks)
}

} // namespace

void beforeMapLoad(Fight& fight)
{
    World& world = fight.world;
    // Each player drops the flags it carries (class 0x10000000), each at a point
    // within 50 of it that it sees, by Obj_DropAt: they stay with the old map.
    if (fight.things) {
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        std::vector<ServerObject*> players;
        for (ServerObject* pu : world.playerUnits()) {
            ServerObject& o = *pu;
            if (o.player && !o.gone()) {
                players.push_back(&o);
            }
        }
        for (ServerObject* p : players) {
            for (const std::uint32_t e : std::vector<std::uint32_t>(p->inventory)) {
                ServerObject* item = world.byExtent(e);
                if (item && item->holder == p->extent && (item->classes & oc::Flag)) {
                    const auto [x, y] = clearPointNear(world, fight.rng, p->x, p->y, 50.0f);
                    items.drop(*p, *item, x, y);
                }
            }
        }
    }
    // Every player's status 0x10 (in the game) off, till its CLIENT_READY on the new
    // map (Plr_ClearStatusFlags: 0x10 isn't told).
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player) {
            o.player->statusFlags &= ~0x10u;
        }
    }
    // The share-out(1) with gameplay flags 2 and 4, not game flags 0xC000 nor a chat game (0x80):
    // every team emptied (Team_RemoveAllMembers), then the players shared out afresh.
    if ((world.gameplayFlags & 2u) && !(world.gameFlags & 0xC000u) && (world.gameplayFlags & 4u) &&
        !(world.gameFlags & GameFlag::Chat)) {
        shareOutPlayers(fight, true);
    }
}

void shareOutPlayers(Fight& fight, bool emptyFirst)
{
    if (emptyFirst) {
        for (ServerTeam& t : fight.world.teams) {
            if (t.active) {
                removeAllTeamMembers(fight.world, t); // (Team_RemoveAllMembers)
            }
        }
    }
    shareOutLoose(fight);
}

namespace {
constexpr const char* kColorTitles[10] = {
    "advserv.wnd:None", "modifier.db:MaterialTeamRedDesc", "modifier.db:MaterialTeamBlueDesc",
    "modifier.db:MaterialTeamGreenDesc", "modifier.db:MaterialTeamCyanDesc", "modifier.db:MaterialTeamYellowDesc",
    "modifier.db:MaterialTeamVioletDesc", "modifier.db:MaterialTeamBlackDesc", "modifier.db:MaterialTeamWhiteDesc",
    "modifier.db:MaterialTeamOrangeDesc"}; // (Team_GetColorTitle's)
} // namespace

void flagGameTeams(World& world)
{
    // Go (with Capture the Flag or Flag Ball, settings 0x60): teams made till there
    // are two (Team_Create(0)), the first coloured 1 when there was none, else 2, each named by
    // its colour's title (Team_GetColorTitle; a clan game's (clanTeams) would be nameless: no
    // clan games here) and told (SrvNet_SendTeamCreate); then team play (gameplay flag 4).
    for (int n = teamCount(world); n < 2; ++n) {
        ServerTeam* team = createTeam(world, 0);
        if (!team) {
            break;
        }
        const int color = (n != 0) + 1;
        setTeamName(*team, world.text(kColorTitles[color]), true);
        team->color = static_cast<std::uint8_t>(color);
        toAll(world, teamMessage(world, *team));
    }
    world.gameplayFlags |= 4u;
}

void createFlagTeams(Fight& fight)
{
    // wndGuiTeamCreate.
    World& world = fight.world;
    world.gameplayFlags |= 4u;
    createTeam(world, 0);
    createTeam(world, 0);
    for (auto& [extent, o] : world.objects()) {
        if (!(o.classes & oc::Flag) || o.gone()) {
            continue;
        }
        ServerTeam* team = teamById(world, o.team);
        if (!team) {
            continue;
        }
        const int color = flagColor(o, fight.modifiers);
        if (0 <= color && color < 10) {
            setTeamName(*team, world.text(kColorTitles[color]), true);
        }
        team->color = static_cast<std::uint8_t>(color);
        toAll(world, teamMessage(world, *team)); // (SrvNet_SendTeamCreate)
        team->base = o.extent;
    }
}

void setTeamBases(World& world, bool on)
{
    // toggleAllTeamFlags: each team's base on (objectSetOn) or off.
    for (ServerTeam& t : world.teams) {
        if (ServerObject* base = t.active && t.base ? world.byExtent(t.base) : nullptr) {
            setOnOff(*base, on);
            if (on) {
                queueMotion(world, *base); // (objectSetOn's unitHasCollideOrUpdateFn)
            }
        }
    }
}

void renameTeam(World& world, ServerTeam& team, std::u16string_view name)
{
    // Team_Rename: no colour title now; MSG_TEAM_MSG 4 {u32 id, the name: 20 wide characters}.
    team.useTitle = false;
    std::vector<std::uint8_t> m = {0xC4, 0x04};
    putU32(m, team.id);
    for (std::size_t i = 0; i < 20; ++i) {
        const char16_t c = i < name.size() && i < 19 ? name[i] : 0;
        m.push_back(static_cast<std::uint8_t>(c));
        m.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    toAll(world, std::move(m));
    team.name = std::u16string(name.substr(0, 19));
}

void setupMode(Fight& fight, std::uint32_t mapFlags, std::uint32_t chosen)
{
    World& world = fight.world;
    // (The game flags' mode bits, as modeIndex's.)
    constexpr std::uint32_t kModes = 0x17F0u;  // every mode bit but Solo (0x800): KotR, CTF, FlagBall, Chat, Arena, Coop, Elimination, Quest
    const auto setMode = [&](std::uint32_t mode) { world.gameFlags = (world.gameFlags & ~kModes) | mode; };
    if ((world.gameFlags & GameFlag::Elimination) && ((mapFlags & 0x80000000u) || !world.keepAutoRespawn)) {
        world.autoRespawn = false;
    }
    // Out of a quest to a chat map: the stat multipliers back (told), the teams gone.
    if ((world.gameFlags & GameFlag::Quest) && (mapFlags & 0x80000000u)) {
        endQuestStats(world, true);
        removeAllTeams(world, true);
    }
    // A solo map (type bit 1): the solo game (soloTeam first: 0x200, team 1).
    if (mapFlags & 1u) {
        world.gameFlags &= ~kModes;
        soloTeam(world, fight.rng, fight.frame);
        world.gameFlags |= GameFlag::Solo;
        return;
    }
    // A quest map (bit 2): the quest, begun when it follows a chat game.
    if (mapFlags & 2u) {
        const bool fromChat = (world.gameFlags & GameFlag::Chat) != 0;
        setMode(GameFlag::Quest);
        if (fromChat) {
            startQuest(fight);
        }
        return;
    }
    if (mapFlags & 0x80000000u) {
        // A chat map: the timer off, chat mode; with teams, its flags their bases, and on.
        setTimerRunning(world, false, fight.frame);
        setMode(GameFlag::Chat);
        if (0 < teamCount(world)) {
            for (auto& [extent, o] : world.objects()) {
                if ((o.classes & oc::Flag) && o.team != 0) {
                    if (ServerTeam* t = teamById(world, o.team)) {
                        t->base = o.extent; // (its colour from the flag's: not yet)
                    }
                }
            }
            if (!(world.gameplayFlags & 2u) && !(world.gameFlags & 0x8000u)) {
                for (ServerTeam& t : world.teams) {
                    if (ServerObject* base = t.active && t.base ? world.byExtent(t.base) : nullptr) {
                        setOnOff(*base, true);
                        queueMotion(world, *base); // (objectSetOn's unitHasCollideOrUpdateFn)
                    }
                }
            }
        }
        return;
    }
    // The modes the map's type bits allow.
    std::uint32_t modes = 0;
    if (mapFlags & 4u) {
        modes |= GameFlag::Arena;
    }
    if (mapFlags & 0x20u) {
        modes |= GameFlag::Elimination;
    }
    if (mapFlags & 8u) {
        modes |= GameFlag::CaptureTheFlag;
    }
    if (mapFlags & 0x10u) {
        modes |= GameFlag::KingOfTheRealm;
    }
    if (mapFlags & 0x40u) {
        modes |= GameFlag::FlagBall;
    }
    // The chosen mode not one the map allows: its mode bits (0xE80F keeps the rest) replaced by the map's first.
    if ((modes & chosen) == 0) { // (the map's first)
        std::uint32_t first = (mapFlags & 2u) ? 0xF80u + 0x80u : 0x80u;
        if (mapFlags & 4u) {
            first = GameFlag::Arena;
        } else if (mapFlags & 0x20u) {
            first = GameFlag::Elimination;
        } else if (mapFlags & 8u) {
            first = GameFlag::CaptureTheFlag;
        } else if (mapFlags & 0x10u) {
            first = GameFlag::KingOfTheRealm;
        } else if (mapFlags & 0x40u) {
            first = GameFlag::FlagBall;
        }
        chosen = (chosen & 0xE80Fu) | first;
    }
    world.chosenMode = chosen;
    if (!(chosen & GameFlag::KingOfTheRealm)) {
        for (auto& [extent, o] : world.objects()) {
            if (o.typeName == NameId::Crown) { // (mapFindCrown)
                deleteObject(o);
                forgetRespawn(world, o.extent);
            }
        }
    }
    if (chosen & GameFlag::CaptureTheFlag) {
        if (flagsOnMap(fight)) {
            setMode(GameFlag::CaptureTheFlag);
        }
    } else if (chosen & GameFlag::FlagBall) {
        // mapInfoSetFlagball: flags, and a ball made at a start; Wall (0x84) off.
        if (flagsOnMap(fight) && spawnBall(fight, nullptr)) {
            setMode(GameFlag::FlagBall);
            world.spellDisabled[0x84] = true;
        }
    } else if (chosen & GameFlag::KingOfTheRealm) {
        // mapInfoSetKotr: the flags go; a crown with no team goes in team play, one
        // with a team (its team's crown) without; those kept on everyone's minimap;
        // any makes it KotR.
        for (auto& [extent, o] : world.objects()) {
            if (o.classes & oc::Flag) {
                deleteObject(o);
            }
        }
        int crowns = 0;
        for (auto& [extent, o] : world.objects()) {
            if (o.typeName != NameId::Crown || o.pendingDelete) {
                continue;
            }
            ++crowns;
            if (!(world.gameplayFlags & 4u)) {  // gameplayFlags bit 2: team play
                if (o.team != 0) {
                    deleteObject(o);
                    forgetRespawn(world, o.extent);
                } else {
                    markMinimapForAll(world, o, 1);
                }
            } else if (o.team == 0) {
                deleteObject(o);
                forgetRespawn(world, o.extent);
            } else if (ServerTeam* t = teamById(world, o.team)) {
                t->crown = o.extent;
                markMinimapForAll(world, o, 1);
            }
        }
        if (crowns != 0) {
            setMode(GameFlag::KingOfTheRealm);
        }
    } else if (chosen & GameFlag::Elimination) {
        if (!(world.gameFlags & GameFlag::Elimination)) {
            world.autoRespawn = true; // (ruleSetNoRespawn(1): respawning by itself)
        }
        setMode(GameFlag::Elimination);
    } else {
        setMode(GameFlag::Arena);
    }
}

int flagColor(const ServerObject& flag, const things::ModifierDb* modifiers)
{
    if (!(flag.classes & oc::Flag) || !modifiers) {
        return 0;
    }
    const things::ModifierDef* m = modifiers->modifier(flag.modifiers[1]);
    if (!m) {
        return 0;
    }
    static constexpr std::pair<std::string_view, int> kColors[] = {
        {"MaterialTeamRed", 1},  {"MaterialTeamGreen", 3},  {"MaterialTeamBlue", 2},  {"MaterialTeamYellow", 5}, {"MaterialTeamCyan", 4},
        {"MaterialTeamViolet", 6}, {"MaterialTeamBlack", 7}, {"MaterialTeamWhite", 8}, {"MaterialTeamOrange", 9},
    };
    for (const auto& [name, color] : kColors) {
        if (m->name == name) {
            return color;
        }
    }
    return 0;
}

std::vector<std::uint8_t> flagStatusMessage(const World& world, std::uint8_t team)
{
    const World::FlagStatus& s = world.flagStatus[team];
    return {0xD8, s.state, team, s.color, lo(s.carrier), hi(s.carrier)};
}

void flagStatus(World& world, std::uint8_t team, std::uint8_t state, std::uint8_t color, std::uint16_t carrier)
{
    world.flagStatus[team] = {state, color, carrier};
    toAll(world, flagStatusMessage(world, team));
}

namespace {

/// SrvNet_BroadcastInform: MSG_INFORM 0xA9 to every player, its code and u32s.
void informAll(World& world, std::uint8_t code, std::initializer_list<std::uint32_t> values)
{
    std::vector<std::uint8_t> m = {0xA9, code};
    for (const std::uint32_t v : values) {
        putU32(m, v);
    }
    toAll(world, std::move(m));
}

} // namespace

void placeAt(World& world, ServerObject& o, float x, float y, std::uint32_t frame)
{
    o.flags = (o.flags & 0x35E9FEDBu) | ObjFlag::Active;
    o.born = frame;
    o.x = o.frameStartX = o.newX = x;
    o.y = o.frameStartY = o.newY = y;
    o.vx = o.vy = 0;
    o.touch();
    world.noteNew(o); // (the new list: the grid and the lists at the next pass)
}

namespace {

/// Its enchantments from spells of flag 0x80000 (Haste...) end.
void stripCarrierSpells(Fight& fight, ServerObject& unit)
{
    for (int e = 0; e < 32; ++e) {
        if (!enchanted(unit, e)) {
            continue;
        }
        const std::uint32_t spell = enchantmentSpell(e);
        const things::SpellDef* def = spell && fight.things ? fight.things->findSpell(spell) : nullptr;
        if (def && (def->flags & 0x80000u)) {
            disenchant(fight.world, unit, e);
        }
    }
}

/// pickupFlagCtf: a live player touching a flag in CTF. Another team's flag taken, its own
/// brought home, or (its own at home) the flags it carries captured.
void pickupFlagCtf(Fight& fight, ServerObject& flag, ServerObject& player)
{
    World& world = fight.world;
    const int color = flagColor(flag, fight.modifiers);
    const std::uint8_t team = flag.team;
    const bool sameTeam = player.team != 0 && flag.team != 0 && player.team == flag.team; // (servCompareTeams)
    if (!sameTeam) {
        if (flag.holder != 0 || !fight.things || !player.player) {
            return;
        }
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        const ServerTeam* owners = teamById(world, flag.team);
        if (!owners || teamPlayers(world, *owners) == 0) {
            world.broadcasts().push_back({informStringBytes("objcoll.c:FlagNoTeam"), true, player.extent}); // (SrvNet_SendInformString)
            return;
        }
        // A flag it carries is dropped first (invForceDropItem: within 50).
        for (const std::uint32_t e : player.inventory) {
            if (ServerObject* o = world.byExtent(e); o && (o->classes & oc::Flag)) {
                const auto [x, y] = clearPointNear(world, fight.rng, player.x, player.y, 50.0f);
                items.drop(player, *o, x, y);
                break;
            }
        }
        // Out of the world (servFinalizeDelObject), into its inventory.
        world.removed().push_back(flag.extent);
        flag.flags &= ~ObjFlag::Active;
        flag.decayAt = 0;
        items.receive(player, flag);
        player.player->weaponEquip |= 1u;
        items.reportEquip(player, flag);
        informAll(world, 6, {netCodeOf(player), static_cast<std::uint32_t>(color)});
        unmarkMinimapForAll(world, flag, 1); // (netUnmarkMinimapSpec)
        flag.flag->dropped = 0;
        flagStatus(world, team, 1, static_cast<std::uint8_t>(color), netCodeOf(player));
        stripCarrierSpells(fight, player);
        return;
    }
    // Its own team's flag: away from home, back there.
    if (5.0f < std::abs(flag.x - flag.flag->homeX) || 5.0f < std::abs(flag.y - flag.flag->homeY)) {
        setPosition(world, flag, flag.flag->homeX, flag.flag->homeY, fight.frame);
        informAll(world, 4, {netCodeOf(player)});
        flag.flag->dropped = 0;
        flagStatus(world, player.team, 0, static_cast<std::uint8_t>(color), 0);
        return;
    }
    // At home: the flags it carries are captured.
    if (!fight.things) {
        return;
    }
    Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
        ServerObject* taken = world.byExtent(e);
        if (!taken || !(taken->classes & oc::Flag) || !taken->flag) {
            continue;
        }
        // (NoX Core's map editor session sandbox: the capture counts nothing, no limit; the flag
        // still goes home. Never set elsewhere.)
        const std::uint16_t limit = world.sandboxNoScoring ? 0 : world.lessonLimits[2]; // (servGamedataGet(0x20): CTF's)
        const int takenColor = flagColor(*taken, fight.modifiers);
        const std::uint8_t takenTeam = taken->team;
        if (!world.sandboxNoScoring) {
            changeLessons(player, 1);
            reportLesson(world, player);
            if (ServerTeam* t = player.team != 0 ? teamById(world, player.team) : nullptr) {
                setTeamScore(world, *t, t->lessons + 1); // (and the WOL kill record: not here)
            }
        }
        items.removeFromInventory(player, *taken);
        placeAt(world, *taken, taken->flag->homeX, taken->flag->homeY, fight.frame);
        markMinimapForAll(world, *taken, 1); // (netMarkMinimapForAll)
        taken->flag->dropped = 0;
        flagStatus(world, takenTeam, 0, static_cast<std::uint8_t>(takenColor), 0);
        informAll(world, 5, {netCodeOf(player), static_cast<std::uint32_t>(takenColor)});
        if (limit != 0) {
            for (const ServerTeam& t : world.teams) {
                if (t.active && static_cast<int>(limit) <= t.lessons) {
                    world.gameFlags |= GameFlag::GameOver;
                    winnerMessage(world, 0x57, t.id, false, fight.frame); // (SrvNet_BroadcastFlagWinner)
                    return;
                }
            }
            return;
        }
    }
}

} // namespace

void flagUpdate(Fight& fight, ServerObject& flag)
{
    if (!flag.flag || flag.flag->dropped == 0) {
        return;
    }
    // On the ground past 30 s (gameFPS x 30 frames): home, 0x131 where it lay.
    if (30u * 30u < fight.frame - flag.flag->dropped) {
        const int color = flagColor(flag, fight.modifiers);
        playSound(fight.world, 0x131, flag);
        flag.flag->dropped = 0;
        flagStatus(fight.world, flag.team, 0, static_cast<std::uint8_t>(color), 0);
        setPosition(fight.world, flag, flag.flag->homeX, flag.flag->homeY, fight.frame);
        informAll(fight.world, 8, {static_cast<std::uint32_t>(color)});
    }
}

void flagCollide(Fight& fight, ServerObject& flag, ServerObject* other)
{
    if (!other || (other->flags & ObjFlag::Dead) || !flag.flag) {
        return;
    }
    if (fight.world.gameFlags & GameFlag::CaptureTheFlag) {
        if (other->classes & oc::Player) {
            pickupFlagCtf(fight, flag, *other);
        }
    } else if ((fight.world.gameFlags & GameFlag::FlagBall) && (other->typeName == "GameBall" || (other->classes & oc::Player))) {
        ballAtFlag(fight, flag, *other);
    }
}

std::vector<std::uint8_t> ballStatusMessage(const World& world)
{
    return {0xD9, world.ballState, lo(world.ballCarrier), hi(world.ballCarrier)};
}

void ballStatus(World& world, std::uint8_t state, std::uint16_t carrier)
{
    world.ballState = state;
    world.ballCarrier = carrier;
    toAll(world, ballStatusMessage(world));
}

ServerObject* heldBall(World& world, const ServerObject& unit)
{
    // (The objects with a ball's record, in the objects' order: the first a walk of them all finds.)
    for (ServerObject* o : world.ballUnits()) {
        if (o->owner == unit.extent && o->ball && !o->gone()) {
            return o;
        }
    }
    return nullptr;
}

namespace {

/// The ball's last holder (the player up its owners; else nobody), its team and
/// the frame.
void setBallToucher(Fight& fight, ServerObject& ball, ServerObject* by)
{
    ServerObject* p = by ? fight.parentPlayer(by) : nullptr;
    if (p && (p->classes & oc::Player)) {
        ball.ball->toucher = p->extent;
        ball.ball->toucherTeam = p->team;
        ball.ball->touchedAt = fight.frame;
        return;
    }
    ball.ball->toucher = 0;
    ball.ball->toucherTeam = 0;
}

/// A random GameBallStart (randInt over their count), or null.
ServerObject* randomBallStart(Fight& fight)
{
    std::vector<ServerObject*> starts;
    for (auto& [extent, o] : fight.world.objects()) {
        if (o.typeName == NameId::GameBallStart && !o.gone()) {
            starts.push_back(&o);
        }
    }
    if (starts.empty()) {
        return nullptr;
    }
    return starts[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(starts.size()) - 1)(fight.rng))];
}

/// Put at (x, y), still.
void placeStill(Fight& fight, ServerObject& o, float x, float y)
{
    setPosition(fight.world, o, x, y, fight.frame);
    o.vx = o.vy = 0;
    o.forceX = o.forceY = 0;
}

/// Back to a random start after a score (the scoring's end).
void ballToStart(Fight& fight, ServerObject& ball)
{
    ServerObject* start = randomBallStart(fight);
    if (!start) {
        return;
    }
    clearOwner(fight.world, ball);
    setBallToucher(fight, ball, nullptr);
    removeTeamMember(fight.world, ball);
    ball.health = ball.maxHealth; // (unitHPsetOnMax)
    ball.ball->heldAt = fight.frame;
    placeStill(fight, ball, start->x, start->y);
    ballStatus(fight.world, 0, 0);
    pointFx(fight.world, 0x81, ball.x, ball.y);
}

/// Let go of: no longer NO_COLLIDE, pushed away from (x, y), nobody's.
void looseBall(World& world, ServerObject& ball, float x, float y, float force)
{
    ball.flags &= ~ObjFlag::NoCollide;
    applyForce(world, ball, x, y, force);
    clearOwner(world, ball);
}

} // namespace

bool spawnBall(Fight& fight, ServerObject* old)
{
    World& world = fight.world;
    ServerObject* start = randomBallStart(fight);
    if (!start || !fight.things) {
        return false;
    }
    ServerObject* ball = world.createAt(*fight.things, "GameBall", 0, 0, fight.frame);
    if (!ball || !ball->ball) {
        return false;
    }
    ball->ball->heldAt = fight.frame;
    ball->ball->possession = static_cast<std::uint32_t>(static_cast<int>(world.number("FlagballPossDuration"))); // (Math_FloatToInt: chopped)
    ball->ball->resetSpeed = world.number("FlagballResetVel");
    markMinimapForAll(world, *ball, 1);
    clearOwner(world, *ball);
    setBallToucher(fight, *ball, nullptr);
    ballStatus(world, 0, 0);
    placeStill(fight, *ball, start->x, start->y);
    if (old) {
        for (ServerObject* pu : world.playerUnits()) {
            ServerObject& o = *pu;
            if (o.player && o.player->cameraFollow == old->extent) {
                o.player->cameraFollow = ball->extent; // (playerCameraUnlock, then follow)
            }
        }
        old->pendingDelete = true;
    }
    return true;
}

// Each frame for the ball: a new one after 20 s unheld; loose, it collides; held, carried in front
// of its holder till the possession time runs out or the holder dies.
void ballUpdate(Fight& fight, ServerObject& ball)
{
    World& world = fight.world;
    if (!ball.ball) {
        return;
    }
    auto& b = *ball.ball;
    ball.drag = 0.01f;
    if (b.toucher != 0 && !world.byExtent(b.toucher)) {
        setBallToucher(fight, ball, nullptr);
        removeTeamMember(world, ball);
        ballStatus(world, 1, 0);
    }
    // Not held for 20 s (the exe's 20000 ms): a new ball.
    if (600u < fight.frame - b.heldAt) {
        spawnBall(fight, &ball);
        return;
    }
    ServerObject* owner = ball.owner ? world.byExtent(ball.owner) : nullptr;
    if (!owner) {
        ball.owner = 0;
        ball.flags &= ~ObjFlag::NoCollide;
        if (util::sqrt(util::X87(ball.vy) * ball.vy + util::X87(ball.vx) * ball.vx) < util::X87(b.resetSpeed)) { // (on the stack)
            b.toucher = 0; // (its team kept)
        }
        return;
    }
    if (owner->extent == b.toucher && b.possession < fight.frame - b.touchedAt) {
        // Held too long: it gets away, within 32 either side of the holder's facing.
        const int d = (nox::util::uniform_int_distribution<int>(-32, 32)(fight.rng) + static_cast<std::int16_t>(owner->direction)) & 0xFF;
        const auto [dx, dy] = directionVector(d);
        // (On the x87: x 20 and the difference on the stack, each stored.)
        looseBall(world, ball, (util::X87(ball.x) - util::X87(dx) * 20.0f).f(), (util::X87(ball.y) - util::X87(dy) * 20.0f).f(), 30.0f);
        ballStatus(world, 1, 0);
        playSound(world, 0x39E, ball);
        return;
    }
    ball.flags |= ObjFlag::NoCollide;
    b.heldAt = fight.frame;
    if (owner->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
        clearOwner(world, ball);
        setBallToucher(fight, ball, nullptr);
        removeTeamMember(world, ball);
        ballStatus(world, 1, 0);
        return;
    }
    // Carried in front of its holder, when the way there is clear (flags 5).
    // (On the x87: the reach kept on the stack for both, each place one store.)
    const util::X87 reach = util::X87(owner->radius) + ball.radius + 10.0f;
    const auto [dx, dy] = directionVector(static_cast<std::int16_t>(owner->direction));
    const float x = (reach * dx + owner->x).f();
    const float y = (reach * dy + owner->y).f();
    if (world.traceRay(owner->x, owner->y, x, y, Trace::Doors | Trace::ThroughWindows)) {
        setPosition(world, ball, x, y, fight.frame);
    }
}

// A thing touching the loose ball: a player takes it (its team's now), unless it was the last to
// hold it and has a team-mate about; anything else bounces off.
void ballCollide(Fight& fight, ServerObject& ball, ServerObject* other)
{
    World& world = fight.world;
    if (!ball.ball) {
        return;
    }
    ServerTeam* newTeam = other ? teamById(world, other->team) : nullptr;
    if (other && ball.ball->toucher == other->extent && newTeam && 1 < teamPlayers(world, *newTeam)) {
        // Its last holder, with a team-mate about: not again ("objcoll.c:CantPickupBall", at
        // most every 45 frames, g_ballCantPickupMsgFrame); it bounces (0x3A0).
        if (0x2Du < fight.frame - world.ballRefusedAt) {
            world.broadcasts().push_back({informStringBytes("objcoll.c:CantPickupBall"), true, other->extent});
            world.ballRefusedAt = fight.frame;
        }
        playSound(world, 0x3A0, ball);
        return;
    }
    if (ball.owner != 0 || !other || !(other->classes & oc::Player)) {
        playSound(world, 0x3A0, ball);
        return;
    }
    if (heldBall(world, *other)) {
        return;
    }
    setOwner(world, other, ball); // (unitSetOwner)
    if (ball.team == 0) {
        addTeamMember(world, other->team, ball, true, false, fight.rng, fight.frame);
    } else if (newTeam) {
        moveTeamMember(world, ball, *newTeam, false, fight.rng, fight.frame);
    }
    if (newTeam) {
        ballStatus(world, newTeam->color == 2 ? 4 : 2, netCodeOf(*other));
    }
    setBallToucher(fight, ball, other);
    playSound(world, 0x39F, ball);
    ball.flags |= ObjFlag::NoCollide;
    stripCarrierSpells(fight, *other);
}

void ballAtFlag(Fight& fight, const ServerObject& flag, ServerObject& other)
{
    World& world = fight.world;
    ServerObject* ball = &other;
    if (other.classes & oc::Player) {
        ball = heldBall(world, other);
        if (!ball) {
            return;
        }
    }
    if (!ball->ball) {
        return;
    }
    auto& b = *ball->ball;
    if (b.toucher != 0 && !world.byExtent(b.toucher)) {
        b.toucher = 0;
    }
    // The team after the flag's (by slot; the first after the last).
    const ServerTeam* from = teamById(world, flag.team);
    ServerTeam* team = nullptr;
    if (from) {
        for (std::size_t i = static_cast<std::size_t>(from - world.teams.data()) + 1; i < world.teams.size() && !team; ++i) {
            if (world.teams[i].active) {
                team = &world.teams[i];
            }
        }
    }
    if (!team) {
        for (ServerTeam& t : world.teams) {
            if (t.active) {
                team = &t;
                break;
            }
        }
    }
    ServerObject* scorer = b.toucher ? world.byExtent(b.toucher) : nullptr;
    if (!scorer || !team || scorer->team != team->id) {
        return;
    }
    // (NoX Core's map editor session sandbox: the goal counts nothing, no limit; never set elsewhere.)
    const std::uint16_t limit = world.sandboxNoScoring ? 0 : world.lessonLimits[4]; // (servGamedataGet(0x40): flagball's)
    if (!world.sandboxNoScoring) {
        changeLessons(*scorer, 1);
        reportLesson(world, *scorer);
        setTeamScore(world, *team, team->lessons + 1);
    }
    playSound(world, 0x3A1, other);
    informAll(world, 9, {netCodeOf(*scorer), team->id}); // (u16 team, then 2 bytes the exe leaves unset)
    pointFx(fight.world, 0x9A, ball->x, ball->y);
    if (limit != 0) {
        for (const ServerTeam& t : world.teams) {
            if (t.active && static_cast<int>(limit) <= t.lessons) {
                world.gameFlags |= GameFlag::GameOver;
                winnerMessage(world, 0x56, t.id, false, fight.frame); // (SrvNet_BroadcastFlagBallWinner)
                break;
            }
        }
    }
    ballToStart(fight, *ball);
}

void homeBaseCollide(Fight& fight, ServerObject& base, ServerObject* other)
{
    World& world = fight.world;
    if (!other || other->typeName != "GameBall" || !other->ball) {
        return;
    }
    ServerTeam* team = base.team ? teamById(world, base.team) : nullptr;
    ServerObject* toucher = other->ball->toucher ? world.byExtent(other->ball->toucher) : nullptr;
    ServerTeam* toucherTeam = toucher && toucher->team ? teamById(world, toucher->team) : nullptr;
    const bool scores = !world.sandboxNoScoring; // (NoX Core's map editor session sandbox; never set elsewhere)
    if (team == toucherTeam && toucher && scores) {
        changeLessons(*toucher, 1);
        reportLesson(world, *toucher);
    }
    if (team) {
        if (scores) {
            setTeamScore(world, *team, team->lessons + 1);
        }
        playSound(world, 0x3A1, base);
        pointFx(fight.world, 0x9A, other->x, other->y);
    }
    // To a random start (its holder, team and status kept).
    if (ServerObject* start = randomBallStart(fight)) {
        clearOwner(world, *other);
        placeStill(fight, *other, start->x, start->y);
        pointFx(fight.world, 0x81, other->x, other->y);
    }
}

bool throwBall(Fight& fight, ServerObject& unit)
{
    ServerObject* ball = heldBall(fight.world, unit);
    if (!ball) {
        return false;
    }
    looseBall(fight.world, *ball, unit.x, unit.y, 100.0f);
    playSound(fight.world, 0x39E, unit);
    ballStatus(fight.world, 1, 0);
    return true;
}

void ballHitCarrier(Fight& fight, ServerObject* attacker, ServerObject& victim, int damage)
{
    World& world = fight.world;
    // (Knocked loose only by 30 damage or more.)
    if (!(victim.classes & oc::Player) || damage <= 0x1D) {
        return;
    }
    ServerObject* ball = heldBall(world, victim);
    if (!ball || !ball->ball) {
        return;
    }
    looseBall(world, *ball, victim.x, victim.y, 30.0f);
    setBallToucher(fight, *ball, &victim);
    const std::uint8_t team = attacker ? attacker->team : 0;
    if (ball->team == 0) {
        addTeamMember(world, team, *ball, true, false, fight.rng, fight.frame);
    } else if (ServerTeam* t = teamById(world, team)) {
        moveTeamMember(world, *ball, *t, false, fight.rng, fight.frame);
    }
    playSound(world, 0x39E, victim);
}

void flagDropped(World& world, const ServerObject& holder, ServerObject& flag, const things::ModifierDb* modifiers, std::uint32_t frame)
{
    const int color = flagColor(flag, modifiers);
    informAll(world, 7, {netCodeOf(holder), static_cast<std::uint32_t>(color)});
    markMinimapForAll(world, flag, 1); // (Obj_DropDefault's, after the INFORM)
    if (flag.flag) {
        flag.flag->dropped = frame;
    }
    flagStatus(world, flag.team, 2, static_cast<std::uint8_t>(color), 0);
}

bool pickupCrown(Items& items, ServerObject& who, ServerObject& crown)
{
    World& world = items.world;
    if (!(who.classes & oc::Player)) {
        return false;
    }
    const bool taken = items.pickUpDefault(who, crown, true);
    if (taken) {
        if (who.player) {
            who.player->crownAt = items.frame;
        }
        setOwner(world, &who, crown); // (unitSetOwner)
        enchant(world, who, Enchant::Crown, 0, 5);
        playSound(world, 0x139, who);
        informAll(world, 0x0A, {netCodeOf(who), who.team != 0 && teamById(world, who.team) ? who.team : 0u});
        unmarkMinimapForAll(world, crown, 1); // (netUnmarkMinimapSpec)
    }
    if (crown.crown) {
        crown.crown->returnTo = 0;
    }
    return taken;
}

bool dropCrown(Items& items, ServerObject& holder, ServerObject& crown, float x, float y)
{
    World& world = items.world;
    // Team KotR: a team's crown goes back to its player who took one longest ago.
    if ((world.gameFlags & GameFlag::KingOfTheRealm) && (world.gameplayFlags & 4u) && crown.team != 0 && crown.crown) {
        std::uint32_t earliest = items.frame;
        ServerObject* to = &crown; // (the exe starts from the crown itself)
        for (ServerObject* pu : world.playerUnits()) {
            ServerObject& o = *pu;
            if (o.player && !o.gone() && inTeam(world, o, crown.team) && o.player->crownAt < earliest) {
                earliest = o.player->crownAt;
                to = &o;
            }
        }
        if (to != &holder) {
            crown.crown->returnTo = to->extent;
        }
    }
    if (!items.dropDefault(holder, crown, x, y)) {
        return false;
    }
    clearOwner(world, crown);
    disenchant(world, holder, Enchant::Crown);
    informAll(world, 0x0B, {netCodeOf(holder), holder.team != 0 && teamById(world, holder.team) ? holder.team : 0u});
    markMinimapForAll(world, crown, 1);
    return true;
}

void crownUpdate(Fight& fight, ServerObject& crown)
{
    World& world = fight.world;
    if (!crown.crown || !fight.things) {
        return;
    }
    if (ServerObject* who = crown.crown->returnTo ? world.byExtent(crown.crown->returnTo) : nullptr;
        who && !(who->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        pickupCrown(items, *who, crown);
        return;
    }
    ServerObject* owner = crown.owner ? world.byExtent(crown.owner) : nullptr;
    if (!owner) {
        return;
    }
    if (owner->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
        clearOwner(world, crown);
        return;
    }
    const float reach = owner->radius + crown.radius + 10.0f;
    const auto [dx, dy] = directionVector(static_cast<std::int16_t>(owner->direction));
    const float x = reach * dx + owner->x;
    const float y = reach * dy + owner->y;
    if (world.traceRay(owner->x, owner->y, x, y, Trace::Doors | Trace::ThroughWindows)) {
        setPosition(world, crown, x, y, fight.frame);
    }
}

void crownCollide(Fight& fight, ServerObject& crown, ServerObject* other)
{
    if (other && !(other->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) && (other->classes & oc::Player) && fight.things) {
        Items items{fight.world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        pickupCrown(items, *other, crown);
    }
}

void dropKingsCrown(Fight& fight, ServerObject& victim, ServerObject* killer)
{
    if (!fight.things) {
        return;
    }
    Items items{fight.world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    for (const std::uint32_t e : std::vector<std::uint32_t>(victim.inventory)) {
        ServerObject* crown = fight.world.byExtent(e);
        if (crown && crown->typeName == NameId::Crown && crown->owner == victim.extent) {
            dropCrown(items, victim, *crown, victim.x, victim.y);
            if (crown->crown) {
                crown->crown->returnTo = killer ? killer->extent : 0;
            }
        }
    }
}

void takeTeamCrown(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    if (!(world.gameFlags & GameFlag::KingOfTheRealm) || !(world.gameplayFlags & 4u) || !fight.things) {
        return;
    }
    const ServerTeam* team = teamById(world, unit.team);
    ServerObject* crown = team && team->crown ? world.byExtent(team->crown) : nullptr;
    if (crown && crown->holder == 0) {
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        pickupCrown(items, unit, *crown);
    }
}

void crownsToTeams(Fight& fight)
{
    World& world = fight.world;
    if (!fight.things) {
        return;
    }
    std::vector<ServerObject*> players;
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player && !o.gone()) {
            players.push_back(&o);
        }
    }
    for (ServerTeam& t : world.teams) {
        ServerObject* crown = t.active && t.crown ? world.byExtent(t.crown) : nullptr;
        if (!crown || crown->holder != 0 || teamPlayers(world, t) == 0) {
            continue;
        }
        // A random player, again till one of the team's (randInt over them all).
        ServerObject* p = nullptr;
        do {
            p = players[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(players.size()) - 1)(fight.rng))];
        } while (!inTeam(world, *p, t.id));
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        pickupCrown(items, *p, *crown);
    }
}

void addRespawn(World& world, const ServerObject& o)
{
    if (!world.respawnsOpen) {
        return;
    }
    World::Respawn r;
    r.type = o.typeName;
    r.object = o.extent;
    r.x = o.x;
    r.y = o.y;
    r.direction = o.direction;
    // (A flag, armour, weapon or wand: its modifiers kept.)
    if (o.classes & 0x13001000u) {
        r.modifiers = o.modifiers;
    }
    // (Weapon bits 0x82, a quiver or chakram: its ammunition kept.)
    if ((o.classes & oc::Weapon) && (things::weaponBitOf(o.typeName) & 0x82u)) {
        r.maxCharges = o.maxCharges;
        r.charges = o.charges;
    }
    world.respawns.push_back(std::move(r));
}

void forgetRespawn(World& world, std::uint32_t extent)
{
    // (The newest entry for it.)
    for (auto it = world.respawns.rbegin(); it != world.respawns.rend(); ++it) {
        if (it->object == extent) {
            world.respawns.erase(std::next(it).base());
            return;
        }
    }
}

// Each frame outside a quest or coop game (the respawn list closed on the first): each listed map
// thing (only those flagged RESPAWN are listed: Server::loadMap) gone or dead, or (by its kind) taken, comes back 30 s on; a weapon, armour, wand or crown left
// lying far from its spot goes back. The newest entries first.
void respawnPass(Fight& fight)
{
    World& world = fight.world;
    if (world.gameFlags & (GameFlag::Quest | GameFlag::Coop)) {
        return;
    }
    world.respawnsOpen = false;
    const std::uint32_t frame = fight.frame;
    for (auto it = world.respawns.rbegin(); it != world.respawns.rend(); ++it) {
        World::Respawn& r = *it;
        if (!r.waiting) {
            ServerObject* o = r.object ? world.byExtent(r.object) : nullptr;
            bool wait = false;
            if (!o) {
                r.object = 0; // (gone: broken, used up)
                wait = true;
            } else if (o->classes & oc::Monster) {
                wait = (o->flags & ObjFlag::Dead) != 0;
            } else if (!(o->classes & 0x3001000u) && o->typeName != NameId::Crown) {
                // (Not armour, a weapon, a wand nor the crown: due again once taken.)
                wait = o->holder != 0; // (taken)
            } else if (o->holder == 0) {
                // A weapon, armour, wand or the crown put down, 5 s on and more than 50 from its
                // spot: back there.
                if (o->born + 30u * 5u < frame) {
                    const util::X87 dx = util::X87(r.x) - o->x; // (on the x87's stack)
                    const util::X87 dy = util::X87(r.y) - o->y;
                    if (util::X87(2500.0f) < dx * dx + dy * dy) {  // 50 squared
                        pointFx(world, 0x81, o->x, o->y);
                        playSoundAt(world, 0x11B, o->x, o->y);
                        setPosition(world, *o, r.x, r.y, frame);
                        if (o->classes & oc::Wand) {
                            o->chargePercent = 100; // (rechargeItem(100))
                            o->charges = o->maxCharges;
                        } else if ((o->classes & oc::Weapon) && (things::weaponBitOf(o->typeName) & 0x82u)) {
                            o->maxCharges = r.maxCharges;
                            o->charges = r.charges;
                        }
                        if (o->maxHealth != 0) {
                            o->health = o->maxHealth;
                        }
                        o->touch();
                        pointFx(world, 0x81, r.x, r.y);
                        playSoundAt(world, 0x11B, r.x, r.y);
                    }
                }
            } else if (o->typeName != NameId::Crown && (world.serverOptions & 2u)) {
                wait = true; // (a weapon or armour taken, weapons respawning: serverOption(2))
            }
            if (wait) {
                r.waiting = true;
                r.at = frame + 30u * 30u;  // 30 s at 30 frames a second
            }
        }
        if (r.waiting && r.at <= frame) {
            ServerObject* n = fight.things ? world.createAt(*fight.things, r.type, r.x, r.y, frame) : nullptr;
            if (n) {
                pointFx(world, 0x81, r.x, r.y);
                playSound(world, 0x11B, *n);
                n->direction = r.direction;
                if (n->classes & 0x13001000u) {  // only a weapon, armour, wand or flag takes modifiers
                    n->modifiers = r.modifiers;
                }
                if ((n->classes & oc::Weapon) && (things::weaponBitOf(n->typeName) & 0x82u)) {
                    n->maxCharges = r.maxCharges;
                    n->charges = r.charges;
                }
            }
            if (ServerObject* old = r.object ? world.byExtent(r.object) : nullptr; old && (old->classes & oc::Monster)) {
                old->pendingDelete = true; // (the body)
            }
            r.waiting = false;
            r.object = n ? n->extent : 0;
        }
    }
}

} // namespace nox::server
