// The map editor's per-user settings.
#include "editor/editor_settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace noxsdl::editor {

namespace {

bool flag(const std::string& v)
{
    return v == "1" || v == "true";
}

} // namespace

void EditorSettings::addRecent(const std::string& path)
{
    std::erase(recent, path);
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecent) {
        recent.resize(kMaxRecent);
    }
}

void EditorSettings::load(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in) {
        return;
    }
    recent.clear();
    bindings.clear();
    sessionAlwaysEditors.clear();
    builtinPriorities.clear();
    bool themeSaved = false; // (ui.theme read: it wins over the old lightTheme, whichever comes first)
    bool oldLight = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        const auto i = [&] { return std::atoi(v.c_str()); };
        if (k == "recent") recent.push_back(v);
        else if (k == "uiScale") uiScale = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.5f, 3.0f);
        else if (k == "ui.theme") themeSaved = themeFromKey(v, theme) || themeSaved;
        else if (k == "lightTheme") oldLight = flag(v); // (settings from before ui.theme)
        else if (k == "layer.floor") layers.floor = flag(v);
        else if (k == "layer.edges") layers.edges = flag(v);
        else if (k == "layer.walls") layers.walls = flag(v);
        else if (k == "layer.objects") layers.objects = flag(v);
        else if (k == "layer.waypoints") layers.waypoints = flag(v);
        else if (k == "layer.polygons") layers.polygons = flag(v);
        else if (k == "layer.lighting") layers.lighting = flag(v);
        else if (k == "layer.seeThroughWalls") layers.seeThroughWalls = flag(v);
        else if (k == "layer.grid") layers.grid = std::clamp(i(), 0, 2);
        else if (k == "layer.extents") layers.extents = std::clamp(i(), 0, 2);
        else if (k == "layer.labelTypes") layers.labelTypes = flag(v);
        else if (k == "layer.labelScriptNames") layers.labelScriptNames = flag(v);
        else if (k == "layer.labelExtents") layers.labelExtents = flag(v);
        else if (k == "layer.facing") layers.facing = flag(v);
        else if (k == "layer.markDisabled") layers.markDisabled = flag(v);
        else if (k == "layer.schematic") layers.schematic = flag(v);
        else if (k == "layer.specialWalls") layers.specialWalls = flag(v);
        else if (k == "layer.triggers") layers.triggers = flag(v);
        else if (k == "layer.lightRings") layers.lightRings = flag(v);
        else if (k == "layer.links") layers.links = flag(v);
        else if (k == "layer.livePlayers") layers.livePlayers = flag(v);
        else if (k == "layer.liveCreatures") layers.liveCreatures = flag(v);
        else if (k == "layer.extents3d") layers.extents3d = flag(v);
        else if (k == "layer.pendingView") layers.pendingView = std::clamp(i(), 0, 2);
        else if (k == "lock.floor") layers.lockFloor = flag(v);
        else if (k == "lock.walls") layers.lockWalls = flag(v);
        else if (k == "lock.objects") layers.lockObjects = flag(v);
        else if (k == "lock.waypoints") layers.lockWaypoints = flag(v);
        else if (k == "lock.polygons") layers.lockPolygons = flag(v);
        else if (k == "pick.objects") pick.objects = flag(v);
        else if (k == "pick.walls") pick.walls = flag(v);
        else if (k == "pick.tiles") pick.tiles = flag(v);
        else if (k == "pick.waypoints") pick.waypoints = flag(v);
        else if (k == "regionShape") regionShape = std::clamp(i(), 0, 2);
        else if (k == "snap") snap = std::clamp(i(), 0, 3);
        else if (k == "snapCustom") snapCustom = std::clamp(static_cast<float>(std::atof(v.c_str())), 1.0f, 230.0f);
        else if (k == "copy.walls") copyLayers.walls = flag(v);
        else if (k == "copy.tiles") copyLayers.tiles = flag(v);
        else if (k == "copy.objects") copyLayers.objects = flag(v);
        else if (k == "copy.waypoints") copyLayers.waypoints = flag(v);
        else if (k == "autosaveMinutes") autosaveMinutes = std::clamp(i(), 0, 60);
        else if (k == "play.fullWindow") playFullWindow = flag(v);
        else if (k == "ui.floatingPanels") floatingPanels = flag(v);
        else if (k == "script.allowNoxCoreFeatures") allowNoxCoreScript = flag(v);
        else if (k == "script.fontScale") scriptFontScale = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.5f, 3.0f);
        else if (k == "session.alwaysEditor" && !v.empty()) sessionAlwaysEditors.push_back(v);
        else if (k == "script.builtinSort") builtinSort = std::clamp(i(), 0, kBuiltinSortCount - 1);
        else if (k == "script.builtinFavouritesOnly") builtinFavouritesOnly = flag(v);
        else if (k == "script.builtinPriority") {
            // "Name,level" (BuiltinPriority).
            const auto comma = v.rfind(',');
            if (comma != std::string::npos && comma > 0) {
                setPriority(builtinPriorities, v.substr(0, comma), static_cast<BuiltinPriority>(std::clamp(std::atoi(v.c_str() + comma + 1), 0, 3)));
            }
        }
        else if (k.starts_with("bind.")) bindings += k.substr(5) + "=" + v + "\n";
    }
    if (recent.size() > kMaxRecent) {
        recent.resize(kMaxRecent);
    }
    if (!themeSaved) {
        // Settings from before the three themes: a chosen Light kept; the old dark (now Medium),
        // or no choice at all, gets the new default.
        theme = oldLight ? Theme::Light : Theme::Dark;
    }
}

