// The map generator's native blocks as its scripts see them (gen_bindings.hpp). Cells are the
// scripts' numbers (gen_runtime.hpp's genCellKey); chambers tables {x, y, radius, shape, a, b,
// seed}; streams gen_runtime's. A block's own exception is carried out to the host as it was
// (genNative): no Lua API calls happen inside one.
#include "gen_bindings.hpp"

#include "blocks.hpp"

#include "noxcore/mapgen/biome.hpp"

#include "noxcore/modes/script/gen_runtime.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nox::mapgen::detail {

namespace {

using modes::script::genCellKey;
using modes::script::genCellOf;
using modes::script::genNative;

constexpr const char* kCells = "gen.cells";
constexpr const char* kMap = "gen.cellmap";
constexpr const char* kBacks = "gen.backs";
constexpr const char* kList = "gen.list";
constexpr const char* kPlot = "gen.plot";

Cell cellArg(lua_State* L, int arg)
{
    return genCellOf(luaL_checknumber(L, arg));
}

/// A script's value of a native type: its own (made inside the capture, so a failure making it
/// comes out as it was), and the native memory charged for it (gen_runtime's genCharge: counted
/// with the script's own, released when it's collected).
template <class T>
struct Holder {
    std::optional<T> own;
    T* p = nullptr;
    std::size_t charged = 0;
};

/// What a value of each type is charged when it's made: a cell set's bits (made at its first
/// insert, charged whole); a cell map's keys and every page it could fill; a plot's buckets (its
/// blobs charged as they're added); a list's or back walls' cells once they're filled (chargeMore).
template <class T>
constexpr std::size_t kCharge = sizeof(Holder<T>);
template <>
constexpr std::size_t kCharge<CellSet> = sizeof(Holder<CellSet>) + CellSet::kBytes;
template <>
constexpr std::size_t kCharge<CellMap> = sizeof(Holder<CellMap>) + CellSet::kBytes + CellMap::kMostPageBytes;
template <>
constexpr std::size_t kCharge<blocks::Plot> = sizeof(Holder<blocks::Plot>) + blocks::Plot::kBucketBytes;

template <class T>
void chargeMore(lua_State* L, Holder<T>& h, std::size_t bytes)
{
    h.charged += bytes;
    modes::script::genCharge(L, bytes);
}

template <class T>
Holder<T>& newHolder(lua_State* L, const char* type)
{
    void* at = lua_newuserdatadtor(L, sizeof(Holder<T>), [](lua_State* L2, void* p) {
        auto* h = static_cast<Holder<T>*>(p);
        modes::script::genRelease(L2, h->charged);
        h->~Holder<T>();
    });
    auto* h = new (at) Holder<T>(); // (nothing made yet: can't throw)
    luaL_getmetatable(L, type);
    lua_setmetatable(L, -2);
    genNative(L, [&] { h->own.emplace(); });
    h->p = &*h->own;
    chargeMore(L, *h, kCharge<T>);
    return *h;
}

template <class T>
T* newOwned(lua_State* L, const char* type)
{
    return newHolder<T>(L, type).p;
}

template <class T>
Holder<T>& holderOf(lua_State* L, int arg, const char* type)
{
    return *static_cast<Holder<T>*>(luaL_checkudata(L, arg, type));
}

CellSet& cellsOf(lua_State* L, int arg)
{
    return *holderOf<CellSet>(L, arg, kCells).p;
}

CellMap& mapOf(lua_State* L, int arg)
{
    return *holderOf<CellMap>(L, arg, kMap).p;
}

std::vector<Cell>& listOf(lua_State* L, int arg)
{
    return *holderOf<std::vector<Cell>>(L, arg, kList).p;
}

blocks::Backs& backsOf(lua_State* L, int arg)
{
    return *holderOf<blocks::Backs>(L, arg, kBacks).p;
}

blocks::Plot& plotOf(lua_State* L, int arg)
{
    return *holderOf<blocks::Plot>(L, arg, kPlot).p;
}

/// Room for `n` more in `v`, made inside the capture (so the pushes after can't fail).
template <class V>
void reserveFor(lua_State* L, V& v, int n)
{
    genNative(L, [&] { v.reserve(v.size() + static_cast<std::size_t>(std::max(n, 0))); });
}

void pushCells(lua_State* L, const std::vector<Cell>& cells)
{
    lua_createtable(L, static_cast<int>(cells.size()), 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        lua_pushnumber(L, genCellKey(cells[i].first, cells[i].second));
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
}

std::vector<Cell> cellsArg(lua_State* L, int arg)
{
    std::vector<Cell> out;
    if (lua_istable(L, arg)) {
        const int n = lua_objlen(L, arg);
        reserveFor(L, out, n);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, arg, i);
            out.push_back(genCellOf(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    }
    return out;
}

int field(lua_State* L, int idx, const char* name)
{
    lua_rawgetfield(L, idx, name);
    const int v = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return v;
}

Chamber chamberArg(lua_State* L, int idx)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    Chamber c;
    c.centre = {field(L, idx, "x"), field(L, idx, "y")};
    c.radius = field(L, idx, "radius");
    c.shape = static_cast<RoomShape>(field(L, idx, "shape"));
    c.a = field(L, idx, "a");
    c.b = field(L, idx, "b");
    lua_rawgetfield(L, idx, "seed");
    c.seed = static_cast<std::uint32_t>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return c;
}

void pushChamber(lua_State* L, const Chamber& c)
{
    lua_createtable(L, 0, 7);
    for (const auto& [name, v] : {std::pair{"x", c.centre.first}, std::pair{"y", c.centre.second}, std::pair{"radius", c.radius},
                                  std::pair{"shape", static_cast<int>(c.shape)}, std::pair{"a", c.a}, std::pair{"b", c.b}}) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, name);
    }
    lua_pushnumber(L, c.seed);
    lua_setfield(L, -2, "seed");
}

