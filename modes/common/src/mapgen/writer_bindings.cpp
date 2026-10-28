// The map writer as the generator's scripts see it: a
// MapWriter's methods on a userdata, and its NoxScript builder's. Each call's own exception (a
// name the thing database hasn't, a builder's check, memory) comes out of the script as it was
// thrown (genNative), as when the design was C++; an argument no call could take (an index past
// the table, a number no field holds) is the script's error. Cells are the scripts' numbers
// (gen_runtime.hpp), points floats as the script made them (gen.f32's).
#include "gen_bindings.hpp"

#include "noxcore/modes/script/gen_runtime.hpp"
#include "noxcore/script/opcodes.hpp"

#include "lua.h"
#include "lualib.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nox::mapgen::detail {

namespace {

using modes::script::genCellOf;
using modes::script::genNative;

constexpr const char* kWriter = "gen.map";
constexpr const char* kBuilder = "gen.script";
constexpr const char* kFn = "gen.scriptfn";

MapWriter& writerOf(lua_State* L)
{
    return **static_cast<MapWriter**>(luaL_checkudata(L, 1, kWriter));
}

ScriptBuilder& builderOf(lua_State* L)
{
    return **static_cast<ScriptBuilder**>(luaL_checkudata(L, 1, kBuilder));
}

ScriptBuilder::Fn& fnOf(lua_State* L)
{
    return **static_cast<ScriptBuilder::Fn**>(luaL_checkudata(L, 1, kFn));
}

float floatArg(lua_State* L, int arg)
{
    return static_cast<float>(luaL_checknumber(L, arg)); // (a float already: the script's f32)
}

/// A Luau string's text, copied natively.
std::string copied(lua_State* L, const char* s, std::size_t n)
{
    return genNative(L, [&] { return std::string(s, n); });
}

std::string stringArg(lua_State* L, int arg)
{
    std::size_t n = 0;
    const char* s = luaL_checklstring(L, arg, &n);
    return copied(L, s, n);
}

/// A whole number from lo to hi (as a field of that type holds it): anything else is the
/// script's error, never a cast out of range.
double wholeIn(lua_State* L, double v, double lo, double hi, const char* what)
{
    if (!(v >= lo && v <= hi && v == std::floor(v))) {
        luaL_error(L, "%s: %f isn't a whole number from %.0f to %.0f", what, v, lo, hi);
    }
    return v;
}

std::uint32_t u32Of(lua_State* L, double v, const char* what)
{
    return static_cast<std::uint32_t>(wholeIn(L, v, 0, 4294967295.0, what));
}

std::int32_t i32Of(lua_State* L, double v, const char* what)
{
    return static_cast<std::int32_t>(wholeIn(L, v, -2147483648.0, 2147483647.0, what));
}

double numberField(lua_State* L, int idx, const char* name, double fallback = 0)
{
    lua_rawgetfield(L, idx, name);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return v;
}

std::string stringField(lua_State* L, int idx, const char* name)
{
    lua_rawgetfield(L, idx, name);
    std::size_t n = 0;
    const char* s = lua_isstring(L, -1) ? lua_tolstring(L, -1, &n) : "";
    std::string v = copied(L, s, n);
    lua_pop(L, 1);
    return v;
}

void pushBox(lua_State* L, void* p, const char* type)
{
    *static_cast<void**>(lua_newuserdata(L, sizeof(void*))) = p;
    luaL_getmetatable(L, type);
    lua_setmetatable(L, -2);
}

/// The whole number at t[name] from lo to hi (absent: `fallback`).
double wholeField(lua_State* L, int t, const char* name, double fallback, double lo, double hi, const char* what)
{
    return wholeIn(L, numberField(L, t, name, fallback), lo, hi, what);
}

bool hasField(lua_State* L, int t, const char* name)
{
    lua_rawgetfield(L, t, name);
    const bool has = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return has;
}

/// t[name], a list of at most `most` entries, each read by `each` (its value on the stack's top,
/// 1-based index); absent: nothing read.
template <class F>
void listField(lua_State* L, int t, const char* name, int most, const char* what, F&& each)
{
    lua_rawgetfield(L, t, name);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const int n = lua_objlen(L, -1);
        if (n > most) {
            luaL_error(L, "%s: at most %d", what, most);
        }
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            each(i);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

/// A colour {r, g, b} on the stack's top, each 0-255.
std::array<std::uint8_t, 3> rgbAtTop(lua_State* L, const char* what)
{
    luaL_checktype(L, -1, LUA_TTABLE);
    if (lua_objlen(L, -1) != 3) {
        luaL_error(L, "%s: a colour is {r, g, b}", what);
    }
    std::array<std::uint8_t, 3> c{};
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, -1, i + 1);
        c[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(wholeIn(L, lua_tonumber(L, -1), 0, 255, what));
        lua_pop(L, 1);
    }
    return c;
}

/// The radius the game gives a light of an intensity (lightRadiusFromIntensity: none at 1 or
/// under; sqrt(((4 + min(I, 31)) / (4 + 1) + 1) * I * I / (961 * 0.001)), its x87 sum made whole
/// by truncating, as Math_DoubleToInt does in the game's chop mode).
std::int32_t lightRadiusFromIntensity(float intensity)
{
    if (!(intensity > 1.0f)) {
        return 0;
    }
    const double capped = intensity > 31.0f ? 31.0 : static_cast<double>(intensity);
    const double i = intensity;
    const double q = i * i / (961.0 * static_cast<double>(0.001f));
    return static_cast<std::int32_t>(std::sqrt(((4.0 + capped) / 5.0 + 1.0) * q));
}

/// A light's block from a spec's `light` table at t (InvisibleLightXfer's fields, the map editor's
/// names). Left out, each is what the shipped maps' plain lights hold: coloured (type 2), all
/// round, the animation flags 0x15, the colour and the intensity its first keys, the unused byte
/// 0x80; the stored radius and 16.16 copy worked out from the intensity as the game's
/// Draw_LightSetIntensity does (a radius given: that one or one more, nothing else).
map::xfer::InvisibleLight lightOf(lua_State* L, int t)
{
    map::xfer::InvisibleLight l;
    l.type = static_cast<std::uint32_t>(wholeField(L, t, "type", 2, 0, 2, "light: type"));
    const double intensity = numberField(L, t, "intensity", 0);
    if (!(intensity >= 0 && intensity <= 63)) { // (Draw_LightSetIntensity's most; darkness is `anti`)
        luaL_error(L, "light: intensity %f isn't from 0 to 63", intensity);
    }
    l.intensity = static_cast<float>(intensity);
    // (Draw_LightSetIntensity: the 16.16 copy rounded by adding a half, then truncated by __ftol.)
    l.intensityFx = static_cast<std::int32_t>(static_cast<double>(l.intensity) * 65536.0 + 0.5);
    // The stored radius is the stock client's reach (NoX Core's works it out from the intensity):
    // the game's, or one more (the rounded value many shipped maps hold), so both clients draw
    // the same light.
    const std::int32_t radius = lightRadiusFromIntensity(l.intensity);
    l.radius = static_cast<std::int32_t>(wholeField(L, t, "radius", radius, 0, 0x7FFFFFFF, "light: radius"));
    if (l.radius != radius && l.radius != radius + 1) {
        luaL_error(L, "thing: light radius %d isn't %d (the intensity's) or %d", static_cast<int>(l.radius), static_cast<int>(radius),
                   static_cast<int>(radius + 1));
    }
    lua_rawgetfield(L, t, "color");
    if (!lua_isnil(L, -1)) {
        const auto c = rgbAtTop(L, "light: color");
        l.color = {c[0], c[1], c[2]};
    }
    lua_pop(L, 1);
    l.direction = static_cast<std::uint16_t>(wholeField(L, t, "direction", 0, 0, 65535, "light: direction"));
    l.cone = static_cast<std::uint16_t>(wholeField(L, t, "cone", 0, 0, 65535, "light: cone"));
    lua_rawgetfield(L, t, "omni");
    l.omni = lua_isnil(L, -1) || lua_toboolean(L, -1) ? 0xFFFFu : 0u; // (all round unless told: the editor's 0xFFFF)
    lua_pop(L, 1);
    l.animFlags = static_cast<std::uint16_t>(wholeField(L, t, "anim", 0x15, 0, 65535, "light: anim"));
    // (The keys: its colour and intensity the first of each, unless the lists are given.)
    if (!hasField(L, t, "colorKeys")) {
        l.colorKeys[0] = {static_cast<std::uint8_t>(l.color[0]), static_cast<std::uint8_t>(l.color[1]), static_cast<std::uint8_t>(l.color[2])};
    }
    if (!hasField(L, t, "intensityKeys")) {
        l.intensityKeys[0] = static_cast<std::uint8_t>(l.intensity);
    }
    listField(L, t, "colorKeys", 16, "light: colorKeys", [&](int i) { l.colorKeys[static_cast<std::size_t>(i - 1)] = rgbAtTop(L, "light: colorKeys"); });
    listField(L, t, "intensityKeys", 16, "light: intensityKeys", [&](int i) {
        l.intensityKeys[static_cast<std::size_t>(i - 1)] = static_cast<std::uint8_t>(wholeIn(L, lua_tonumber(L, -1), 0, 255, "light: intensityKeys"));
    });
    listField(L, t, "coneKeys", 16, "light: coneKeys", [&](int i) {
        l.coneKeys[static_cast<std::size_t>(i - 1)] = static_cast<std::uint8_t>(wholeIn(L, lua_tonumber(L, -1), 0, 255, "light: coneKeys"));
    });
    l.colorStep = static_cast<std::uint16_t>(wholeField(L, t, "colorStep", 0, 0, 65535, "light: colorStep"));
    l.intensityStep = static_cast<std::uint16_t>(wholeField(L, t, "intensityStep", 0, 0, 65535, "light: intensityStep"));
    l.coneStep = static_cast<std::uint16_t>(wholeField(L, t, "coneStep", 0, 0, 65535, "light: coneStep"));
    l.target = u32Of(L, numberField(L, t, "target"), "light: target");
    // (A signed speed, as colorLightRotate sign-extends it: stored as its 16 bits.)
    l.rotationSpeed = static_cast<std::uint16_t>(static_cast<std::int16_t>(wholeField(L, t, "rotationSpeed", 0, -32768, 32767, "light: rotationSpeed")));
    l.rotationMax = static_cast<std::uint16_t>(wholeField(L, t, "rotationMax", 0, 0, 65535, "light: rotationMax"));
    l.unused = static_cast<std::uint8_t>(wholeField(L, t, "unused", 0x80, 0, 255, "light: unused")); // (saved, never read)
    lua_rawgetfield(L, t, "anti");
    l.isAnti = lua_toboolean(L, -1) ? 1u : 0u;
    lua_pop(L, 1);
    return l;
}

// ---- The writer ------------------------------------------------------------------------------

int wFloorIndex(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string name = stringArg(L, 2);
    lua_pushinteger(L, genNative(L, [&] { return w.floorIndex(name); }));
    return 1;
}

int wWallIndex(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string name = stringArg(L, 2);
    lua_pushinteger(L, genNative(L, [&] { return w.wallIndex(name); }));
    return 1;
}

int wEdgeIndex(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string name = stringArg(L, 2);
    lua_pushinteger(L, genNative(L, [&] { return w.edgeIndex(name); }));
    return 1;
}

int wFloorName(lua_State* L)
{
    const MapWriter& w = writerOf(L);
    const int index = luaL_checkinteger(L, 2);
    luaL_argcheck(L, index >= 0 && static_cast<std::size_t>(index) < w.floorCount(), 2, "floorName: no floor with that index");
    const std::string_view name = genNative(L, [&] { return w.floorName(index); });
    lua_pushlstring(L, name.data(), name.size());
    return 1;
}

int wIsPlain(lua_State* L)
{
    const MapWriter& w = writerOf(L);
    const std::string type = stringArg(L, 2);
    lua_pushboolean(L, genNative(L, [&] { return w.isPlain(type); }));
    return 1;
}

/// m:object(type, x, y [, kinds [, onDeath]]): kinds the text after a spec's '@' (kindsFromText:
/// as std::stoul read it, its failures as it threw them).
int wObject(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string type = stringArg(L, 2);
    const float x = floatArg(L, 3), y = floatArg(L, 4);
    const bool hasKinds = !lua_isnoneornil(L, 5);
    if (hasKinds) {
        luaL_checktype(L, 5, LUA_TSTRING); // (the text itself, never a number made text)
    }
    const std::string kindsText = hasKinds ? stringArg(L, 5) : std::string();
    std::string onDeath;
    if (!lua_isnoneornil(L, 6)) {
        onDeath = stringArg(L, 6);
    }
    genNative(L, [&] {
        const std::optional<std::uint32_t> kinds = hasKinds ? std::optional(kindsFromText(kindsText)) : std::nullopt;
        w.object(type, x, y, kinds, onDeath);
    });
    return 0;
}

int wBare(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string type = stringArg(L, 2);
    const float x = floatArg(L, 3), y = floatArg(L, 4);
    genNative(L, [&] { w.bare(type, x, y); });
    return 0;
}

int wSign(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string type = stringArg(L, 2);
    const float x = floatArg(L, 3), y = floatArg(L, 4);
    genNative(L, [&] { w.sign(type, x, y); });
    return 0;
}

int wExit(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string type = stringArg(L, 2);
    const float x = floatArg(L, 3), y = floatArg(L, 4);
    genNative(L, [&] { w.exit(type, x, y); });
    return 0;
}

/// m:nextExtent(): the extent the next record written gets (a teleporter's partner written after it).
int wNextExtent(lua_State* L)
{
    lua_pushnumber(L, writerOf(L).nextExtent());
    return 1;
}

/// m:thing(type, x, y, spec): spec's name, enabled, status, onDeath, and one of its xfers:
/// trigger {size, pressed, released, allowed}, glyph {x, y, spell}, hole {fallX, fallY, timeout},
/// door {direction, lock}, mover {speed, extent, base}, transporter {target}, light (lightOf).
/// Its extent.
int wThing(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string type = stringArg(L, 2);
    const float x = floatArg(L, 3), y = floatArg(L, 4);
    luaL_checktype(L, 5, LUA_TTABLE);
    ThingSpec s;
    s.name = stringField(L, 5, "name");
    lua_rawgetfield(L, 5, "enabled");
    s.enabled = lua_isnil(L, -1) || lua_toboolean(L, -1);
    lua_pop(L, 1);
    s.status = u32Of(L, numberField(L, 5, "status"), "thing: status");
    s.onDeath = stringField(L, 5, "onDeath");
    const auto sub = [&](const char* name) {
        lua_rawgetfield(L, 5, name);
        if (lua_istable(L, -1)) {
            return lua_gettop(L);
        }
        lua_pop(L, 1);
        return 0;
    };
    // (Each xfer's text and lists made natively: a failure as it was. Moved in, never copied.)
    if (const int t = sub("trigger")) {
        map::xfer::Trigger x2;
        x2.sizeX = x2.sizeY = i32Of(L, numberField(L, t, "size"), "thing: trigger size");
        x2.edgeColor = {90, 90, 90}; // (TriggerCreate's colours)
        x2.backColor = {10, 10, 10};
        x2.onPressed.function = stringField(L, t, "pressed");
        x2.onReleased.function = stringField(L, t, "released");
        x2.allowedClasses = u32Of(L, numberField(L, t, "allowed", 4), "thing: trigger allowed"); // (4: players)
        genNative(L, [&] { s.trigger = std::move(x2); });
        lua_pop(L, 1);
    }
    if (const int t = sub("glyph")) {
        map::xfer::Glyph x2;
        x2.targetX = static_cast<float>(numberField(L, t, "x"));
        x2.targetY = static_cast<float>(numberField(L, t, "y"));
        std::string spell = stringField(L, t, "spell");
        genNative(L, [&] {
            x2.spells = {std::move(spell)};
            s.glyph = std::move(x2);
        });
        lua_pop(L, 1);
    }
    if (const int t = sub("hole")) {
        map::xfer::Hole x2;
        x2.fallX = i32Of(L, numberField(L, t, "fallX"), "thing: hole fallX");
        x2.fallY = i32Of(L, numberField(L, t, "fallY"), "thing: hole fallY");
        x2.scriptTimeout = static_cast<std::uint16_t>(wholeIn(L, numberField(L, t, "timeout"), 0, 65535, "thing: hole timeout"));
        genNative(L, [&] { s.hole = std::move(x2); });
        lua_pop(L, 1);
    }
    if (const int t = sub("door")) {
        map::xfer::Door x2;
        x2.currentDirection = x2.direction = u32Of(L, numberField(L, t, "direction"), "thing: door direction");
        x2.lock = u32Of(L, numberField(L, t, "lock"), "thing: door lock");
        genNative(L, [&] { s.door = std::move(x2); });
        lua_pop(L, 1);
    }
    if (const int t = sub("mover")) {
        map::xfer::Mover x2;
        x2.movingSpeed = u32Of(L, numberField(L, t, "speed"), "thing: mover speed");
        x2.movedExtent = u32Of(L, numberField(L, t, "extent"), "thing: mover extent");
        x2.speedBase = x2.speedCurrent = static_cast<float>(numberField(L, t, "base"));
        genNative(L, [&] { s.mover = std::move(x2); });
        lua_pop(L, 1);
    }
    if (const int t = sub("transporter")) {
        s.transporter = map::xfer::Transporter{u32Of(L, numberField(L, t, "target"), "thing: transporter target")};
        lua_pop(L, 1);
    }
    if (const int t = sub("light")) {
        s.light = lightOf(L, t);
        lua_pop(L, 1);
    }
    lua_pushnumber(L, genNative(L, [&] { return w.thing(type, x, y, s); }));
    return 1;
}

int wObjectGroup(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string name = stringArg(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::uint32_t> extents;
    const int n = lua_objlen(L, 3);
    genNative(L, [&] { extents.reserve(static_cast<std::size_t>(n)); });
    for (int i = 1; i <= lua_objlen(L, 3); ++i) {
        lua_rawgeti(L, 3, i);
        extents.push_back(u32Of(L, lua_tonumber(L, -1), "objectGroup: an extent"));
        lua_pop(L, 1);
    }
    genNative(L, [&] { w.objectGroup(name, extents); });
    return 0;
}

int wWallGroup(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string name = stringArg(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<Cell> cells;
    const int n = lua_objlen(L, 3);
    genNative(L, [&] { cells.reserve(static_cast<std::size_t>(n)); });
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        cells.push_back(genCellOf(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    genNative(L, [&] { w.wallGroup(name, cells); });
    return 0;
}

int wSecretWall(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const Cell c = genCellOf(luaL_checknumber(L, 2));
    const bool open = lua_toboolean(L, 3) != 0;
    genNative(L, [&] { w.secretWall(c, open); });
    return 0;
}

int wWaypoint(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const std::string name = stringArg(L, 2);
    const float x = floatArg(L, 3), y = floatArg(L, 4);
    lua_pushnumber(L, genNative(L, [&] { return w.waypoint(name, x, y); }));
    return 1;
}

int wLink(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const int a = luaL_checkinteger(L, 2), b = luaL_checkinteger(L, 3);
    const auto made = static_cast<int>(w.waypointCount());
    luaL_argcheck(L, a >= 1 && a <= made, 2, "link: no waypoint with that id");
    luaL_argcheck(L, b >= 1 && b <= made, 3, "link: no waypoint with that id");
    genNative(L, [&] { w.link(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)); });
    return 0;
}

/// m:walls(shapes, material): the wall section from a gen.cellmap of the walls' shapes.
int wWalls(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const CellMap& shapes = cellMapArg(L, 2);
    const auto material = static_cast<std::uint8_t>(luaL_checkinteger(L, 3));
    genNative(L, [&] { w.walls(shapes, material); });
    return 0;
}

/// m:floors(floor, layer, edgeMask or nil, rng): the floor section from the plan's two gen.cellmaps,
/// blended (an edge mask given) drawing from the gen.minstd stream.
int wFloors(lua_State* L)
{
    MapWriter& w = writerOf(L);
    const CellMap& floor = cellMapArg(L, 2);
    const CellMap& layer = cellMapArg(L, 3);
    const std::optional<std::uint8_t> mask =
        lua_isnumber(L, 4) ? std::optional(static_cast<std::uint8_t>(wholeIn(L, lua_tonumber(L, 4), 0, 255, "floors: the edge mask"))) : std::nullopt;
    std::minstd_rand& rng = modes::script::genMinstd(L, 5);
    genNative(L, [&] { w.floors(floor, layer, mask, rng); });
    return 0;
}

int wAmbient(lua_State* L)
{
    writerOf(L).ambient(u32Of(L, luaL_checknumber(L, 2), "ambient"));
    return 0;
}

int wScript(lua_State* L)
{
    pushBox(L, &writerOf(L).script(), kBuilder);
    return 1;
}

// ---- Its NoxScript builder ---------------------------------------------------------------------

int sGlobal(lua_State* L)
{
    ScriptBuilder& s = builderOf(L);
    const std::string name = stringArg(L, 2);
    lua_pushinteger(L, genNative(L, [&] { return s.global(name); }));
    return 1;
}

int sSetGlobalAtLoad(lua_State* L)
{
    ScriptBuilder& s = builderOf(L);
    const int g = luaL_checkinteger(L, 2), v = luaL_checkinteger(L, 3);
    genNative(L, [&] { s.setGlobalAtLoad(g, v); });
    return 0;
}

int sFn(lua_State* L)
{
    ScriptBuilder& s = builderOf(L);
    const std::string name = stringArg(L, 2);
    const auto args = static_cast<std::uint32_t>(luaL_optinteger(L, 3, 0));
    const auto locals = static_cast<std::uint32_t>(luaL_optinteger(L, 4, 0));
    ScriptBuilder::Fn* f = genNative(L, [&] { return &s.function(name, args, locals); });
    pushBox(L, f, kFn);
    return 1;
}

int sHas(lua_State* L)
{
    const ScriptBuilder& s = builderOf(L);
    const std::string name = stringArg(L, 2);
    lua_pushboolean(L, genNative(L, [&] { return s.has(name); }));
    return 1;
}

/// An instruction: `emit` on the function (natively), then the function itself again (to chain).
template <class F>
int chain(lua_State* L, F&& emit)
{
    ScriptBuilder::Fn& f = fnOf(L);
    genNative(L, [&] { emit(f); });
    lua_settop(L, 1);
    return 1;
}

std::optional<script::Op> opNamed(std::string_view name)
{
    // (never destroyed (made on the heap, never freed): a Delve level still being built on a
    // worker as the program exits must not read it after its destruction)
    static const std::map<std::string, script::Op, std::less<>>& ops = *[] {
        auto* m = new std::map<std::string, script::Op, std::less<>>();
        for (std::uint32_t i = 0; i < 0x100; ++i) {
            if (const auto n = script::opName(i)) {
                m->emplace(std::string(*n), static_cast<script::Op>(i));
            }
        }
        return m;
    }();
    const auto it = ops.find(name);
    return it == ops.end() ? std::nullopt : std::optional(it->second);
}

int fPushInt(lua_State* L)
{
    const int v = luaL_checkinteger(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.pushInt(v); });
}
int fPushString(lua_State* L)
{
    const std::string s = stringArg(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.pushString(s); });
}
int fPushFunction(lua_State* L)
{
    const std::string s = stringArg(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.pushFunction(s); });
}
int fObject(lua_State* L)
{
    const std::string s = stringArg(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.object(s); });
}
int fCall(lua_State* L)
{
    const std::string s = stringArg(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.call(s); });
}
int fCallScript(lua_State* L)
{
    const std::string s = stringArg(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.callScript(s); });
}
int fOp(lua_State* L)
{
    const std::string name = stringArg(L, 2);
    const auto op = genNative(L, [&] { return opNamed(name); }); // (its table made at the first use)
    if (!op) {
        luaL_error(L, "script: no op %s", lua_tostring(L, 2));
    }
    return chain(L, [&](ScriptBuilder::Fn& f) { f.op(*op); });
}
int fLoadGlobal(lua_State* L)
{
    const int g = luaL_checkinteger(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.loadGlobal(g); });
}
int fSetGlobal(lua_State* L)
{
    const int g = luaL_checkinteger(L, 2), v = luaL_checkinteger(L, 3);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.setGlobal(g, v); });
}
int fStoreGlobal(lua_State* L)
{
    const int g = luaL_checkinteger(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.storeGlobal(g); });
}
int fEndStore(lua_State* L)
{
    return chain(L, [&](ScriptBuilder::Fn& f) { f.endStore(); });
}
int fLoadLocal(lua_State* L)
{
    const int l = luaL_checkinteger(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.loadLocal(l); });
}
int fLabel(lua_State* L)
{
    const std::string s = stringArg(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.label(s); });
}
int fJumpIf(lua_State* L)
{
    const std::string s = stringArg(L, 2);
    return chain(L, [&](ScriptBuilder::Fn& f) { f.jumpIf(s); });
}

