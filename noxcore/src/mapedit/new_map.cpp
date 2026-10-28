// A new map for the editor's New dialog.
#include "noxcore/mapedit/new_map.hpp"

#include "noxcore/mapedit/markers.hpp"
#include "noxcore/mapedit/selection.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <array>

namespace nox::mapedit {

std::int16_t alignedVariant(const things::ThingDb& things, std::uint8_t floor, int gx, int gy)
{
    if (floor >= things.floors.size()) {
        return 0;
    }
    const auto& def = things.floors[floor];
    const int cols = def.cols, rows = def.rows;
    if (cols <= 0 || rows <= 0) {
        return 0;
    }
    // u = (x + y) / 2, v = (x - y) / 2 in full-grid cells (x + y is even); positive remainders.
    const int u = (gx + gy) / 2;
    const int v = (gx - gy) / 2;
    const auto mod = [](int a, int m) { return ((a % m) + m) % m; };
    return static_cast<std::int16_t>(mod(u, cols) + cols * mod(-v, rows));
}

MapDocument newMap(const things::ThingDb& things, const NewMapOptions& o)
{
    MapDocument d = MapDocument::blank();
    d.info.title.set(fitMapText(o.title, 63)); // (typed UTF-8: the map's 8-bit text)
    d.info.gameFlags = o.gameFlags;
    const int size = std::clamp(o.size, 0, 254);
    if (size > 0 && o.floor < things.floors.size()) {
        const float lo = static_cast<float>(128 - size / 2) * kWallCellSize;
        const float hi = static_cast<float>(128 + (size + 1) / 2) * kWallCellSize;
        for (int gy = -1; gy <= 254; ++gy) {
            for (int gx = (gy & 1); gx <= 255; gx += 2) {
                const WorldPoint c = tileCentre({static_cast<std::int16_t>(gx), static_cast<std::int16_t>(gy)});
                if (c.x < lo || c.x >= hi || c.y < lo || c.y >= hi) {
                    continue;
                }
                map::FloorTile t;
                t.image = o.floor;
                t.variant = alignedVariant(things, o.floor, gx, gy);
                d.floor.setTile(gx, gy, t);
            }
        }
    }
    if (o.playerStart) {
        DocObject start;
        start.type = "PlayerStart";
        start.header.x = 128 * kWallCellSize;
        start.header.y = 128 * kWallCellSize;
        start.header.details.emplace();
        start.header.details->flags = 0x01000000u; // ENABLED, as the game makes every object
        start.data = map::xfer::Default{};
        d.addObject(start);
    }
    if (o.lit) {
        // The map's ambient lights the view wherever no polygon covers it, so a map with no
        // polygons is lit by it everywhere, and the author's own polygons still apply once added
        // (a whole-map polygon would keep the view inside it for good: the game holds the current
        // polygon's light while the view stays in it).
        d.ambient.r = kNewMapAmbient[0];
        d.ambient.g = kNewMapAmbient[1];
        d.ambient.b = kNewMapAmbient[2];
    }
    return d;
}

} // namespace nox::mapedit
