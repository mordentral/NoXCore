// The NoxScript builtins as the source language sees them: each one's name, argument types and
// result, by its index in the game's builtins table (CallBuiltin's operand). The names are
// opcodes.hpp's; the argument counts and results agree with the server host's table
// (script_host.cpp), and the types come from the host's pops and the game's own builtins.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::script {

/// A value's type in the source language. `Object` is the opaque handle type of waypoints,
/// groups and walls (game objects are plain ints); `Function` is a builtin argument taking a
/// script function (its index).
enum class Type : std::uint8_t { Void, Int, Float, String, Object, Function };

std::string_view typeName(Type t);

struct BuiltinSignature {
    std::string_view name;
    Type returns = Type::Void;
    std::vector<Type> args; ///< in source order (the first pushed first)
};

/// Builtin `id`'s signature (id < kBuiltinCount).
const BuiltinSignature& builtinSignature(std::uint32_t id);
/// A builtin's id by its source name, or by the old name of one NoX Core renamed (the
/// community tools' "Unknownb8" for IsFriendly, say: community scripts keep compiling).
std::optional<std::uint32_t> findBuiltin(std::string_view name);
/// The old name of a builtin NoX Core renamed (Unknownb8, Unknownb9, Unknownc4), if it has one.
std::optional<std::string_view> builtinAlias(std::uint32_t id);
/// A name NoX Core gave a builtin the community tools had no name for: a script's own function
/// or variable of that name is allowed and wins over the builtin (scripts written before the
/// name existed keep compiling); the builtin stays reachable by its old name.
bool isNewBuiltinName(std::string_view name);
/// Every builtin as a prototype line ("int Object(string)"), for the editor's reference.
std::string builtinPrototypes();

} // namespace nox::script
