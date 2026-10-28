// The map generator's native blocks as its scripts see them (gen_bindings.cpp, writer_bindings.cpp):
// the `gen` table's cell sets and maps, plots, rooms, the placing scans, and the map writer.
#pragma once

#include "blocks.hpp"
#include "map_writer.hpp"

#include "noxcore/mapgen/cell_set.hpp"

struct lua_State;

namespace nox::mapgen::detail {

/// The blocks' part of `gen`, into the table on the stack's top (at a generator VM's making).
void openGenBlocks(lua_State* L);

/// A gen.cells or gen.cellmap argument's own (an error for anything else). Every one a script
/// holds is its own: none is lent by the host.
CellSet& cellSetArg(lua_State* L, int idx);
CellMap& cellMapArg(lua_State* L, int idx);

/// `writer` pushed as a userdata (gen.map's methods) for the length of one call into the
/// generator's VM: it must outlive every use of it there.
void pushMapWriter(lua_State* L, MapWriter& writer);

} // namespace nox::mapgen::detail