// ---- gen.cells ---------------------------------------------------------------------------------

int newCells(lua_State* L)
{
    const std::vector<Cell> cells = cellsArg(L, 1);
    CellSet* s = newOwned<CellSet>(L, kCells);
    genNative(L, [&] {
        for (const Cell& c : cells) {
            s->insert(c);
        }
    });
    return 1;
}

int cellsHas(lua_State* L)
{
    lua_pushboolean(L, cellsOf(L, 1).contains(cellArg(L, 2)));
    return 1;
}

int cellsAdd(lua_State* L)
{
    CellSet& s = cellsOf(L, 1);
    const Cell c = cellArg(L, 2);
    lua_pushboolean(L, genNative(L, [&] { return s.insert(c); }));
    return 1;
}

int cellsRemove(lua_State* L)
{
    lua_pushboolean(L, cellsOf(L, 1).erase(cellArg(L, 2)));
    return 1;
}

int cellsRemoveAll(lua_State* L)
{
    CellSet& s = cellsOf(L, 1);
    for (const Cell& c : cellsArg(L, 2)) {
        s.erase(c);
    }
    return 0;
}

int cellsCount(lua_State* L)
{
    lua_pushinteger(L, static_cast<int>(cellsOf(L, 1).size()));
    return 1;
}

int cellsList(lua_State* L)
{
    const CellSet& s = cellsOf(L, 1);
    pushCells(L, genNative(L, [&] { return s.list(); }));
    return 1;
}

int cellsClone(lua_State* L)
{
    const CellSet& s = cellsOf(L, 1);
    CellSet* copy = newOwned<CellSet>(L, kCells);
    genNative(L, [&] { *copy = s; });
    return 1;
}

/// s:reach(from [, without]): the cells reached from `from` by side steps, less `without`.
int cellsReach(lua_State* L)
{
    const CellSet& s = cellsOf(L, 1);
    const Cell from = cellArg(L, 2);
    const std::vector<Cell> without = cellsArg(L, 3);
    CellSet* r = newOwned<CellSet>(L, kCells);
    genNative(L, [&] { *r = blocks::reachable(s, from, without); });
    return 1;
}

/// s:addSquare(c, r): every cell within r of c on both axes (both checkerboard colours).
int cellsAddSquare(lua_State* L)
{
    CellSet& s = cellsOf(L, 1);
    const Cell c = cellArg(L, 2);
    const int r = luaL_checkinteger(L, 3);
    genNative(L, [&] {
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                s.insert({c.first + dx, c.second + dy});
            }
        }
    });
    return 0;
}

/// s:lineAcross(at, d, most [, gone]): the line across a corridor going d (a side step) at `at`
/// (blocks::lineAcross), less the cells of the gen.cells `gone`.
int cellsLineAcross(lua_State* L)
{
    const CellSet& s = cellsOf(L, 1);
    const Cell at = cellArg(L, 2);
    const Cell d = genCellOf(luaL_checknumber(L, 3) + modes::script::genCellKey(0, 0));
    const int most = luaL_checkinteger(L, 4);
    const CellSet* gone = lua_isnoneornil(L, 5) ? nullptr : &cellsOf(L, 5);
    pushCells(L, genNative(L, [&] { return blocks::lineAcross(s, at, d, most, gone); }));
    return 1;
}

