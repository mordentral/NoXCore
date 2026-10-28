// The in-game map (TAB) as the game draws it (cliDrawMinimap, from
// drawMinimapAndLines while ToggleMap's bit is set), at the screen's resolution:
//
//   frame     a square, a sixth of the screen's width, against the left edge and centred up and
//             down; a translucent black fill and a soft black edge (three lines, alpha 90, 60,
//             40); what's drawn is clipped to it
//   scale     `zoom` world units per 100 of the game's pixels (1110 at first; 1 and 2 zoom out and
//             in by 10 a frame, 500-4000, client_mapZoomOut / client_mapZoomIn), centred on the view
//   group     the minimap group of the map polygon the view's centre is in (the last one it was
//             in; 1 before any): walls, markers and players of other groups are left out,
//             which keeps a map's floors apart
//   walls     those seen so far (drawn on screen once; all of them under game flag 0x10000), in
//             grey (115, 115, 115), each as its shape's lines in a 23-unit cell, a dot
//             past zoom 2000; not the invisible wall sets, nor broken or open secret walls
//   doors     a red line from the door along its facing (the table), once a wall
//             beside it (a diagonal neighbour cell) has been seen
//   markers   the client's "interesting" objects: crowns, game balls, flags, obelisks once seen
//             (IMMOBILE + VISIBLE_OBELISK, blue), monitored creatures and INTERESTING_ID objects
//             (red dots); a crown (outline) and ball (circle) in their team's colour, a flag's
//             outline in its team's, a CTF flag carrier as the flag in the other team's colour
//   players   ourselves, teammates and a crown carrier (everyone when observing) as dots: green
//             for us and teammates, red otherwise; the crown's outline for its carrier
//
// Dots shrink as the map zooms out (minimapDrawPoint: 4 up to zoom 1200, 3 to 1749, then 2). The game
// draws all of this in its own pixels; here the lines and dots are drawn smooth at their exact
// positions (effects_draw.hpp), scaled with the HUD. Not drawn while we're blind (enchantment 2).
#pragma once

#include "camera.hpp"
#include "map_view.hpp"
#include "sight.hpp"

#include "noxcore/client/world.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace noxsdl {

class Minimap {
public:
    explicit Minimap(const nox::things::ThingDb& things);
    /// The modifiers, for a flag's colour (its material).
    void setModifiers(const nox::things::ModifierDb* modifiers) { modifiers_ = modifiers; }

    /// ToggleMap (TAB, client_toggleMap).
    void toggle() { shown_ = !shown_; }
    bool shown() const { return shown_; }
    /// MapZoomIn / MapZoomOut (2 / 1), held: 10 a game frame. `ms` is the time since the last call.
    void zoom(bool in, bool out, std::uint64_t ms);
    int zoomLevel() const { return zoom_; }

    /// An obelisk (IMMOBILE with VISIBLE_OBELISK) the scene's sort drew this frame, lingering
    /// too: the scene draw lists it after its DRAW (Cli_AddMinimapMarker). Call whether the map is
    /// shown or not.
    void markDrawn(std::uint16_t netCode) { seenMarkers_.insert(netCode); }
    /// Without the scene's sort (the 3D view): the obelisks on screen this frame (and where the
    /// eye sees, given a sight) become markers.
    void observe(const nox::client::World& world, const Camera& camera, const SightMap* sight);
    /// A map loaded (mapCliReadAll): the old one's markers gone with its drawables;
    /// the obelisks the client loads itself (client_mapSpecialRWObjectData's records of a class 0x20400000 type,
    /// IMMOBILE with subclass 0x18) listed when their xfer's byte is 1 (a solo
    /// save's memory, ObeliskXfer).
    void mapLoaded(const nox::map::MapFile& map);
    /// Whether the drawable of `netCode` is on the client's marker list (g_cliMinimapMarkerList): an obelisk
    /// seen or listed by the map, or one marked interesting or monitored.
    bool marked(std::uint16_t netCode, const nox::client::World& world) const;
    /// The obelisks listed so far (seen, or by the map).
    std::size_t obelisksListed() const { return seenMarkers_.size(); }

    /// Draws the map on a w x h screen whose HUD scale is `scale`, centred on the view's centre
    /// (`camera`), for the local player `us`.
    void draw(SDL_Renderer* renderer, const nox::client::World& world, const MapView& view, std::uint16_t us,
              const Camera& camera, int w, int h, float scale);

private:
    bool shown_ = false;
    int zoom_ = 1110;              ///< the exe's starting zoom
    std::uint64_t zoomMs_ = 0;     ///< held-key time not yet turned into steps
    std::set<std::uint16_t> seenMarkers_; ///< obelisks drawn on screen so far (or listed by the map), by net code
    /// The obelisks the client loaded from the map (net code: extent | 0x8000): its type and place,
    /// for those listed before the server has sent them.
    struct Static {
        std::uint16_t typeId = 0;
        float x = 0;
        float y = 0;
    };
    std::map<std::uint16_t, Static> statics_;
    const nox::things::ThingDb& things_;
    /// By type id: 1 the Crown, 2 the GameBall (drawn as their markers), else 0; from thing.bin's
    /// names, worked out on the first draw.
    std::vector<std::uint8_t> markerTypes_;
    const nox::things::ModifierDb* modifiers_ = nullptr;
    int invisibleWall_ = -1;       ///< InvisibleWallSet's material index
    int invisibleBlockingWall_ = -1; ///< InvisibleBlockingWallSet's
};

} // namespace noxsdl
