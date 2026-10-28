// Ported from opennox-lib datapath (data.go, data_gog.go, data_reloaded.go, data_origin.go,
// registry.go, paths.go), MIT, Copyright (c) 2022 NoX World. See THIRD_PARTY_NOTICES.txt.
#include "noxcore/io/game_dir.hpp"

#include "noxcore/io/file.hpp"

#include <cstdlib>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nox::io {

namespace fs = std::filesystem;

namespace {

// The files that mark a folder as holding the game's data.
constexpr std::string_view kDataFiles[] = {"gamedata.bin", "modifier.bin", "monster.bin", "thing.bin"};

std::optional<fs::path> envPath(const char* name)
{
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) {
        return std::nullopt;
    }
    std::string s(value);
    std::free(value);
#else
    const char* raw = std::getenv(name);
    if (!raw) {
        return std::nullopt;
    }
    std::string s(raw);
#endif
    if (s.empty()) {
        return std::nullopt;
    }
    return fs::path(s);
}

bool present(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

/// The folders to put the fixed install paths under (paths.go pathPrefixes).
std::vector<fs::path> prefixes()
{
#ifdef _WIN32
    return {"C:\\", "D:\\"};
#else
    std::vector<fs::path> out;
    auto home = envPath("HOME");
    if (auto real = envPath("SNAP_REAL_HOME")) { // Snapcraft replaces HOME
        home = real;
    }
    if (auto p = envPath("SNAP_USER_COMMON")) {
        out.push_back(*p);
    }
    if (auto p = envPath("XDG_DATA_HOME")) {
        out.push_back(*p);
    }
    if (home) {
        for (const char* sub : {".local/share", ".wine/drive_c", ".wine/drive_d", ".wine/dosdevices/c:",
                                ".wine/dosdevices/d:", "Games/gog/nox/drive_c", "Games/gog/nox/drive_d",
                                "Games/Heroic"}) {
            out.push_back(*home / sub);
        }
        out.push_back(*home);
    }
    return out;
#endif
}

using Candidates = std::vector<GameDirCandidate>;

/// The existing ones of `paths` under each prefix (paths.go tryWithPrefixes).
void withPrefixes(std::initializer_list<std::string_view> paths, const char* reason, Candidates& out)
{
    for (const auto& prefix : prefixes()) {
        for (const auto rel : paths) {
            std::string r(rel);
#ifndef _WIN32
            for (auto& c : r) {
                if (c == '\\') {
                    c = '/';
                }
            }
#endif
            fs::path p = prefix / fs::path(r);
            if (present(p)) {
                out.push_back({p, reason});
            }
        }
    }
}

std::optional<fs::path> registryString([[maybe_unused]] std::wstring_view key, [[maybe_unused]] std::wstring_view value)
{
#ifdef _WIN32
    const std::wstring k(key);
    const std::wstring v(value);
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, k.c_str(), v.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr,
                     nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, k.c_str(), v.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr,
                     s.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    s.resize(wcsnlen(s.c_str(), s.size()));
    if (s.empty()) {
        return std::nullopt;
    }
    return fs::path(s);
#else
    return std::nullopt;
#endif
}

/// registry.go: the install keys the original game, GOG, Nox Reloaded and Origin write.
void registryPaths(Candidates& out)
{
    struct Key {
        const wchar_t* path;
        const wchar_t* value;
    };
    static constexpr Key kKeys[] = {
        {L"SOFTWARE\\WOW6432Node\\Westwood\\Nox", L"InstallPath"},
        {L"SOFTWARE\\Westwood\\Nox", L"InstallPath"},
        {L"SOFTWARE\\WOW6432Node\\Westwood\\Nox", L"FolderPath"},
        {L"SOFTWARE\\Westwood\\Nox", L"FolderPath"},
        {L"SOFTWARE\\WOW6432Node\\Westwood Studios\\NOX", L"Install Dir"},
        {L"SOFTWARE\\Westwood Studios\\NOX", L"Install Dir"},
    };
    for (const auto& k : kKeys) {
        if (auto p = registryString(k.path, k.value)) {
            // A value naming the game's .exe means its folder.
            auto ext = p->extension().string();
            for (auto& c : ext) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            out.push_back({ext == ".exe" ? p->parent_path() : *p, "the install the registry names"});
        }
    }
}

/// data_gog.go.
void gogPaths(Candidates& out)
{
    for (const std::wstring_view pref : {L"SOFTWARE\\WOW6432Node\\GOG.com", L"SOFTWARE\\GOG.com"}) {
        const std::wstring p(pref);
        // GOG Galaxy installs.
        if (auto path = registryString(p + L"\\Games\\1207658964", L"PATH")) {
            out.push_back({*path, "the GOG install"});
        }
        if (auto path = registryString(p + L"\\Games\\1207658964", L"WORKINGDIR")) {
            out.push_back({*path, "the GOG install"});
        }
        // The offline installer.
        if (auto path = registryString(p + L"\\GOGNOX", L"PATH")) {
            out.push_back({*path, "the GOG install"});
        }
        // Galaxy's own folder.
        if (auto path = registryString(p + L"\\GalaxyClient\\paths", L"client")) {
            out.push_back({*path / "Games" / "Nox", "GOG Galaxy's games folder"});
        }
    }
    withPrefixes({"Program Files (x86)\\GOG Galaxy\\Games\\Nox", "Program Files\\GOG Galaxy\\Games\\Nox",
                  "GOG Games\\Nox", "Games\\GOG.com\\Nox", "Program Files (x86)\\GOG.com\\Nox",
                  "Program Files\\GOG.com\\Nox", "Program Files (x86)\\GOGcom\\Nox", "Program Files\\GOGcom\\Nox"},
                 "a usual GOG folder", out);
}

bool sameDir(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    return a == b || fs::equivalent(a, b, ec);
}

} // namespace