void EditorSettings::save(const std::filesystem::path& file) const
{
    std::ostringstream out;
    const auto b = [](bool v) { return v ? "1" : "0"; };
    for (const auto& r : recent) out << "recent=" << r << "\n";
    out << "uiScale=" << uiScale << "\n" << "ui.theme=" << themeKey(theme) << "\n";
    out << "layer.floor=" << b(layers.floor) << "\n" << "layer.edges=" << b(layers.edges) << "\n"
        << "layer.walls=" << b(layers.walls) << "\n" << "layer.objects=" << b(layers.objects) << "\n"
        << "layer.waypoints=" << b(layers.waypoints) << "\n" << "layer.polygons=" << b(layers.polygons) << "\n"
        << "layer.lighting=" << b(layers.lighting) << "\n" << "layer.seeThroughWalls=" << b(layers.seeThroughWalls) << "\n"
        << "layer.grid=" << layers.grid << "\n"
        << "layer.extents=" << layers.extents << "\n" << "layer.labelTypes=" << b(layers.labelTypes) << "\n"
        << "layer.labelScriptNames=" << b(layers.labelScriptNames) << "\n" << "layer.labelExtents=" << b(layers.labelExtents) << "\n"
        << "layer.facing=" << b(layers.facing) << "\n" << "layer.markDisabled=" << b(layers.markDisabled) << "\n"
        << "layer.schematic=" << b(layers.schematic) << "\n" << "layer.specialWalls=" << b(layers.specialWalls) << "\n"
        << "layer.triggers=" << b(layers.triggers) << "\n" << "layer.lightRings=" << b(layers.lightRings) << "\n"
        << "layer.links=" << b(layers.links) << "\n" << "layer.livePlayers=" << b(layers.livePlayers) << "\n" << "layer.liveCreatures=" << b(layers.liveCreatures) << "\n" << "layer.extents3d=" << b(layers.extents3d) << "\n"
        << "layer.pendingView=" << layers.pendingView << "\n"
        << "lock.floor=" << b(layers.lockFloor) << "\n" << "lock.walls=" << b(layers.lockWalls) << "\n"
        << "lock.objects=" << b(layers.lockObjects) << "\n" << "lock.waypoints=" << b(layers.lockWaypoints) << "\n"
        << "lock.polygons=" << b(layers.lockPolygons) << "\n";
    out << "pick.objects=" << b(pick.objects) << "\n" << "pick.walls=" << b(pick.walls) << "\n" << "pick.tiles=" << b(pick.tiles)
        << "\n" << "pick.waypoints=" << b(pick.waypoints) << "\n";
    out << "regionShape=" << regionShape << "\n" << "snap=" << snap << "\n" << "snapCustom=" << snapCustom << "\n";
    out << "copy.walls=" << b(copyLayers.walls) << "\n" << "copy.tiles=" << b(copyLayers.tiles) << "\n"
        << "copy.objects=" << b(copyLayers.objects) << "\n" << "copy.waypoints=" << b(copyLayers.waypoints) << "\n";
    out << "autosaveMinutes=" << autosaveMinutes << "\n";
    out << "play.fullWindow=" << b(playFullWindow) << "\n";
    out << "ui.floatingPanels=" << b(floatingPanels) << "\n";
    out << "script.allowNoxCoreFeatures=" << b(allowNoxCoreScript) << "\n";
    out << "script.fontScale=" << scriptFontScale << "\n";
    out << "script.builtinSort=" << builtinSort << "\n";
    out << "script.builtinFavouritesOnly=" << b(builtinFavouritesOnly) << "\n";
    for (const auto& [name, level] : builtinPriorities) out << "script.builtinPriority=" << name << "," << level << "\n";
    for (const auto& name : sessionAlwaysEditors) out << "session.alwaysEditor=" << name << "\n";
    std::istringstream lines(bindings);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty()) out << "bind." << line << "\n";
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << out.str();
        if (!f) {
            return;
        }
    }
    std::filesystem::rename(tmp, file, ec);
}

} // namespace noxsdl::editor
