// The live world's map as the solo save writes it (mapSaveMap ->
// map -> mapWriteSectionsMB, each section's reader in write mode
// with no rectangle):
//
//   - The header: magic 0xFADEFACE (the CRC is the caller's, map_save.hpp), then the wall
//     offsets: the least x and y of the wall list's walls (wallForeachFn: broken
//     walls and door leaves passed over), from 0x100 (mapCountWallsMB).
//   - The Wall spell's walls that changed a map wall have its old shape for the save
//     (mapSetWallInGlobalDir0pr1) and their own after it (map).
//   - MapInfo: version 3, the loaded blocks as they are, v3's two strings (empty for
//     a map read at a version below 3: 98); no player counts.
//   - WallMap: version 7; the walls' box (over the list); the cells from
//     its least corner to one past its width and height (inclusive loops), each even cell's wall
//     (Map_GetWallAtGrid) not of MagicWallSystemUseOnly: x, y, its shape | 0x80 by its flag
//     0x80, material, look, the minimap group of the polygon at its middle
//     (polygonIsPlayerInPolygon, else within 10, else 100), its byte
//     0xC (0 under game flag 0x200000); 0xFF.
//   - FloorMap: version 4; the floor grid's box; its pairs row by row (y, then x).
//     The floor never changes in a game: the loaded map's, as the grid holds it.
//   - SecretWalls, DestructableWalls, WindowWalls: the wall
//     list's (newest first) secret / breakable / window walls; a secret one's record as it is
//     now. A Wall spell's new wall is breakable: listed too.
//   - WayPoints: the list in the map's order; flags = the on state.
//   - DebugData: the map's, the list newest first (mapDebug_Add): reversed.
//   - GroupData: version 3; groups and members newest first:
//     reversed; names with their NUL, at most 75 characters.
//   - ScriptObject: the map's script as its load left it in nc.obj.
//   - AmbientData: the ambient light, the map's (25, 25, 25 without: the level switch).
//   - Polygons: version 4; the corners numbered 1.. in order (polygonSetAngle)
//     and the polygons' corners by those numbers; each polygon's handlers by
//     their function's name in the script (none when it has none), its flags as they are now.
//   - MapIntro: the map's text under game flag 0x200000 (its .txt), else none.
#pragma once

#include "noxcore/map/map_file.hpp"

namespace nox::things {
struct ThingDb;
}

namespace nox::server {

class World;

/// The live world's map as the save writes it (mapSaveMap -> map ->
/// mapWriteSectionsMB), but for ScriptData, ObjectTOC and ObjectData (the caller
/// appends those in the table's order): the header (magic 0xFADEFACE, the wall offsets; crc 0)
/// and the sections MapInfo ... MapIntro.
///
/// `loaded` is the map file the world was loaded from: what the exe writes from load-time
/// globals the world doesn't keep (MapInfo, the floor, DebugData, the script, the ambient light,
/// the polygons' names, colours and corners, the intro). `things` names the WALL materials. The
/// world is non-const as the exe's save is: its polygon lookups move the point-in-polygon ray.
map::MapFile saveMapSections(World& world, const things::ThingDb& things, const map::MapFile& loaded);

} // namespace nox::server
