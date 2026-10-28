// The mod editor's diagnostics (mod_analysis.hpp).
#include "editor/mod_analysis.hpp"
#include "editor/code_lexer.hpp"
#include "editor/mod_defs.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4324) // (Luau's headers' unused parameters and padded variants, not ours)
#endif
#include "Luau/BuiltinDefinitions.h"
#include "Luau/ConfigResolver.h"
#include "Luau/Frontend.h"
#include "Luau/LinterConfig.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <chrono>
#include <set>

namespace noxsdl::editor {

namespace {

/// The half a project path's script is: ui/... the UI's, anything else the server's.
ModHalf halfOf(const std::string& path)
{
    return path.starts_with("ui/") ? ModHalf::Ui : ModHalf::Server;
}

/// The project's files as Luau's frontend reads them, by module name (the project path).
struct ProjectFiles final : Luau::FileResolver {
    const ModAnalysis::Files* files = nullptr;
    ModHalf half = ModHalf::Server;

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override
    {
        if (!files) {
            return std::nullopt;
        }
        const auto it = files->find(name);
        if (it == files->end()) {
            return std::nullopt;
        }
        return Luau::SourceCode{it->second, Luau::SourceCode::Module};
    }

    std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo*, Luau::AstExpr* expr, const Luau::TypeCheckLimits&) override
    {
        // require("name"): server/name.luau, as the toolbox's require loads it (the server's only).
        const auto* s = expr ? expr->as<Luau::AstExprConstantString>() : nullptr;
        if (!s || half != ModHalf::Server) {
            return std::nullopt;
        }
        return Luau::ModuleInfo{"server/" + std::string(s->value.data, s->value.size) + ".luau"};
    }

    std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override { return name; }
};

/// One half's checker: a frontend with Luau's own globals and the half's declarations.
struct Checker {
    ProjectFiles files;
    Luau::NullConfigResolver config;
    std::unique_ptr<Luau::Frontend> frontend;
    std::set<std::string> seen; ///< modules it has read (marked dirty before each check)
    std::string loadError;

    explicit Checker(ModHalf half)
    {
        files.half = half;
        config.defaultConfig.mode = Luau::Mode::Nonstrict;
        Luau::FrontendOptions options;
        options.runLintChecks = true;
        options.moduleTimeLimitSec = 2.0; // (a pathological file can't hold the worker)
        frontend = std::make_unique<Luau::Frontend>(Luau::SolverMode::New, &files, &config, options);
        Luau::registerBuiltinGlobals(*frontend, frontend->globals);
        // The half's declarations, less the require line: Luau's own require stays (its module
        // tracing resolves what it loads: resolveModule).
        std::string defs;
        const std::string_view source = modDefSource(half);
        for (std::size_t at = 0; at < source.size();) {
            const std::size_t end = std::min(source.find('\n', at), source.size());
            const std::string_view line = source.substr(at, end - at);
            if (!line.starts_with("declare function require")) {
                defs.append(line).push_back('\n');
            }
            at = end + 1;
        }
        Luau::unfreeze(frontend->globals.globalTypes);
        const Luau::LoadDefinitionFileResult r =
            frontend->loadDefinitionFile(frontend->globals, frontend->globals.globalScope, defs, "@nox", false, false);
        Luau::freeze(frontend->globals.globalTypes);
        if (!r.success) {
            loadError = "the toolbox's declarations didn't load";
            for (const auto& e : r.parseResult.errors) {
                loadError += "; line " + std::to_string(e.getLocation().begin.line + 1) + ": " + e.getMessage();
            }
            if (r.module) {
                for (const auto& e : r.module->errors) {
                    loadError += "; line " + std::to_string(e.location.begin.line + 1) + ": " + Luau::toString(e);
                }
            }
        }
    }

