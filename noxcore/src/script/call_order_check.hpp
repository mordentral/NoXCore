// migrateCallOrder with its check given by the caller (compiler.cpp's own, and the decompiler's
// for a source kept in string 0, which has compiled the old source already).
#pragma once

#include "noxcore/script/compiler.hpp"

#include <functional>
#include <string_view>

namespace nox::script::detail {

/// How a migration checks the turned text against the old source: whether the old compiler
/// compiles the old source (asked once, and only when there is a call to turn), then today's
/// compile of the turned text with `afterOptions`, and whether that gives the old one's bytes.
struct CallOrderCheck {
    std::function<bool()> beforeCompiles;
    CompileOptions afterOptions;
    std::function<bool(const ScriptFile&)> same;
    /// (When given: the turned text's compile, kept for the caller; left as it was when nothing
    /// was turned or the old source doesn't compile.)
    CompileResult* after = nullptr;
};

/// script::migrateCallOrder, checked as `check` says (migrateCallOrder(source) is this with the
/// old source compiled by the old order and the turned text by today's, both bytes compared).
CallOrderMigration migrateCallOrder(std::string_view oldSource, const CallOrderCheck& check);

} // namespace nox::script::detail