/// s:addAll(cells): each cell of a list.
int cellsAddAll(lua_State* L)
{
    CellSet& s = cellsOf(L, 1);
    const std::vector<Cell> cells = cellsArg(L, 2);
    genNative(L, [&] {
        for (const Cell& c : cells) {
            s.insert(c);
        }
    });
    return 0;
}

/// s:addAroundFloors(floor, floors, r): a square of reach r round each cell of the gen.cellmap
/// `floor` holding one of `floors`.
int cellsAddAroundFloors(lua_State* L)
{
    CellSet& s = cellsOf(L, 1);
    const CellMap& floor = mapOf(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<int> floors;
    reserveFor(L, floors, lua_objlen(L, 3));
    for (int i = 1; i <= lua_objlen(L, 3); ++i) {
        lua_rawgeti(L, 3, i);
        floors.push_back(static_cast<int>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    const int r = luaL_checkinteger(L, 4);
    if (floors.empty()) {
        return 0;
    }
    genNative(L, [&] {
        for (const Cell c : floor.keys()) {
            if (std::ranges::find(floors, floor.get(c)) != floors.end()) {
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        s.insert({c.first + dx, c.second + dy});
                    }
                }
            }
        }
    });
    return 0;
}

// ---- gen.list (cells, native) ------------------------------------------------------------------

int listCount(lua_State* L)
{
    lua_pushinteger(L, static_cast<int>(listOf(L, 1).size()));
    return 1;
}

int listAt(lua_State* L)
{
    const std::vector<Cell>& l = listOf(L, 1);
    const int i = luaL_checkinteger(L, 2);
    luaL_argcheck(L, i >= 1 && static_cast<std::size_t>(i) <= l.size(), 2, "list: no such cell");
    const Cell c = l[static_cast<std::size_t>(i - 1)];
    lua_pushnumber(L, genCellKey(c.first, c.second));
    return 1;
}

/// gen.wallsAround(open): the walls round the open floor (the exe's wall-combine table), a
/// gen.cellmap of their shapes.
int genWallsAround(lua_State* L)
{
    const CellSet& open = cellsOf(L, 1);
    CellMap* m = newOwned<CellMap>(L, kMap);
    genNative(L, [&] { *m = blocks::wallsMap(open); });
    return 1;
}

/// gen.backWalls(walls, open [, exclude]): the back walls the view looks at (a gen.backs).
int genBackWalls(lua_State* L)
{
    const CellMap& walls = mapOf(L, 1);
    const CellSet& open = cellsOf(L, 2);
    const CellSet none;
    const CellSet& exclude = lua_isnoneornil(L, 3) ? none : cellsOf(L, 3);
    Holder<blocks::Backs>& b = newHolder<blocks::Backs>(L, kBacks);
    genNative(L, [&] { *b.p = blocks::backWalls(walls, open, exclude); });
    chargeMore(L, b, b.p->capacity() * sizeof(blocks::Backs::value_type));
    return 1;
}

/// gen.roomy(open): the open cells with open sides all round, in order (a gen.list).
int genRoomy(lua_State* L)
{
    const CellSet& open = cellsOf(L, 1);
    Holder<std::vector<Cell>>& l = newHolder<std::vector<Cell>>(L, kList);
    genNative(L, [&] { *l.p = blocks::roomy(open); });
    chargeMore(L, l, l.p->capacity() * sizeof(Cell));
    return 1;
}

/// gen.freeCell(rng, cells, used, start, nearest, near or nil, farthest, tries): blocks::freeCell.
int genFreeCell(lua_State* L)
{
    std::minstd_rand& rng = modes::script::genMinstd(L, 1);
    const std::vector<Cell>& cells = listOf(L, 2);
    CellSet& used = cellsOf(L, 3);
    const Cell start = cellArg(L, 4);
    const int nearest = luaL_checkinteger(L, 5);
    const std::optional<Cell> near = lua_isnoneornil(L, 6) ? std::nullopt : std::optional(cellArg(L, 6));
    const int farthest = luaL_checkinteger(L, 7), tries = luaL_checkinteger(L, 8);
    luaL_argcheck(L, !cells.empty(), 2, "freeCell: no cells");
    const auto c = genNative(L, [&] { return blocks::freeCell(rng, cells, used, start, nearest, near, farthest, tries); });
    if (c) {
        lua_pushnumber(L, genCellKey(c->first, c->second));
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// gen.scatter(rng, cells, count, kinds, tries, used, clear, near or nil, deepIn or nil, start,
/// minFromStart, marks {set, reach, ...}): blocks::scatter's {cell, kind (from 1)} list.
int genScatter(lua_State* L)
{
    std::minstd_rand& rng = modes::script::genMinstd(L, 1);
    const std::vector<Cell>& cells = listOf(L, 2);
    const int count = luaL_checkinteger(L, 3);
    const auto kinds = static_cast<std::size_t>(std::max(luaL_checkinteger(L, 4), 0));
    const int tries = luaL_checkinteger(L, 5);
    CellSet& used = cellsOf(L, 6);
    const CellSet& clear = cellsOf(L, 7);
    const CellSet* near = lua_isnoneornil(L, 8) ? nullptr : &cellsOf(L, 8);
    const CellSet* deepIn = lua_isnoneornil(L, 9) ? nullptr : &cellsOf(L, 9);
    const Cell start = cellArg(L, 10);
    const int minFromStart = luaL_checkinteger(L, 11);
    std::vector<std::pair<CellSet*, int>> marks;
    if (lua_istable(L, 12)) {
        reserveFor(L, marks, lua_objlen(L, 12) / 2);
        for (int i = 1; i + 1 <= lua_objlen(L, 12); i += 2) {
            lua_rawgeti(L, 12, i);
            CellSet* set = &cellsOf(L, lua_gettop(L));
            lua_pop(L, 1);
            lua_rawgeti(L, 12, i + 1);
            marks.emplace_back(set, static_cast<int>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    }
    luaL_argcheck(L, !cells.empty() || count <= 0, 2, "scatter: no cells");
    const auto placed = genNative(L, [&] { return blocks::scatter(rng, cells, count, kinds, tries, used, clear, near, deepIn, start, minFromStart, marks); });
    lua_createtable(L, static_cast<int>(placed.size()), 0);
    for (std::size_t i = 0; i < placed.size(); ++i) {
        lua_createtable(L, 2, 0);
        lua_pushnumber(L, genCellKey(placed[i].cell.first, placed[i].cell.second));
        lua_rawseti(L, -2, 1);
        lua_pushinteger(L, static_cast<int>(placed[i].kind) + 1);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

/// gen.harmingFloor(name): a floor that harms what stands on it (the biomes' data, read once).
int genHarmingFloor(lua_State* L)
{
    std::size_t n = 0;
    const char* name = luaL_checklstring(L, 1, &n);
    // (Its first use reads the biomes' table, which can fail: as it was, out through the script.)
    lua_pushboolean(L, genNative(L, [&] { return harmingFloor(std::string_view(name, n)); }));
    return 1;
}

// ---- gen.cellmap -------------------------------------------------------------------------------

int newMap(lua_State* L)
{
    newOwned<CellMap>(L, kMap);
    return 1;
}

int mapGet(lua_State* L)
{
    const int v = mapOf(L, 1).get(cellArg(L, 2));
    if (v < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, v);
    }
    return 1;
}

int mapSet(lua_State* L)
{
    CellMap& m = mapOf(L, 1);
    const Cell c = cellArg(L, 2);
    if (lua_isnoneornil(L, 3)) {
        m.erase(c);
        return 0;
    }
    const int v = luaL_checkinteger(L, 3);
    luaL_argcheck(L, v >= 0 && v <= 32767, 3, "cellmap: a value from 0 to 32767");
    genNative(L, [&] { m.set(c, v); });
    return 0;
}

int mapCount(lua_State* L)
{
    lua_pushinteger(L, static_cast<int>(mapOf(L, 1).size()));
    return 1;
}

int mapCells(lua_State* L)
{
    const CellMap& m = mapOf(L, 1);
    pushCells(L, genNative(L, [&] { return m.keys().list(); }));
    return 1;
}

/// Whether the value at idx is a userdata of the type.
bool isType(lua_State* L, int idx, const char* type)
{
    if (!lua_getmetatable(L, idx)) {
        return false;
    }
    luaL_getmetatable(L, type);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same;
}

/// m:fill(cells, v): v on each cell of a gen.cells, or of a gen.cellmap's.
int mapFill(lua_State* L)
{
    CellMap& m = mapOf(L, 1);
    const CellSet& cells = isType(L, 2, kCells) ? cellsOf(L, 2) : mapOf(L, 2).keys();
    const int v = luaL_checkinteger(L, 3);
    luaL_argcheck(L, v >= 0 && v <= 32767, 3, "cellmap: a value from 0 to 32767");
    genNative(L, [&] {
        for (const Cell c : cells) {
            m.set(c, v);
        }
    });
    return 0;
}

std::vector<int> intsArg(lua_State* L, int arg)
{
    luaL_checktype(L, arg, LUA_TTABLE);
    std::vector<int> out;
    reserveFor(L, out, lua_objlen(L, arg));
    for (int i = 1; i <= lua_objlen(L, arg); ++i) {
        lua_rawgeti(L, arg, i);
        out.push_back(static_cast<int>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    return out;
}

/// floor:roughBlobs(layer, rng, cells, count, floors, harms, open, avoid, layerValue): blocks::roughBlobs.
int mapRoughBlobs(lua_State* L)
{
    CellMap& floor = mapOf(L, 1);
    CellMap& layer = mapOf(L, 2);
    std::minstd_rand& rng = modes::script::genMinstd(L, 3);
    const std::vector<Cell> cells = cellsArg(L, 4);
    const auto count = static_cast<std::size_t>(std::max(luaL_checkinteger(L, 5), 0));
    const std::vector<int> floors = intsArg(L, 6);
    luaL_checktype(L, 7, LUA_TTABLE);
    std::vector<bool> harms;
    reserveFor(L, harms, lua_objlen(L, 7));
    for (int i = 1; i <= lua_objlen(L, 7); ++i) {
        lua_rawgeti(L, 7, i);
        harms.push_back(lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);
    }
    luaL_argcheck(L, harms.size() == floors.size(), 7, "roughBlobs: a harm for each floor");
    const CellSet& open = cellsOf(L, 8);
    const CellSet& avoid = cellsOf(L, 9);
    const int layerValue = luaL_checkinteger(L, 10);
    genNative(L, [&] { blocks::roughBlobs(floor, layer, rng, cells, count, floors, harms, open, avoid, layerValue); });
    return 0;
}

/// floor:paintWithin(layer, c, radius, open, f, layerValue)
int mapPaintWithin(lua_State* L)
{
    CellMap& floor = mapOf(L, 1);
    CellMap& layer = mapOf(L, 2);
    const Cell c = cellArg(L, 3);
    const int radius = luaL_checkinteger(L, 4);
    const CellSet& open = cellsOf(L, 5);
    const int f = luaL_checkinteger(L, 6), l = luaL_checkinteger(L, 7);
    genNative(L, [&] { blocks::paintWithin(floor, layer, c, radius, open, f, l); });
    return 0;
}

/// floor:paintAround(layer, cells, open, f, layerValue)
int mapPaintAround(lua_State* L)
{
    CellMap& floor = mapOf(L, 1);
    CellMap& layer = mapOf(L, 2);
    const std::vector<Cell> cells = cellsArg(L, 3);
    const CellSet& open = cellsOf(L, 4);
    const int f = luaL_checkinteger(L, 5), l = luaL_checkinteger(L, 6);
    genNative(L, [&] { blocks::paintAround(floor, layer, cells, open, f, l); });
    return 0;
}

/// floor:absorb(layer, base, never, passes)
int mapAbsorb(lua_State* L)
{
    CellMap& floor = mapOf(L, 1);
    CellMap& layer = mapOf(L, 2);
    const int base = luaL_checkinteger(L, 3);
    const std::vector<int> never = intsArg(L, 4);
    const int passes = luaL_checkinteger(L, 5);
    genNative(L, [&] { blocks::absorb(floor, layer, base, never, passes); });
    return 0;
}

// ---- gen.backs ---------------------------------------------------------------------------------

int backsCount(lua_State* L)
{
    lua_pushinteger(L, static_cast<int>(backsOf(L, 1).size()));
    return 1;
}

/// b:at(i): the back wall i's wall cell and the open cell before it.
int backsAt(lua_State* L)
{
    const blocks::Backs& b = backsOf(L, 1);
    const int i = luaL_checkinteger(L, 2);
    luaL_argcheck(L, i >= 1 && static_cast<std::size_t>(i) <= b.size(), 2, "backs: no such back wall");
    const auto& [w, front] = b[static_cast<std::size_t>(i - 1)];
    lua_pushnumber(L, genCellKey(w.first, w.second));
    lua_pushnumber(L, genCellKey(front.first, front.second));
    return 2;
}

void pushIndices(lua_State* L, const std::vector<std::size_t>& list)
{
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        lua_pushinteger(L, static_cast<int>(list[i]) + 1);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
}

/// b:mounts(walls, shape, open, used, path, from, to, beside, apart, most): blocks::wallMounts'
/// indices (from 1; path indices from 1, `to` past the last).
int backsMounts(lua_State* L)
{
    const blocks::Backs& b = backsOf(L, 1);
    const CellMap& walls = mapOf(L, 2);
    const int shape = luaL_checkinteger(L, 3);
    const CellSet& open = cellsOf(L, 4);
    const CellSet& used = cellsOf(L, 5);
    const std::vector<Cell> path = cellsArg(L, 6);
    const auto from = static_cast<std::size_t>(std::max(luaL_checkinteger(L, 7) - 1, 0));
    const auto to = static_cast<std::size_t>(std::max(luaL_checkinteger(L, 8) - 1, 0));
    const int beside = luaL_checkinteger(L, 9), apart = luaL_checkinteger(L, 10);
    const auto most = static_cast<std::size_t>(luaL_checkinteger(L, 11));
    pushIndices(L, genNative(L, [&] { return blocks::wallMounts(b, walls, shape, open, used, path, from, to, beside, apart, most); }));
    return 1;
}

/// b:straightBack(walls, shape, near, avoid): blocks::straightBack's wall cell, or nil.
int backsStraight(lua_State* L)
{
    const blocks::Backs& b = backsOf(L, 1);
    const CellMap& walls = mapOf(L, 2);
    const int shape = luaL_checkinteger(L, 3);
    const Cell near = cellArg(L, 4);
    const std::vector<Cell> avoid = cellsArg(L, 5);
    const auto w = genNative(L, [&] { return blocks::straightBack(b, walls, shape, near, avoid); });
    if (w) {
        lua_pushnumber(L, genCellKey(w->first, w->second));
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// b:near(c, radius): the back walls whose fronts lie within the radius of c (indices from 1).
int backsNear(lua_State* L)
{
    const blocks::Backs& b = backsOf(L, 1);
    const Cell c = cellArg(L, 2);
    const int radius = luaL_checkinteger(L, 3);
    pushIndices(L, genNative(L, [&] { return blocks::backsNear(b, c, radius); }));
    return 1;
}

// ---- The placing scans -------------------------------------------------------------------------

/// gen.unusedAround(open, used, c)
int genUnusedAround(lua_State* L)
{
    lua_pushboolean(L, blocks::unusedAround(cellsOf(L, 1), cellsOf(L, 2), cellArg(L, 3)));
    return 1;
}

/// gen.spotsNear(path, near, from, to, open, used, start, minFromStart): blocks::spotsNear.
int genSpotsNear(lua_State* L)
{
    const std::vector<Cell> path = cellsArg(L, 1);
    const Cell near = cellArg(L, 2);
    const int from = luaL_checkinteger(L, 3), to = luaL_checkinteger(L, 4);
    const CellSet& open = cellsOf(L, 5);
    const CellSet& used = cellsOf(L, 6);
    const Cell start = cellArg(L, 7);
    const int minFromStart = luaL_checkinteger(L, 8);
    pushCells(L, genNative(L, [&] { return blocks::spotsNear(path, near, from, to, open, used, start, minFromStart); }));
    return 1;
}

/// gen.firstAround(c, from, to, open, used, around): blocks::firstAround's cell, or nil.
int genFirstAround(lua_State* L)
{
    const Cell c = cellArg(L, 1);
    const int from = luaL_checkinteger(L, 2), to = luaL_checkinteger(L, 3);
    const CellSet& open = cellsOf(L, 4);
    const CellSet& used = cellsOf(L, 5);
    const bool around = lua_toboolean(L, 6) != 0;
    const auto at = genNative(L, [&] { return blocks::firstAround(c, from, to, open, used, around); });
    if (at) {
        lua_pushnumber(L, genCellKey(at->first, at->second));
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ---- gen.plot ----------------------------------------------------------------------------------

int newPlot(lua_State* L)
{
    newOwned<blocks::Plot>(L, kPlot);
    return 1;
}

/// A blob into the plot (natively), charged; its index (from 1) pushed.
int plotPush(lua_State* L, const Chamber& c, int tag)
{
    Holder<blocks::Plot>& h = holderOf<blocks::Plot>(L, 1, kPlot);
    const std::size_t i = genNative(L, [&] { return h.p->add(c, tag); });
    chargeMore(L, h, blocks::Plot::kBlobBytes);
    lua_pushinteger(L, static_cast<int>(i) + 1);
    return 1;
}

/// p:add(chamber [, tag]): its index (from 1).
int plotAdd(lua_State* L)
{
    const Chamber c = chamberArg(L, 2);
    return plotPush(L, c, luaL_optinteger(L, 3, 0));
}

/// p:point(x, y [, tag]): a point (radius 0): its index.
int plotPoint(lua_State* L)
{
    const Chamber c{{luaL_checkinteger(L, 2), luaL_checkinteger(L, 3)}, 0};
    return plotPush(L, c, luaL_optinteger(L, 4, 0));
}

int plotCount(lua_State* L)
{
    lua_pushinteger(L, static_cast<int>(plotOf(L, 1).size()));
    return 1;
}

/// p:clear(x, y, r, gap, before): no blob of the first `before` within gap of the disc.
int plotClear(lua_State* L)
{
    const blocks::Plot& p = plotOf(L, 1);
    const Cell c{luaL_checkinteger(L, 2), luaL_checkinteger(L, 3)};
    const int before = luaL_checkinteger(L, 6);
    lua_pushboolean(L, p.clear(c, luaL_checkinteger(L, 4), luaL_checkinteger(L, 5), static_cast<std::size_t>(std::max(before, 0))));
    return 1;
}

/// p:run(x, y, dx, dy, length, margin, width, gap, before, tag): the run's cells, or nil.
int plotRun(lua_State* L)
{
    blocks::Plot& p = plotOf(L, 1);
    const Cell from{luaL_checkinteger(L, 2), luaL_checkinteger(L, 3)};
    const Cell way{luaL_checkinteger(L, 4), luaL_checkinteger(L, 5)};
    const int length = luaL_checkinteger(L, 6), margin = luaL_checkinteger(L, 7), width = luaL_checkinteger(L, 8);
    const int gap = luaL_checkinteger(L, 9), before = luaL_checkinteger(L, 10), tag = luaL_checkinteger(L, 11);
    const auto cells = genNative(L, [&] { return p.run(from, way, length, margin, width, gap, static_cast<std::size_t>(std::max(before, 0)), tag); });
    if (cells) {
        pushCells(L, *cells);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// p:spur(x, y, dx, dy, length, margin, width, gap, room, skip, exX, exY, exWithin, exTag): the
/// corridor's cells (the room's x and y set), or nil.
int plotSpur(lua_State* L)
{
    blocks::Plot& p = plotOf(L, 1);
    const Cell from{luaL_checkinteger(L, 2), luaL_checkinteger(L, 3)};
    const Cell way{luaL_checkinteger(L, 4), luaL_checkinteger(L, 5)};
    const int length = luaL_checkinteger(L, 6), margin = luaL_checkinteger(L, 7), width = luaL_checkinteger(L, 8);
    const int gap = luaL_checkinteger(L, 9);
    Chamber room = chamberArg(L, 10);
    const int skip = luaL_checkinteger(L, 11);
    const blocks::Except except{{luaL_checkinteger(L, 12), luaL_checkinteger(L, 13)}, luaL_checkinteger(L, 14), luaL_checkinteger(L, 15)};
    const auto cells = genNative(L, [&] { return p.spur(from, way, length, margin, width, gap, room, static_cast<std::size_t>(std::max(skip, 0)), except); });
    if (!cells) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, room.centre.first);
    lua_setfield(L, 10, "x");
    lua_pushinteger(L, room.centre.second);
    lua_setfield(L, 10, "y");
    pushCells(L, *cells);
    return 1;
}

/// p:lRun(ax, ay, bx, by, uFirst, width, margin, gap, endsWithin, route, routeReach, middle): the
/// L's cells, or nil.
int plotLRun(lua_State* L)
{
    blocks::Plot& p = plotOf(L, 1);
    const Cell a{luaL_checkinteger(L, 2), luaL_checkinteger(L, 3)};
    const Cell b{luaL_checkinteger(L, 4), luaL_checkinteger(L, 5)};
    const bool uFirst = lua_toboolean(L, 6) != 0;
    const int width = luaL_checkinteger(L, 7), margin = luaL_checkinteger(L, 8), gap = luaL_checkinteger(L, 9);
    const int endsWithin = luaL_checkinteger(L, 10);
    const blocks::Plot& route = plotOf(L, 11);
    const int routeReach = luaL_checkinteger(L, 12), middle = luaL_checkinteger(L, 13);
    const auto cells = genNative(L, [&] { return p.lRun(a, b, uFirst, width, margin, gap, endsWithin, route, routeReach, middle); });
    if (cells) {
        pushCells(L, *cells);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// p:lEnds(start, minAlong, tail, minLen, maxLen, minEach): the points' indices (from 1).
int plotLEnds(lua_State* L)
{
    const blocks::Plot& p = plotOf(L, 1);
    const int start = luaL_checkinteger(L, 2) - 1;
    luaL_argcheck(L, start >= 0 && static_cast<std::size_t>(start) < p.size(), 2, "lEnds: no such point");
    const auto minAlong = static_cast<std::size_t>(std::max(luaL_checkinteger(L, 3), 0));
    const auto tail = static_cast<std::size_t>(std::max(luaL_checkinteger(L, 4), 0));
    const int minLen = luaL_checkinteger(L, 5), maxLen = luaL_checkinteger(L, 6), minEach = luaL_checkinteger(L, 7);
    const auto ends = genNative(L, [&] { return p.lEnds(static_cast<std::size_t>(start), minAlong, tail, minLen, maxLen, minEach); });
    lua_createtable(L, static_cast<int>(ends.size()), 0);
    for (std::size_t i = 0; i < ends.size(); ++i) {
        lua_pushinteger(L, static_cast<int>(ends[i]) + 1);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

/// p:carve(ways, clearOf, start, startRadius [, fillPinholes]): the floor (a gen.cells).
int plotCarve(lua_State* L)
{
    const blocks::Plot& p = plotOf(L, 1);
    const std::vector<Cell> ways = cellsArg(L, 2);
    const int clearOf = luaL_checkinteger(L, 3);
    const Cell start = cellArg(L, 4);
    const int startRadius = luaL_checkinteger(L, 5);
    const bool fill = lua_isnone(L, 6) || lua_toboolean(L, 6);
    CellSet* open = newOwned<CellSet>(L, kCells);
    genNative(L, [&] { *open = p.carve(ways, clearOf, start, startRadius, fill); });
    return 1;
}

/// gen.room(rng, x, y, radius, shape): a room of the shape within the radius (its sizes drawn).
int genRoom(lua_State* L)
{
    std::mt19937& rng = modes::script::genMt19937(L, 1);
    const Cell at{luaL_checkinteger(L, 2), luaL_checkinteger(L, 3)};
    const int radius = luaL_checkinteger(L, 4);
    const int shape = luaL_checkinteger(L, 5);
    luaL_argcheck(L, shape >= 0 && shape <= static_cast<int>(RoomShape::Cavern), 5, "room: no such shape");
    pushChamber(L, genNative(L, [&] { return blocks::makeRoom(at, radius, static_cast<RoomShape>(shape), rng); }));
    return 1;
}

void metatable(lua_State* L, const char* type, std::initializer_list<std::pair<const char*, lua_CFunction>> methods)
{
    luaL_newmetatable(L, type);
    lua_createtable(L, 0, static_cast<int>(methods.size()));
    for (const auto& [name, fn] : methods) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, type);
    lua_setfield(L, -2, "__type");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
}

} // namespace

CellSet& cellSetArg(lua_State* L, int idx)
{
    return cellsOf(L, idx);
}

CellMap& cellMapArg(lua_State* L, int idx)
{
    return mapOf(L, idx);
}

void openGenBlocks(lua_State* L)
{
    metatable(L, kCells,
              {{"has", cellsHas}, {"add", cellsAdd}, {"remove", cellsRemove}, {"removeAll", cellsRemoveAll}, {"count", cellsCount},
               {"list", cellsList}, {"clone", cellsClone}, {"reach", cellsReach}, {"addSquare", cellsAddSquare},
               {"lineAcross", cellsLineAcross}, {"addAll", cellsAddAll}, {"addAroundFloors", cellsAddAroundFloors}});
    metatable(L, kList, {{"count", listCount}, {"at", listAt}});
    metatable(L, kMap,
              {{"get", mapGet}, {"set", mapSet}, {"count", mapCount}, {"cells", mapCells}, {"fill", mapFill}, {"roughBlobs", mapRoughBlobs},
               {"paintWithin", mapPaintWithin}, {"paintAround", mapPaintAround}, {"absorb", mapAbsorb}});
    metatable(L, kBacks,
              {{"count", backsCount}, {"at", backsAt}, {"mounts", backsMounts}, {"near", backsNear}, {"straightBack", backsStraight}});
    metatable(L, kPlot,
              {{"add", plotAdd}, {"point", plotPoint}, {"count", plotCount}, {"clear", plotClear}, {"run", plotRun}, {"spur", plotSpur},
               {"lRun", plotLRun}, {"lEnds", plotLEnds}, {"carve", plotCarve}});
    for (const auto& [name, fn] : {std::pair<const char*, lua_CFunction>{"cells", newCells}, {"plot", newPlot}, {"room", genRoom},
                                   {"cellmap", newMap}, {"unusedAround", genUnusedAround}, {"spotsNear", genSpotsNear},
                                   {"firstAround", genFirstAround}, {"wallsAround", genWallsAround},
                                   {"backWalls", genBackWalls}, {"roomy", genRoomy}, {"freeCell", genFreeCell},
                                   {"scatter", genScatter}, {"harmingFloor", genHarmingFloor}}) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
}

} // namespace nox::mapgen::detail