    void check(const std::string& name, const ModAnalysis::Files& all, std::vector<ModDiagnostic>& out)
    {
        files.files = &all;
        for (const std::string& m : seen) {
            frontend->markDirty(m);
        }
        seen.insert(name);
        for (const auto& [path, text] : all) {
            if (halfOf(path) == files.half) {
                seen.insert(path);
                frontend->markDirty(path);
            }
        }
        if (!loadError.empty()) {
            out.push_back({name, 1, 1, true, loadError});
        }
        Luau::CheckResult r;
        try {
            r = frontend->check(name);
        } catch (const std::exception& e) {
            out.push_back({name, 1, 1, true, std::string("the check failed: ") + e.what()});
            files.files = nullptr;
            return;
        }
        for (const Luau::TypeError& e : r.errors) {
            if (e.moduleName != name) {
                continue; // (a required file's own: reported with it)
            }
            out.push_back({name, static_cast<int>(e.location.begin.line) + 1, static_cast<int>(e.location.begin.column) + 1, true,
                           Luau::toString(e, Luau::TypeErrorToStringOptions{&files})});
        }
        const auto lint = [&](const Luau::LintWarning& w, bool error) {
            out.push_back({name, static_cast<int>(w.location.begin.line) + 1, static_cast<int>(w.location.begin.column) + 1, error,
                           std::string(Luau::LintWarning::getName(w.code)) + ": " + w.text});
        };
        for (const Luau::LintWarning& w : r.lintResult.errors) lint(w, true);
        for (const Luau::LintWarning& w : r.lintResult.warnings) lint(w, false);
        if (const auto it = all.find(name); it != all.end() && it->second.find("--!strict") == std::string::npos) {
            unknownMembers(name, it->second, out);
        }
        files.files = nullptr;
    }

    /// Non-strict checking lets a misspelt nox.* through (an unknown key reads as any): each
    /// `nox.<name>` the half doesn't declare is an error of its own here. (Strict files: Luau's.)
    void unknownMembers(const std::string& name, const std::string& text, std::vector<ModDiagnostic>& out) const
    {
        std::set<std::string_view> known;
        for (const ModDef& d : modDefs(files.half)) {
            if (d.kind == ModDef::Kind::Member) known.insert(d.name);
        }
        std::uint8_t state = 0;
        int lineNo = 0;
        for (std::size_t at = 0; at <= text.size();) {
            const std::size_t end = std::min(text.find('\n', at), text.size());
            const std::string_view line(text.data() + at, end - at);
            ++lineNo;
            const auto runs = lexLine(CodeLanguage::Luau, line, state, {});
            for (std::size_t i = 0; i + 2 < runs.size(); ++i) {
                const auto word = [&](std::size_t k) { return line.substr(static_cast<std::size_t>(runs[k].begin), static_cast<std::size_t>(runs[k].end - runs[k].begin)); };
                if (runs[i].tok == Tok::Plain && word(i) == "nox" && word(i + 1) == "." && runs[i + 2].tok == Tok::Plain && !known.contains(word(i + 2)) &&
                    (i == 0 || word(i - 1) != ".")) {
                    out.push_back({name, lineNo, runs[i + 2].begin + 1, true,
                                   "nox." + std::string(word(i + 2)) + " isn't in the " + (files.half == ModHalf::Ui ? "UI API" : "server toolbox") +
                                       " (the Mod Reference lists what is)"});
                }
            }
            at = end + 1;
        }
    }
};

} // namespace

ModAnalysis::ModAnalysis() = default;

ModAnalysis::~ModAnalysis()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::uint64_t ModAnalysis::request(Files files)
{
    std::lock_guard lock(mutex_);
    waiting_.emplace(++serial_, std::move(files));
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { run(); });
    }
    wake_.notify_one();
    return serial_;
}

std::optional<ModAnalysis::Result> ModAnalysis::take()
{
    std::lock_guard lock(mutex_);
    return std::exchange(done_, std::nullopt);
}

bool ModAnalysis::busy() const
{
    std::lock_guard lock(mutex_);
    return working_ || waiting_.has_value();
}

void ModAnalysis::run()
{
    while (true) {
        std::pair<std::uint64_t, Files> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || waiting_.has_value(); });
            if (stop_) {
                return;
            }
            job = std::move(*waiting_);
            waiting_.reset();
            working_ = true;
        }
        const auto t0 = std::chrono::steady_clock::now();
        Result r;
        r.serial = job.first;
        r.diagnostics = checkNow(job.second);
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard lock(mutex_);
        done_ = std::move(r);
        working_ = false;
    }
}

std::vector<ModDiagnostic> ModAnalysis::checkNow(const Files& files)
{
    // (Each thread's checkers made once: Luau's globals and the declarations take a while.)
    thread_local Checker server(ModHalf::Server);
    thread_local Checker ui(ModHalf::Ui);
    std::vector<ModDiagnostic> out;
    for (const auto& [path, text] : files) {
        if (!path.ends_with(".luau")) {
            continue;
        }
        (halfOf(path) == ModHalf::Ui ? ui : server).check(path, files, out);
    }
    return out;
}

} // namespace noxsdl::editor
