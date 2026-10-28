// Draws a map's static layers from its .map file: floor tiles and their edges so far.
//
// Floor layout (server_mapRWFloorMap / tileDrawMB): a 128 x 128 grid of
// 46-unit cells, each with up to two 46 x 46 diamond tiles; the "odd" one is drawn with its
// top-left at world (46x + 12, 46y - 11), the "even" one at (46x - 11, 46y + 12)
// (tileDrawImpl draws in world units, offset by the tile buffer's origin). The
// image is the FLOR entry's image[frameOffset + variant].
//
// Edges (tileCallDrawEdges -> drawTexEdgesProbably) are drawn over
// the tile in file order, each with the same 46 x 46 diamond: the EDGE entry's
// image[frameOffset + FloorEdge::direction] masks where its own pixels go and where the
// FloorEdge's floor texture (floor, variant) shows through; elsewhere the tile stays. The game
// does this into its tile buffer every time a tile scrolls in; we composite each distinct
// tile + edge stack once into a texture.
//
// Walls (drawWalls) sit on the 23-unit wall grid. The sprite is the WALL entry's
// image[slot][direction][variant] (slot 0 unless the wall is in front of the player or flagged
// 8), drawn with the image's own offset at (23x - 50 + dx, 23y - 72 - dy), where (dx, dy) is
// the image's thing.bin offset.
//
// Objects (the map's top-level ObjectData records) draw a still of their type (see thing_art.hpp),
// or its animation for AnimateDraw types (flames, torches).
// Walls and objects are drawn together back to front by y, a wall at its cell's centre.
#pragma once

#include "camera.hpp"
#include "lighting.hpp"
#include "polygon_ambient.hpp"
#include "sight.hpp"
#include "sprite_cache.hpp"
#include "thing_art.hpp"

#include "noxcore/client/world.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/mapedit/pending.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <bitset>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noxsdl {

class MapView {
    friend class View3D; // draws the map's walls and objects in 3D (view3d.hpp)
public:
    enum class ObjectSet {
        All,          ///< everything the map places (the offline viewer)
        ClientLoaded, ///< what the vanilla client loads itself (CLIENT_PERSIST or IMMOBILE types)
        None,
        /// The map editor's: everything, NPCs and maidens drawn from their records as the game
        /// will draw them (their colours, an NPC's worn things: recordLook), and the items' colours
        /// from every xfer that names modifiers.
        Editor,
    };

    /// `modifiers` colours the map's weapons and armour by the modifiers their xfer names.
    MapView(const nox::map::MapFile& map, const nox::things::ThingDb& things, ThingArt& art,
            const nox::things::ModifierDb& modifiers, ObjectSet objectSet = ObjectSet::All);
    ~MapView();
    MapView(const MapView&) = delete;
    MapView& operator=(const MapView&) = delete;

    /// Tiles are composited at the art's own resolution into an offscreen target covering the
    /// view, which is then scaled to the window in one draw: scaling each diamond separately
    /// leaves seams at fractional zooms.
    /// With `light`, the floor is multiplied by the light map (before scaling to the window).
    /// With `sight`, everything the eye can't see is black.
    void drawFloor(SDL_Renderer* renderer, const Camera& camera, SpriteCache& sprites, LightMap* light = nullptr,
                   SightMap* sight = nullptr);
    /// Subtracted from a BELOW thing's sort key (FLAGS BELOW: fire pits, plates, scorch marks), so
    /// it's drawn under everything standing on the floor, in its own y order.
    static constexpr float kBelow = 100000.0f;

