// NoxScript's instructions and builtins, as the game's interpreter (script_callByIndex)
// runs them. An instruction is a u32 opcode and its operands, u32 words too. Names follow the
// community's NoxScript tools (noxworld-dev/noxscript ns/asm); what each does is from GAME.EXE.
#pragma once

#include "noxcore/script/script_file.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nox::script {

enum class Op : std::uint32_t {
    LoadVarInt = 0x00,    ///< global?, var: push its value
    LoadVarFloat = 0x01,
    LoadVarPtr = 0x02,    ///< global?, var: push [size if an array], global?, offset (negated for an
                          ///< object variable)
    LoadVarString = 0x03,
    PushInt = 0x04,       ///< value
    PushFloat = 0x05,     ///< value
    PushString = 0x06,    ///< string id
    IntAdd = 0x07,
    FloatAdd = 0x08,
    IntSub = 0x09,
    FloatSub = 0x0A,
    IntMul = 0x0B,
    FloatMul = 0x0C,
    IntDiv = 0x0D,
    FloatDiv = 0x0E,
    IntMod = 0x0F,
    IntAnd = 0x10,
    IntOr = 0x11,
    IntXor = 0x12,
    Jump = 0x13,          ///< word index in the function's code
    JumpIf = 0x14,
    JumpIfNot = 0x15,
    StoreInt = 0x16,      ///< (a LoadVarPtr's global?, offset), value: store it, push it
    StoreFloat = 0x17,
    StoreString = 0x18,
    StoreIntMul = 0x19,
    StoreFloatMul = 0x1A,
    StoreIntDiv = 0x1B,
    StoreFloatDiv = 0x1C,
    StoreIntAdd = 0x1D,
    StoreFloatAdd = 0x1E,
    StoreStringAdd = 0x1F,
    StoreIntSub = 0x20,
    StoreFloatSub = 0x21,
    StoreIntMod = 0x22,
    IntEq = 0x23,
    FloatEq = 0x24,
    StringEq = 0x25,
    IntLsh = 0x26,
    IntRsh = 0x27,
    IntLt = 0x28,
    FloatLt = 0x29,
    StringLt = 0x2A,
    IntGt = 0x2B,
    FloatGt = 0x2C,
    StringGt = 0x2D,
    IntLte = 0x2E,
    FloatLte = 0x2F,
    StringLte = 0x30,
    IntGte = 0x31,
    FloatGte = 0x32,
    StringGte = 0x33,
    IntNeq = 0x34,
    FloatNeq = 0x35,
    StringNeq = 0x36,
    BoolAnd = 0x37,
    BoolOr = 0x38,
    StoreIntLsh = 0x39,
    StoreIntRsh = 0x3A,
    StoreIntAnd = 0x3B,
    StoreIntOr = 0x3C,
    StoreIntXor = 0x3D,
    BoolNot = 0x3E,
    IntNot = 0x3F,
    IntNeg = 0x40,
    FloatNeg = 0x41,
    IndexInt = 0x42,      ///< size, global?, offset, index: push the element (out of bounds: and return)
    IndexFloat = 0x43,    ///< the same, but an index equal to the size passes the check
    IndexPtr = 0x44,      ///< size, global?, offset, index: push global?, the element's offset (an index
                          ///< equal to the size passes the check, as IndexFloat's)
    CallBuiltin = 0x45,   ///< builtin id
    CallScript = 0x46,    ///< function index
    Return0 = 0x47,
    Return = 0x48,
    StringAdd = 0x49,
};

constexpr std::uint32_t kOpCount = 0x4A;
/// Builtins in the game's table.
constexpr std::uint32_t kBuiltinCount = 211;

