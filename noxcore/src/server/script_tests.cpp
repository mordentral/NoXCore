// The headless runner of a map script's Test* functions (script_tests.hpp).
#include "noxcore/server/script_tests.hpp"

#include "noxcore/script/compiler.hpp"
#include "noxcore/server/server.hpp"

#include <algorithm>
#include <memory>

namespace nox::server {

namespace {

std::unique_ptr<Server> loaded(const map::MapFile& map, std::string_view fileName, const things::ThingDb& things,
                               const ScriptTestOptions& options)
{
    Server::Options o;
    o.startWithoutPlayers = true;
    o.gameData = options.gameData;
    o.modifiers = options.modifiers;
    if (options.gameFlags != 0) {
        o.gameFlags = options.gameFlags;
    }
    auto server = std::make_unique<Server>(things, o);
    server->loadMap(map, fileName);
    server->runFrames(1); // (the map's start: MapInitialize, MapEntry)
    return server;
}

/// The script's prints since the last call, sorted into failed asserts and the rest.
void takePrints(Server& server, std::vector<std::string>& failures, std::vector<std::string>* output)
{
    for (const ScriptEvent& e : server.script()->takeEvents()) {
        if (e.kind != ScriptEvent::Kind::Print && e.kind != ScriptEvent::Kind::PrintToAll) {
            continue;
        }
        if (e.text.starts_with(script::kAssertPrefix)) {
            failures.push_back(e.text.substr(script::kAssertPrefix.size()));
        } else if (output) {
            output->push_back(e.text);
        }
    }
}

} // namespace

std::size_t ScriptTestRun::passed() const
{
    return static_cast<std::size_t>(std::ranges::count_if(tests, [](const ScriptTestResult& t) { return t.passed(); }));
}

std::size_t ScriptTestRun::failed() const
{
    return tests.size() - passed();
}

std::string ScriptTestRun::report() const
{
    if (!error.empty()) {
        return "the map didn't load: " + error + "\n";
    }
    std::string out;
    for (const auto& f : startFailures) {
        out += "FAIL (map start): " + f + "\n";
    }
    for (const auto& t : tests) {
        if (t.passed()) {
            out += "PASS " + t.function + "\n";
            continue;
        }
        out += "FAIL " + t.function + (t.stopped ? " (stopped: it ran too long)" : "") + "\n";
        for (const auto& f : t.failures) {
            out += "    " + f + "\n";
        }
    }
    out += std::to_string(passed()) + " passed, " + std::to_string(failed()) + " failed\n";
    return out;
}

ScriptTestRun runScriptTests(const map::MapFile& map, std::string_view fileName, const things::ThingDb& things, const ScriptTestOptions& options)
{
    ScriptTestRun run;
    std::vector<std::string> names;
    try {
        auto first = loaded(map, fileName, things, options);
        takePrints(*first, run.startFailures, nullptr);
        const auto& functions = first->scriptFile().functions;
        for (std::size_t i = 2; i < functions.size(); ++i) {
            if (functions[i].name.starts_with(script::kTestPrefix) && functions[i].args == 0) {
                names.push_back(functions[i].name);
            }
        }
    } catch (const std::exception& ex) {
        run.error = ex.what();
        return run;
    }
    for (const std::string& name : names) {
        ScriptTestResult t;
        t.function = name;
        auto server = loaded(map, fileName, things, options);
        ScriptHost& host = *server->script();
        host.takeEvents(); // (the start's, reported once above)
        const std::uint32_t stoppedBefore = host.stopped();
        const std::uint32_t index = *host.function(name);
        host.call(index);
        takePrints(*server, t.failures, &t.output);
        // A test that waits (Wait(frames): the compiler ends the call there with a FrameTimer back
        // to the test, which resumes after the Wait) runs on while its timer is pending; its
        // asserts count, and its other timers run meanwhile. Then the frames after, as for any.
        const auto waiting = [&] {
            return std::ranges::any_of(host.timers(), [&](const ScriptTimer& timer) { return timer.function == index; });
        };
        std::uint32_t waited = 0;
        while (waiting() && waited < options.maxWaitFrames) {
            server->runFrames(1);
            ++waited;
        }
        takePrints(*server, t.failures, &t.output);
        if (waiting()) {
            t.failures.push_back("still waiting after " + std::to_string(waited) + " frames (ScriptTestOptions::maxWaitFrames): stopped there");
        }
        server->runFrames(options.framesAfter);
        takePrints(*server, t.failures, &t.output);
        t.stopped = host.stopped() != stoppedBefore;
        run.tests.push_back(std::move(t));
    }
    return run;
}

} // namespace nox::server
