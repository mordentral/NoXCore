// Path finding for the headless client over the map's wall grid.
//
// Walls sit on a 256 x 256 grid of 23-unit cells (WallMap coordinates are absolute cells, so
// world = cell x 23); each wall is a segment inside its cell, often diagonal. A cell with any
// wall counts as blocked, and a diagonal step needs both side cells free, so a path can't slip
// through a diagonal wall line. The map's solid objects (obelisks, columns, rocks, barrels:
// addObstacles) block the cells they cover too, in a layer of their own, so a path goes round
// them while the line of sight (wallsOnly) still sees past them.
#pragma once

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace nox::map {
struct MapFile;
}

namespace nox::things {
struct ThingDb;
}

namespace nox::client {

class NavGrid {
public:
    static constexpr int kCellSize = 23;
    static constexpr int kCells = 256;

    NavGrid() = default;
    static NavGrid fromMap(const map::MapFile& map);
    /// Blocks the cells under the map's solid objects: things of CLASS IMMOBILE or OBSTACLE with
    /// an EXTENT (a circle or a box), not flagged BELOW (lying on the floor: plates, start
    /// points) or NO_COLLIDE, and not doors (they open). A cell is blocked when its centre is
    /// within the extent, and the cell under the object always is: no margin for a player's
    /// body, which would close gaps people walk through.
    void addObstacles(const map::MapFile& map, const things::ThingDb& things);
    /// Blocks the cells no floor tile covers (the void round a map, or past its edge): a walk
    /// or a roam never heads off the floor. Sight isn't blocked. Lava tiles (FLOR entry 6, which
    /// burns whoever stands on it and isn't jumping) are hazards. A map without a FloorMap is
    /// left as it is.
    void addFloor(const map::MapFile& map);

    bool empty() const noexcept { return blocked_.empty(); }
    /// A wall or a solid object in the cell.
    bool blocked(int cx, int cy) const noexcept;
    /// A wall in the cell that blocks sight (a window's doesn't: WindowWalls; it still blocks
    /// walking).
    bool wall(int cx, int cy) const noexcept;
    /// Something in the cell hurts to touch (flames, lava fountains, spikes, pits, poison gas):
    /// walkable, but paths go round it if they can (kHazardCost a cell), and a walker jumps it.
    bool hazard(int cx, int cy) const noexcept;
    static constexpr float kHazardCost = 12; ///< a hazard cell costs this many cells of walking
    void setBlocked(int cx, int cy, bool value);
    void setObstacle(int cx, int cy, bool value);
    void setHazard(int cx, int cy, bool value);
    /// A window in the cell's wall: sight goes through (fromMap sets the map's).
    void setWindow(int cx, int cy, bool value);
    /// The walls as they stand now, over the map's: `raised` cells hold a wall that wasn't there
    /// (the Wall spell's, MSG_CREATE_WALL), `gone` cells a map wall that no longer is (broken:
    /// MSG_DESTROY_WALL; a secret wall open). Replaces the last call's.
    void setLiveWalls(const std::vector<std::pair<int, int>>& raised, const std::vector<std::pair<int, int>>& gone);

    /// A ride from one place to another: a teleporter's pentagram to its destination, an
    /// elevator to its shaft and back. Stood on, it carries whoever's on it there.
    struct Link {
        std::pair<int, int> from; ///< world point to stand on
        std::pair<int, int> to;   ///< world point it puts us
        float cost = 2;           ///< in cells of walking: the wait for it
        /// How long a walker waits on its pad for it before trying another way: a teleporter
        /// takes once its last trip's cooldown is over; an elevator may be up (or on its way)
        /// and has to come back down.
        std::uint32_t patienceFrames = 120;
        /// A teleporter's pentagram: it starts only on a touch while idle (PentagramUpdate
        /// updateTeleportPentagram; ~88 frames' cooldown after a trip, and the walker must still be on it 8
        /// frames after the touch). One standing on it through the cooldown steps off and on.
        bool restep = false;
    };
    void setLinks(std::vector<Link> links) { links_ = std::move(links); }
    const std::vector<Link>& links() const noexcept { return links_; }
    /// The link whose pad is in the cell of world point `at`, if any.
    const Link* linkAt(std::pair<int, int> at) const noexcept;

    /// Cells a walker found it couldn't pass though the grid has them open (a locked door, a
    /// wall the grid misses): planned round as if blocked.
    using CellSet = std::set<std::pair<int, int>>;

    /// World-coordinate waypoints from `from` to `to` (cell centres, straightened where there's
    /// a clear line), ending at `to`. Empty if there's no way. Links count as a step from their
    /// pad's cell to their exit's: the path has the pad then the exit (no straightening across).
    /// `avoid`'s cells count as blocked. With `complete`, a goal there's no way to gives the way
    /// to the cell nearest it that can be reached instead (*complete false; empty if that's
    /// where we are).
    std::vector<std::pair<int, int>> findPath(std::pair<int, int> from, std::pair<int, int> to,
                                              const CellSet* avoid = nullptr, bool* complete = nullptr) const;

    /// True if a straight walk from `a` to `b` crosses no blocked cell (with `wallsOnly`, no wall:
    /// a line of sight; with `noHazards`, no hazard either; `avoid`'s cells block a walk).
    bool clearLine(std::pair<int, int> a, std::pair<int, int> b, bool wallsOnly = false, bool noHazards = false,
                   const CellSet* avoid = nullptr) const;

private:
    static constexpr std::uint8_t kWall = 1;
    static constexpr std::uint8_t kObstacle = 2;
    static constexpr std::uint8_t kVoid = 4;
    static constexpr std::uint8_t kHazard = 8; ///< not blocking: see hazard()
    static constexpr std::uint8_t kRaised = 16; ///< a wall that's come since the map (setLiveWalls)
    static constexpr std::uint8_t kGone = 32;   ///< the map's wall here is gone (setLiveWalls)
    static constexpr std::uint8_t kWindow = 64; ///< the map's wall here is a window: seen through
    /// A wall stands in the cell (the map's unless gone, or a raised one).
    static bool wallBits(std::uint8_t b) noexcept { return ((b & kWall) && !(b & kGone)) || (b & kRaised); }
    std::vector<std::uint8_t> blocked_; // kCells * kCells: kWall | kObstacle
    std::vector<Link> links_;
};

} // namespace nox::client
