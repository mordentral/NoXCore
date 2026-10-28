// noxcore.exe's mods folder (mods_folder.hpp).
#include "mods_folder.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/server/script_host.hpp"

#include <SDL3/SDL.h>

#include <cctype>
#include <string>
#include <system_error>

namespace noxsdl {

namespace {

nox::modes::ModsReport& report()
{
    static nox::modes::ModsReport r;
    return r;
}

void logReport()
{
    for (const std::string& line : nox::modes::modsReportLines(report())) {
        SDL_Log("%s", line.c_str());
    }
}

std::filesystem::path& folderOverride()
{
    static std::filesystem::path p;
    return p;
}

} // namespace

void setModsFolder(std::filesystem::path dir)
{
    folderOverride() = std::move(dir);
}

std::filesystem::path modsFolderPath()
{
    if (!folderOverride().empty()) {
        return folderOverride();
    }
    const char* base = SDL_GetBasePath();
    return nox::modes::modsFolder(base ? std::filesystem::path(reinterpret_cast<const char8_t*>(base)) : std::filesystem::current_path());
}

void loadModsAtStart()
{
    report() = nox::modes::loadMods(modsFolderPath(), nox::modes::registry());
    logReport();
    // The content packs (mods\<id>\content\): applied to the data files as they load, from here on
    // (not again on a refresh: the files are loaded once).
    nox::content::setContentPackLog([](const std::string& line) { SDL_Log("%s", line.c_str()); });
    // (Not a mod's, but set here at start-up too: a map script's call stopped by its step budget,
    // script::Vm::kMaxSteps, once per function and map.)
    nox::server::setScriptLog([](const std::string& line) { SDL_Log("%s", line.c_str()); });
    for (const std::string& line : nox::content::installContentPacks(modsFolderPath())) {
        SDL_Log("%s", line.c_str());
    }
}

void refreshMods()
{
    report() = nox::modes::reloadMods(modsFolderPath(), nox::modes::registry(), report());
    logReport();
}

const nox::modes::ModsReport& modsReport()
{
    return report();
}

bool openModsFolder()
{
    const std::filesystem::path dir = modsFolderPath();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // (A file: URL: the system's file browser on Windows and Linux alike.)
    std::string url = "file:///";
    for (const char8_t c : dir.generic_u8string()) {
        const auto b = static_cast<unsigned char>(c);
        if (std::isalnum(b) || b == '/' || b == ':' || b == '-' || b == '_' || b == '.' || b == '~') {
            url += static_cast<char>(b);
        } else {
            static constexpr char kHex[] = "0123456789ABCDEF";
            url += '%';
            url += kHex[b >> 4];
            url += kHex[b & 15];
        }
    }
    if (url.starts_with("file:////")) {
        url.erase(7, 1); // (a Linux path's own leading /)
    }
    if (!SDL_OpenURL(url.c_str())) {
        SDL_Log("mods: couldn't open %s: %s", url.c_str(), SDL_GetError());
        return false;
    }
    return true;
}

} // namespace noxsdl
