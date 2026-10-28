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

/// An opcode's name, or nothing for one the interpreter doesn't know (it returns there).
std::optional<std::string_view> opName(std::uint32_t op);
/// Operand words after the opcode.
std::uint32_t operandCount(std::uint32_t op);
/// A builtin's name ("Builtin" + the index for one past the table).
std::string builtinName(std::uint32_t id);

/// One function's code as text, an instruction a line: "  12  Jump 30". Strings and builtins by
/// name. A malformed instruction (cut short) ends it.
std::string disassemble(const ScriptFile& file, std::size_t function);

/// What's wrong with a function's code, if anything: an unknown opcode, a jump outside it, a
/// call to a function or builtin that isn't there, a string that isn't, or code cut short.
std::optional<std::string> check(const ScriptFile& file, std::size_t function);

} // namespace nox::script
