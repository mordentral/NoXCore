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
    bool identical = false; ///< ... to the same script, byte for byte
    std::vector<Form> forms; ///< per function, from 2 on
    std::vector<std::string> notes;
};

DecompileResult decompile(const ScriptFile& file, const DecompileOptions& options = {});

/// The first difference between two compiled functions ("" when they're the same).
std::string compareFunctions(const Function& a, const Function& b);

} // namespace nox::script
