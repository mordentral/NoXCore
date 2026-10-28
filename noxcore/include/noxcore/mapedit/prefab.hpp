// The map editor's prefabs: a selection of placed things (and optionally walls, floor and
// waypoints) saved under a name in the user's folder, placed again with the same relative positions
// and its internal links (transporter, elevator and mover extents, a light's target, a mover's
// waypoints, waypoint links, escorts by script name) remapped to the new copies and clashing script
// names suffixed, as a paste does.
//
// A prefab file is text: a "NoX Core prefab 1" line, a "name <name>" line, then the content as
// the clipboard's text (selection.hpp clipToText).
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/selection.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

struct Prefab {
    std::string name;
    Clip clip;

    /// Walls or floor in it: it is placed by whole wall cells (x + y even), so they stay on
    /// their grid; otherwise its objects go exactly where it's put.
    bool cellAligned() const { return !clip.walls.empty() || !clip.tiles.empty(); }
    /// Its centre in world units: the middle of what it holds (what a placement puts at the point).
    WorldPoint centre() const;
    std::size_t objectCount() const;
};

/// The selection as a prefab (copySelection with `layers`).
Prefab makePrefab(const MapDocument& doc, const Selection& selection, std::string name, const CopyLayers& layers = {});

std::string prefabToText(const Prefab& prefab, const things::ThingDb& things);
/// Nothing for text that isn't a prefab (or one that doesn't fit this thing.bin).
std::optional<Prefab> prefabFromText(std::string_view text, const things::ThingDb& things);

/// Places the prefab centred on `at` (cell-aligned ones by whole cells; one holding a door by
/// whole cells too, so its doors stay on the grid). As a paste does: new extents with the links
/// between the copies following them, and script names (objects' and waypoints') the map already
/// has given the next free "_N" (placed twice: Guard_2, Guard_3), a copied creature's escort
/// following its renamed copy; names that don't clash stay. `placed` gets what was placed, as a
/// selection.
Op placePrefabOp(const MapDocument& doc, const Prefab& prefab, WorldPoint at, Selection* placed = nullptr);

/// The prefab's file name in a folder: its name with characters other than letters, digits,
/// spaces, '-' and '_' replaced by '_', then ".nxprefab".
std::string prefabFileName(std::string_view name);
/// Writes the prefab into `dir` (made if missing); returns the file written.
std::filesystem::path savePrefab(const std::filesystem::path& dir, const Prefab& prefab, const things::ThingDb& things);
/// Every prefab file in `dir`, sorted by name (unreadable ones skipped).
std::vector<Prefab> loadPrefabs(const std::filesystem::path& dir, const things::ThingDb& things);

} // namespace nox::mapedit
