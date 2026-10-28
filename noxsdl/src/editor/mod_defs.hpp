// The toolbox's and the UI API's declarations for the mod editor: modes/script/defs/server.d.luau and ui.d.luau, built into the program. The Luau
// diagnostics load them whole (mod_analysis.hpp); the Mod Reference panel lists what's read here:
// each `nox.*` member with its type and its one line of doc, the global functions (require), and
// the hooks a script's table may have ("--- @hook" lines).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace noxsdl::editor {

/// Which half of a mod a script is: its server scripts (server/) or its UI (ui/).
enum class ModHalf { Server, Ui };

struct ModDef {
    enum class Kind { Member, Global, Hook };
    Kind kind = Kind::Member;
    std::string section;   ///< the "--- # Section" it's under
    std::string name;      ///< "say", "require", "onJoin"
    std::string signature; ///< its type as declared ("(peer: number, text: string) -> ()"), a hook's call
    std::string doc;       ///< its one line
};

/// The entries of a definition file in the format above, in the file's order.
std::vector<ModDef> parseModDefs(std::string_view source);

/// The built-in definition file of a half (empty when the build has none), and its entries.
std::string_view modDefSource(ModHalf half);
const std::vector<ModDef>& modDefs(ModHalf half);

} // namespace noxsdl::editor