    /// A sprite to sort into the scene; (x, y) is its origin before the image's own offset, and
    /// (lightX, lightY) the point its light is sampled at (its base).
    struct SceneSprite {
        float sortY = 0;
        float x = 0;
        float y = 0;
        const Sprite* sprite = nullptr;
        float lightX = 0;
        float lightY = 0;
        std::uint8_t alpha = 255; ///< the game's draw alpha (a summon's fading-in ghost)
        /// Draws something other than a sprite in the sprite's place in the order (glows and
        /// lines round a drawable); `sprite` is then unused.
        std::function<void(SDL_Renderer*, const Camera&)> draw;
        bool bloom = false; ///< a light source's sprite (CLASS LIGHT): its bright parts bloom
        /// Its light instead of the map's (the drawable draw's colour multiplier): white for a sprite drawn
        /// unlit, blue / white for an invulnerable creature, grey for an afraid one.
        std::optional<std::array<std::uint8_t, 3>> light;
        /// World y below which it isn't drawn: a drawable below the floor (z < 0, an elevator going
        /// down and its riders) sinks into it, its bottom cut by its depth (client_drawImg_bbb).
        float clipBottom = 1e9f;
        /// A drawable's sprite (its body's or its enchantments'): what its sight test
        /// (SightMap::drawableSeen at (lightX, lightY)) takes: its type, its blocker key
        /// (SightMap::liveKey), its height, a door's angle and the generic draw's last image of it.
        /// Null `sightArt`: anything else, point-tested there (SightMap::visible).
        const TypeArt* sightArt = nullptr;
        std::uint32_t sightKey = 0;
        float sightZ = 0;
        std::uint8_t sightDoorAngle = 0;
        nox::client::DrawnImage* drawnImage = nullptr;
        /// A body image the generic drawable draw draws: drawn, its size is noted (drawnImage).
        bool notesImage = false;
        /// A drawable's body: its Fade Objects marks (client/fade.hpp), which the scene's sort and
        /// draw keep. A drawable's body sprites come together and share them, the first's sight
        /// test deciding for all. Its enchantment effects share them too (drawn with it, never
        /// faded: `fadeKeepAlpha`). Null for anything else (other effects, icons, name plates).
        nox::client::FadeState* fade = nullptr;
        /// What the sort goes by: the general list (anything but BELOW things and wall hangings)
        /// lingers out of sight; the others are drawn only in sight, faded while lingering,
        /// but the floor's decals (`fadeNever`), which never fade.
        bool fadeGeneral = false;
        bool fadeNever = false;
        bool fadeSelf = false;     ///< our own unit's: always in sight
        bool fadeCreature = false; ///< a creature's or player's: never lingers of itself
        /// Its alpha is the invisible's look (or our own observer's), which a fade doesn't change;
        /// or its DRAW doesn't go through the generic drawable draw, which alone fades
        /// (TypeArt::fades).
        bool fadeKeepAlpha = false;
        /// Its alpha is the Ghost's own: lingering, the smaller of it and the fade.
        bool fadeMin = false;
        /// A map editor session's pending look (nox::mapedit::PendingLook: 1 pending, 2 ghost),
        /// and its video.bag image for the CPU's copy when there's no pending shader.
        std::uint8_t pendingLook = 0;
        std::int32_t pendingImage = -1;
    };

    /// A sprite drawn this frame that matters to the bloom (bloomSprites), as it was drawn: a light
    /// source (its bright parts bloom), or something drawn after one (it hides what's behind it).
    struct BloomSprite {
        const Sprite* sprite = nullptr;
        SDL_FRect src{};
        SDL_FRect dst{};
        Rgb light{};
        std::uint8_t alpha = 255;
        bool source = false; ///< a light source; else an occluder
    };
    /// The map editor's: each frame's images all made before the first of them is drawn (floor
    /// and scene apart), so the new ones go up to the GPU a page's shelf at a time instead of in an
    /// upload between draws each (thousands of them the first time a big map is seen whole).
    /// Off (the game's): drawn as they're made.
    void setBatchImages(bool on) { batchImages_ = on; }
    /// The map editor's: below this zoom (screen pixels a world unit) drawFloor draws the original
    /// tiles even with the image pack, whose upscaled tiles' composites would each be shrunk to a
    /// few pixels (zoomed out over a big map, thousands of them, past the texture budget). 0: never.
    void setSdFloorBelow(float zoom) { sdFloorBelow_ = zoom; }
    /// The map editor's: at most this long a frame making floor tiles and their edge composites
    /// not made before; the rest are left out of that frame (black) and made in the next ones, so
    /// a first look over a big map fills in over a few frames instead of stopping in one. 0 (the
    /// game's): everything in view made at once.
    void setFloorMakeBudget(double ms) { floorMakeBudgetMs_ = ms; }
    /// Whether drawScene notes the sprites for the bloom.
    void setCollectBloom(bool on) { collectBloom_ = on; }
    /// The game frame drawScene animates the map's objects at (flames, torches: AnimateDraw), and
    /// the one Fade Objects' sort and fade go by.
    void setTime(double gameFrames) { time_ = gameFrames; }
    /// Fade Objects (nox.cfg FadeObjects): the live drawables seen that leave sight linger a game
    /// second, fading out (SceneSprite::fade).
    void setFadeObjects(bool on) { fadeObjects_ = on; }
    /// Front walls (those facing the eye from below, frontWall) drawn last with their front image
    /// at half alpha (TranslucentFrontWalls; on, the game's look). Off (the map editor's default),
    /// no wall is a front wall: every solid wall is drawn whole and opaque, under the objects.
    void setTranslucentFrontWalls(bool on) { translucentFrontWalls_ = on; }
#ifndef NOX_NO_TEST_COMMANDS
    /// --trace-fade (tests): logs the map's objects lingering after the last drawScene, with their fade.
    void traceFade() const;
#endif
    /// In drawing order: the light sources' sprites drawScene drew this frame (things of CLASS
    /// LIGHT: flames, fireballs, explosions, torches), and every sprite drawn after the first of
    /// them (walls and objects in front can hide them), with their draw rectangles, light tint and
    /// alpha. Replayed into the bloom, the sources add their bright parts and the rest cut their
    /// silhouettes out, so only what's seen of a light blooms.
    const std::vector<BloomSprite>& bloomSprites() const { return bloom_; }
    /// The extents of the map's obelisks (IMMOBILE with VISIBLE_OBELISK, extent 0x7FFF or less)
    /// the last drawScene's sort drew: the minimap lists them (Cli_AddMinimapMarker).
    const std::vector<std::uint32_t>& drawnMarkers() const { return drawnMarkers_; }

