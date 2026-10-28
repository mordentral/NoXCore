// The monsters' path finder, as GAME.EXE's server has it.
//
//   - The grid (256 x 256 tiles of 23, a 12-byte record each in the exe): static flags
//     made once (1 a fixed obstacle to walkers, 2 to flyers (unless the obstacle
//     is flag 0x10), 4 an elevator, 8 its shaft, 0x10 a hole, 0x20 a transporter, 0x40 a
//     roaming waypoint (link flag 0x80)), and the moving obstacles marked afresh at most every
//     15 frames when a search wants them (obstacles and the NoUpdate
//     0x100, 0x200 unless flag 0x10; fire 0x400; DANGEROUS ones grown by 11.5; a tile counts
//     when one of its 9 sample points is inside).
//   - A wall in a tile: none, an open secret wall (or one passable by its flag
//     2), a window to a flyer, an open door, an unlocked one, one the mover holds the key to;
//     a door with an owner half the time. The diagonal steps' corner rules.
//   - The search (pathFind): breadth first from the mover's tile (stepped off a
//     wall it's in), the 8 neighbours each node from a random one round, every tile once;
//     diagonals only past the corner walls and fixed obstacles; no wall, no water (but to a
//     flyer or a swimmer), no obstacle (a mover inside one first gets out); the target's tile
//     taken when it can be reached straight from the node; holes, transporters and elevators
//     lead on to where they go. At most 1024 nodes. It ends at the target (status 0), or with
//     the nodes used up (1) or nowhere left to go (2): then to the node nearest the target.
//   - The path: from the start to the end, a point in each tile by the corner
//     or edge nearest the next (the transporters' and elevators' own positions), the target
//     itself last; at most 31 points (more: status 1).
//   - The waypoint graph: the roaming waypoints nearest the two ends (found by
//     a tile search to a tile with one, then the nearest seen within 1000 with a usable link),
//     joined breadth first (BuildWaypointPath; at most 16).
#pragma once

#include "noxcore/server/world.hpp"

#include <cstdint>
#include <functional>
#include <random>
#include <utility>
#include <vector>

namespace nox::server {

/// A search's outcome: the points from the start, the transporters and elevators on the way
/// (at most 8) and how it ended (0 reached, 1 cut short, 2 no further).
struct PathSearch {
    std::vector<std::pair<float, float>> points;
    std::vector<std::uint32_t> transits;
    int status = 0;
};

/// The tile a position lies in (x / 23, chopped).
int tileOf(float v);

/// A wall in the way of `mover` at (tx, ty): its shape, -1 none.
int wallBlocking(World& world, const ServerObject& mover, int tx, int ty, std::minstd_rand& rng);

/// The search (pathFind): to (toX, toY) from the mover; `away` looks for the
/// tile farthest from it instead; `inside`, when given, keeps it to the tiles it allows (one
/// that starts outside them gets in first); `maxSteps` (0: none) the levels it may go.
PathSearch findPath(World& world, const ServerObject& mover, float toX, float toY, std::minstd_rand& rng, std::uint32_t frame,
                    bool away = false, const std::function<bool(int, int)>& inside = {}, int maxSteps = 0);

/// A path for a monster (genPathToPoint): the search, cut to 31 points.
PathSearch pathTo(World& world, const ServerObject& mover, float toX, float toY, std::minstd_rand& rng, std::uint32_t frame);
/// A path away from (fromX, fromY) (generateRetreatPath): the search for the
/// tile farthest from it, 6 levels at most, through tiles with no enemy of the mover within 100
/// of their middle; cut to 31 points.
PathSearch retreatPath(World& world, const ServerObject& mover, float fromX, float fromY, std::minstd_rand& rng, std::uint32_t frame);
/// No body in the way of the line (mapTraceObstacles): of the
/// creatures, the mover's own side (its enemies don't count); of the rest, fixed obstacles
/// (IMMOBILE or obstacles, not ALLOW_OVERLAP / NO_COLLIDE, not doors): a circle within its
/// radius of it, a box with a side across it.
bool lineClear(World& world, const ServerObject& mover, float x0, float y0, float x1, float y1);

/// The roaming waypoint nearest (x, y) it can see within 1000 (with flag `flags`;
/// `linked`: with a usable link). Its index in the world's waypoints + 1, 0 none.
std::uint32_t nearestWaypoint(World& world, float x, float y, std::uint8_t flags, bool linked);
/// The roaming waypoint to make for from (x, y): the first tile with one,
/// breadth first, then the nearest from its middle.
std::uint32_t waypointFrom(World& world, const ServerObject& mover, float x, float y, std::minstd_rand& rng);
/// Whether a waypoint (index + 1) is on and has any of `flags`.
bool waypointUsable(World& world, std::uint32_t wp, std::uint8_t flags);
/// The waypoints from one to the other (BuildWaypointPath; status into
/// world.waypointPathStatus: 0 found, 1 too long, 2 none). At most `most`.
std::vector<std::uint32_t> waypointRoute(World& world, std::uint32_t from, std::uint32_t to, int most);
/// the route between the waypoints nearest the mover and (x, y).
std::vector<std::uint32_t> waypointPathTo(World& world, const ServerObject& mover, float x, float y, std::minstd_rand& rng);

} // namespace nox::server
