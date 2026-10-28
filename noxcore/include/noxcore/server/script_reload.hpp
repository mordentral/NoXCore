// Hot reload of a running map script: NoX Core's map editor only. GAME.EXE has nothing like it, and
// no game ever calls it: only the editor's in-process play-test (noxsdl's editor/script_panel) and
// the tests do, between frames, with the server's lock held. Normal play never reaches this file
// (Server and ScriptHost only befriend it), so the VM and the host behave exactly as the exe's.
//
// Two modes:
//   hotReload       the new compiled script swapped in with the game's state kept, matched by
//                   name through the old and the new debug info (compiler.hpp DebugInfo):
//                   - globals by (name, type, size): their values KEPT;
//                     new ones (or ones whose type changed) get their initialisers, run alone
//                     from the new GLOBAL's code (Vm::callRange over DebugInfo::globalInits);
//                     removed ones are dropped;
//                   - persistent locals by (function, name, type, size);
//                   - object variables by (name, type, size), on every object: values kept;
//                     new ones (or retyped / resized) 0 on every object, removed ones dropped;
//                   - string values by text: a constant's id to the new script's constant with
//                     that text (its n-th occurrence for the n-th), run-time strings carried over
//                     in order (their ids moved by the change in the constants' count);
//                   - function indices by name: the timers (one whose function was deleted is
//                     cancelled and reported), the monsters' dialog start and end (cleared and
//                     reported when gone); handlers are names already (one naming a function the
//                     script no longer has is reported; its calls are skipped, as for any unknown
//                     name);
//                   - the VM's stack (between frames: what timers and entry points left) and
//                     everything else the host holds (timers' args, quest status, the waiting
//                     handlers) stay as they are.
//   restartScripts  the new script with GLOBAL, MapInitialize and MapEntry run again on the live
//                   world: objects stay, timers are cleared (dialog indices remapped by name).
// ("Restart map", a fresh load of the edited map, is the play-test's own restart.)
//
// Everything here is deterministic: the same game, scripts and debug info give the same result,
// notes in the same order.
#pragma once

#include "noxcore/script/compiler.hpp"
#include "noxcore/script/script_file.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nox::server {

class Server;

namespace dev {

struct ReloadNote {
    enum class Severity : std::uint8_t { Info, Warning };
    Severity severity = Severity::Info;
    std::string message;
    script::SourceLoc loc; ///< in the new source where known (line 0: none)
};

struct ReloadReport {
    bool applied = false; ///< false: nothing changed (the notes say why)
    std::uint32_t globalsKept = 0;
    std::uint32_t globalsNew = 0;     ///< new or retyped: initialised afresh
    std::uint32_t globalsDropped = 0;
    std::uint32_t initialisersRun = 0;
    std::uint32_t localsKept = 0;
    std::uint32_t localsReset = 0;    ///< a function's local whose type or size changed
    std::uint32_t stringsRemapped = 0; ///< string values whose id changed
    std::uint32_t timersRemapped = 0;
    std::uint32_t timersCancelled = 0;
    std::uint32_t dialogsRemapped = 0;
    std::uint32_t dialogsCleared = 0;
    std::uint32_t handlersMissing = 0; ///< handlers naming a function the script doesn't have
    // Object variables (`object.name`), by (name, type, size); counts of variables, each applied
    // to every object that has its words.
    std::uint32_t objectVarsKept = 0;
    std::uint32_t objectVarsNew = 0;     ///< 0 on every object
    std::uint32_t objectVarsReset = 0;   ///< type or size changed: 0 again on every object
    std::uint32_t objectVarsDropped = 0;
    std::uint32_t objectsWithVars = 0;   ///< objects whose words were laid out again
    std::vector<ReloadNote> notes;

    /// One line: what was kept, made new, remapped and lost.
    std::string summary() const;
};

/// Whether `debug` describes `file`: the same functions by index and name, GLOBAL's symbols,
/// function 0's (the object variables).
bool debugMatches(const script::ScriptFile& file, const script::DebugInfo& debug);

/// The hot mode (above). `oldDebug` describes the running script (for a stock map's first
/// reload, the debug info of its decompiled source compiled, which is the same bytes), `newDebug`
/// the new one's. Refused (nothing changed) when the server has no script host or either debug
/// info doesn't match its script.
ReloadReport hotReload(Server& server, script::ScriptFile newFile, const script::DebugInfo& oldDebug, const script::DebugInfo& newDebug);

/// The restart mode (above).
ReloadReport restartScripts(Server& server, script::ScriptFile newFile);

} // namespace dev
} // namespace nox::server
