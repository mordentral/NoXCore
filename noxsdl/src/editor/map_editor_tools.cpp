// The map editor's wall, floor and edge tools: their mouse, strokes previewed live on the document
// and committed as one op, the view updated in place, the tool options and wall properties panels,
// the edge rules window, the eyedropper, and the pickers' thumbnails.
#include "editor/map_editor.hpp"

#include "editor/editor_text.hpp"
#include "editor/marker_tools.hpp"
#include "editor/object_tools.hpp"
#include "editor/panel_textures.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"
#include "sprite_cache.hpp"

#include "noxcore/mapedit/writer.hpp"
#include "noxcore/things/thing_db.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace me = nox::mapedit;

namespace noxsdl {

namespace {

using editor::help;
using editor::texId;

bool containsNoCase(std::string_view hay, std::string_view needle)
{
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != hay.end();
}

/// The wall cell (x + y even) nearest a world point: the cell under it, or the side neighbour it
/// leans towards.
std::optional<me::Cell> evenCellAt(float x, float y)
{
    const auto c = me::wallCellAt(x, y);
    if (!c) return std::nullopt;
    if (((c->x + c->y) & 1) == 0) return c;
    const float fx = x / me::kWallCellSize - static_cast<float>(c->x) - 0.5f;
    const float fy = y / me::kWallCellSize - static_cast<float>(c->y) - 0.5f;
    int nx = c->x, ny = c->y;
    if (std::abs(fx) > std::abs(fy)) nx += fx > 0 ? 1 : -1;
    else ny += fy > 0 ? 1 : -1;
    if (nx < 0 || ny < 0 || nx > 254 || ny > 255) return std::nullopt;
    return me::Cell{static_cast<std::uint8_t>(nx), static_cast<std::uint8_t>(ny)};
}

/// The diagonal steps from a to b (walls join only diagonally): each step moves x and y by one.
std::vector<me::Cell> diagonalPath(me::Cell a, me::Cell b)
{
    std::vector<me::Cell> out;
    int x = a.x, y = a.y;
    for (int guard = 0; (x != b.x || y != b.y) && guard < 600; ++guard) {
        int dx = b.x > x ? 1 : b.x < x ? -1 : 0;
        int dy = b.y > y ? 1 : b.y < y ? -1 : 0;
        if (dx == 0) dx = (guard & 1) ? 1 : -1; // (straight up or down: a zigzag)
        if (dy == 0) dy = (guard & 1) ? 1 : -1;
        x += dx;
        y += dy;
        if (x < 0 || y < 0 || x > 254 || y > 255) break;
        out.push_back({static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)});
    }
    return out;
}

constexpr const char* kFacingNames[13] = {"0  SW-NE",      "1  NW-SE",      "2  cross",      "3  T (NW NE SW)", "4  T (NW NE SE)",
                                          "5  T (NE SE SW)", "6  T (NW SW SE)", "7  corner NW NE", "8  corner NE SE", "9  corner SE SW",
                                          "10 corner NW SW", "11 window SW-NE", "12 window NW-SE"};
constexpr const char* kShapeNames[12] = {"S point", "SW side", "W point", "SE side", "NW side", "E point",
                                         "NE side", "N point", "SE+SW",   "SW+NW",   "NE+NW",   "NE+SE"};

} // namespace

// --- Setup and settings ------------------------------------------------------------------------

void MapEditor::initTools()
{
    rules_ = me::EdgeRules::defaults(things_);
    const auto findFloor = [&](std::string_view name, int fallback) {
        for (std::size_t i = 0; i < things_.floors.size(); ++i) {
            if (things_.floors[i].name == name) return static_cast<int>(i);
        }
        return fallback;
    };
    brush_.floor = static_cast<std::uint8_t>(findFloor("DirtDark2", 0));
    edgeFloor_ = brush_.floor;
    for (std::size_t i = 0; i < things_.edges.size(); ++i) {
        if (things_.edges[i].name == "BlendEdge") edgeSet_ = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < things_.walls.size(); ++i) {
        if (things_.walls[i].name.find("Stone") != std::string::npos && me::wallVariantCount(things_, static_cast<int>(i), 0) > 1) {
            wallMaterial_ = static_cast<int>(i);
            break;
        }
    }
    wallVariantMax_ = std::max(0, me::wallRandomVariations(things_, wallMaterial_) - 1);
    wallSecretProps_ = me::defaultSecretWall({});
}

void MapEditor::loadPaintSettings()
{
    rules_ = me::EdgeRules::defaults(things_);
    paint_ = {};
    if (const auto text = sidecar_.text("paint")) {
        paint_ = me::PaintSettings::fromText(*text);
        rules_.applyText(paint_.rulesText, things_);
    }
}

void MapEditor::storePaintSettings()
{
    paint_.rulesText = rules_.toText(things_);
    if (paint_.rulesText.empty() && paint_.customEdges.empty() && paint_.edgeSeed == 0) {
        sidecar_.other.erase("paint");
    } else {
        sidecar_.setText("paint", paint_.toText());
    }
}

me::WallBrush MapEditor::wallBrush() const
{
    me::WallBrush b;
    b.material = static_cast<std::uint8_t>(wallMaterial_);
    b.facing = static_cast<std::uint8_t>(wallFacing_ > 10 ? wallFacing_ - 11 : wallFacing_);
    b.autoFacing = wallMode_ != WallMode::Place && wallAutoFacing_;
    b.smartDraw = wallSmart_ || ImGui::GetIO().KeyShift;
    b.autoVariation = wallAutoVariation_;
    b.variantMin = wallVariantMin_;
    b.variantMax = std::max(wallVariantMin_, wallVariantMax_);
    if (!wallGroupAuto_) b.minimapGroup = static_cast<std::uint8_t>(std::clamp(wallGroup_, 0, 255));
    b.window = wallWindow_ || wallFacing_ >= 11;
    b.destructible = wallBreakable_;
    if (wallSecret_) b.secret = wallSecretProps_;
    b.replace = wallReplace_;
    b.seed = strokeSeed_;
    return b;
}

me::AutoEdgeOptions MapEditor::edgeOptions() const
{
    me::AutoEdgeOptions o;
    o.wallBlock = floorWallBlock_;
    if (floorOnTop_) o.onTop = brush_.floor;
    if (floorExclude_ >= 0) o.excludeFloor = floorExclude_;
    if (floorOnly_ >= 0) o.onlyFloor = floorOnly_;
    if (floorCover_ >= 0) o.coverFloor = floorCover_;
    if (floorHoleTrim_ >= 0) o.holeTrim = floorHoleTrim_;
    o.seed = paint_.edgeSeed;
    return o;
}

me::FloorBrush MapEditor::floorBrush() const
{
    me::FloorBrush b = brush_;
    b.edges = edgeOptions();
    b.seed = strokeSeed_;
    return b;
}

// --- The view ------------------------------------------------------------------------------------

void MapEditor::viewTiles(const std::vector<me::TileCell>& tiles)
{
    if (!view_ || !settings_.layers.floor) return;
    for (const auto t : tiles) {
        const auto* tile = doc_.floor.tile(t.gx, t.gy);
        if (tile && !settings_.layers.edges && !tile->edges.empty()) {
            nox::map::FloorTile bare = *tile;
            bare.edges.clear();
            view_->setTile(t.gx, t.gy, &bare);
        } else {
            view_->setTile(t.gx, t.gy, tile);
        }
    }
}

void MapEditor::viewWalls()
{
    if (!view_) return;
    std::vector<nox::map::Wall> walls;
    if (settings_.layers.walls) {
        walls.reserve(doc_.walls.size());
        for (const auto& [c, w] : doc_.walls) walls.push_back(w);
    }
    // (No secret wall hidden as open: the editor shows every wall.)
    view_->setWalls(walls, doc_.windowWalls, doc_.destructableWalls, nox::map::SecretWalls{});
}

void MapEditor::viewAfterOp(const me::Op& op)
{
    if (!view_ || viewBuiltFor_ == ~0ull) return;
    std::vector<me::TileCell> tiles;
    bool walls = false;
    bool objects = false;
    bool ambient = false;
    for (const auto& part : op.parts) {
        if (const auto* t = std::get_if<me::SetTiles>(&part)) {
            for (const auto& c : t->tiles) tiles.push_back({c.gx, c.gy});
        } else if (std::holds_alternative<me::SetWalls>(part)) {
            walls = true;
        } else if (std::holds_alternative<me::SetAmbient>(part) || std::holds_alternative<me::SetPolygons>(part)) {
            ambient = true; // (the lighting preview's ambient: the map's, the polygons')
        } else if (std::holds_alternative<me::AddObjects>(part) || std::holds_alternative<me::RemoveObjects>(part) ||
                   std::holds_alternative<me::SetObjects>(part) || std::holds_alternative<me::MoveObjects>(part)) {
            objects = true;
        } else if (!std::holds_alternative<me::SetWaypoints>(part) && !std::holds_alternative<me::SetScriptSource>(part) &&
                   !std::holds_alternative<me::SetScript>(part) && !std::holds_alternative<me::SetGroups>(part) &&
                   !std::holds_alternative<me::SetMapInfo>(part) && !std::holds_alternative<me::SetIntro>(part) &&
                   !std::holds_alternative<me::SetDebug>(part)) {
            return; // (anything else: the view is made again)
        }
    }
    viewTiles(tiles);
    if (walls) viewWalls();
    if (objects) viewObjects();
    if (ambient) viewAmbient();
    viewBuiltFor_ = history_.revision() ^ (layerRevision_ << 40);
}

