// The NoxScript decompiler: a compiled script (script_file.hpp) back to source in the compiler's
// language (compiler.hpp). Structured statements (if / else, while, for, switch, break,
// continue, return) are recovered from the shapes the game's compiler made; a function whose
// structured form doesn't compile back to the same code is written with labels and gotos
// instead. Every result is checked by compiling it again.
#pragma once

#include "noxcore/script/compiler.hpp"
#include "noxcore/script/script_file.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nox::script {

struct DecompileOptions {
    /// Names for globals, locals and functions' arguments (the editor's sidecar); generated
    /// names otherwise.
    const DebugInfo* names = nullptr;
    /// Give back the source kept in string 0 (nsc's habit) when there is one.
    bool useEmbeddedSource = true;
};

struct DecompileResult {
    enum class Form : std::uint8_t {
        Structured, ///< compiles back to the same code
        Gotos,      ///< compiles back to the same code, written with labels and gotos
        Different,  ///< doesn't compile back to the same code (see notes)
    };
    std::string source;
    bool fromEmbeddedSource = false;
    bool compiles = false; ///< the source compiles (object variables allowed: CompileOptions::allowObjectVariables)
    /// ... to the same script, byte for byte (an embedded source: kept in string 0 again, but for
    /// what string 0 holds: the mark, or the text a migration turned round)
    bool identical = false;
    std::vector<Form> forms; ///< per function, from 2 on
    std::vector<std::string> notes;
    /// An embedded source without kEmbeddedSourceMark whose old call order (CompileOptions::
    /// legacyCallOrder) gives the script's own bytes and today's doesn't: an older NoX Core's
    /// embed (CompileOptions::embedSource). What migrateCallOrder did; `source` is its text (as kept when it
    /// Failed). Nothing otherwise (a marked embed, nsc's, one the same both ways, a decompiled one).
    std::optional<CallOrderMigration> migration;
};

DecompileResult decompile(const ScriptFile& file, const DecompileOptions& options = {});

/// The first difference between two compiled functions ("" when they're the same).
std::string compareFunctions(const Function& a, const Function& b);

} // namespace nox::script
