// The mod editor's diagnostics: a mod's Luau files checked
// by Luau's analysis library (type errors and lints) against the toolbox's and the UI API's
// declarations (mod_defs.hpp): a server script (server/...) with the server's `nox` and require,
// a UI script (ui/...) with the UI's `nox`. Non-strict unless a file says --!strict.
//
// The checks run on a worker thread of their own: the Mod windows ask (after a pause in typing, and
// on save) with the files' text as it stands, and take the answer when it's ready; a newer ask
// replaces one still waiting. No SDL or ImGui (the tests check files directly with checkNow).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace noxsdl::editor {

struct ModDiagnostic {
    std::string file; ///< the project's path ("server/main.luau")
    int line = 1;     ///< 1-based
    int col = 1;      ///< 1-based
    bool error = true; ///< (a lint: a warning)
    std::string message;
};

class ModAnalysis {
public:
    /// The files, by project path, and their text.
    using Files = std::map<std::string, std::string>;
    struct Result {
        std::uint64_t serial = 0; ///< the ask it answers (request's)
        std::vector<ModDiagnostic> diagnostics;
        double ms = 0;            ///< how long the check took
    };

    ModAnalysis();
    ~ModAnalysis();
    ModAnalysis(const ModAnalysis&) = delete;
    ModAnalysis& operator=(const ModAnalysis&) = delete;

    /// The .luau files of `files` checked on the worker (any waiting ask replaced); its serial.
    std::uint64_t request(Files files);
    /// The newest answer not taken yet.
    std::optional<Result> take();
    /// An ask waiting or being checked.
    bool busy() const;

    /// The same check here and now (tests; the worker runs this).
    static std::vector<ModDiagnostic> checkNow(const Files& files);

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<std::pair<std::uint64_t, Files>> waiting_;
    std::optional<Result> done_;
    std::uint64_t serial_ = 0;
    bool working_ = false;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace noxsdl::editor
