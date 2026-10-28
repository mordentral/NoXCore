// The map editor's object placement: new objects as the game makes them, their facing, snapping,
// placement as one op, and the scatter brush.
//
// A new object is the record the game's own object save writes for a thing just created
// (createAt, then the xfer in write mode: noxcore/server object_save.hpp), so it carries exactly
// the type's fresh state: TriggerCreate's plate colours, MonsterCreate's AI numbers with
// monster.bin's, a weapon's durability from modifier.bin, a reward marker's every kind, an
// obelisk's full charge... Two things differ from a host's save, both as the game writes them:
// the AI state block is written as a non-host writes it (XFer_ActionData writes its "present"
// byte as 0 unless the game is the host, as Westwood's editor wrote every stock monster), and the
// header has no extent or script id yet (the document gives the extent; 0 means "assign one on
// load"). The xfer version is each xfer's current one.
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/selection.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
struct ModifierDb;
struct MonsterDb;
struct GameData;
} // namespace nox::things

namespace nox::mapedit {

/// Makes new objects as the game does (see above). Not thread-safe (it keeps a scratch world).
class ObjectFactory {
public:
    /// `modifiers` gives weapons and armour their durability, `monsters` the creatures' numbers
    /// (pass monster.bin's solo table: every stock map's creatures carry its health, whatever the
    /// map's mode), `gameData` the ammunition counts; each may be null (the type's own values
    /// then). Made as the editor build's game flag 0x200000 makes things (solo numbers).
    explicit ObjectFactory(const things::ThingDb& things, const things::ModifierDb* modifiers = nullptr,
                           const things::MonsterDb* monsters = nullptr, const things::GameData* gameData = nullptr);
    ~ObjectFactory();
    ObjectFactory(const ObjectFactory&) = delete;
    ObjectFactory& operator=(const ObjectFactory&) = delete;

    /// A new object of `type` at (x, y), parent 0, extent 0 (the document gives one). Throws
    /// FormatError for a type thing.bin doesn't have.
    DocObject make(std::string_view type, float x, float y) const;
    /// A new thing's header flags as the game's save would write them (its type's flags, masked
    /// 0x11408162): what header details added to an object of the type start with.
    std::uint32_t freshFlags(std::string_view type) const;
    const things::ThingDb& things() const { return things_; }

private:
    struct Impl;
    const things::ThingDb& things_;
    std::unique_ptr<Impl> impl_;
};

// --- Facing ----------------------------------------------------------------------------------

/// How a type turns (the old editor's per-type rotation, from the xfers' fields).
enum class FacingKind {
    None,
    Door,     ///< DoorXfer's direction (both copies): 0, 8, 16 or 24, the four ways a door hangs
    Creature, ///< MonsterXfer / NPCXfer's direction: one of the 8 grid steps
    Sentry,   ///< SentryXfer's base angle, radians, in 5-degree steps
};
FacingKind facingKind(const DocObject& o);
/// The facing as a compass angle in degrees (0 east, 90 south: the world's y grows downwards),
/// or nothing for a type that doesn't turn.
std::optional<float> facingDegrees(const DocObject& o);
/// Turns by `steps` of the type's step (a door 8 directions = a quarter turn... see FacingKind);
/// false for a type that doesn't turn.
bool rotateFacing(DocObject& o, int steps);
/// Sets the facing nearest `degrees` the type allows; false for a type that doesn't turn.
bool setFacingDegrees(DocObject& o, float degrees);

// --- Snapping ----------------------------------------------------------------------------------

enum class Snap { None, Grid, HalfGrid, Custom };
/// The point snapped: Grid to the wall grid's corners (multiples of 23, the old editor's
/// "Center/Door Snap"), HalfGrid to multiples of 11.5, Custom to multiples of `custom`. A door
/// always snaps to the grid, as the old editor placed them.
WorldPoint snapPoint(WorldPoint p, Snap snap, float custom = 23, bool door = false);
/// Whether objects of this type always take the door snap.
bool snapsAsDoor(const DocObject& o);

// --- Placement --------------------------------------------------------------------------------

/// Adds `objects` (parents 0, each its inventory after it with parent set to the owner's index
/// + 1 in this list... see below) as new world objects: fresh document ids and extents, after the
/// map's objects. Items (parent != 0) name their owner by its 1-based position in `objects`.
/// One op; `placed` gets the new top-level objects.
Op placeObjectsOp(const MapDocument& doc, std::vector<DocObject> objects, Selection* placed = nullptr, std::string label = {});

/// Replaces objects' records (a rotation, ...): one SetObjects op.
Op setObjectsOp(const MapDocument& doc, const std::vector<std::pair<ObjId, DocObject>>& changed, std::string label);

// --- Scatter brush -------------------------------------------------------------------------------

struct ScatterBrush {
    struct Entry {
        std::string type;
        float weight = 1;
    };
    std::vector<Entry> types;  ///< a weighted pick each time (variants: trees, rocks...)
    float radius = 92;         ///< world units (4 wall cells)
    float density = 0.5f;      ///< objects per floor tile (46 x 46 diamond, 1058 square units) a dab
    float spacing = 30;        ///< the least distance between scattered objects (and others on the map)
    bool randomFacing = true;  ///< creatures, doors and sentries a random facing
    bool avoidWalls = true;    ///< never in a wall cell
    bool avoidHoles = true;    ///< only on floor
    std::uint32_t seed = 1;
};

/// One stroke of the scatter brush: dabs pick points (Poisson-disc darts against `spacing`),
/// deterministic for the same document, brush, seed and dab points; finish() is ONE op.
class ScatterStroke {
public:
    ScatterStroke(const MapDocument& doc, const ObjectFactory& factory, ScatterBrush brush);
    /// Scatters round `at`; returns the objects this dab added (for the preview).
    std::vector<DocObject> dab(WorldPoint at);
    const std::vector<DocObject>& objects() const { return objects_; }
    /// The stroke's objects as one placement op (empty if none).
    Op finish(Selection* placed = nullptr) const;

private:
    const MapDocument& doc_;
    const ObjectFactory& factory_;
    ScatterBrush brush_;
    std::vector<WorldPoint> taken_; ///< existing objects round the stroke, and those it placed
    std::vector<DocObject> objects_;
    std::uint64_t counter_ = 0;
};

/// A deterministic random number in [0, 1) from a seed and a counter (the same on every build).
float scatterRandom(std::uint64_t seed, std::uint64_t n);

} // namespace nox::mapedit
