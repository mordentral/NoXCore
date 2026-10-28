// Luau values as bytes (value_codec.hpp).
#include "value_codec.hpp"

#include "noxcore/modes/script/ui_proto.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>

namespace nox::modes::script {

namespace {

static_assert(CodecLimits{}.bytes == uimsg::kMaxValue, "the UI's limits are the default");

void putU16(std::vector<std::uint8_t>& out, std::size_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

bool encodeAt(lua_State* L, int idx, std::vector<std::uint8_t>& out, std::string& error, int depth, const CodecLimits& lim)
{
    if (out.size() > lim.bytes) {
        error = "the value is over " + std::to_string(lim.bytes) + " bytes";
        return false;
    }
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out.push_back(0); return true;
    case LUA_TBOOLEAN: out.push_back(lua_toboolean(L, idx) ? 2 : 1); return true;
    case LUA_TNUMBER: {
        out.push_back(3);
        const double d = lua_tonumber(L, idx);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof bits);
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
        }
        return true;
    }
    case LUA_TSTRING: {
        size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        if (n > 0xFFFF) {
            error = "a string over 64 KB";
            return false;
        }
        out.push_back(4);
        putU16(out, n);
        out.insert(out.end(), s, s + n);
        return true;
    }
    case LUA_TTABLE: {
        if (depth >= lim.depth) {
            error = "tables nested over " + std::to_string(lim.depth) + " deep (or a cycle)";
            return false;
        }
        if (!lua_checkstack(L, 3)) {
            error = "tables nested too deep for the stack";
            return false;
        }
        out.push_back(5);
        const std::size_t countAt = out.size();
        putU16(out, 0);
        int pairs = 0;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            const int kt = lua_type(L, -2);
            if (kt != LUA_TSTRING && kt != LUA_TNUMBER) {
                lua_pop(L, 2);
                error = "a table key that isn't a string or a number";
                return false;
            }
            if (++pairs > lim.pairs) {
                lua_pop(L, 2);
                error = "a table of over " + std::to_string(lim.pairs) + " entries";
                return false;
            }
            if (!encodeAt(L, -2, out, error, depth + 1, lim) || !encodeAt(L, -1, out, error, depth + 1, lim)) {
                lua_pop(L, 2);
                return false;
            }
            lua_pop(L, 1); // (the value; the key stays for lua_next)
        }
        out[countAt] = static_cast<std::uint8_t>(pairs);
        out[countAt + 1] = static_cast<std::uint8_t>(pairs >> 8);
        return true;
    }
    default:
        error = std::string("a ") + lua_typename(L, lua_type(L, idx)) + " can't be sent";
        return false;
    }
}

struct In {
    std::span<const std::uint8_t> d;
    std::size_t at = 0;
};

bool decodeAt(lua_State* L, In& in, int depth, const CodecLimits& lim)
{
    if (in.at >= in.d.size() || !lua_checkstack(L, 4)) {
        return false;
    }
    switch (in.d[in.at++]) {
    case 0: lua_pushnil(L); return true;
    case 1: lua_pushboolean(L, 0); return true;
    case 2: lua_pushboolean(L, 1); return true;
    case 3: {
        if (in.at + 8 > in.d.size()) {
            return false;
        }
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits |= static_cast<std::uint64_t>(in.d[in.at + static_cast<std::size_t>(i)]) << (8 * i);
        }
        in.at += 8;
        double v = 0;
        std::memcpy(&v, &bits, sizeof v);
        lua_pushnumber(L, v);
        return true;
    }
    case 4: {
        if (in.at + 2 > in.d.size()) {
            return false;
        }
        const std::size_t n = in.d[in.at] | static_cast<std::size_t>(in.d[in.at + 1]) << 8;
        in.at += 2;
        if (in.at + n > in.d.size()) {
            return false;
        }
        lua_pushlstring(L, reinterpret_cast<const char*>(in.d.data() + in.at), n);
        in.at += n;
        return true;
    }
    case 5: {
        if (depth >= lim.depth || in.at + 2 > in.d.size()) {
            return false;
        }
        const int pairs = in.d[in.at] | in.d[in.at + 1] << 8;
        in.at += 2;
        if (pairs > lim.pairs) {
            return false;
        }
        lua_createtable(L, 0, pairs);
        for (int k = 0; k < pairs; ++k) {
            if (!decodeAt(L, in, depth + 1, lim)) {
                lua_pop(L, 1);
                return false;
            }
            const int kt = lua_type(L, -1);
            if ((kt != LUA_TSTRING && kt != LUA_TNUMBER) || (kt == LUA_TNUMBER && std::isnan(lua_tonumber(L, -1)))) {
                lua_pop(L, 2);
                return false;
            }
            if (!decodeAt(L, in, depth + 1, lim)) {
                lua_pop(L, 2);
                return false;
            }
            lua_rawset(L, -3);
        }
        return true;
    }
    default: return false;
    }
}

} // namespace

bool encodeValue(lua_State* L, int idx, std::vector<std::uint8_t>& out, std::string& error, const CodecLimits& limits)
{
    const std::size_t start = out.size();
    if (!encodeAt(L, idx, out, error, 0, limits)) {
        out.resize(start);
        return false;
    }
    if (out.size() - start > limits.bytes) {
        out.resize(start);
        error = "the value is over " + std::to_string(limits.bytes) + " bytes";
        return false;
    }
    return true;
}

bool decodeValue(lua_State* L, std::span<const std::uint8_t> bytes, const CodecLimits& limits)
{
    In in{bytes};
    const int top = lua_gettop(L);
    if (!decodeAt(L, in, 0, limits) || in.at != bytes.size()) {
        lua_settop(L, top);
        return false;
    }
    return true;
}

} // namespace nox::modes::script
