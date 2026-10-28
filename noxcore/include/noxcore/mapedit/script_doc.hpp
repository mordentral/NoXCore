// The map editor's script, on the document's side (headless, so it is tested): the NoxScript
// source and its debug info in the sidecar (never in the .map), the script handlers the map names,
// compiling for the map (with the lints and the map's own checks) and decompiling a stock map's
// script on first open.
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/sidecar.hpp"
#include "noxcore/script/arg_values.hpp"
#include "noxcore/script/compiler.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::mapedit {

/// The sidecar's keys (Sidecar::other) for the source (a JSON string) and the debug info of its
/// last compile (DebugInfo::toJson).
constexpr std::string_view kSidecarScriptSource = "scriptSource";
constexpr std::string_view kSidecarScriptDebug = "scriptDebug";

std::optional<std::string> sidecarScriptSource(const Sidecar& side);
std::optional<script::DebugInfo> sidecarScriptDebug(const Sidecar& side);
/// Stores the source (none: the key removed) and the debug info (null: removed).
void storeScriptInSidecar(Sidecar& side, const std::optional<std::string>& source, const script::DebugInfo* debug);

/// One script handler field on the map (an object's or a polygon's), and the function it names.
struct HandlerRef {
    enum class Owner : std::uint8_t { Object, Polygon };
    Owner owner = Owner::Object;
    ObjId object = 0;        ///< (Owner::Object)
    std::size_t polygon = 0; ///< (Owner::Polygon) its index in the list
    std::string field;       ///< "Pressed", "Enemy sighted", "Enter"...
    std::string function;    ///< empty: none
    /// "Door (extent 12): Pressed", "polygon 3 (Hall): Enter".
    std::string where(const MapDocument& doc) const;
};

/// Every script handler field of the map's objects (inventories included) and polygons, with or
/// without a function, in document order.
std::vector<HandlerRef> handlerRefs(const MapDocument& doc);
/// The functions the map's handlers name, each once, sorted (CompileOptions::externalNames).
std::vector<std::string> handlerNames(const MapDocument& doc);
/// An object's handler fields by label, for the properties panel's function pickers (none for
/// a type without any). The pointers are into `object`.
std::vector<std::pair<std::string, map::ScriptHandler*>> objectHandlers(DocObject& object);

/// A stock map's script as source, for the panel's first open (names from `names`, a sidecar's
/// debug info, when given).
struct DecompiledSource {
    std::string source;
    bool identical = false; ///< it compiles back to the map's own bytes
    std::vector<std::string> notes;
};
DecompiledSource decompileDocumentScript(const MapDocument& doc, const script::DebugInfo* names = nullptr);

/// The map's own value sets (script/arg_values.hpp) put into `sets`: its objects' script names,
/// its waypoints' names and its object, waypoint and wall groups' names, each with a note (an
/// object's type, a waypoint's place, a group's size). `things` (may be null) gives the objects
/// their types' icons.
void addMapArgValues(script::ArgValueSets& sets, const MapDocument& doc, const things::ThingDb* things = nullptr);

/// Compiles `source` for this map: the lints on (the map's handlers as the external names; the
/// builtins' string literals against their value sets: `gameValues`' sets, from the game's data,
/// when given, else the built-in ones (effects, spells, enchantments, classes, dialog types,
/// sounds), and the map's own names always, so an Object / Waypoint / ObjectGroup /
/// WaypointGroup / WallGroup call whose literal name nothing on the map has is warned), then the
/// map's own check as a warning: a handler naming a function the script doesn't define (at line
/// 0: the map). `allowObjectVariables`: the editor's setting for NoX Core-only script features
/// (CompileOptions::allowObjectVariables).
script::CompileResult compileForDocument(const MapDocument& doc, std::string_view source, bool allowObjectVariables = false,
                                         const script::ArgValueSets* gameValues = nullptr);

/// The sidecar's key recording that the map needs a NoX Core server (a JSON string: why). Written
/// on every save from the map's compiled script (markNoxCoreNeeds); the .map itself carries
/// nothing the original game can't read.
constexpr std::string_view kSidecarNeedsNoxCore = "needsNoxCore";
/// Why the map needs a NoX Core server (its compiled script uses object variables, which the
/// original game crashes on: OB-210), or nothing when the original game can run it.
std::optional<std::string> documentNeedsNoxCore(const MapDocument& doc);
/// The sidecar's record of it (kSidecarNeedsNoxCore), if any.
std::optional<std::string> sidecarNeedsNoxCore(const Sidecar& side);
/// Sets or clears the sidecar's record from the map's compiled script (documentNeedsNoxCore).
void markNoxCoreNeeds(Sidecar& side, const MapDocument& doc);

/// Why the Script panel shows this map's source read-only: the map's compiled script already uses
/// object variables (a custom map's) and the editor's setting doesn't allow them, so it can't be
/// compiled again; the map's script stays as it is (never stripped). Nothing when editable.
std::optional<std::string> scriptReadOnlyReason(const MapDocument& doc, bool allowObjectVariables);

/// The op that sets the map's compiled script to `file` ("Compile script"); empty when the map
/// already holds those bytes.
Op compiledScriptOp(const MapDocument& doc, const script::ScriptFile& file);

} // namespace nox::mapedit
