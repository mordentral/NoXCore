// The mods folder (mods.hpp).
#include "noxcore/modes/mods.hpp"

#include "noxcore/modes/script/scripted_mode.hpp"
#include "noxcore/util/text.hpp"

#include "mod_generate.hpp"
#include "mode_shared.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace nox::modes {

namespace fs = std::filesystem;

fs::path modsFolder(const fs::path& programDir)
{
    return programDir / "mods";
}

ModsReport loadMods(const fs::path& modsDir, Registry& registry)
{
    ModsReport report;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) {
        return report;
    }
    std::vector<fs::path> folders;
    for (const auto& e : fs::directory_iterator(modsDir, ec)) {
        if (e.is_directory(ec)) {
            folders.push_back(e.path());
        }
    }
    std::ranges::sort(folders);
    if (folders.size() > 64) {
        for (std::size_t k = 64; k < folders.size(); ++k) {
            report.broken.push_back({folders[k], "more than 64 mods: left out"});
        }
        folders.resize(64);
    }
    for (const fs::path& folder : folders) {
        auto package = std::make_shared<script::ModPackage>();
        package->folder = folder;
        const auto json = [&]() -> std::optional<std::string> {
            if (!fs::exists(folder / "mode.json", ec)) {
                return std::nullopt;
            }
            script::ModPackage probe;
            probe.folder = folder;
            return probe.read("mode.json");
        }();
        if (!json) {
            report.broken.push_back({folder, "no mode.json"});
            continue;
        }
        script::ManifestResult read = script::readManifest(*json);
        if (!read.manifest) {
            report.broken.push_back({folder, read.error});
            continue;
        }
        package->manifest = std::move(*read.manifest);
        if (!package->read(package->manifest.serverScript)) {
            report.broken.push_back({folder, "no " + package->manifest.serverScript});
            continue;
        }
        // A UI of API 2's pictures and sounds, checked from their headers (decoded only while the
        // mode is hosted): a mod over the caps is broken.
        if (package->manifest.uiApi >= 2 && !package->manifest.uiScript.empty()) {
            if (std::string why = script::checkModAssets(folder / "assets"); !why.empty()) {
                report.broken.push_back({folder, std::move(why)});
                continue;
            }
        }
        Mode m;
        m.info = package->manifest.info;
        m.info.fromMods = true; // (not built into the program: the lists show it after those that are)
        m.makeServer = [package](const ServerContext& ctx) { return std::make_unique<script::ScriptedMode>(package, ctx); };
        // Its generated maps (mode.json's mapPrefix): <prefix>_<slot><TAG>, its own and no other
        // mode's (a prefix whose names another mode already claims is refused), kept out of the
        // map lists and swept as a host starts.
        if (const std::string prefix = package->manifest.info.mapPrefix; !prefix.empty()) {
            const auto owns = [prefix](std::string_view name) {
                return name.size() > prefix.size() + 1 && util::equalsNoCase(name.substr(0, prefix.size()), prefix) &&
                       name[prefix.size()] == '_' && name[prefix.size() + 1] >= '0' && name[prefix.size() + 1] <= '9';
            };
            const std::string probe = prefix + "_0A";
            const Mode* other = nullptr;
            for (const Mode* o : registry.all()) {
                if (util::equalsNoCase(o->info.mapPrefix, prefix) || (o->info.ownsMap && o->info.ownsMap(probe))) {
                    other = o;
                    break;
                }
            }
            if (other) {
                report.broken.push_back({folder, "\"mapPrefix\" " + prefix + " is the mode " + other->info.id + "'s"});
                continue;
            }
            m.info.ownsMap = owns;
            m.sweepAll = [owns](const fs::path& mapsDir) {
                return server::detail::sweepMaps(mapsDir, [&](const std::string& name) { return owns(name); });
            };
            m.finishBackground = [] { script::Generation::finishReleased(); };
        }
        try {
            registry.add(std::move(m));
        } catch (const std::invalid_argument&) {
            report.broken.push_back({folder, "the id \"" + package->manifest.info.id + "\" is another mode's"});
            continue;
        }
        report.loaded.push_back(package->manifest.info.id);
    }
    return report;
}

ModsReport reloadMods(const fs::path& modsDir, Registry& registry, const ModsReport& previous)
{
    for (const std::string& id : previous.loaded) {
        registry.remove(id);
    }
    return loadMods(modsDir, registry);
}

std::vector<std::string> modsReportLines(const ModsReport& report)
{
    std::vector<std::string> out;
    for (const std::string& id : report.loaded) {
        out.push_back("mods: " + id + " loaded");
    }
    for (const ModsReport::Broken& b : report.broken) {
        out.push_back("mods: " + b.folder.filename().string() + ": " + b.why);
    }
    return out;
}

} // namespace nox::modes