// ---- NoX Core's own builtins (maps made for a NoX Core mod; GAME.EXE's table ends at 210 and the
// game faults on any id past it). They have a range of their own, [kNoxBuiltinFirst,
// kNoxBuiltinEnd): far past the game's table, past the ids the Unused builtins and any fan build
// might take, and kept apart from them. Each id there says what it pops and pushes: bit 0 set when
// it pushes a result, bits 1-3 its argument count, the rest (from bit 4) which builtin it is. So a
// server that doesn't know one (a map made for a newer NoX Core) still pops its arguments and
// pushes a 0 for its result, the stack as the map expects it.
constexpr std::uint32_t kNoxBuiltinFirst = 1024;
constexpr std::uint32_t kNoxBuiltinEnd = 2048;
/// NoX Core builtin `n` (0-63) taking `args` (0-7) arguments, with a result or not.
constexpr std::uint32_t noxBuiltinId(std::uint32_t n, std::uint32_t args, bool returns) noexcept
{
    return kNoxBuiltinFirst + n * 16 + args * 2 + (returns ? 1u : 0u);
}
/// Whether `id` is in NoX Core's range (one this build has or not).
constexpr bool inNoxBuiltinRange(std::uint32_t id) noexcept { return id >= kNoxBuiltinFirst && id < kNoxBuiltinEnd; }
/// A NoX Core range id's argument count, and whether it pushes a result.
constexpr int noxBuiltinArgs(std::uint32_t id) noexcept { return static_cast<int>((id >> 1) & 7u); }
constexpr bool noxBuiltinReturns(std::uint32_t id) noexcept { return (id & 1u) != 0; }
/// LuauCall(string name, int a, int b, int c, int d): the hosting mod's handler for `name`
/// (nox.onMapCall), its result dropped. LuauCallReturn: the same, its result pushed.
constexpr std::uint32_t kLuauCall = noxBuiltinId(0, 5, false);      // 1034
constexpr std::uint32_t kLuauCallReturn = noxBuiltinId(1, 5, true); // 1051
/// NoX Core's builtins this build has, in order.
constexpr std::uint32_t kNoxBuiltinCount = 2;
constexpr std::uint32_t kNoxBuiltinIds[kNoxBuiltinCount] = {kLuauCall, kLuauCallReturn};
/// Their names, in the same order (builtinName; builtins.cpp's signatures).
constexpr std::string_view kNoxBuiltinNames[kNoxBuiltinCount] = {"LuauCall", "LuauCallReturn"};
/// Whether `id` is one of NoX Core's builtins this build has.
constexpr bool isNoxBuiltin(std::uint32_t id) noexcept { return id == kLuauCall || id == kLuauCallReturn; }
/// Whether `id` is a builtin this build has: the game's or NoX Core's.
constexpr bool isKnownBuiltin(std::uint32_t id) noexcept { return id < kBuiltinCount || isNoxBuiltin(id); }
/// Every builtin this build has in one run (for lists: the game's 0-210, then NoX Core's), by
/// place: a builtin's id by its place, and its place by its id (kAllBuiltinCount: none).
constexpr std::uint32_t kAllBuiltinCount = kBuiltinCount + kNoxBuiltinCount;
constexpr std::uint32_t builtinIdAt(std::uint32_t place) noexcept
{
    return place < kBuiltinCount ? place : kNoxBuiltinIds[place - kBuiltinCount];
}
constexpr std::uint32_t builtinPlace(std::uint32_t id) noexcept
{
    if (id < kBuiltinCount) {
        return id;
    }
    for (std::uint32_t i = 0; i < kNoxBuiltinCount; ++i) {
        if (kNoxBuiltinIds[i] == id) {
            return kBuiltinCount + i;
        }
    }
    return kAllBuiltinCount;
}

/// An opcode's name, or nothing for one the interpreter doesn't know (it returns there).
std::optional<std::string_view> opName(std::uint32_t op);
/// Operand words after the opcode.
std::uint32_t operandCount(std::uint32_t op);
/// A builtin's name ("Builtin" + the id for one this build doesn't have).
std::string builtinName(std::uint32_t id);

/// One function's code as text, an instruction a line: "  12  Jump 30". Strings and builtins by
/// name. A malformed instruction (cut short) ends it.
std::string disassemble(const ScriptFile& file, std::size_t function);

/// What's wrong with a function's code, if anything: an unknown opcode, a jump outside it, a
/// call to a function or builtin that isn't there (NoX Core's own are there), a string that isn't,
/// or code cut short.
std::optional<std::string> check(const ScriptFile& file, std::size_t function);

/// The first builtin id a script calls that is neither the game's (0-210) nor in NoX Core's range
/// (kNoxBuiltinFirst to kNoxBuiltinEnd), if any: a script made for the original game's memory
/// tricks (an id far past the table to run its own code, extra builtins to read and write raw
/// memory). NoX Core never runs such a script: it would hang or run on garbage here, as nothing is
/// popped for such an id. One walk of the code, made once at a map's load.
std::optional<std::uint32_t> memoryTrickBuiltin(const ScriptFile& file);
inline bool usesMemoryTricks(const ScriptFile& file) { return memoryTrickBuiltin(file).has_value(); }
/// The refusal's words for map `map` (English: the server sends them to stock clients too; noxsdl
/// shows its own translation, "script.memory_tricks").
std::string memoryTricksNotice(std::string_view map);

} // namespace nox::script
