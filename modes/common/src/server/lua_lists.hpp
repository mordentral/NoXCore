// Lists handed to a mode's script: a table from 1 of numbers or strings, made from a container in
// one call (the directors' pushes and the generator's). (Internal to the modes' sources.)
#pragma once

#include "lua.h"

#include <iterator>

namespace nox::modes::detail {

/// A list (from 1) of what `push` pushes for each of `items` (one value each), pushed.
template <class Items, class Push>
void pushList(lua_State* L, const Items& items, Push push)
{
    lua_createtable(L, static_cast<int>(std::size(items)), 0);
    int i = 0;
    for (const auto& item : items) {
        push(item);
        lua_rawseti(L, -2, ++i);
    }
}

/// A list of `items`' numbers, or of `number(item)` for each.
template <class Items>
void pushNumbers(lua_State* L, const Items& items)
{
    pushList(L, items, [L](const auto& v) { lua_pushnumber(L, v); });
}
template <class Items, class Number>
void pushNumbers(lua_State* L, const Items& items, Number number)
{
    pushList(L, items, [L, &number](const auto& v) { lua_pushnumber(L, number(v)); });
}

/// A list of `items`' strings (each with its length: a zero byte kept).
template <class Items>
void pushStrings(lua_State* L, const Items& items)
{
    pushList(L, items, [L](const auto& s) { lua_pushlstring(L, s.data(), s.size()); });
}

} // namespace nox::modes::detail