    /// Walls, the map's objects and `extra`, back to front. Map objects whose extent is in
    /// `liveCodes` are skipped: the server has sent that object, so `extra` draws it. With
    /// `light`, each sprite is tinted by the light at its base. With `sight`, walls are drawn as
    /// the sight reaches them (drawWalls: only those it reaches, as the arms it sees make them, in
    /// the columns where their base is seen), and objects as the drawable sight test sees them.
    void drawScene(SDL_Renderer* renderer, const Camera& camera, SpriteCache& sprites,
                   std::span<const SceneSprite> extra = {}, const std::set<std::uint32_t>* liveCodes = nullptr,
                   const LightMap* light = nullptr, const SightMap* sight = nullptr) const;

    /// The walls as they stand: the map's, less the broken and open ones, plus Wall-spell walls.
    const std::vector<nox::map::Wall>& walls() const { return walls_; }

    /// Applies the server's wall state:
    /// - `destroyed` indexes the map's DestructableWalls; broken walls are gone (flag 0x20);
    /// - `opened` / `closed` index its SecretWalls; an open secret wall is gone too (the
    ///   minimap, cliDrawMinimap, hides states 2 and 3, and nothing else of the client's draws or
    ///   blocks with them). Walls named in neither start as the map says (flag 8 = open);
    /// - `magic` are walls the Wall spell raised, by cell: (x, y) -> (material, direction, variant).
    /// The ids are the sections' indexes: server_mapRWDestructableWalls and server_mapRWSecretWalls number the walls as they load.
    void setWallState(const std::set<std::uint16_t>& destroyed, const std::set<std::uint16_t>& opened,
                      const std::set<std::uint16_t>& closed,
                      const std::vector<std::pair<std::pair<int, int>, nox::map::Wall>>& magic);
    /// The map editor's incremental update (no rebuild): the full-grid tile (gx, gy) (gx + gy
    /// even; FloorMap pair ((gx - 1) / 2, (gy + 1) / 2)'s odd tile for odd gx, (gx / 2, gy / 2)'s
    /// even one else) set to `tile`, or removed for null. Its composite is made when drawn.
    void setTile(int gx, int gy, const nox::map::FloorTile* tile);
    /// The map editor's: the map's walls replaced, with its window, breakable and secret lists
    /// (none broken, secret walls as the lists start them).
    void setWalls(const std::vector<nox::map::Wall>& walls, const nox::map::WindowWalls& windows,
                  const nox::map::DestructableWalls& breakable, const nox::map::SecretWalls& secret);
    /// The map editor's: the map's objects (and their lights and ambient sounds) made again from
    /// `map`'s ObjectTOC and ObjectData, the floor and walls left as they are.
    void setObjects(const nox::map::MapFile& map);
    /// A map editor session's pending edits as planned for this view (nox::mapedit::planPending,
    /// replaced whole): the walls, tiles and client-loaded things of the view's own it covers left
    /// out, its own drawn in their looks (greyed, or a ghost). Drawing only: walls(),
    /// sightBlockers(), useableAt() and the minimap's seen walls never take them; its walls are
    /// see-through decorations drawn where the floor under them is in sight. Empty (the default,
    /// and always outside a session): nothing is drawn differently and no time is spent on it.
    void setPending(const nox::mapedit::PendingPlan& plan);
    bool hasPending() const { return pendingActive_; }
    /// A wall of the plan's own (a decoration: never in walls()), in its look (PendingLook).
    struct PendingWall {
        nox::map::Wall wall;
        bool window = false;
        std::uint8_t look = 0;
    };
    /// For the minimap (red pending walls): the plan's own walls...
    const std::vector<PendingWall>& pendingWalls() const { return pendingWalls_; }
    /// ...the look the plan gives one of our walls at (x, y) (0: none; 2: a ghost of a removal)...
    std::uint8_t pendingWallLook(int x, int y) const
    {
        if (pendingWallLook_.empty()) return 0;
        const auto it = pendingWallLook_.find(wallKey(x, y));
        return it == pendingWallLook_.end() ? 0 : it->second;
    }
    /// ...whether the plan leaves our wall at (x, y) out...
    bool pendingHidesWall(int x, int y) const { return !pendingHideWalls_.empty() && pendingHideWalls_.contains(wallKey(x, y)); }
    /// ...and whether a decoration's cell has been drawn in sight (fog keeps unseen ones off the
    /// map, as the minimap's seen walls).
    bool pendingWallSeen(int x, int y) const { return pendingSeen_.contains(wallKey(x, y)); }
    /// A number no other MapView of this run has (what was given to which view, by those keeping track).
    std::uint64_t serial() const { return serial_; }
    /// Door angles the server sent (MSG_DOOR_ANGLE), by net code (bit 15 = a map object).
    void setDoorAngles(const std::map<std::uint16_t, std::uint8_t>& angles);
    /// What the server changed of the map's objects, by extent: whether they're enabled (a
    /// VISIBLE_ENABLE object shows, and a light shines, only while enabled), the SlaveDraw image
    /// (an obelisk's charge, a pentagram), and their light.
    void setStatics(const std::map<std::uint16_t, nox::client::StaticObjectState>& statics);
    /// A quest (game flag 0x1000): DoorDraw draws a locked door's lock over it.
    void setQuestLocks(bool on) { questLocks_ = on; }
    /// Where one of the map's own objects is, by extent (a ray's end on an obelisk: net code bit
    /// 15, Cli_GetStaticDrawableByNetCode).
    std::optional<SDL_FPoint> objectPosition(std::uint32_t extent) const
    {
        const auto it = objectIndex_.find(extent);
        if (it == objectIndex_.end()) {
            return std::nullopt;
        }
        return SDL_FPoint{objects_[it->second].x, objects_[it->second].y};
    }
    /// The map's objects that block line of sight (SHADOW things; see sight.hpp), skipping those
    /// in `liveCodes` (the server sends them), added to `out` in the map's order.
    void sightBlockers(std::vector<SightMap::Blocker>& out, const std::set<std::uint32_t>* liveCodes = nullptr) const;