void MapEditor::viewAmbient()
{
    if (!view_) return;
    // As the document writes them: AmbientData only when the map lists it.
    const bool listed = std::ranges::find(doc_.sections, std::string(nox::map::AmbientData::kName)) != doc_.sections.end();
    view_->setAmbient(listed ? &doc_.ambient : nullptr, doc_.polygons);
}

void MapEditor::viewObjects()
{
    if (!view_) return;
    // The objects alone written as a map's object sections (a few ms on a stock map), drawn again;
    // the floor's composites and the walls stay.
    me::MapDocument objectsOnly;
    objectsOnly.sections = {"ObjectTOC", "ObjectData"};
    if (settings_.layers.objects) {
        objectsOnly.objects = doc_.objects;
        objectsOnly.nextObjId = doc_.nextObjId;
        objectsOnly.storedToc = doc_.storedToc;
    }
    try {
        view_->setObjects(me::toMapFile(objectsOnly, things_));
    } catch (const std::exception& ex) {
        status(std::string("The objects can't be drawn: ") + ex.what());
    }
}

me::Op MapEditor::withWallEdges(me::Op op) const
{
    return wallEdges_ ? me::withEdgesAtWalls(doc_, things_, rules_, edgeOptions(), std::move(op), paint_.customEdges) : op;
}

// --- Mouse ---------------------------------------------------------------------------------------

me::KindFilter MapEditor::pickFilter() const
{
    const editor::Layers& L = settings_.layers;
    me::KindFilter f = settings_.pick;
    f.objects = f.objects && !L.lockObjects;
    f.walls = f.walls && !L.lockWalls;
    f.tiles = f.tiles && !L.lockFloor;
    f.waypoints = f.waypoints && !L.lockWaypoints;
    return f;
}

bool MapEditor::toolInput(const me::WorldPoint& w, bool hovered)
{
    // A locked layer's tool does nothing (the layer shows, but stays as it is).
    const editor::Layers& L = settings_.layers;
    const bool locked = (tool_ == Tool::Wall && L.lockWalls) || ((tool_ == Tool::Floor || tool_ == Tool::Edge) && L.lockFloor) ||
                        (tool_ == Tool::Object && L.lockObjects) || (tool_ == Tool::Waypoint && L.lockWaypoints) ||
                        (tool_ == Tool::Polygon && L.lockPolygons);
    if (locked) {
        if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || toolRightClick_)) status("That layer is locked (Layers panel)");
        toolRightClick_ = false;
        return true;
    }
    switch (tool_) {
    case Tool::Wall: wallInput(w, hovered); return true;
    case Tool::Floor: floorInput(w, hovered); return true;
    case Tool::Edge: edgeInput(w, hovered); return true;
    case Tool::Object:
        objects_->input(w.x, w.y, hovered, toolRightClick_);
        toolRightClick_ = false;
        return true;
    case Tool::Waypoint:
    case Tool::Polygon:
        markers_->input(w.x, w.y, hovered, toolRightClick_);
        toolRightClick_ = false;
        return true;
    default: break;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        status("This tool comes in P2d (Select is 1)");
    }
    toolRightClick_ = false;
    return false;
}

void MapEditor::eyedropper(const me::WorldPoint& w, bool full)
{
    switch (tool_) {
    case Tool::Wall: {
        const auto c = me::wallCellAt(w.x, w.y);
        const auto st = c ? me::wallState(doc_, *c) : std::nullopt;
        if (!st) {
            status("Eyedropper: no wall there");
            return;
        }
        wallMaterial_ = st->wall.material;
        if (full) {
            wallFacing_ = st->wall.direction & 0x7F;
            wallVariantMin_ = wallVariantMax_ = st->wall.variant;
            wallAutoVariation_ = false;
            wallGroupAuto_ = false;
            wallGroup_ = st->wall.minimapGroup;
            wallWindow_ = st->window;
            wallBreakable_ = st->destructible;
            wallSecret_ = st->secret.has_value();
            if (st->secret) wallSecretProps_ = *st->secret;
        }
        status("Eyedropper: " + things_.walls[wallMaterial_].name + (full ? " (facing, variant and kind too)" : ""));
        break;
    }
    case Tool::Floor: {
        const auto t = me::tileAt(w.x, w.y);
        const auto* tile = t ? doc_.floor.tile(t->gx, t->gy) : nullptr;
        if (!tile) {
            status("Eyedropper: no floor there");
            return;
        }
        brush_.floor = tile->image;
        if (full) {
            brush_.variants = me::FloorBrush::Variants::Fixed;
            brush_.fixedVariant = tile->variant;
        }
        status("Eyedropper: " + things_.floors[tile->image].name + (full ? " variant " + std::to_string(tile->variant) : ""));
        break;
    }
    case Tool::Edge: {
        const auto t = me::tileAt(w.x, w.y);
        const auto* tile = t ? doc_.floor.tile(t->gx, t->gy) : nullptr;
        if (!tile || tile->edges.empty()) {
            status("Eyedropper: no edge there");
            return;
        }
        const auto& e = tile->edges.back();
        edgeSet_ = e.edge;
        edgeDirection_ = e.direction;
        edgeFloor_ = e.floor;
        status("Eyedropper: " + things_.edges[e.edge].name + " direction " + std::to_string(e.direction));
        break;
    }
    default: status("The eyedropper works with the wall, floor and edge tools"); break;
    }
}

void MapEditor::wallPreview(const std::vector<me::Cell>& cells, bool erase)
{
    if (wallPreviewUndo_) {
        me::applyOp(doc_, *wallPreviewUndo_);
        wallPreviewUndo_.reset();
    }
    const me::Op op = erase ? me::removeWallsOp(doc_, things_, cells, wallReface_) : me::placeWallsOp(doc_, things_, cells, wallBrush());
    if (!op.empty()) {
        try {
            wallPreviewUndo_ = me::applyOp(doc_, op);
        } catch (const std::exception& ex) {
            status(std::string("Walls: ") + ex.what());
        }
    }
    viewWalls();
}

void MapEditor::endWallStroke(bool commit)
{
    if (wallPreviewUndo_) {
        me::applyOp(doc_, *wallPreviewUndo_);
        wallPreviewUndo_.reset();
    }
    const std::vector<me::Cell> cells = wallStroke_;
    const bool erase = wallErasing_;
    wallStroking_ = false;
    wallErasing_ = false;
    wallStroke_.clear();
    if (commit && !cells.empty()) {
        const me::Op op = withWallEdges(erase ? me::removeWallsOp(doc_, things_, cells, wallReface_) : me::placeWallsOp(doc_, things_, cells, wallBrush()));
        if (!perform(op)) viewWalls();
    } else {
        viewWalls();
    }
    ++strokeSeed_;
}

void MapEditor::wallInput(const me::WorldPoint& w, bool hovered)
{
    const ImGuiIO& io = ImGui::GetIO();
    const auto c = evenCellAt(w.x, w.y);
    if (toolRightClick_) {
        // Right click: the wall under the mouse removed, its neighbours re-faced.
        toolRightClick_ = false;
        if (const auto at = me::wallCellAt(w.x, w.y); at && doc_.walls.contains(*at)) {
            perform(withWallEdges(me::removeWallsOp(doc_, things_, {*at}, wallReface_)));
        }
        return;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyAlt) {
            eyedropper(w, io.KeyShift);
            return;
        }
        if (!c) return;
        switch (wallMode_) {
        case WallMode::Place: {
            me::WallBrush b = wallBrush();
            b.autoFacing = false;
            perform(withWallEdges(me::placeWallsOp(doc_, things_, {*c}, b)));
            ++strokeSeed_;
            return;
        }
        case WallMode::Outline: {
            const auto t = me::tileAt(w.x, w.y);
            const auto ring = t ? me::wallOutline(doc_, *t) : std::map<me::Cell, std::uint8_t>{};
            if (ring.empty()) {
                status("Outline: click inside a floor area bounded by holes or walls (an open map has no outline)");
                return;
            }
            me::Op op = withWallEdges(me::placeWallsOp(doc_, things_, me::cellsOf(ring), wallBrush(), &ring));
            op.label = "Wall outline (" + std::to_string(ring.size()) + " walls)";
            perform(op);
            ++strokeSeed_;
            return;
        }
        case WallMode::Pick: {
            me::Selection s = io.KeyShift ? selection_ : me::Selection{};
            if (const auto at = me::wallCellAt(w.x, w.y); at && doc_.walls.contains(*at)) {
                me::Selection one;
                one.walls.insert(*at);
                if (io.KeyShift) s.toggle(one);
                else s = one;
            }
            setSelection(std::move(s));
            return;
        }
        default: break;
        }
        wallStroking_ = true;
        wallErasing_ = wallMode_ == WallMode::Erase;
        wallStart_ = *c;
        wallStroke_ = {*c};
        wallPreview(wallStroke_, wallErasing_);
        return;
    }
    if (!wallStroking_) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (c) {
            bool grew = false;
            if (wallMode_ == WallMode::Line || wallMode_ == WallMode::Rect) {
                auto cells = wallMode_ == WallMode::Line ? me::wallLine(wallStart_, *c) : me::wallRectangle(wallStart_, *c);
                if (cells != wallStroke_) {
                    wallStroke_ = std::move(cells);
                    grew = true;
                }
            } else if (wallStroke_.empty() || *c != wallStroke_.back()) {
                // Dragged fast: the cells between filled in (the old editor's gap fill), joining diagonally.
                for (const auto step : diagonalPath(wallStroke_.empty() ? *c : wallStroke_.back(), *c)) {
                    if (std::ranges::find(wallStroke_, step) == wallStroke_.end()) wallStroke_.push_back(step);
                }
                grew = true;
            }
            if (grew) wallPreview(wallStroke_, wallErasing_);
        }
    } else {
        endWallStroke(true);
    }
}

