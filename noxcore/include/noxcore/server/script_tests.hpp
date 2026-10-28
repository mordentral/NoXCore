// The headless runner of a map script's tests (NoX Core's): each
// function named Test* (void, no arguments) run in our server on the map, with no players. A
// test fails where its asserts print: the compiler's assert(cond, "msg") lowers to plain stock
// code, `if (!cond) PrintToAll("ASSERT line N: msg")`, so a map with tests still loads and runs
// in GAME.EXE (where the prints would just show).
//
// Each test runs on a fresh load of the map, after its start (GLOBAL, MapInitialize and MapEntry,
// as startWithoutPlayers runs them), then `framesAfter` frames so its timers can fire (their
// asserts count too). Deterministic: the same map gives the same results.
//
// `Wait(frames);` in a test (the compiler's, test functions only) ends the call there with a
// FrameTimer back to the test, whose prologue resumes after the Wait (its locals persist, as
// every function's do). The runner runs frames while that timer is pending (at most
// `maxWaitFrames`), so code after a Wait runs `frames` frames later, the test's other timers
// firing meanwhile, and its asserts count; then `framesAfter` frames.
#pragma once

#include "noxcore/map/map_file.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nox::things {
struct ThingDb;
struct GameData;
struct ModifierDb;
} // namespace nox::things

namespace nox::server {

struct ScriptTestResult {
    std::string function;
    std::vector<std::string> failures; ///< each failed assert's "line N: message"
    std::vector<std::string> output;   ///< the test's other prints (Print, PrintToAll)
    bool stopped = false;              ///< the VM's step budget stopped it (a hang)
    bool passed() const { return failures.empty() && !stopped; }
};

struct ScriptTestRun {
    std::vector<ScriptTestResult> tests;
    /// The map's start's own failed asserts (MapInitialize...), from the first load.
    std::vector<std::string> startFailures;
    std::string error; ///< the map didn't load (nothing ran)

    std::size_t passed() const;
    std::size_t failed() const;
    /// A line per test ("PASS TestDoors", "FAIL TestTraps: line 12: ..."), then a total.
    std::string report() const;
};

struct ScriptTestOptions {
    std::uint32_t framesAfter = 30; ///< frames run after each test (its timers)
    /// The most frames a test may spend in its Waits (5 minutes of game time); past it the test
    /// fails, "still waiting".
    std::uint32_t maxWaitFrames = 30 * 60 * 5;
    const things::GameData* gameData = nullptr;
    const things::ModifierDb* modifiers = nullptr;
    std::uint32_t gameFlags = 0; ///< the mode (0: the server's default, an arena)
};

/// Runs every Test* function of the map's script. `fileName` is the map's ("Name.map").
ScriptTestRun runScriptTests(const map::MapFile& map, std::string_view fileName, const things::ThingDb& things,
                             const ScriptTestOptions& options = {});

} // namespace nox::server
