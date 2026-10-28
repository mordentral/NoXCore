// NoxScript's language tables, one copy for the compiler (compiler.cpp), which reads the words
// and turns the operators into instructions, and the decompiler (decompiler.cpp), which keeps its
// names off the words and turns the instructions back into operators.
#pragma once

#include "noxcore/script/opcodes.hpp"

#include <string_view>

namespace nox::script::detail {

/// The words no variable or function may be named.
inline constexpr std::string_view kKeywords[] = {"if",     "else",     "while",  "for",  "switch", "case",  "default",
                                                 "break",  "continue", "return", "goto", "int",    "float", "string",
                                                 "object", "void",     "__goto_if", "__goto_ifnot", "do"};

/// The language's own globals, GLOBAL's first four symbols (ScriptHost::setLanguageGlobals: -2
/// the trigger, -1 the caller, 1, 0).
inline constexpr std::string_view kLanguageGlobals[] = {"self", "other", "true", "false"};

/// Op::Return in an operator's row: no instruction for that type.
inline constexpr Op kNoOp = Op::Return;

/// A binary operator: its text, its precedence (C's, loosest 1), its instruction for ints,
/// floats and strings, and whether it compares (its result an int whatever it compares).
struct BinaryOperator {
    std::string_view text;
    int precedence;
    Op i, f, s;
    bool compare;
};
inline constexpr BinaryOperator kBinaryOperators[] = {
    {"+", 9, Op::IntAdd, Op::FloatAdd, Op::StringAdd, false},
    {"-", 9, Op::IntSub, Op::FloatSub, kNoOp, false},
    {"*", 10, Op::IntMul, Op::FloatMul, kNoOp, false},
    {"/", 10, Op::IntDiv, Op::FloatDiv, kNoOp, false},
    {"%", 10, Op::IntMod, kNoOp, kNoOp, false},
    {"&", 5, Op::IntAnd, kNoOp, kNoOp, false},
    {"|", 3, Op::IntOr, kNoOp, kNoOp, false},
    {"^", 4, Op::IntXor, kNoOp, kNoOp, false},
    {"<<", 8, Op::IntLsh, kNoOp, kNoOp, false},
    {">>", 8, Op::IntRsh, kNoOp, kNoOp, false},
    {"&&", 2, Op::BoolAnd, kNoOp, kNoOp, false},
    {"||", 1, Op::BoolOr, kNoOp, kNoOp, false},
    {"==", 6, Op::IntEq, Op::FloatEq, Op::StringEq, true},
    {"!=", 6, Op::IntNeq, Op::FloatNeq, Op::StringNeq, true},
    {"<", 7, Op::IntLt, Op::FloatLt, Op::StringLt, true},
    {">", 7, Op::IntGt, Op::FloatGt, Op::StringGt, true},
    {"<=", 7, Op::IntLte, Op::FloatLte, Op::StringLte, true},
    {">=", 7, Op::IntGte, Op::FloatGte, Op::StringGte, true},
};

/// An assignment: its text and its store instruction for ints, floats and strings.
struct AssignOperator {
    std::string_view text;
    Op i, f, s;
};
inline constexpr AssignOperator kAssignOperators[] = {
    {"=", Op::StoreInt, Op::StoreFloat, Op::StoreString},
    {"+=", Op::StoreIntAdd, Op::StoreFloatAdd, Op::StoreStringAdd},
    {"-=", Op::StoreIntSub, Op::StoreFloatSub, kNoOp},
    {"*=", Op::StoreIntMul, Op::StoreFloatMul, kNoOp},
    {"/=", Op::StoreIntDiv, Op::StoreFloatDiv, kNoOp},
    {"%=", Op::StoreIntMod, kNoOp, kNoOp},
    {"&=", Op::StoreIntAnd, kNoOp, kNoOp},
    {"|=", Op::StoreIntOr, kNoOp, kNoOp},
    {"^=", Op::StoreIntXor, kNoOp, kNoOp},
    {"<<=", Op::StoreIntLsh, kNoOp, kNoOp},
    {">>=", Op::StoreIntRsh, kNoOp, kNoOp},
};

/// The binary operator written `text`, or nullptr.
constexpr const BinaryOperator* binaryOperator(std::string_view text)
{
    for (const BinaryOperator& o : kBinaryOperators) {
        if (o.text == text) {
            return &o;
        }
    }
    return nullptr;
}

/// The assignment written `text`, or nullptr.
constexpr const AssignOperator* assignOperator(std::string_view text)
{
    for (const AssignOperator& o : kAssignOperators) {
        if (o.text == text) {
            return &o;
        }
    }
    return nullptr;
}

/// The text of the binary operator `op` is an instruction of, or "?" for none.
constexpr std::string_view binaryText(Op op)
{
    for (const BinaryOperator& o : kBinaryOperators) {
        if (op == o.i || (op != kNoOp && (op == o.f || op == o.s))) {
            return o.text;
        }
    }
    return "?";
}

/// The text of the assignment `op` is a store of, or "?" for none.
constexpr std::string_view storeText(Op op)
{
    for (const AssignOperator& o : kAssignOperators) {
        if (op == o.i || (op != kNoOp && (op == o.f || op == o.s))) {
            return o.text;
        }
    }
    return "?";
}

} // namespace nox::script::detail