void MapEditor::endFloorStroke(bool commit)
{
    if (!floorStroke_) return;
    std::vector<me::TileCell> restored;
    const me::Op op = commit ? floorStroke_->finish(&restored) : me::Op{};
    if (!commit) restored = floorStroke_->cancel();
    floorStroke_.reset();
    viewTiles(restored);
    if (commit && !op.empty()) perform(op);
    ++strokeSeed_;
}

void MapEditor::floorInput(const me::WorldPoint& w, bool hovered)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (toolRightClick_) {
        // Right click: the tile under the mouse removed (its neighbours' edges recomputed).
        toolRightClick_ = false;
        me::FloorBrush erase = floorBrush();
        erase.mode = me::FloorBrush::Mode::Erase;
        erase.size = 1;
        me::FloorStroke s(doc_, things_, rules_, erase, paint_.customEdges);
        s.dab(w);
        std::vector<me::TileCell> restored;
        const me::Op op = s.finish(&restored);
        perform(op);
        return;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyAlt) {
            eyedropper(w, io.KeyShift);
            return;
        }
        floorStroke_.emplace(doc_, things_, rules_, floorBrush(), paint_.customEdges);
        viewTiles(floorStroke_->dab(w));
        lastDab_ = w;
        return;
    }
    if (!floorStroke_) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Dabs along the drag, a quarter of the brush apart.
        const float spacing = std::max(8.0f, static_cast<float>(brush_.size) * 32.5f * 0.25f);
        const float dx = w.x - lastDab_.x, dy = w.y - lastDab_.y, d = std::hypot(dx, dy);
        if (d >= spacing) {
            const int n = static_cast<int>(d / spacing);
            std::vector<me::TileCell> changed;
            for (int i = 1; i <= n; ++i) {
                const float t = static_cast<float>(i) * spacing / d;
                const auto part = floorStroke_->dab({lastDab_.x + dx * t, lastDab_.y + dy * t});
                changed.insert(changed.end(), part.begin(), part.end());
            }
            lastDab_ = {lastDab_.x + dx * (static_cast<float>(n) * spacing / d), lastDab_.y + dy * (static_cast<float>(n) * spacing / d)};
            viewTiles(changed);
        }
    } else {
        endFloorStroke(true);
    }
}

void MapEditor::edgeInput(const me::WorldPoint& w, bool hovered)
{
    const ImGuiIO& io = ImGui::GetIO();
    const auto t = me::tileAt(w.x, w.y);
    const auto removeEdges = [&](me::TileCell at, bool allOfSet) {
        const auto* tile = doc_.floor.tile(at.gx, at.gy);
        if (!tile || tile->edges.empty()) return;
        nox::map::FloorTile next = *tile;
        if (allOfSet) {
            std::erase_if(next.edges, [&](const nox::map::FloorEdge& e) { return e.edge == edgeSet_; });
        } else {
            next.edges.pop_back();
        }
        if (next.edges == tile->edges) return;
        if (edgeMarkCustom_) paint_.customEdges.insert(at);
        perform(me::Op{allOfSet ? "Remove " + things_.edges[edgeSet_].name + " edges" : "Remove an edge", {me::SetTiles{{{at.gx, at.gy, next}}}}});
    };
    if (toolRightClick_) {
        toolRightClick_ = false;
        if (t) removeEdges(*t, io.KeyShift);
        return;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyAlt) {
            eyedropper(w, io.KeyShift);
            return;
        }
        if (!t) return;
        if (edgeMode_ == EdgeMode::Remove) {
            removeEdges(*t, io.KeyShift);
            return;
        }
        if (edgeMode_ == EdgeMode::Add) {
            const auto* tile = doc_.floor.tile(t->gx, t->gy);
            if (!tile) {
                status("No tile there to put an edge on");
                return;
            }
            const auto& set = things_.edges.at(static_cast<std::size_t>(edgeSet_));
            if (edgeDirection_ >= 2 * (set.rows + set.cols)) {
                status(set.name + " has no direction " + std::to_string(edgeDirection_));
                return;
            }
            nox::map::FloorTile next = *tile;
            nox::map::FloorEdge e;
            e.floor = static_cast<std::uint8_t>(edgeFloor_);
            e.variant = me::VariantAnchors::learn(doc_, things_).variant(things_, edgeFloor_, t->gx, t->gy);
            e.edge = static_cast<std::uint8_t>(edgeSet_);
            e.direction = static_cast<std::uint8_t>(edgeDirection_);
            if (std::ranges::find(next.edges, e) != next.edges.end()) {
                status("That edge is on the tile already");
                return;
            }
            next.edges.push_back(e);
            if (edgeMarkCustom_) paint_.customEdges.insert(*t);
            perform(me::Op{"Add a " + set.name + " edge", {me::SetTiles{{{t->gx, t->gy, next}}}}});
            return;
        }
        edgeStroke_ = {*t};
        return;
    }
    if (edgeStroke_.empty()) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (t) edgeStroke_.insert(*t);
    } else {
        // Auto: the tiles dragged over get auto-edge again (hand-placed marks on them let go).
        for (const auto c : edgeStroke_) paint_.customEdges.erase(c);
        perform(me::autoEdgeOp(doc_, things_, rules_, edgeOptions(), edgeStroke_, paint_.customEdges));
        edgeStroke_.clear();
    }
}

// --- Overlays ------------------------------------------------------------------------------------

void MapEditor::drawToolOverlay(ImDrawList* dl, const ImVec2& origin)
{
    const float z = camera_.zoom;
    const float left = camera_.left(), top = camera_.top();
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - left) * z, origin.y + (y - top) * z); };
    const auto diamond = [&](me::TileCell t, ImU32 c, float thick) {
        const me::WorldPoint p = me::tileCentre(t);
        const ImVec2 q[4] = {S(p.x, p.y - 23), S(p.x + 23, p.y), S(p.x, p.y + 23), S(p.x - 23, p.y)};
        dl->AddPolyline(q, 4, c, ImDrawFlags_Closed, thick);
    };
    // Special walls tinted: secret blue, breakable red, window cyan (the old editor fork's toggle).
    if (settings_.layers.specialWalls && settings_.layers.walls) {
        const auto cell = [&](int x, int y, ImU32 c) {
            dl->AddRectFilled(S(x * 23.0f + 3, y * 23.0f + 3), S(x * 23.0f + 20, y * 23.0f + 20), c, 3 * z);
        };
        for (const auto& s : doc_.secretWalls.walls) cell(s.pos.x, s.pos.y, IM_COL32(80, 140, 255, 90));
        for (const auto& p : doc_.destructableWalls.walls) cell(p.x, p.y, IM_COL32(255, 90, 70, 90));
        for (const auto& p : doc_.windowWalls.walls) cell(p.x, p.y, IM_COL32(90, 230, 230, 90));
    }
    objects_->drawOverlay(dl, origin); // (the object layers, the object tool's cursor)
    markers_->drawOverlay(dl, origin); // (the waypoint and polygon tools, group members)
    if (!mapHovered_) return;
    const auto mw = mouseWorld();
    if (!mw) return;
    if (tool_ == Tool::Floor) {
        const ImU32 c = brush_.mode == me::FloorBrush::Mode::Erase ? IM_COL32(255, 110, 90, 160) : IM_COL32(255, 230, 120, 140);
        me::FloorBrush b = floorBrush();
        if (b.mode == me::FloorBrush::Mode::Fill) b.size = 1;
        for (const auto t : me::brushFootprint(b, *mw)) diamond(t, c, 1.0f);
        dl->AddCircle(S(mw->x, mw->y), std::max(2.0f, static_cast<float>(brush_.size) * 16.26f * z), IM_COL32(255, 255, 255, 70), 0, 1.0f);
    } else if (tool_ == Tool::Edge) {
        if (const auto t = me::tileAt(mw->x, mw->y)) diamond(*t, IM_COL32(140, 220, 255, 200), 2.0f);
    } else if (tool_ == Tool::Wall) {
        if (const auto c = evenCellAt(mw->x, mw->y)) {
            const ImU32 col = wallMode_ == WallMode::Erase ? IM_COL32(255, 110, 90, 220) : IM_COL32(255, 230, 120, 220);
            dl->AddRect(S(c->x * 23.0f, c->y * 23.0f), S(c->x * 23.0f + 23, c->y * 23.0f + 23), col, 0, 0, 2.0f);
        }
    }
}

// --- Thumbnails and pickers ------------------------------------------------------------------------

