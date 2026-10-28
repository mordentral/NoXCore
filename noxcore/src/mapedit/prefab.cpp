// The map editor's prefabs (prefab.hpp).
#include "noxcore/mapedit/prefab.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>

namespace nox::mapedit {

namespace {

constexpr std::string_view kHeader = "NoX Core prefab 1";

} // namespace

WorldPoint Prefab::centre() const
{
    float x0 = std::numeric_limits<float>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    const auto take = [&](float x, float y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    };
    for (const auto& e : clip.objects) {
        if (e.object.parent == 0) take(e.object.header.x, e.object.header.y);
    }
    for (const auto& w : clip.waypoints) take(w.x, w.y);
    for (const auto& [c, st] : clip.walls) {
        const auto p = wallCentre(c);
        take(p.x, p.y);
    }
    for (const auto& [t, tile] : clip.tiles) {
        const auto p = tileCentre(t);
        take(p.x, p.y);
    }
    if (x0 > x1) return wallCentre(clip.anchor);
    return {(x0 + x1) / 2, (y0 + y1) / 2};
}

std::size_t Prefab::objectCount() const
{
    return static_cast<std::size_t>(std::ranges::count_if(clip.objects, [](const ObjectEntry& e) { return e.object.parent == 0; }));
}

Prefab makePrefab(const MapDocument& doc, const Selection& selection, std::string name, const CopyLayers& layers)
{
    return {std::move(name), copySelection(doc, selection, layers)};
}

std::string prefabToText(const Prefab& prefab, const things::ThingDb& things)
{
    std::string name = prefab.name;
    std::ranges::replace(name, '\n', ' ');
    std::ranges::replace(name, '\r', ' ');
    return std::string(kHeader) + "\nname " + name + "\n" + clipToText(prefab.clip, things);
}

std::optional<Prefab> prefabFromText(std::string_view text, const things::ThingDb& things)
{
    const auto line = [&](std::string_view& rest) {
        const auto nl = rest.find('\n');
        std::string_view l = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        return l;
    };
    std::string_view rest = text;
    if (line(rest) != kHeader) return std::nullopt;
    const std::string_view n = line(rest);
    if (!n.starts_with("name ")) return std::nullopt;
    auto clip = clipFromText(rest, things);
    if (!clip) return std::nullopt;
    return Prefab{std::string(n.substr(5)), std::move(*clip)};
}

Op placePrefabOp(const MapDocument& doc, const Prefab& prefab, WorldPoint at, Selection* placed)
{
    const WorldPoint c = prefab.centre();
    Op op;
    if (prefab.cellAligned()) {
        // The anchor cell moved by the whole cells nearest (at - centre).
        const int cx = static_cast<int>(std::lround((at.x - c.x) / kWallCellSize));
        const int cy = static_cast<int>(std::lround((at.y - c.y) / kWallCellSize));
        const int ax = std::clamp(prefab.clip.anchor.x + cx, 0, 254);
        const int ay = std::clamp(prefab.clip.anchor.y + cy, 0, 255);
        op = pasteOp(doc, prefab.clip, Cell{static_cast<std::uint8_t>(ax), static_cast<std::uint8_t>(ay)}, placed);
    } else {
        float dx = at.x - c.x, dy = at.y - c.y;
        // A door in it keeps it on the grid: moved by whole cells (doors stand on the grid's
        // corners, every stock door, and the editor places them so).
        if (std::ranges::any_of(prefab.clip.objects, [](const ObjectEntry& e) { return std::holds_alternative<map::xfer::Door>(e.object.data); })) {
            dx = std::round(dx / kWallCellSize) * kWallCellSize;
            dy = std::round(dy / kWallCellSize) * kWallCellSize;
        }
        op = pasteMarkersOp(doc, prefab.clip, dx, dy, placed);
    }
    op.label = "Place prefab " + prefab.name;
    return op;
}

std::string prefabFileName(std::string_view name)
{
    std::string out;
    for (const char ch : name) {
        const auto u = static_cast<unsigned char>(ch);
        out += (std::isalnum(u) || ch == ' ' || ch == '-' || ch == '_') ? ch : '_';
    }
    if (out.empty()) out = "prefab";
    return out + ".nxprefab";
}

std::filesystem::path savePrefab(const std::filesystem::path& dir, const Prefab& prefab, const things::ThingDb& things)
{
    std::filesystem::create_directories(dir);
    const auto path = dir / prefabFileName(prefab.name);
    const std::string text = prefabToText(prefab, things);
    const auto tmp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) throw std::runtime_error("can't write " + tmp.string());
    }
    std::filesystem::rename(tmp, path);
    return path;
}

std::vector<Prefab> loadPrefabs(const std::filesystem::path& dir, const things::ThingDb& things)
{
    std::vector<Prefab> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".nxprefab") continue;
        std::ifstream f(e.path(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        if (auto p = prefabFromText(text, things)) out.push_back(std::move(*p));
    }
    std::ranges::sort(out, [](const Prefab& a, const Prefab& b) { return a.name < b.name; });
    return out;
}

} // namespace nox::mapedit
