// The map editor's "Play From Here": a play-test (arena, solo or quest, in the editor or a
// session's private test) whose player comes into the game at a world point (the cursor's) instead
// of a map spawn. The editor's tooling only: no ordinary game asks for it, and nothing here runs
// unless a test asked.
//
// Our player is put there once it's in play on the test's map (a network game's join brings it in
// from observing as the observer's rejoin does, at the point instead of a player start; a quest
// brings its players in itself first; a solo game's after its ready). Whatever the game does to it
// in the moments after (its own placing at a player start: a join's team, a ready) is undone: for
// a while after the jump a move no walk can make (a teleport-sized jump in one step) that lands
// on a PlayerStart puts it back at the point (never after a death: the respawn is the game's).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace nox::server {

class GameHost;
class World;
struct Fight;
struct ServerObject;

/// A watching character back in play as the observer's rejoin brings it (Plr_UpdateObserver's:
/// its team, out of observing, its camera its own), to be put where the editor's tooling says.
void rejoinFromObserving(Fight& fight, ServerObject& unit);

/// A player's character put at (x, y) by the editor's tooling (Play From Here, a session's
/// unpark): Obj_SetPosition (the clients see it move), its camera there with it.
void placePlayerAt(World& world, ServerObject& unit, float x, float y, std::uint32_t frame);

/// Whether a player may be put at (x, y) as it is (noxcpp's, the editor's placing only): on the
/// map (cells 1-254), its cell holding no wall of any kind (a wall, a window, a secret wall, a
/// door's leaf; a broken wall is none), and over a floor tile (when the map has a floor).
bool standableSpot(const World& world, float x, float y);

/// The nearest spot to (x, y) a player can stand on (noxcpp's; the exe has no such search: its
/// teleports refuse a bad target instead). (x, y) itself when standableSpot; else the middle of
/// the nearest standable cell within `maxCells` cells (ring by ring, nearest first) that isn't a
/// no-teleport floor (mapTileAllowTeleport's) and that a player can walk to:
///   - "walk to" is over the wall shapes' diagonals as the game's walls lie now (windows and shut
///     secret walls count, doors open), from the map's PlayerStarts, the players in play and
///     `from` (a character's own place);
///   - the side of the wall the point is on comes first, when that side can be walked to; else
///     any side that can: never an island walled off from all of those when a reachable spot is
///     in range;
///   - with none in range (no start; a quest's rooms behind walls its script opens later), the
///     point's side, then any standable cell;
///   - in each of those, a cell clear of solid things (a pillar, a door, a generator) and of pits,
///     fires, traps and teleporter pads first, beside them only when there's no clear one.
/// Nothing when there's no standable cell in range.
std::optional<std::pair<float, float>> nearestOpenSpot(const World& world, float x, float y, int maxCells = 12,
                                                       std::span<const std::pair<float, float>> from = {});

class PlayFromHere {
public:
    /// `map`: the test's map (nothing is done on another: a quest's chat map before it). `quest`:
    /// a quest's test (its players come in by the quest).
    PlayFromHere(float x, float y, std::string map, bool quest);

    /// Each frame (the host's game held): puts our player (the host's own peer's, else the one
    /// player's, else a solo game's host player) at the point once it's in, and keeps it there
    /// against the game's own placing for `settleFrames` game frames after. True once done.
    bool tick(GameHost& g);
    /// Placed (it may still be watching).
    bool placed() const { return placed_; }
    /// Where it was put (the point, or the nearest open spot to it).
    std::pair<float, float> spot() const { return spot_; }
    /// How often the game's own placing at a start was undone.
    int undone() const { return undone_; }

    /// Game frames the placing is kept after the jump.
    static constexpr std::uint32_t settleFrames = 90;

private:
    float x_ = 0, y_ = 0;
    std::string map_;
    bool quest_ = false;
    bool placed_ = false;
    std::pair<float, float> spot_{};
    std::uint32_t placedFrame_ = 0;
    std::uint32_t extent_ = 0;
    std::pair<float, float> last_{};
    int undone_ = 0;
};

} // namespace nox::server
