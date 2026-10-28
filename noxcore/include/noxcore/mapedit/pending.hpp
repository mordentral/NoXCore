// A collaborative session's pending edits: what the working copy has that a view's game doesn't
// show yet, and how a view draws them.
//
// The rule: objects go live for everyone; walls, floor tiles and edges, and the things the clients
// draw from their own map file (`clientLoadedType`: scenery, furniture, doors, trees, fireplaces,
// the client-only types), reach the game only on a publish. Until then a noxsdl client draws them
// over the game as *pending* (greyed, translucent), and what the working copy removed (or moved
// away) but the game still has as a *ghost* (darker, see-through, hatched). Creatures, players and
// items are never pending: the game's own objects draw them, alive or dead.
//
// Drawing only: nothing here is ever given to the World, its collision, line of sight or
// pathfinding, nor to the client's sight (noxsdl's SightMap is built from the loaded map's walls
// alone). Pending walls are see-through decorations until they're published. The session's host
// holds the same edits back from its World by the same rule (`heldForPublish`): what a client draws
// as pending is exactly what the server doesn't have yet.
#pragma once

#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/selection.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
struct ThingDef;
}

namespace nox::mapedit {

/// A type every client draws from its own copy of the map, never from the server's objects
/// (CLASS CLIENT_PERSIST or IMMOBILE: the client's own map load, client_mapSpecialRWObjectData,
/// takes those; the server sends the immobile only their state, by map extent, and none of them
/// in the update stream). So nothing made, moved or removed in play reaches a client's view of
/// one: only a map change does, and a session's edits to one are pending until a publish. Every
/// client-only type (server/world.hpp clientOnlyType) is one; no creature, player or item is.
bool clientLoadedType(const things::ThingDef& t);

/// The session's one rule for its running game: an edit to document object `id` waits for a publish
/// (the World keeps the published state; nothing is made, moved, changed or removed there) when the
/// thing it belongs to, its top-level owner, is of a `clientLoadedType` in the published copy or in
/// the working copy. So a fireplace, tree or door, and a chest's contents, wait; an item taken out
/// of such a thing or put into one waits with it; a creature, item or movable thing on its own goes
/// live. `clientLoadedType` is the pending view's test too, so the host's held edits and what
/// clients draw as pending can't disagree.
bool heldForPublish(const MapDocument& published, const MapDocument& working, ObjId id, const things::ThingDb& things);

/// The objects as the running game has them while edits are held: the working copy's, with every
/// thing `heldForPublish` keeps back as the published copy has it (moved back, changed back, a
/// removed one kept, an added one left out). An object-only publish writes these, so a late
/// joiner's map has what the World has.
std::map<ObjId, DocObject> playedObjects(const MapDocument& published, const MapDocument& working, const things::ThingDb& things);

/// The "Pending changes" view setting: Shaded (the default: pending items greyed, removed ones as
/// ghosts), As final (the working copy as it will look once published), Hidden (only what the
/// game has: the published map).
enum class PendingView : std::uint8_t { Shaded = 0, AsFinal = 1, Hidden = 2 };

/// "Shaded", "As final", "Hidden".
const char* pendingViewName(PendingView view);
/// "shaded" / "final" / "asfinal" / "as final" / "hidden" (any case), or 0 / 1 / 2.
std::optional<PendingView> parsePendingView(std::string_view text);

/// How an item is drawn: as usual, pending (greyed, translucent) or a ghost (removed but still in
/// the game: darker, more see-through, hatched).
enum class PendingLook : std::uint8_t { Normal = 0, Pending = 1, Ghost = 2 };

/// A wall cell whose drawn wall differs (material, facing, variant, window, secret or breakable
/// entry). `reference` is what the game has there.
struct PendingWall {
    Cell cell;
    std::optional<WallState> working;
    std::optional<WallState> reference;
};

/// A full-grid floor tile (with its edges) that differs.
struct PendingTile {
    TileCell at;
    std::optional<map::FloorTile> working;
    std::optional<map::FloorTile> reference;
};

/// A client-loaded thing (`clientLoadedType`) placed in one copy and not the other, matched by
/// type and place: a move is one in each.
struct PendingObject {
    std::string type;
    float x = 0;
    float y = 0;
    bool inWorking = false; ///< else only in the reference (removed, or moved away)
};

struct PendingEdits {
    std::vector<PendingWall> walls;
    std::vector<PendingTile> tiles;
    std::vector<PendingObject> objects;
    bool empty() const { return walls.empty() && tiles.empty() && objects.empty(); }
};

/// What `working` has that `reference` (the published copy, or the map a game view loaded)
/// doesn't, and the other way round.
PendingEdits diffPending(const MapDocument& working, const MapDocument& reference, const things::ThingDb& things);

/// Which copy a view draws its map from: the editor's the working copy, a game's the reference
/// (the map it loaded).
enum class PendingBase : std::uint8_t { Working, Reference };

/// What a view draws for the pending edits in one mode: the base's items to leave out (their
/// place is drawn by the plan, or left empty) and the items to draw with their look. Items the
/// base already draws as they are drop out of the plan, so a game view's Hidden and an editor's
/// As final are empty plans.
struct PendingPlan {
    struct Wall {
        map::Wall wall;
        bool window = false;
        PendingLook look = PendingLook::Normal;
        /// The base has this very wall: the view draws its own (its sight, its broken state) with
        /// the look. Else a decoration of the plan's own: never in the view's walls or sight.
        bool inBase = false;
    };
    struct Tile {
        TileCell at;
        map::FloorTile tile;
        PendingLook look = PendingLook::Normal;
    };
    struct Object {
        std::string type;
        float x = 0;
        float y = 0;
        PendingLook look = PendingLook::Normal;
    };
    std::vector<Cell> hideWalls;       ///< the base's walls not drawn (the plan has the cell)
    std::vector<TileCell> hideTiles;   ///< the base's tiles not drawn (the plan draws its own there, or nothing)
    std::vector<Object> hideObjects;   ///< the base's client-loaded things not drawn (by type and place; look unused)
    std::vector<Wall> walls;
    std::vector<Tile> tiles;
    std::vector<Object> objects;
    bool empty() const
    {
        return hideWalls.empty() && hideTiles.empty() && hideObjects.empty() && walls.empty() && tiles.empty() && objects.empty();
    }
};

PendingPlan planPending(const PendingEdits& edits, PendingBase base, PendingView view);

} // namespace nox::mapedit