    /// Window walls (the map's WindowWalls), keyed by wallKey(x, y).
    const std::set<std::uint16_t>& windows() const { return windows_; }

    /// Whether the wall has been seen, for the minimap: drawn on screen at some point
    /// (drawWalls sets its byte 0xC), or saved as seen in the map (a solo map).
    bool wallSeen(const nox::map::Wall& w) const
    {
        return w.unknown != 0 || seenWalls_.test(wallKey(w.x, w.y));
    }
    /// Whether the wall at grid (x, y) has been drawn on screen since this map loaded (the 1
    /// drawWalls stores for it; HostClient::wallDrawn).
    bool wallDrawn(int x, int y) const
    {
        return x >= 0 && x < 256 && y >= 0 && y < 256 &&
               seenWalls_.test(wallKey(static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)));
    }
    /// The map's doors, where they stand and which way they face (0-31).
    struct Door {
        float x = 0;
        float y = 0;
        std::uint8_t angle = 0;
    };
    const std::vector<Door>& doors() const { return doors_; }
    /// The wall standing at cell key `key` (wallKey; the last of walls() there), or null.
    const nox::map::Wall* wallByKey(std::uint16_t key) const
    {
        const auto it = wallByKey_.find(key);
        return it == wallByKey_.end() ? nullptr : &walls_[it->second];
    }
    /// Whether a live drawable with net code `netCode` at (x, y) is the server's copy of one of
    /// our map objects (the same number as its extent, where it stands), so the map's isn't drawn
    /// twice. The number alone says nothing: the server's net codes are its own series, and a
    /// client-loaded object is named by extent | 0x8000 (a drawable numbered 2 hid door 2).
    bool duplicates(std::uint32_t netCode, float x, float y) const;
    /// The minimap group of the first map polygon containing (x, y) (polygon byte 0x82), or
    /// nullopt outside them all.
    std::optional<std::uint8_t> minimapGroupAt(float x, float y) const;
    /// The minimap's group with the view's centre at (x, y) (the player's currentPolygonByte130,
    /// which polygonDrawColor sets each frame with the ambient: 1 outside every polygon, else the
    /// group of the one the view is in, tested again only when the centre moves).
    std::uint8_t minimapGroup(float x, float y) const;
    static std::uint16_t wallKey(int x, int y) { return static_cast<std::uint16_t>((x << 8) | (y & 0xFF)); }

    /// A light the map places.
    struct MapLight {
        std::uint32_t extent = 0;
        LightSource light;
        bool clientLoaded = false; ///< loaded by the client itself (ColorLight); else the server sends it
        bool enabled = true;       ///< the map's flag, then the server's (MSG_ENABLE_OBJECT); off: dark
        std::optional<float> intensity; ///< set by the server (MSG_REPORT_LIGHT_INTENSITY, an obelisk's charge)
        std::optional<Rgb> color;       ///< set by the server (MSG_REPORT_LIGHT_COLOR)
        /// A ColorLight's animation (its InvisibleLight block): the first n non-zero keys of
        /// each kind, stepped through every `step` frames. Flags: 1 colours loop, 4 intensities
        /// loop; otherwise they run back and forth.
        std::vector<Rgb> colorKeys;
        std::uint16_t colorStep = 0;
        std::vector<std::uint8_t> intensityKeys;
        std::uint16_t intensityStep = 0;
        std::uint16_t animFlags = 0;
        /// An InvisibleLight's cone, its animation and aim (light_cone.hpp).
        std::optional<lightcone::ConeAnim> cone;

        /// The light at game frame `frame` (ColorLightUpdateDraw: colorLightAlterB0ArrayColor,
        /// colorLightAlterIntensity, then the cone's: colorLightAlterRadius, colorLightRotate,
        /// colorLightAimAtTarget).
        LightSource at(std::uint32_t frame) const;
    };
    const std::vector<MapLight>& lights() const { return lights_; }

    /// A map object whose type has an AUDIOLOOP (ambient birds and wind, torches, fires, gears,
    /// generators): it plays the loop while enabled and near enough.
    struct SoundSource {
        std::uint32_t extent = 0;
        float x = 0;
        float y = 0;
        std::string sound;         ///< the AUDIOLOOP sound's name
        std::optional<std::uint32_t> soundId; ///< its id (nox::audio::soundId), nullopt for a name it doesn't know
        bool clientLoaded = false; ///< loaded by the client itself; else the server sends it
        bool enabled = true;       ///< the map's flag, then the server's
    };
    const std::vector<SoundSource>& soundSources() const { return sounds_; }
    /// A USEABLE map object (levers, switches, buttons, chain pulls: IMMOBILE, SUBCLASS USEABLE)
    /// under world point (cx, cy), as the cursor's hover finds things (clientOnCursorHover): enabled, its
    /// shape stood up from the floor; the one furthest forward wins.
    struct Useable {
        std::uint32_t extent = 0; ///< its net code is extent | 0x8000
        float x = 0;
        float y = 0;
        float depth = 0;          ///< hoverDepth: bigger is further forward
        const TypeArt* art = nullptr;
    };
    /// With `sight`, only those its drawable sight test sees (as the hover tests each).
    std::optional<Useable> useableAt(float cx, float cy, const SightMap* sight = nullptr) const;
    /// The whole view's ambient light with its centre at (x, y), called once a frame, as the game's
    /// polygonDrawColor works it out: when the centre (whole units) has moved, or nothing is
    /// known yet, the polygon it's in (the one it was in last tested first, then the rest in list
    /// order: of overlapping polygons the one entered first keeps it) gives its ambient on
    /// entering it; outside them all (or with none), the map's AmbientData (25, 25, 25 without
    /// one: the level switch's). It switches at once, no fade. Remembers the polygon it's in.
    Rgb ambientAt(float x, float y) const;
    /// The polygon (its place in the map's list + 1) whose ambient ambientAt last gave; 0 the
    /// map's own (outside every polygon), or nothing worked out yet.
    std::uint32_t ambientPolygon() const { return ambient_.polygon(); }
    /// The map editor's: the map's ambient and polygons replaced (no rebuild), the polygon the
    /// view is in forgotten. Null `ambient`: the map has no AmbientData.
    void setAmbient(const nox::map::AmbientData* ambient, const nox::map::Polygons& polygons);

    /// World-space centre of the tiled area, for the initial camera.
    SDL_FPoint centre() const { return centre_; }

    /// The images the map uses itself, for SpriteCache::beginMap (map_preload.cpp), most wanted
    /// first: its floor tiles and edges, its walls (every slot: the cut-away ones too, and window
    /// pieces), its doors (every angle), the scenery and objects it places (every frame of their
    /// draw), then its monsters and NPCs (every animation) with its generators' monsters.
    /// `map` is the file this view was made from: every object it places counts, the ones the
    /// server sends included.
    SpriteCache::MapImages preloadList(const nox::map::MapFile& map, ThingArt& art, SpriteCache& sprites,
                                       std::string name) const;
    /// The HD floor round `camera`'s view asked for from the sprite cache's workers, nothing
    /// drawn (while the map is held back, so its first frame has them). Returns whether any in
    /// view is still on its way.
    bool prefetchFloor(const Camera& camera, SpriteCache& sprites) const;
    /// Without the pack's workers: the original images round `camera`'s view made ahead (its
    /// floor tiles and their edge composites, then its walls, then the map's things), nothing
    /// drawn, until SDL_GetTicksNS() passes `untilNs`; while a live map is held back, so its
    /// first frame doesn't make them all at once (a join's long frame). Returns whether any is
    /// still to make.
    bool prefetchOriginals(const Camera& camera, SpriteCache& sprites, std::uint64_t untilNs) const;

