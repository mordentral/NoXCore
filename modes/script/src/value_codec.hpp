// A Luau value as bytes and back, for what a script mode's server and its UI send each other
// (ui_proto.hpp's data, views and sends): nil, booleans, numbers, strings and tables of them.
// (Internal to the script modes' sources.)
//
//   0 nil   1 false   2 true   3 number (f64)   4 string (u16 length, bytes)
//   5 table (u16 pairs, then each key and value; keys strings or numbers)
// The UI's limits (the default): at most 8 tables deep, 1024 pairs a table, uimsg::kMaxValue
// bytes in all (one reliable message); anything else (a function, userdata, a cycle) is refused
// with why. The mod store (mod_store.hpp) uses the same bytes with bigger limits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct lua_State;

namespace nox::modes::script {

struct CodecLimits {
    int depth = 8;          ///< tables nested at most this deep
    int pairs = 1024;       ///< pairs a table (at most 65535: the count is a u16)
    std::size_t bytes = 900; ///< the whole value's bytes (uimsg::kMaxValue)
};

/// The value at `idx` encoded onto `out`; false: why in `error`.
bool encodeValue(lua_State* L, int idx, std::vector<std::uint8_t>& out, std::string& error, const CodecLimits& limits = {});
/// The value `in` holds pushed; false: nothing pushed (it isn't one).
bool decodeValue(lua_State* L, std::span<const std::uint8_t> in, const CodecLimits& limits = {});

} // namespace nox::modes::script