void metatable(lua_State* L, const char* type, std::initializer_list<std::pair<const char*, lua_CFunction>> methods)
{
    if (luaL_newmetatable(L, type) == 0) {
        lua_pop(L, 1); // (made already, in this VM)
        return;
    }
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

void pushMapWriter(lua_State* L, MapWriter& writer)
{
    metatable(L, kWriter,
              {{"floorIndex", wFloorIndex}, {"wallIndex", wWallIndex}, {"edgeIndex", wEdgeIndex}, {"floorName", wFloorName},
               {"isPlain", wIsPlain}, {"object", wObject}, {"bare", wBare}, {"sign", wSign}, {"exit", wExit}, {"thing", wThing},
               {"objectGroup", wObjectGroup}, {"wallGroup", wWallGroup}, {"secretWall", wSecretWall}, {"waypoint", wWaypoint},
               {"link", wLink}, {"ambient", wAmbient}, {"script", wScript}, {"walls", wWalls}, {"floors", wFloors},
               {"nextExtent", wNextExtent}});
    metatable(L, kBuilder, {{"global", sGlobal}, {"setGlobalAtLoad", sSetGlobalAtLoad}, {"fn", sFn}, {"has", sHas}});
    metatable(L, kFn,
              {{"pushInt", fPushInt}, {"pushString", fPushString}, {"pushFunction", fPushFunction}, {"object", fObject},
               {"call", fCall}, {"callScript", fCallScript}, {"op", fOp}, {"loadGlobal", fLoadGlobal}, {"setGlobal", fSetGlobal},
               {"storeGlobal", fStoreGlobal}, {"endStore", fEndStore}, {"loadLocal", fLoadLocal}, {"label", fLabel},
               {"jumpIf", fJumpIf}});
    pushBox(L, &writer, kWriter);
}

} // namespace nox::mapgen::detail