private:
    struct Tile {
        float x = 0; ///< top-left, world units
        float y = 0;
        std::uint8_t floor = 0;
        std::int16_t variant = 0;
        std::vector<nox::map::FloorEdge> edges;
        std::string key; ///< cache key of the composited tile when it has edges (or a pending look)
        std::uint8_t look = 0; ///< a session's pending look (setPending's tiles): composited greyed
        // Worked out on its first draw, for the draws after (drawTiles, hdTile): its floor image's
        // index and each edge's floor and edge image's (-1: none; kUnresolved: not yet), the HD
        // composite's key at the scale it was made for, and where the sprite cache last found its
        // composites.
        static constexpr std::int32_t kUnresolved = -2;
        mutable std::int32_t baseIndex = kUnresolved;
        mutable std::vector<std::array<std::int32_t, 2>> edgeIndexes;
        mutable std::string hdKey;
        mutable int hdKeyScale = 0;
        mutable SpriteCache::CompositeHint hdHint;
        mutable SpriteCache::CompositeHint sdHint;
    };
    /// The tile's image indexes (Tile::baseIndex, edgeIndexes), worked out on its first call.
    void resolveTile(const Tile& tile, SpriteCache& sprites) const;

    /// PressurePlateDraw: no image, the plate's box outlined twice, the second pass a
    /// pixel to the right in the second colour; nothing if both colours are black.
    struct PlateOutline {
        float width = 0; ///< the box (TriggerXfer's size)
        float height = 0;
        std::array<Rgb, 2> colors{Rgb{90, 90, 90}, Rgb{10, 10, 10}};
    };

    struct Object {
        float x = 0;
        float y = 0;
        std::uint32_t extent = 0; ///< map-unique number; the server's net code for it
        const TypeArt* art = nullptr;
        std::optional<SpriteCache::Materials> colours; ///< weapons and armour (and, ObjectSet::Editor, maidens)
        /// ObjectSet::Editor's NPCs: NewPlayer's layers in its colours (lookLayers), drawn in its
        /// image's place; empty for everything else.
        std::vector<std::pair<nox::things::ImageRef, std::optional<SpriteCache::Materials>>> layers;
        std::optional<std::uint8_t> doorAngle;         ///< doors: 0-31 (DoorXfer, then MSG_DOOR_ANGLE)
        std::uint8_t lock = 0;                         ///< doors: DoorXfer's lock (1 silver key)
        bool lockMark = false;                         ///< doors: locked at load till MSG_GAUNTLET 0x0F
        bool enabled = true;                           ///< hidden while off if its type is VISIBLE_ENABLE
        std::optional<std::uint8_t> frame;             ///< SlaveDraw's image, as the server set it
        std::uint32_t xStatus = 0;                     ///< AnimateStateDraw's state, as the server set it
        double xStatusSince = 0;                       ///< game frame it last changed (its animation's start)
        std::optional<PlateOutline> plate;             ///< a pressure plate, drawn as an outline
        /// Its Fade Objects marks (client/fade.hpp): the client's own map objects are drawables
        /// too, and the scene's sort lingers them out of sight the same way.
        mutable nox::client::FadeState fade;
        /// The generic draw's last image of it (its sight test's box).
        mutable nox::client::DrawnImage drawn;
        std::uint8_t look = 0; ///< a session's pending look (setPending's things)
    };
    /// The drawable sight test for the map's object `index` (SightMap::drawableSeen); seen
    /// without `sight`.
    bool objectSeen(std::size_t index, const SightMap* sight) const;
    void drawPlate(SDL_Renderer* renderer, const Camera& camera, const Object& o) const;

    /// `hd`: the pack's upscaled tiles where it has them (drawFloor: not below sdFloorBelow_).
    void drawTiles(SDL_Renderer* renderer, const Camera& camera, SpriteCache& sprites, bool hd = true) const;
    /// Draws a sprite whose origin (before its own offset) is at world (x, y), clipped to world
    /// columns [clipLeft, clipRight]. `look`: a session's pending look (its alpha, and the pending
    /// shader or, without it, the CPU's copy of video.bag image `image`).
    void drawSprite(SDL_Renderer* renderer, SpriteCache& sprites, const Camera& camera, const Sprite& s, float x,
                    float y, Rgb light, float clipLeft = -1e9f, float clipRight = 1e9f, std::uint8_t alpha = 255,
                    bool bloom = false, float clipBottom = 1e9f, std::uint8_t look = 0, std::int32_t image = -1) const;
    /// Without line of sight: whether a wall is a front wall seen from (eyeX, eyeY), drawn last and
    /// translucent (drawBlack's rule, for every wall in view). With it, the sight says.
    bool frontWall(const nox::map::Wall& w, float eyeX, float eyeY) const;
    void addMapLight(const TypeArt& art, const nox::map::ObjectXfer& xfer, std::size_t recordSize, bool clientLoaded);
    const Sprite* edgedTile(const Tile& tile, SpriteCache& sprites) const;
    /// The tile at the pack's upscaled floors' resolution, or nullptr (map_view.cpp); `waiting`
    /// (if given) set when that's because its pixels or composite are on their way.
    const Sprite* hdTile(const Tile& tile, SpriteCache& sprites, bool* waiting = nullptr) const;
    const nox::video::DecodedImage* floorImage(std::uint8_t floor, std::int16_t variant, SpriteCache& sprites) const;
    /// A floor's image for a tile variant, or an edge's for its direction (thing.bin's FLOR /
    /// EDGE); nullptr past their lists.
    const nox::things::ImageRef* floorRef(std::uint8_t floor, std::int16_t variant) const;
    const nox::things::ImageRef* edgeRef(const nox::map::FloorEdge& e) const;

    /// The map's objects, lights and ambient sounds from its object sections (the constructor's,
    /// setObjects').
    void loadObjects(const nox::map::MapFile& map);

    const nox::things::ThingDb& things_;
    ThingArt* art_ = nullptr;
    const nox::things::ModifierDb* modifiers_ = nullptr;
    ObjectSet objectSet_ = ObjectSet::All;
    std::vector<Tile> tiles_;
    /// tiles_ by full-grid position ((gy + 1) << 9 | gx), made on the first setTile.
    std::unordered_map<std::int32_t, std::size_t> tileIndex_;
    std::vector<nox::map::Wall> walls_;     ///< as they stand (setWallState)
    std::unordered_map<std::uint16_t, std::size_t> wallByKey_; ///< walls_ by wallKey (the last there), with them
    std::vector<nox::map::Wall> mapWalls_;  ///< as the map places them
    std::vector<std::uint16_t> breakable_;  ///< DestructableWalls cells (wallKey), by wall id
    std::vector<std::pair<std::uint16_t, bool>> secret_; ///< SecretWalls cells and "starts open", by id
    std::set<std::uint16_t> windows_;
    std::vector<Object> objects_;
    std::map<std::uint32_t, std::size_t> objectIndex_; ///< objects_ by extent
    std::vector<Door> doors_; ///< the doors among objects_, as they stand (listDoors)
    /// doors_ made again from objects_ (at load, and when setDoorAngles turns one).
    void listDoors();
    std::vector<MapLight> lights_;
    std::vector<SoundSource> sounds_;
    /// What the per-frame calls go through instead of every object (indexObjects, at load):
    /// objects_' doors (setDoorAngles), sight blockers (SHADOW: sightBlockers) and USEABLE ones
    /// (useableAt), by index in objects_' order; lights_ by extent (the first with it) and sounds_
    /// by extent (setStatics).
    std::vector<std::size_t> doorObjects_;
    std::vector<std::size_t> shadowObjects_;
    std::vector<std::size_t> useableObjects_;
    std::unordered_map<std::uint32_t, std::size_t> lightByExtent_;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> soundsByExtent_;
    void indexObjects();
    /// The view's ambient from the map's AmbientData and polygons (polygonDrawColor; polygon_ambient.hpp).
    mutable PolygonAmbient ambient_;
    mutable std::bitset<65536> seenWalls_; ///< walls drawn on screen so far (by wallKey)
    bool collectBloom_ = false;
    bool batchImages_ = false; ///< setBatchImages
    float sdFloorBelow_ = 0;   ///< setSdFloorBelow
    double floorMakeBudgetMs_ = 0; ///< setFloorMakeBudget
    /// batchImages_: the images made and the view at the last drawScene's end (its next makes
    /// them first only if either has changed).
    mutable std::size_t preparedMade_ = ~std::size_t{0};
    mutable std::array<float, 4> preparedView_{};
    bool questLocks_ = false;
    /// A quest door's lock images: silver SW and SE, then gold SW and SE (null: not in thing.bin).
    std::array<const nox::things::ImageRef*, 4> lockImages_{};
    double time_ = 0;
    bool fadeObjects_ = false;
    bool translucentFrontWalls_ = true;
    mutable std::vector<BloomSprite> bloom_; ///< drawScene's light-source sprites this frame
    /// drawScene's sorted list: a see-through wall, an object or a scene sprite (with a lingering
    /// drawable's fade over its own alpha, -1 its own), or a session's pending wall.
    struct SceneItem {
        float sortY;
        const nox::map::Wall* wall;
        const Object* object;
        const SceneSprite* extra;
        std::int16_t alpha = -1;
        const PendingWall* pendingWall = nullptr;
    };
    // drawScene's lists, kept between frames for their memory.
    mutable std::vector<SceneItem> sceneItems_;
    mutable std::vector<const nox::map::Wall*> backWalls_;
    mutable std::vector<const nox::map::Wall*> frontWalls_;
    // drawTiles' lists, likewise.
    mutable std::vector<std::pair<const Tile*, const Sprite*>> deferredTiles_;
    mutable std::vector<const Tile*> missedTiles_;
    mutable std::vector<const Tile*> soonTiles_;
    mutable std::vector<std::uint32_t> drawnMarkers_; ///< drawScene's obelisks drawn (drawnMarkers)
    SDL_Texture* floorTarget_ = nullptr;
    int floorTargetWidth_ = 0;
    int floorTargetHeight_ = 0;
    SDL_FPoint centre_{};

    // A session's pending edits (setPending): all empty outside one.
    std::uint64_t serial_ = 0;
    mutable std::unordered_set<std::uint16_t> pendingSeen_;          ///< decorations drawn in sight so far (wallKey)
    bool pendingActive_ = false;
    std::unordered_set<std::uint16_t> pendingHideWalls_;           ///< our walls not drawn (wallKey)
    std::unordered_map<std::uint16_t, std::uint8_t> pendingWallLook_; ///< our walls drawn in a look
    std::vector<PendingWall> pendingWalls_;                          ///< decorations of the plan's own
    std::unordered_set<std::int32_t> pendingHideTiles_;             ///< our tiles not drawn (by full-grid key)
    std::vector<Tile> pendingTiles_;
    std::vector<Object> pendingObjects_;
    std::vector<nox::mapedit::PendingPlan::Object> pendingHidePlan_; ///< our things not drawn, by type and place
    std::vector<char> pendingHideObject_;                            ///< ... by objects_ index (empty: none)
    /// pendingHideObject_ worked out again from pendingHidePlan_ (setPending, setObjects).
    void matchPendingObjects();
};

/// The images any map can need, for SpriteCache::beginSession (map_preload.cpp), most wanted
/// first: the players' bodies, armour and weapons (every state, direction and frame), the things
/// carried or thrown (weapons, armour, missiles and spell effects, items and pickups: every frame
/// of their draw, their icons), thing.bin's named images and animations, the spell and ability
/// icons, then the interface's art (the pack's HD images nothing in thing.bin names, smallest
/// first, the loading screens left out: noxsdl doesn't show them). `counts` (if given) gets how many
/// of each, for the log.
SpriteCache::MapImages sessionImages(const nox::things::ThingDb& things, ThingArt& art, SpriteCache& sprites,
                                     std::string* counts = nullptr);

} // namespace noxsdl