SDL_Texture* MapEditor::thumb(const std::string& key, const std::function<std::vector<std::uint8_t>(int& w, int& h)>& build)
{
    if (const auto it = thumbs_.find(key); it != thumbs_.end()) return it->second;
    int w = 0, h = 0;
    const std::vector<std::uint8_t> rgba = build(w, h);
    SDL_Texture* t = nullptr;
    if (w > 0 && h > 0 && rgba.size() == static_cast<std::size_t>(w) * h * 4) {
        t = SDL_CreateTexture(ctx_.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (t) {
            SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            editor::panelTextureSet(t, rgba.data(), w, h, w * 4); // (for a floating panel's copy)
        }
    }
    thumbs_[key] = t;
    return t;
}

SDL_Texture* MapEditor::floorThumb(int floor, int variant)
{
    if (floor < 0 || static_cast<std::size_t>(floor) >= things_.floors.size()) return nullptr;
    return thumb("f" + std::to_string(floor) + "." + std::to_string(variant), [&](int& w, int& h) -> std::vector<std::uint8_t> {
        const auto& images = things_.floors[static_cast<std::size_t>(floor)].images;
        if (variant < 0 || static_cast<std::size_t>(variant) >= images.size()) return {};
        const auto* d = ctx_.sprites->decoded(ctx_.sprites->indexOf(images[static_cast<std::size_t>(variant)]));
        if (!d) return {};
        w = d->width;
        h = d->height;
        return d->rgba;
    });
}

SDL_Texture* MapEditor::wallThumb(int material, int facing, int variant)
{
    if (material < 0 || static_cast<std::size_t>(material) >= things_.walls.size() || facing < 0 || facing >= 15) return nullptr;
    return thumb("w" + std::to_string(material) + "." + std::to_string(facing) + "." + std::to_string(variant), [&](int& w, int& h) -> std::vector<std::uint8_t> {
        const auto& dir = things_.walls[static_cast<std::size_t>(material)].directions[static_cast<std::size_t>(facing)];
        if (variant < 0 || static_cast<std::size_t>(variant) >= dir.size()) return {};
        const auto* d = ctx_.sprites->decoded(ctx_.sprites->indexOf(dir[static_cast<std::size_t>(variant)][0].image));
        if (!d) return {};
        w = d->width;
        h = d->height;
        return d->rgba;
    });
}

SDL_Texture* MapEditor::edgeThumb(int edge, int direction, int overFloor, int baseFloor)
{
    if (edge < 0 || static_cast<std::size_t>(edge) >= things_.edges.size()) return nullptr;
    const std::string key = "e" + std::to_string(edge) + "." + std::to_string(direction) + "." + std::to_string(overFloor) + "." + std::to_string(baseFloor);
    return thumb(key, [&](int& w, int& h) -> std::vector<std::uint8_t> {
        const auto& set = things_.edges[static_cast<std::size_t>(edge)];
        if (direction < 0 || static_cast<std::size_t>(direction) >= set.images.size()) return {};
        const auto* mask = ctx_.sprites->decoded(ctx_.sprites->indexOf(set.images[static_cast<std::size_t>(direction)]));
        const auto image = [&](int floor) -> const nox::video::DecodedImage* {
            if (floor < 0 || static_cast<std::size_t>(floor) >= things_.floors.size() || things_.floors[static_cast<std::size_t>(floor)].images.empty()) return nullptr;
            const auto* d = ctx_.sprites->decoded(ctx_.sprites->indexOf(things_.floors[static_cast<std::size_t>(floor)].images[0]));
            return d && d->width == 46 && d->height == 46 ? d : nullptr;
        };
        const auto* base = image(baseFloor);
        const auto* over = image(overFloor);
        if (!mask || mask->edgeMask.size() != 46u * 46u) return {};
        w = h = 46;
        std::vector<std::uint8_t> out = base ? base->rgba : std::vector<std::uint8_t>(46u * 46u * 4, 0);
        for (std::size_t i = 0; i < mask->edgeMask.size(); ++i) {
            const std::uint8_t* src = mask->edgeMask[i] == 1 ? &mask->rgba[i * 4] : (mask->edgeMask[i] == 2 && over) ? &over->rgba[i * 4] : nullptr;
            if (src) std::copy(src, src + 4, out.begin() + static_cast<std::ptrdiff_t>(i * 4));
        }
        return out;
    });
}

bool MapEditor::floorPicker(const char* id, int& floor, bool allowNone)
{
    bool changed = false;
    ImGui::PushID(id);
    const float s = ImGui::GetFrameHeight() * 1.4f;
    if (floor >= 0) {
        if (SDL_Texture* t = floorThumb(floor, 0)) {
            ImGui::Image(texId(t), ImVec2(s, s));
            ImGui::SameLine();
        }
    }
    const std::string label = floor >= 0 && static_cast<std::size_t>(floor) < things_.floors.size() ? things_.floors[static_cast<std::size_t>(floor)].name : "(none)";
    const float browseW = ImGui::GetFrameHeight() * 1.6f; // (the content browser's folder: its button)
    ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##pick", label.c_str(), ImGuiComboFlags_HeightLargest)) {
        if (ImGui::IsWindowAppearing()) {
            pickerFilter_[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "filter", pickerFilter_, sizeof pickerFilter_);
        if (allowNone && ImGui::Selectable("(none)", floor < 0)) {
            floor = -1;
            changed = true;
        }
        for (std::size_t i = 0; i < things_.floors.size(); ++i) {
            const auto& f = things_.floors[i];
            if (!containsNoCase(f.name, pickerFilter_)) continue;
            ImGui::PushID(static_cast<int>(i));
            if (SDL_Texture* t = floorThumb(static_cast<int>(i), 0)) ImGui::Image(texId(t), ImVec2(s, s));
            else ImGui::Dummy(ImVec2(s, s));
            ImGui::SameLine();
            const std::string text = f.name + "  (" + std::to_string(f.cols) + "x" + std::to_string(f.rows) + (f.frames > 1 ? ", animated" : "") + ")";
            if (ImGui::Selectable(text.c_str(), static_cast<int>(i) == floor, 0, ImVec2(0, s))) {
                floor = static_cast<int>(i);
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("...##browse", ImVec2(browseW, 0))) objects_->browse("Floors");
    help("Pick from the Content Browser's Floors folder, with thumbnails.");
    ImGui::PopID();
    return changed;
}

bool MapEditor::edgePicker(const char* id, int& edge)
{
    bool changed = false;
    ImGui::PushID(id);
    const float s = ImGui::GetFrameHeight() * 1.4f;
    const auto sample = [&](int e) {
        const auto& set = things_.edges[static_cast<std::size_t>(e)];
        const auto dirs = me::edgeDirections(set.rows, set.cols, me::kEdgeSESW);
        return edgeThumb(e, dirs.empty() ? 0 : dirs.front(), brush_.floor, edgeFloor_ == brush_.floor ? -1 : edgeFloor_);
    };
    if (edge >= 0 && static_cast<std::size_t>(edge) < things_.edges.size()) {
        if (SDL_Texture* t = sample(edge)) {
            ImGui::Image(texId(t), ImVec2(s, s));
            ImGui::SameLine();
        }
    }
    const std::string label = edge >= 0 && static_cast<std::size_t>(edge) < things_.edges.size() ? things_.edges[static_cast<std::size_t>(edge)].name : "(none)";
    const float browseW = ImGui::GetFrameHeight() * 1.6f; // (the content browser's folder: its button)
    ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##pick", label.c_str(), ImGuiComboFlags_HeightLargest)) {
        if (ImGui::IsWindowAppearing()) {
            pickerFilter_[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "filter", pickerFilter_, sizeof pickerFilter_);
        for (std::size_t i = 0; i < things_.edges.size(); ++i) {
            const auto& e = things_.edges[i];
            if (!containsNoCase(e.name, pickerFilter_)) continue;
            ImGui::PushID(static_cast<int>(i));
            if (SDL_Texture* t = sample(static_cast<int>(i))) ImGui::Image(texId(t), ImVec2(s, s));
            else ImGui::Dummy(ImVec2(s, s));
            ImGui::SameLine();
            const std::string text = e.name + "  (" + std::to_string(2 * (e.rows + e.cols)) + " shapes)";
            if (ImGui::Selectable(text.c_str(), static_cast<int>(i) == edge, 0, ImVec2(0, s))) {
                edge = static_cast<int>(i);
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("...##browse", ImVec2(browseW, 0))) objects_->browse("Edges");
    help("Pick from the Content Browser's Edges folder, with thumbnails.");
    ImGui::PopID();
    return changed;
}

bool MapEditor::wallPicker(const char* id, int& material)
{
    bool changed = false;
    ImGui::PushID(id);
    const float s = ImGui::GetFrameHeight() * 1.8f;
    const auto sample = [&](int m) { return wallThumb(m, 0, 0) ? wallThumb(m, 0, 0) : wallThumb(m, 1, 0); };
    const auto image = [&](SDL_Texture* t) {
        float w = 0, h = 0;
        SDL_GetTextureSize(t, &w, &h);
        const float k = std::min(s / std::max(w, 1.0f), s / std::max(h, 1.0f));
        ImGui::Image(texId(t), ImVec2(w * k, h * k));
    };
    if (material >= 0 && static_cast<std::size_t>(material) < things_.walls.size()) {
        if (SDL_Texture* t = sample(material)) {
            image(t);
            ImGui::SameLine();
        }
    }
    const std::string label = material >= 0 && static_cast<std::size_t>(material) < things_.walls.size() ? things_.walls[static_cast<std::size_t>(material)].name : "?";
    const float browseW = ImGui::GetFrameHeight() * 1.6f; // (the content browser's folder: its button)
    ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##pick", label.c_str(), ImGuiComboFlags_HeightLargest)) {
        if (ImGui::IsWindowAppearing()) {
            pickerFilter_[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "filter", pickerFilter_, sizeof pickerFilter_);
        for (std::size_t i = 0; i < things_.walls.size(); ++i) {
            const auto& w = things_.walls[i];
            if (!containsNoCase(w.name, pickerFilter_)) continue;
            ImGui::PushID(static_cast<int>(i));
            if (SDL_Texture* t = sample(static_cast<int>(i))) image(t);
            else ImGui::Dummy(ImVec2(s, s));
            ImGui::SameLine();
            if (ImGui::Selectable(w.name.c_str(), static_cast<int>(i) == material, 0, ImVec2(0, s))) {
                material = static_cast<int>(i);
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("...##browse", ImVec2(browseW, 0))) objects_->browse("Walls");
    help("Pick from the Content Browser's Walls folder, with thumbnails.");
    ImGui::PopID();
    return changed;
}

// --- Panels ------------------------------------------------------------------------------------------

void MapEditor::wallToolPanel()
{
    ImGui::SeparatorText("Wall");
    const struct {
        WallMode mode;
        const char* label;
        const char* tip;
    } modes[] = {
        {WallMode::Draw, "Draw", "Drag to draw walls; each takes its facing from its neighbours (auto-facing). Shift: Smart Draw."},
        {WallMode::Place, "Place", "Click to place one wall with the exact facing chosen below; its neighbours are left alone."},
        {WallMode::Line, "Line", "Drag a straight wall line (along a diagonal)."},
        {WallMode::Rect, "45\xC2\xB0 Rectangle", "Drag a rectangle with its sides along the diagonals."},
        {WallMode::Outline, "Outline", "Click inside a floor area: walls all round its edge (the bucket)."},
        {WallMode::Erase, "Erase", "Drag to remove walls; their neighbours are re-faced."},
        {WallMode::Pick, "Properties", "Click walls to edit them in Properties (Shift adds). Alt+click is the eyedropper in every mode."},
    };
    for (std::size_t i = 0; i < std::size(modes); ++i) {
        if (i % 4 != 0) ImGui::SameLine();
        if (ImGui::RadioButton(modes[i].label, wallMode_ == modes[i].mode)) wallMode_ = modes[i].mode;
        help(modes[i].tip);
    }
    ImGui::TextDisabled("Right click removes a wall; Alt+click picks (Shift: all its properties).");
    ImGui::SeparatorText("Material");
    if (wallPicker("wallmat", wallMaterial_)) {
        wallVariantMin_ = 0;
        wallVariantMax_ = std::max(0, me::wallRandomVariations(things_, wallMaterial_) - 1);
    }
    ImGui::SeparatorText("Facing");
    ImGui::Checkbox("Auto-facing", &wallAutoFacing_);
    help("Draw, line, rectangle and outline walls take the shape their neighbours give them, and neighbours are re-faced to join them.");
    ImGui::SameLine();
    ImGui::Checkbox("Smart Draw", &wallSmart_);
    help("Only this stroke's walls count as neighbours: walls already there neither shape it nor change (also: hold Shift).");
    // The facings' pictures (greyed where the material has none).
    const float s = ImGui::GetFrameHeight() * 2.0f;
    for (int f = 0; f < 13; ++f) {
        if (f % 5 != 0) ImGui::SameLine();
        ImGui::PushID(f);
        SDL_Texture* t = wallThumb(wallMaterial_, f, 0);
        const bool active = wallFacing_ == f;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        bool clicked = false;
        if (t) {
            clicked = ImGui::ImageButton("##f", texId(t), ImVec2(s, s), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("-", ImVec2(s + 2 * ImGui::GetStyle().FramePadding.x, s + 2 * ImGui::GetStyle().FramePadding.y));
            ImGui::EndDisabled();
        }
        if (active) ImGui::PopStyleColor();
        if (clicked) wallFacing_ = f;
        help(kFacingNames[f]);
        ImGui::PopID();
    }
    ImGui::TextDisabled("Facing %s%s", kFacingNames[wallFacing_], wallMode_ == WallMode::Place ? "" : " (for lone walls)");
    ImGui::SeparatorText("Variation");
    const int count = me::wallVariantCount(things_, wallMaterial_, wallFacing_ > 10 ? wallFacing_ - 11 : wallFacing_);
    ImGui::Checkbox("Auto variation", &wallAutoVariation_);
    help("Straight pieces get a variant picked in the range (seeded per stroke); corners and junctions take 0.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##vmin", &wallVariantMin_, 0, std::max(0, count - 1), "from %d");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##vmax", &wallVariantMax_, 0, std::max(0, count - 1), "to %d");
    wallVariantMin_ = std::clamp(wallVariantMin_, 0, std::max(0, count - 1));
    wallVariantMax_ = std::clamp(wallVariantMax_, wallVariantMin_, std::max(0, count - 1));
    const int game = me::wallRandomVariations(things_, wallMaterial_);
    ImGui::TextDisabled("%d variants for this facing; the game's own range %d", count, game);
    if (game > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Use it")) {
            wallVariantMin_ = 0;
            wallVariantMax_ = std::min(count, game) - 1;
        }
        help("The material's thing.bin variation count: the Wall spell's straight pieces take x mod it.");
    }
    ImGui::SeparatorText("Kind");
    ImGui::Checkbox("Window", &wallWindow_);
    help("Sight passes it. Straight pieces (facing 0 or 1) draw the window picture; others look plain.");
    ImGui::SameLine();
    ImGui::Checkbox("Breakable", &wallBreakable_);
    ImGui::SameLine();
    ImGui::Checkbox("Secret", &wallSecret_);
    if (wallSecret_) {
        bool open = wallSecretProps_.state == 3;
        if (ImGui::Checkbox("Starts open", &open)) me::setSecretStartOpen(wallSecretProps_, open);
        const struct {
            int bit;
            const char* label;
            const char* tip;
        } flags[] = {{2, "Opens when touched", "A player or monster walking into it opens it (and paths go through it)."},
                     {4, "Moves on its own", "After its wait it closes again (or, with the next flag, opens again)."},
                     {8, "Timer opens it", "With 'moves on its own': the timer opens it when shut, instead of closing it when open."},
                     {1, "Scripted", "Marked for scripts (no engine effect)."}};
        for (const auto& f : flags) {
            bool on = (wallSecretProps_.flags & f.bit) != 0;
            if (ImGui::Checkbox(f.label, &on)) wallSecretProps_.flags = static_cast<std::uint8_t>(on ? (wallSecretProps_.flags | f.bit) : (wallSecretProps_.flags & ~f.bit));
            help(f.tip);
        }
        int wait = static_cast<int>(wallSecretProps_.openWait);
        ImGui::SetNextItemWidth(120 * imgui_->scale());
        if (ImGui::InputInt("Wait (seconds)", &wait)) wallSecretProps_.openWait = static_cast<std::uint32_t>(std::clamp(wait, 0, 100000));
    }
    ImGui::SeparatorText("Placing");
    ImGui::Checkbox("Minimap group from polygons", &wallGroupAuto_);
    help("The polygon round the wall gives its minimap group (as the game's save does), else 100.");
    if (!wallGroupAuto_) {
        ImGui::SetNextItemWidth(120 * imgui_->scale());
        ImGui::InputInt("Minimap group", &wallGroup_);
        wallGroup_ = std::clamp(wallGroup_, 0, 255);
    }
    ImGui::Checkbox("Replace existing walls", &wallReplace_);
    ImGui::Checkbox("Recompute floor edges at walls", &wallEdges_);
    help("Placing or removing walls recomputes the auto-edges of the floor tiles the walls touch (wall block: no edge laps across a wall). Hand-edited tiles are kept.");
    ImGui::Checkbox("Re-face neighbours on erase", &wallReface_);
    help("A removed wall's neighbours take the shape of what they still join (the old editor left them as they were).");
}

void MapEditor::floorToolPanel()
{
    using Mode = me::FloorBrush::Mode;
    ImGui::SeparatorText("Floor brush");
    const struct {
        Mode mode;
        const char* label;
        const char* tip;
    } modes[] = {{Mode::Paint, "Paint", "Paint the floor; the edges round it follow (auto-edge)."},
                 {Mode::Erase, "Erase", "Remove tiles (black holes, as the stock maps' void)."},
                 {Mode::Replace, "Replace", "Paint only over tiles of the chosen floor."},
                 {Mode::Fill, "Fill", "Flood the clicked tile's floor (or hole) region, bounded by walls."},
                 {Mode::Smooth, "Smooth", "Each tile takes the floor most of its 3 x 3 window has: specks go."},
                 {Mode::EdgesOnly, "Edges only", "Recompute edges under the brush, floors unchanged."}};
    for (std::size_t i = 0; i < std::size(modes); ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        if (ImGui::RadioButton(modes[i].label, brush_.mode == modes[i].mode)) brush_.mode = modes[i].mode;
        help(modes[i].tip);
    }
    ImGui::TextDisabled("Right click removes a tile; Alt+click picks the floor (Shift: its variant). [ and ] size the brush.");
    int floor = brush_.floor;
    if (floorPicker("brushfloor", floor)) brush_.floor = static_cast<std::uint8_t>(floor);
    if (brush_.mode == Mode::Replace) {
        int rep = brush_.replaceFloor;
        ImGui::TextDisabled("Replacing:");
        if (floorPicker("replacefloor", rep)) brush_.replaceFloor = static_cast<std::uint8_t>(rep);
    }
    // Which way the brush's floor and the one under the mouse lap (the priority preview).
    if (const auto m = mouseWorld()) {
        if (const auto t = me::tileAt(m->x, m->y); t && doc_.floor.tile(t->gx, t->gy)) {
            const int under = doc_.floor.tile(t->gx, t->gy)->image;
            if (under != brush_.floor) {
                const auto over = rules_.lap(brush_.floor, under);
                const auto back = rules_.lap(under, brush_.floor);
                const std::string a = things_.floors[brush_.floor].name, b = things_.floors[static_cast<std::size_t>(under)].name;
                if (floorOnTop_ || over) ImGui::TextColored(editor::themeColours().ok, "%s laps over %s", a.c_str(), b.c_str());
                else if (back) ImGui::TextColored(editor::themeColours().warning, "%s will lap over what you paint", b.c_str());
                else ImGui::TextDisabled("%s and %s don't lap", a.c_str(), b.c_str());
            }
        }
    }
    ImGui::SeparatorText("Shape");
    int shape = static_cast<int>(brush_.shape);
    ImGui::RadioButton("Round", &shape, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Square", &shape, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Diamond", &shape, 2);
    help("Diamond: N x N tiles along the floor's diagonals (the old editor's brush).");
    brush_.shape = static_cast<me::FloorBrush::Shape>(shape);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##size", &brush_.size, 1, 16, "size %d tiles");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##hard", &brush_.hardness, 0.0f, 1.0f, "hardness %.2f");
    help("1: a crisp shape. Lower: a soft rim, which the noise makes ragged.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##noise", &brush_.noise, 0.0f, 1.0f, "noise %.2f");
    help("How ragged a soft rim is (seeded per stroke: the same stroke paints the same).");
    ImGui::SeparatorText("Variants");
    int variants = static_cast<int>(brush_.variants);
    ImGui::RadioButton("Aligned", &variants, 0);
    help("The texture block tiled along the diagonals, lined up with the map's own tiles of the floor (as the stock maps).");
    ImGui::SameLine();
    ImGui::RadioButton("Random", &variants, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Fixed", &variants, 2);
    brush_.variants = static_cast<me::FloorBrush::Variants>(variants);
    if (brush_.variants == me::FloorBrush::Variants::Fixed && brush_.floor < things_.floors.size()) {
        const auto& def = things_.floors[brush_.floor];
        const int n = std::max(1, def.cols * def.rows);
        const float s = ImGui::GetFrameHeight() * 1.2f;
        ImGui::BeginChild("##variants", ImVec2(0, std::min(6.0f, std::ceil(static_cast<float>(n) / std::max(1, static_cast<int>(def.cols)))) * (s + 8) + 8),
                          ImGuiChildFlags_Borders);
        for (int v = 0; v < n; ++v) {
            if (v % std::max(1, static_cast<int>(def.cols)) != 0) ImGui::SameLine();
            ImGui::PushID(v);
            SDL_Texture* t = floorThumb(brush_.floor, v);
            const bool active = brush_.fixedVariant == v;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            if (t ? ImGui::ImageButton("##v", texId(t), ImVec2(s, s)) : ImGui::Button("?", ImVec2(s, s))) brush_.fixedVariant = v;
            if (active) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::SeparatorText("Edges");
    ImGui::Checkbox("Auto-edge", &brush_.autoEdge);
    help("Off: tiles placed and removed with no edges touched (the old editor's Place/Remove).");
    ImGui::SameLine();
    ImGui::Checkbox("Wall block", &floorWallBlock_);
    help("No lap across a border a wall stands on.");
    ImGui::Checkbox("Painted floor on top", &floorOnTop_);
    help("This stroke's floor laps over its neighbours whatever the priorities say (the old brush's rule).");
    ImGui::TextDisabled("Exclude floor (its tiles keep their edges):");
    floorPicker("exclude", floorExclude_, true);
    ImGui::TextDisabled("Only lap floor (other edges stay):");
    floorPicker("only", floorOnly_, true);
    ImGui::TextDisabled("Cover with floor (laps show it):");
    floorPicker("cover", floorCover_, true);
    ImGui::TextDisabled("Trim next to holes:");
    if (floorHoleTrim_ < 0) {
        if (ImGui::SmallButton("Add a hole trim")) floorHoleTrim_ = 23 < static_cast<int>(things_.edges.size()) ? 23 : 0;
    } else {
        edgePicker("holetrim", floorHoleTrim_);
        if (ImGui::SmallButton("No hole trim")) floorHoleTrim_ = -1;
    }
    if (ImGui::Button("Edge Rules...")) showEdgeRules_ = true;
    help("Which floor laps over which, and with which edge (the defaults come from the stock maps).");
}

void MapEditor::edgeToolPanel()
{
    ImGui::SeparatorText("Edges");
    int mode = static_cast<int>(edgeMode_);
    ImGui::RadioButton("Auto", &mode, 0);
    help("Click or drag: the tiles get their auto-edges again (with the floor tool's edge options).");
    ImGui::SameLine();
    ImGui::RadioButton("Add", &mode, 1);
    help("Click a tile: the edge below goes on top of its edges.");
    ImGui::SameLine();
    ImGui::RadioButton("Remove", &mode, 2);
    help("Click a tile: its last edge goes (Shift: every edge of the set below).");
    edgeMode_ = static_cast<EdgeMode>(mode);
    ImGui::TextDisabled("Right click removes the last edge (Shift: all of the set); Alt+click picks one.");
    ImGui::TextDisabled("Edge set:");
    edgePicker("edgeset", edgeSet_);
    ImGui::TextDisabled("Floor blended in:");
    floorPicker("edgefloor", edgeFloor_);
    if (edgeSet_ >= 0 && static_cast<std::size_t>(edgeSet_) < things_.edges.size()) {
        const auto& set = things_.edges[static_cast<std::size_t>(edgeSet_)];
        const int n = 2 * (set.rows + set.cols);
        ImGui::TextDisabled("Direction (%d shapes, %dx%d):", n, set.rows, set.cols);
        const float s = ImGui::GetFrameHeight() * 1.6f;
        // The base shown under the masks: the tile under the mouse's floor, else the brush's.
        int base = brush_.floor;
        for (int d = 0; d < n; ++d) {
            if (d % 5 != 0) ImGui::SameLine();
            ImGui::PushID(d);
            SDL_Texture* t = edgeThumb(edgeSet_, d, edgeFloor_, base == edgeFloor_ ? -1 : base);
            const bool active = edgeDirection_ == d;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            if (t ? ImGui::ImageButton("##d", texId(t), ImVec2(s, s)) : ImGui::Button("?", ImVec2(s, s))) edgeDirection_ = d;
            if (active) ImGui::PopStyleColor();
            const int shape = me::edgeShapeOf(set.rows, set.cols, d);
            const std::string tip = std::to_string(d) + ": " + (shape >= 0 ? kShapeNames[shape] : "?") +
                                    (me::edgeDirectionUsable(things_, edgeSet_, d) ? "" : " (a whole-tile image: auto-edge never uses it)");
            help(tip.c_str());
            ImGui::PopID();
        }
        edgeDirection_ = std::clamp(edgeDirection_, 0, n - 1);
    }
    ImGui::Checkbox("Mark hand-edited tiles", &edgeMarkCustom_);
    help("Tiles whose edges you change by hand are left alone by the brush's auto-edge (kept in the map's .nxe).");
    ImGui::TextDisabled("%zu hand-edited tiles", paint_.customEdges.size());
    if (!paint_.customEdges.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Forget them")) paint_.customEdges.clear();
    }
    if (ImGui::Button("Edge Rules...")) showEdgeRules_ = true;
}

std::optional<int> MapEditor::heldInputInt(const char* id, int current, int lo, int hi)
{
    auto& [held, frame] = heldInts_[ImGui::GetID(id)];
    const int now = ImGui::GetFrameCount();
    if (frame != now - 1 && frame != now) held.reset(); // (not drawn last frame: from the document again)
    frame = now;
    return editor::heldInputInt(held, id, current, lo, hi, true); // (a stored value out of range shown as it is)
}

void MapEditor::wallProperties(const std::set<me::Cell>& cells)
{
    if (cells.empty()) return;
    const auto first = me::wallState(doc_, *cells.begin());
    if (!first) return;
    const float labelW = 110 * imgui_->scale();
    const auto row = [&](const char* label) { editor::labelRow(label, labelW); };
    // Every selected wall with one change made.
    const auto apply = [&](const std::string& label, const std::function<void(me::WallState&)>& change) {
        std::map<me::Cell, me::WallState> states;
        for (const auto c : cells) {
            if (auto st = me::wallState(doc_, c)) {
                change(*st);
                states[c] = *st;
            }
        }
        perform(me::setWallsOp(doc_, things_, states, label));
    };
    const bool many = cells.size() > 1;
    ImGui::SeparatorText(many ? (std::to_string(cells.size()) + " walls").c_str()
                              : (first->wall.material < things_.walls.size() ? things_.walls[first->wall.material].name.c_str() : "Wall"));
    if (!many) {
        row("Cell");
        ImGui::Text("%d, %d", cells.begin()->x, cells.begin()->y);
    }
    int material = first->wall.material;
    row("Material");
    if (wallPicker("propmat", material)) {
        apply("Wall material " + things_.walls[static_cast<std::size_t>(material)].name, [&](me::WallState& s) { s.wall.material = static_cast<std::uint8_t>(material); });
    }
    int facing = first->wall.direction & 0x7F;
    row("Facing");
    if (ImGui::BeginCombo("##facing", facing < 13 ? kFacingNames[facing] : std::to_string(facing).c_str())) {
        for (int f = 0; f <= 10; ++f) {
            if (ImGui::Selectable(kFacingNames[f], f == facing)) {
                apply("Wall facing", [&](me::WallState& s) { s.wall.direction = static_cast<std::uint8_t>((s.wall.direction & 0x80) | f); });
            }
        }
        ImGui::EndCombo();
    }
    const int count = me::wallVariantCount(things_, first->wall.material, facing);
    // The slider dragged: each value shown live on the map (the drag preview, in place), one op when
    // it's let go (an op per value passed made a history entry, and in a session a message, for
    // each); Escape takes the preview back and does nothing. Nothing reaches a session mid-drag.
    const auto variantOp = [this, &cells](int v) {
        std::map<me::Cell, me::WallState> states;
        for (const auto c : cells) {
            if (auto st = me::wallState(doc_, c)) {
                st->wall.variant = static_cast<std::uint8_t>(v);
                states[c] = *st;
            }
        }
        return me::setWallsOp(doc_, things_, states, "Wall variant");
    };
    int variant = variantDrag_.shown(first->wall.variant);
    row("Variant");
    ImGui::SliderInt("##variant", &variant, 0, std::max(0, count - 1));
    const bool sliding = ImGui::IsItemActive();
    const bool let = ImGui::IsItemDeactivated();
    switch (variantDrag_.frame(sliding, let, ImGui::IsKeyPressed(ImGuiKey_Escape, false), variant)) {
    case editor::LiveDrag::Step::Show: {
        const int v = variantDrag_.value();
        // (Its key: the value shown. One drag at a time, and a release clears the key.)
        previewDrag({-1.0f, static_cast<float>(v), static_cast<float>(cells.size()), 0.0f}, [&](std::optional<me::Selection>&) { return variantOp(v); });
        break;
    }
    case editor::LiveDrag::Step::Cancel:
        endPreview();
        status("Wall variant cancelled");
        break;
    case editor::LiveDrag::Step::Commit:
        endPreview(); // (the document as it was, then the one op)
        perform(variantOp(variantDrag_.value()));
        break;
    case editor::LiveDrag::Step::None: break;
    }
    row("Minimap group");
    if (const auto group = heldInputInt("##group", first->wall.minimapGroup, 0, 255)) {
        apply("Wall minimap group", [&](me::WallState& s) { s.wall.minimapGroup = static_cast<std::uint8_t>(*group); });
    }
    if (ImGui::SmallButton("From polygons")) {
        apply("Wall minimap group from polygons", [&](me::WallState& s) { s.wall.minimapGroup = me::minimapGroupAt(doc_, {s.wall.x, s.wall.y}); });
    }
    help("The polygon round each wall gives its group (as the game's save does), else 100.");
    bool window = first->window;
    if (ImGui::Checkbox("Window", &window)) apply(window ? "Make a window" : "No window", [&](me::WallState& s) { s.window = window; });
    help("Sight passes it. Straight pieces (facing 0 or 1) draw the window picture; others look plain.");
    ImGui::SameLine();
    bool breakable = first->destructible;
    if (ImGui::Checkbox("Breakable", &breakable)) apply(breakable ? "Make breakable" : "Not breakable", [&](me::WallState& s) { s.destructible = breakable; });
    ImGui::SameLine();
    bool secret = first->secret.has_value();
    if (ImGui::Checkbox("Secret", &secret)) {
        apply(secret ? "Make secret" : "Not secret", [&](me::WallState& s) {
            if (secret) s.secret = me::defaultSecretWall({s.wall.x, s.wall.y});
            else s.secret.reset();
        });
    }
    if (first->secret) {
        const nox::map::SecretWall sw = *first->secret;
        ImGui::SeparatorText("Secret wall");
        const struct {
            int bit;
            const char* label;
            const char* tip;
        } flags[] = {{2, "Opens when touched", "A player or monster walking into it opens it (and paths go through it)."},
                     {4, "Moves on its own", "After its wait it closes again (or, with the next flag, opens again)."},
                     {8, "Timer opens it", "With 'moves on its own': the timer opens it when shut. Also: an old map's wall with no state starts open."},
                     {1, "Scripted", "Marked for scripts (no engine effect)."}};
        for (const auto& f : flags) {
            bool on = (sw.flags & f.bit) != 0;
            if (ImGui::Checkbox(f.label, &on)) {
                apply("Secret wall flags", [&](me::WallState& s) {
                    if (s.secret) s.secret->flags = static_cast<std::uint8_t>(on ? (s.secret->flags | f.bit) : (s.secret->flags & ~f.bit));
                });
            }
            help(f.tip);
        }
        row("Wait (seconds)");
        if (const auto wait = heldInputInt("##wait", static_cast<int>(std::min<std::uint32_t>(sw.openWait, std::numeric_limits<int>::max())), 0, 100000)) {
            apply("Secret wall wait", [&](me::WallState& s) { if (s.secret) s.secret->openWait = static_cast<std::uint32_t>(*wait); });
        }
        const char* states[] = {"0 (old map: by flag 8)", "1 shut", "2 closing", "3 open", "4 opening"};
        int state = std::clamp<int>(sw.state, 0, 4);
        row("State");
        if (ImGui::Combo("##state", &state, states, 5) && state != sw.state) {
            apply("Secret wall state", [&](me::WallState& s) {
                if (!s.secret) return;
                me::setSecretStartOpen(*s.secret, state == 3);
                s.secret->state = static_cast<std::uint8_t>(state);
            });
        }
        help("How it starts: shut, or open (fully open, seen by everyone, as the stock maps store an open one).");
        if (ImGui::TreeNode("Advanced")) {
            row("How far open");
            if (const auto openBy = heldInputInt("##openby", sw.openDelay, 0, 23)) {
                apply("Secret wall opening", [&](me::WallState& s) { if (s.secret) s.secret->openDelay = static_cast<std::uint8_t>(*openBy); });
            }
            row("Timer (frames)");
            constexpr int kTimerMost = std::numeric_limits<int>::max();
            if (const auto timer = heldInputInt("##timer", static_cast<int>(std::min<std::uint32_t>(sw.lastOpen, kTimerMost)), 0, kTimerMost)) {
                apply("Secret wall timer", [&](me::WallState& s) { if (s.secret) s.secret->lastOpen = static_cast<std::uint32_t>(*timer); });
            }
            row("Seen by");
            ImGui::Text("0x%08X", sw.unknown);
            help("Which players have seen it open (bit per player); 0xFFFFFFFF for one that starts open.");
            ImGui::TreePop();
        }
    }
}

void MapEditor::edgeRulesWindow()
{
    ImGui::SetNextWindowSize(ImVec2(420 * imgui_->scale(), 560 * imgui_->scale()), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Edge Rules", &showEdgeRules_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("A floor higher in this list laps over the floors below it. The defaults come from the stock maps "
                       "(which floor laps over which, the edge used most for each pair); changes are saved in the map's .nxe.");
    if (ImGui::Button("Reset to the stock rules")) {
        rules_ = me::EdgeRules::defaults(things_);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100 * imgui_->scale());
    int seed = static_cast<int>(paint_.edgeSeed);
    if (ImGui::InputInt("Side seed", &seed)) paint_.edgeSeed = static_cast<std::uint32_t>(std::max(seed, 0));
    help("The seed side variants are picked with (the same seed: the same edges).");
    ImGui::SeparatorText("Priority (highest first)");
    static int selected = -1;
    std::vector<int> order = rules_.order();
    ImGui::BeginChild("##order", ImVec2(0, 260 * imgui_->scale()), ImGuiChildFlags_Borders);
    for (int i = static_cast<int>(order.size()) - 1; i >= 0; --i) {
        const int f = order[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        if (SDL_Texture* t = floorThumb(f, 0)) {
            ImGui::Image(texId(t), ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
            ImGui::SameLine();
        }
        if (ImGui::Selectable(things_.floors[static_cast<std::size_t>(f)].name.c_str(), selected == f)) selected = f;
        ImGui::PopID();
    }
    ImGui::EndChild();
    const auto at = std::ranges::find(order, selected);
    ImGui::BeginDisabled(at == order.end());
    if (ImGui::Button("Up") && at != order.end() && at + 1 != order.end()) {
        std::iter_swap(at, at + 1);
        rules_.setOrder(order);
    }
    ImGui::SameLine();
    if (ImGui::Button("Down") && at != order.end() && at != order.begin()) {
        std::iter_swap(at, at - 1);
        rules_.setOrder(order);
    }
    ImGui::SameLine();
    if (ImGui::Button("To the top") && at != order.end()) {
        const int f = *at;
        order.erase(at);
        order.push_back(f);
        rules_.setOrder(order);
    }
    ImGui::EndDisabled();
    static int add = -1;
    ImGui::TextDisabled("Add a floor the list lacks (at the bottom):");
    if (floorPicker("addfloor", add, true) && add >= 0 && std::ranges::find(order, add) == order.end()) {
        order.insert(order.begin(), add);
        rules_.setOrder(order);
    }
    ImGui::SeparatorText("Pair overrides");
    static int over = -1, base = -1, kind = 0, edge = -1;
    if (edge < 0) edge = edgeSet_;
    ImGui::TextDisabled("Floor lapping:");
    floorPicker("ovover", over);
    ImGui::TextDisabled("over floor:");
    floorPicker("ovbase", base);
    const char* kinds[] = {"Blend with edge", "Trim with edge", "Never laps"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##kind", &kind, kinds, 3);
    if (kind < 2) edgePicker("ovedge", edge);
    if (over >= 0 && base >= 0 && over != base) {
        const me::EdgeStyle now = rules_.style(over, base);
        ImGui::TextDisabled("Now: %s, %s", rules_.lap(over, base) ? "laps" : "doesn't lap",
                            now.edge >= 0 && static_cast<std::size_t>(now.edge) < things_.edges.size() ? things_.edges[static_cast<std::size_t>(now.edge)].name.c_str() : "-");
        if (ImGui::Button("Set override")) {
            rules_.setOverride(over, base, me::EdgeStyle{static_cast<me::EdgeStyle::Kind>(kind), kind < 2 ? edge : -1});
        }
    }
    std::optional<std::pair<int, int>> drop;
    if (ImGui::BeginTable("##ov", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        for (const auto& [k, s] : rules_.overrides()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(things_.floors[static_cast<std::size_t>(k.first)].name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(things_.floors[static_cast<std::size_t>(k.second)].name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.kind == me::EdgeStyle::Kind::None ? "never" : s.edge >= 0 ? things_.edges[static_cast<std::size_t>(s.edge)].name.c_str() : "-");
            ImGui::TableNextColumn();
            ImGui::PushID(k.first * 1000 + k.second);
            if (ImGui::SmallButton("x")) drop = k;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (drop) rules_.setOverride(drop->first, drop->second, std::nullopt);
    ImGui::End();
}

// --- Test steps (--editor-run) -----------------------------------------------------------------

bool MapEditor::toolStep(const std::string& step)
{
    const auto value = [&](std::string_view prefix) -> std::optional<std::string> {
        if (!step.starts_with(prefix)) return std::nullopt;
        return step.substr(prefix.size());
    };
    const auto floorByName = [&](const std::string& name) {
        for (std::size_t i = 0; i < things_.floors.size(); ++i) {
            if (things_.floors[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
    const auto numbers = [](const std::string& s) { return *editor::numbersAfter(s, "", 0); };
    if (const auto v = value("floor:")) {
        if (const int f = floorByName(*v); f >= 0) brush_.floor = static_cast<std::uint8_t>(f);
        else status("No floor " + *v);
    } else if (const auto v2 = value("edgefloor:")) {
        if (const int f = floorByName(*v2); f >= 0) edgeFloor_ = f;
    } else if (const auto v3 = value("edge:")) {
        for (std::size_t i = 0; i < things_.edges.size(); ++i) {
            if (things_.edges[i].name == *v3) edgeSet_ = static_cast<int>(i);
        }
    } else if (const auto v4 = value("wallmat:")) {
        for (std::size_t i = 0; i < things_.walls.size(); ++i) {
            if (things_.walls[i].name == *v4) wallMaterial_ = static_cast<int>(i);
        }
    } else if (const auto v5 = value("brush:")) {
        const auto n = numbers(*v5);
        if (!n.empty()) brush_.size = std::clamp(static_cast<int>(n[0]), 1, 16);
        if (n.size() > 1) brush_.hardness = std::clamp(n[1], 0.0f, 1.0f);
        if (n.size() > 2) brush_.noise = std::clamp(n[2], 0.0f, 1.0f);
    } else if (const auto v6 = value("shape:")) {
        brush_.shape = *v6 == "square" ? me::FloorBrush::Shape::Square : *v6 == "diamond" ? me::FloorBrush::Shape::Diamond : me::FloorBrush::Shape::Round;
    } else if (const auto v7 = value("floormode:")) {
        using M = me::FloorBrush::Mode;
        brush_.mode = *v7 == "erase" ? M::Erase : *v7 == "replace" ? M::Replace : *v7 == "fill" ? M::Fill : *v7 == "smooth" ? M::Smooth : *v7 == "edges" ? M::EdgesOnly : M::Paint;
    } else if (const auto v8 = value("wallmode:")) {
        wallMode_ = *v8 == "place" ? WallMode::Place : *v8 == "line" ? WallMode::Line : *v8 == "rect" ? WallMode::Rect : *v8 == "outline" ? WallMode::Outline
                    : *v8 == "erase" ? WallMode::Erase : *v8 == "pick" ? WallMode::Pick : WallMode::Draw;
    } else if (const auto v9 = value("edgemode:")) {
        edgeMode_ = *v9 == "auto" ? EdgeMode::Auto : *v9 == "remove" ? EdgeMode::Remove : EdgeMode::Add;
    } else if (const auto v10 = value("edgedir:")) {
        edgeDirection_ = std::atoi(v10->c_str());
    } else if (const auto vk = value("wallkind:")) {
        wallSecret_ = *vk == "secret";
        wallWindow_ = *vk == "window";
        wallBreakable_ = *vk == "breakable";
    } else if (const auto vf = value("wallfacing:")) {
        wallFacing_ = std::clamp(std::atoi(vf->c_str()), 0, 12);
    } else if (const auto v11 = value("ontop:")) {
        floorOnTop_ = *v11 == "1";
    } else if (const auto v12 = value("wmouse:")) {
        // The mouse onto a world point of the map panel (the panel as laid out last frame).
        const auto n = numbers(*v12);
        if (n.size() >= 2 && panelW_ > 0) {
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = SDL_GetWindowID(ctx_.window);
            const ImVec2 o = windowOrigin(); // (an SDL event's point is the window's)
            e.motion.x = panelX_ - o.x + (n[0] - camera_.left()) * camera_.zoom;
            e.motion.y = panelY_ - o.y + (n[1] - camera_.top()) * camera_.zoom;
            testMouseX_ = e.motion.x;
            testMouseY_ = e.motion.y;
            SDL_PushEvent(&e);
        }
    } else if (const auto v13 = value("stroke:")) {
        // A whole stroke from (x0, y0) to (x1, y1) in n dabs, with the current tool (tests).
        const auto n = numbers(*v13);
        if (n.size() < 4) return true;
        const int steps = n.size() > 4 ? std::max(1, static_cast<int>(n[4])) : 1;
        if (tool_ == Tool::Floor) {
            me::FloorStroke s(doc_, things_, rules_, floorBrush(), paint_.customEdges);
            for (int i = 0; i <= steps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                s.dab({n[0] + (n[2] - n[0]) * t, n[1] + (n[3] - n[1]) * t});
            }
            std::vector<me::TileCell> restored;
            const me::Op op = s.finish(&restored);
            perform(op);
            ++strokeSeed_;
        } else if (tool_ == Tool::Wall) {
            const auto a = evenCellAt(n[0], n[1]), b = evenCellAt(n[2], n[3]);
            if (a && b) {
                std::vector<me::Cell> cells;
                if (wallMode_ == WallMode::Rect) {
                    cells = me::wallRectangle(*a, *b);
                } else if (wallMode_ == WallMode::Line) {
                    cells = me::wallLine(*a, *b);
                } else {
                    cells = {*a};
                    for (const auto c : diagonalPath(*a, *b)) cells.push_back(c);
                }
                perform(withWallEdges(wallMode_ == WallMode::Erase ? me::removeWallsOp(doc_, things_, cells, wallReface_) : me::placeWallsOp(doc_, things_, cells, wallBrush())));
                ++strokeSeed_;
            }
        } else if (tool_ == Tool::Edge) {
            std::set<me::TileCell> cells;
            for (int i = 0; i <= steps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                if (const auto c = me::tileAt(n[0] + (n[2] - n[0]) * t, n[1] + (n[3] - n[1]) * t)) cells.insert(*c);
            }
            perform(me::autoEdgeOp(doc_, things_, rules_, edgeOptions(), cells, paint_.customEdges));
        }
    } else if (const auto v14 = value("outline:")) {
        const auto n = numbers(*v14);
        if (n.size() >= 2) {
            if (const auto t = me::tileAt(n[0], n[1])) {
                const auto ring = me::wallOutline(doc_, *t);
                perform(withWallEdges(me::placeWallsOp(doc_, things_, me::cellsOf(ring), wallBrush(), &ring)));
            }
        }
    } else {
        return false;
    }
    return true;
}

} // namespace noxsdl