bool isGameDir(const fs::path& dir)
{
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        return false;
    }
    for (const auto name : kDataFiles) {
        if (!fs::is_regular_file(resolveNoCase(dir / name), ec)) { // (any case: a copied install's THING.BIN)
            return false;
        }
    }
    return true;
}

// The search order documented in game_dir.hpp.
std::vector<GameDirCandidate> gameDirSearch(const fs::path& exeDir, const fs::path& configured)
{
    Candidates out;
    if (auto p = envPath("NOX_DATA")) {
        out.push_back({*p, "NOX_DATA"}); // takes priority
    }
    // The program's own folder, then the current one: a game folder holding the program (or a
    // copy started from in it) overrides the installs.
    if (!exeDir.empty()) {
        out.push_back({exeDir, "next to the program"});
    }
    std::error_code ec;
    if (const auto cwd = fs::current_path(ec); !ec && !cwd.empty() && (exeDir.empty() || !sameDir(cwd, exeDir))) {
        out.push_back({cwd, "the current folder"});
    }
#ifndef _WIN32
    withPrefixes({"Nox"}, "a usual install folder", out);
#endif
    registryPaths(out);
    gogPaths(out); // GOG before Reloaded and Origin: patched and official
    withPrefixes({"Program Files (x86)\\Nox", "Program Files\\Nox"}, "a usual Nox Reloaded folder", out);
    withPrefixes({"Program Files (x86)\\Origin Games\\Nox", "Program Files\\Origin Games\\Nox"}, "a usual Origin folder", out);
    if (!configured.empty()) {
        out.push_back({configured, "the folder this build was set up with"}); // the last resort
    }
    return out;
}

std::optional<GameDirCandidate> locateGameDir(const fs::path& exeDir, const fs::path& configured,
                                              const std::function<void(const std::string&)>& log)
{
    for (auto& c : gameDirSearch(exeDir, configured)) {
        const bool found = isGameDir(c.dir);
        if (log) {
            log((found ? "game data found in " : "no game data in ") + c.dir.string() + " (" + c.reason + ")");
        }
        if (found) {
            std::error_code ec;
            if (auto abs = fs::absolute(c.dir, ec); !ec) {
                c.dir = std::move(abs);
            }
            return std::move(c);
        }
    }
    return std::nullopt;
}

std::vector<fs::path> gameDirCandidates(const fs::path& exeDir)
{
    std::vector<fs::path> out;
    for (auto& c : gameDirSearch(exeDir)) {
        out.push_back(std::move(c.dir));
    }
    return out;
}

std::optional<fs::path> findGameDir(const fs::path& exeDir, const std::function<void(const std::string&)>& log)
{
    if (auto found = locateGameDir(exeDir, {}, log)) {
        return std::move(found->dir);
    }
    return std::nullopt;
}

} // namespace nox::io
