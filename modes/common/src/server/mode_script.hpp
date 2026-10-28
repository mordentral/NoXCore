// A built-in mode's own script (its folder's script/server/main.luau, built into the program) as a
// script mode its director owns: the Delve's and the Tide's. Its rules the director calls, nothing
// on the host (no engine hooks, nothing in a host's options). (Internal to noxcore's server
// sources.)
#pragma once

#include "noxcore/modes/script/builtin_scripts.hpp"
#include "noxcore/modes/script/scripted_mode.hpp"
#include "noxcore/server/game_host.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace nox::server::detail {

/// Mode `id`'s script (`name` its manifest's), its budget `limits`: `owner` adds the director's
/// functions before its set-up; `override` (tests) gives a file in place of the built-in one, by
/// its path ("server/level.luau"; nothing: the built-in's).
inline std::unique_ptr<modes::script::ScriptedMode> makeBuiltinScript(std::string id, std::string name, const things::ThingDb* things,
                                                                      const modes::script::LuauVm::Limits& limits,
                                                                      const std::function<void(modes::script::ScriptedMode&)>& owner,
                                                                      std::function<std::optional<std::string>(std::string_view)> override = {})
{
    auto package = std::make_shared<modes::script::ModPackage>();
    package->manifest.info.id = id;
    package->manifest.info.name = std::move(name);
    package->source = [id = std::move(id), override = std::move(override)](std::string_view path) -> std::optional<std::string> {
        if (override) {
            if (auto s = override(path)) {
                return s; // (a test's)
            }
        }
        const auto s = modes::script::builtinScript(id, path);
        return s ? std::optional<std::string>(std::string(*s)) : std::nullopt;
    };
    auto script = std::make_unique<modes::script::ScriptedMode>(package, modes::ServerContext{.things = things}, limits);
    script->setInstallEngineHooks(false);
    if (owner) {
        owner(*script); // (its owner's functions, before its set-up)
    }
    GameHost::Options none;
    (void)script->configure(none);
    return script;
}

} // namespace nox::server::detail
