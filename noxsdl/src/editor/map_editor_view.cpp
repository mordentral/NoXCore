// The map editor's viewport: the document drawn by MapView into the Map panel, the overlays over
// it (grid, selection, waypoints, polygons, extents, labels), and the mouse: pan, zoom, select,
// box / 45-degree / lasso, move, area copy, the context menu.
#include "editor/map_editor.hpp"

#include "editor/marker_tools.hpp"
#include "editor/object_tools.hpp"
#include "editor/pencil_mark.hpp"
#include "editor/play_view.hpp"
#include "editor/property_panel.hpp"
#include "editor/script_panel.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"
#include "player_art.hpp"
#include "sprite_cache.hpp"
#include "thing_art.hpp"

#include "noxcore/client/edit_session_client.hpp"
#include "noxcore/client/world.hpp"
#include "noxcore/mapedit/markers.hpp"
#include "noxcore/mapedit/placement.hpp"
#include "noxcore/mapedit/writer.hpp"
#include "noxcore/things/thing_db.hpp"

#include <imgui.h>
#include <imgui_internal.h> // (the Map panel kept in the main window: its dock node's flags)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace me = nox::mapedit;
namespace ss = nox::mapedit::session;

namespace noxsdl {

namespace {

using ObjId = me::ObjId;

constexpr std::uint32_t kEnabledFlag = 0x01000000u;  // object flag ENABLED (bit 24)
constexpr std::uint32_t kNoCollideFlag = 0x00000040u; // object flag NO_COLLIDE (bit 6)
constexpr float kBoxHalf = 0.35355339f;              // a BOX extent's corners: 1 / (2 sqrt 2) of its sides

using editor::col;

/// Whether two selections share anything.
bool overlaps(const me::Selection& a, const me::Selection& b)
{
    for (const auto v : a.objects) if (b.objects.contains(v)) return true;
    for (const auto v : a.walls) if (b.walls.contains(v)) return true;
    for (const auto v : a.tiles) if (b.tiles.contains(v)) return true;
    for (const auto v : a.waypoints) if (b.waypoints.contains(v)) return true;
    return false;
}

/// The colour an editor's outlines and name are drawn in (by peer id; pencil_icon.hpp's palette,
/// so the game's pencil over them matches).
ImU32 peerColour(me::PeerId peer)
{
    return editor::peerColourU32(peer);
}

} // namespace

std::optional<me::WorldPoint> MapEditor::mouseWorld() const
{
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (panelW_ <= 0 || m.x < panelX_ || m.y < panelY_ || m.x >= panelX_ + panelW_ || m.y >= panelY_ + panelH_) {
        return std::nullopt;
    }
    return me::WorldPoint{camera_.left() + (m.x - panelX_) / camera_.zoom, camera_.top() + (m.y - panelY_) / camera_.zoom};
}

float MapEditor::snapValue(float v) const
{
    const float step = settings_.snap == 1 ? me::kWallCellSize : settings_.snap == 2 ? me::kWallCellSize / 2 : settings_.snap == 3 ? settings_.snapCustom : 0;
    return step > 0 ? std::round(v / step) * step : v;
}

// --- A drag's live preview ------------------------------------------------------------------------

void MapEditor::previewDrag(const std::array<float, 4>& key, const std::function<me::Op(std::optional<me::Selection>& moved)>& build)
{
    if (previewKey_ == key) {
        return; // (what's shown is this frame's already)
    }
    // Back to the document as it was, then this frame's op built from it and shown.
    const me::Op old = preview_.clear(doc_);
    if (previewSelectionBefore_) {
        selection_ = *std::exchange(previewSelectionBefore_, std::nullopt);
    }
    previewKey_ = key;
    std::optional<me::Selection> moved;
    me::Op op = build(moved);
    if (!op.empty() && !locks_.conflict(self_, me::elementsTouched(doc_, op))) {
        try {
            preview_.show(doc_, std::move(op));
        } catch (const std::exception&) {
            // (an op that doesn't fit shows nothing; its release says why)
        }
    }
    if (preview_.active() && moved) {
        previewSelectionBefore_ = std::exchange(selection_, std::move(*moved)); // (the moved walls' and tiles' cells)
    }
    // The view redrawn where either touches (once: an object move writes every object again).
    me::Op touched = old;
    if (preview_.active()) touched.parts.insert(touched.parts.end(), preview_.shown().parts.begin(), preview_.shown().parts.end());
    if (!touched.empty()) viewAfterOp(touched);
}

void MapEditor::endPreview()
{
    previewKey_.reset();
    const me::Op shown = preview_.clear(doc_);
    if (previewSelectionBefore_) {
        selection_ = *std::exchange(previewSelectionBefore_, std::nullopt);
    }
    if (!shown.empty()) viewAfterOp(shown); // (the view as the document is again)
}

bool MapEditor::moveDragging() const
{
    return drag_ == Drag::Move || markers_->dragging() || props_->gizmoDragging();
}

bool MapEditor::cancelDrags()
{
    const bool any = drag_ != Drag::None || markers_->dragging() || props_->gizmoDragging() || preview_.active();
    if (!any) return false;
    drag_ = Drag::None;
    moveDx_ = moveDy_ = 0;
    markers_->cancelDrag();
    props_->cancelGizmo();
    endPreview();
    status("Drag cancelled");
    return true;
}

// --- The map drawn ------------------------------------------------------------------------------

void MapEditor::rebuildView()
{
    const std::uint64_t key = history_.revision() ^ (layerRevision_ << 40);
    if (view_ && key == viewBuiltFor_) {
        return;
    }
    viewBuiltFor_ = key;
    // MapView draws a map file: the document written as one, with the hidden layers left out.
    // Wall, floor, object, ambient and polygon ops update it in place instead (viewAfterOp);
    // anything else rebuilds it.
    const editor::Layers& L = settings_.layers;
    const me::MapDocument* src = &doc_;
    me::MapDocument filtered;
    if (!L.floor || !L.edges || !L.walls || !L.objects) {
        filtered = doc_;
        if (!L.floor) {
            filtered.floor = me::FloorGrid();
        } else if (!L.edges) {
            for (int y = 0; y < me::FloorGrid::kSize; ++y) {
                for (int x = 0; x < me::FloorGrid::kSize; ++x) {
                    auto& p = filtered.floor.pair(x, y);
                    if (p.odd) p.odd->edges.clear();
                    if (p.even) p.even->edges.clear();
                }
            }
        }
        if (!L.walls) {
            filtered.walls.clear();
            filtered.secretWalls.walls.clear();
            filtered.destructableWalls.walls.clear();
            filtered.windowWalls.walls.clear();
        }
        if (!L.objects) {
            filtered.objects.clear();
        }
        src = &filtered;
    }
    try {
        const nox::map::MapFile file = me::toMapFile(*src, things_);
        view_ = std::make_unique<MapView>(file, things_, *ctx_.art, *ctx_.modifiers, MapView::ObjectSet::Editor);
        // Zoomed out over a big map: new images up together, the original floor tiles under half size.
        view_->setBatchImages(true);
        view_->setSdFloorBelow(0.5f);
        view_->setFloorMakeBudget(8); // (a first look over a big map fills in over a few frames)
        viewWalls(); // (every wall shown: none hidden as an open secret wall)
        if (prefetchedFor_ != docGeneration_ && spritesOwned()) {
            prefetchImages(file);
        }
    } catch (const std::exception& ex) {
        status(std::string("The map can't be drawn: ") + ex.what());
    }
    if (view_ && !cameraSet_) {
        cameraSet_ = true;
        const bool anyFloor = doc_.floor.tileCount() > 0;
        camera_.x = anyFloor ? view_->centre().x : me::kWorldSize / 2;
        camera_.y = anyFloor ? view_->centre().y : me::kWorldSize / 2;
        camera_.zoom = 0.75f;
    }
}

bool MapEditor::spritesOwned() const
{
    return !session_ && !(ctx_.playTest.running && ctx_.playTest.running());
}

void MapEditor::prefetchImages(const nox::map::MapFile& file)
{
    // A document's HD images asked of the decode workers ahead, as a game's map load asks for its
    // map's (kept till the next): the first look round it, or over it zoomed out, finds them made
    // (setDeferHd draws the originals for what isn't in yet). Without a pack, nothing.
    prefetchedFor_ = docGeneration_;
    if (!ctx_.sprites || !ctx_.art || !view_ || !ctx_.sprites->usingPack()) {
        return;
    }
    ctx_.sprites->beginMap(view_->preloadList(file, *ctx_.art, *ctx_.sprites, "editor " + (docName_.empty() ? doc_.origin.name : docName_)));
}

void MapEditor::renderMap()
{
    const int w = static_cast<int>(panelW_);
    const int h = static_cast<int>(panelH_);
    if (w < 8 || h < 8 || playView_->inPanel()) {
        return; // (a play-test's game has the panel: play_view.hpp)
    }
    if (!mapTarget_ || w != targetW_ || h != targetH_) {
        if (mapTarget_) {
            SDL_DestroyTexture(mapTarget_);
        }
        mapTarget_ = SDL_CreateTexture(ctx_.renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
        targetW_ = w;
        targetH_ = h;
        if (!mapTarget_) {
            return;
        }
        SDL_SetTextureBlendMode(mapTarget_, SDL_BLENDMODE_NONE);
    }
    const std::uint64_t startNs = SDL_GetTicksNS();
    const std::size_t madeBefore = ctx_.sprites->made();
    const bool owned = spritesOwned();
    if (!owned) {
        prefetchedFor_ = 0; // (a game has the cache: its map's images; the document's asked for again after it)
    } else if (prefetchedFor_ != docGeneration_ && view_) {
        try {
            prefetchImages(me::toMapFile(doc_, things_)); // (the game over: the document's images asked for ahead again)
        } catch (const std::exception&) {
            prefetchedFor_ = docGeneration_;
        }
    }
    // The editor's own draw: HD images not in yet as their originals meanwhile (never a game's).
    ctx_.sprites->setDeferHd(owned);
    SDL_Texture* previous = SDL_GetRenderTarget(ctx_.renderer);
    SDL_SetRenderTarget(ctx_.renderer, mapTarget_);
    SDL_SetRenderDrawColor(ctx_.renderer, 10, 10, 12, 255);
    SDL_RenderClear(ctx_.renderer);
    camera_.viewWidth = w;
    camera_.viewHeight = h;
    if (view_) {
        const std::uint32_t frame = static_cast<std::uint32_t>(SDL_GetTicks() * 30 / 1000);
        view_->setTime(static_cast<double>(SDL_GetTicks()) * 30 / 1000);
        LightMap* light = nullptr;
        if (settings_.layers.lighting) {
            std::vector<LightSource> lights;
            for (const auto& ml : view_->lights()) {
                if (ml.enabled) {
                    lights.push_back(ml.at(frame));
                }
            }
            light_.build(camera_, view_->ambientAt(camera_.x, camera_.y), lights, frame);
            light = &light_;
        }
        updatePendingView(); // (a session's edits not yet published: greyed, as the Pending changes setting says)
        view_->drawFloor(ctx_.renderer, camera_, *ctx_.sprites, light);
        // Walls opaque unless the layer asks for the game's see-through front walls.
        view_->setTranslucentFrontWalls(settings_.layers.seeThroughWalls);
        auto ghosts = objects_->ghosts(); // (what the object tool would place, translucent)
        // A session's players and creatures as the game draws them, sorted with the map's objects.
        auto live = liveSprites();
        ghosts.insert(ghosts.end(), std::make_move_iterator(live.begin()), std::make_move_iterator(live.end()));
        view_->drawScene(ctx_.renderer, camera_, *ctx_.sprites, ghosts, nullptr, light);
        // Over everything, faint: what a tree, a wall or a roof in front hides of them still shows
        // through (the scene's sort above draws them where they are, as the game does; the editor
        // looks over the whole map, and a player behind a canopy would be lost).
        for (const auto& o : liveOver_) {
            const Sprite* s = o.colours ? ctx_.sprites->tinted(o.image, *o.colours) : ctx_.sprites->get(o.image);
            if (!s) continue;
            const SDL_FRect dst = camera_.toScreen(o.x + s->offsetX, o.y + s->offsetY, s->width, s->height);
            const SDL_FRect src{0, 0, s->imageWidth(), s->rect.h};
            s->scale = s->hd ? SDL_SCALEMODE_LINEAR : camera_.artScaleMode();
            SDL_SetTextureColorMod(s->texture, 255, 255, 255);
            SDL_SetTextureAlphaMod(s->texture, 80);
            ctx_.sprites->render(*s, &src, dst);
            SDL_SetTextureAlphaMod(s->texture, 255);
        }
    }
    SDL_SetRenderTarget(ctx_.renderer, previous);
    ctx_.sprites->setDeferHd(false);
    const SpriteCache::LoadStats loads = ctx_.sprites->takeLoadStats();
    if (const double ms = static_cast<double>(SDL_GetTicksNS() - startNs) / 1e6; ms > 50) {
        // (A slow draw said in the log: what it made, HD images read and made in it, at what zoom.)
        SDL_Log("map editor: the map took %.0f ms to draw (%zu images made, %zu HD read here in %.0f ms, floor waits %.0f ms, zoom %.3f, at %llu ms)",
                ms, ctx_.sprites->made() - madeBefore, loads.misses, loads.missMs, loads.waitMs, camera_.zoom,
                static_cast<unsigned long long>(SDL_GetTicks()));
    }
}

// --- The panel and its mouse ------------------------------------------------------------------

void MapEditor::mapPanel()
{
    // The Map panel stays in the main window (floating panels: imgui_host.hpp): its view is drawn
    // on the main window's renderer (with its GPU states and shaders), and the play-test's game
    // and its mouse are laid out in that window. Its dock node can't be undocked, nor have other
    // panels tabbed onto it; and should it be floating (a layout from before), it's kept in the
    // main window's viewport rather than given a window of its own.
    ImGuiWindowClass mapClass;
    mapClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoUndocking | static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDockingOverMe);
    ImGui::SetNextWindowClass(&mapClass);
    if (const ImGuiWindow* w = ImGui::FindWindowByName("Map"); !w || w->DockId == 0) {
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        panelW_ = panelH_ = 0;
        ImGui::End();
        return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 8.0f);
    size.y = std::max(size.y, 8.0f);
    panelX_ = origin.x;
    panelY_ = origin.y;
    panelW_ = size.x;
    panelH_ = size.y;
    if (playView_->inPanel()) {
        playView_->panel(origin, size); // (the play-test's game in the panel instead of the map)
        mapHovered_ = false;
        ImGui::End();
        return;
    }
    ImGui::InvisibleButton("##map", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    mapHovered_ = hovered;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (mapTarget_) {
        dl->AddImage(editor::texId(mapTarget_), origin, ImVec2(origin.x + static_cast<float>(targetW_), origin.y + static_cast<float>(targetH_)));
    }
    mapInput(origin, hovered);
    drawOverlays(dl, origin, size);
    mapContextMenu();
    liveMenu();
    if (follow_) {
        const auto it = std::ranges::find_if(live_, [&](const LiveEntity& e) { return e.player && e.id == *follow_; });
        if (it != live_.end()) {
            const std::string text = "Following " + it->name + "  (pan or Escape stops)";
            dl->AddRectFilled(ImVec2(origin.x + 8, origin.y + size.y - 28), ImVec2(origin.x + 16 + ImGui::CalcTextSize(text.c_str()).x, origin.y + size.y - 8),
                              IM_COL32(0, 0, 0, 170), 3);
            dl->AddText(ImVec2(origin.x + 12, origin.y + size.y - 26), IM_COL32(255, 220, 90, 255), text.c_str());
        }
    }
    if (hovered && drag_ == Drag::None) {
        if (const auto w = mouseWorld()) {
            // Another editor's element under the mouse: who holds it.
            const me::Selection under = me::pickAt(doc_, w->x, w->y, clickRadius(), pickFilter());
            for (const auto& k : me::elementsOf(doc_, under)) {
                if (const auto h = otherHolder(k)) {
                    ImGui::SetTooltip("being edited by %s", peerName(*h).c_str());
                }
            }
        }
    }
    ImGui::End();
}

void MapEditor::mapInput(const ImVec2& origin, bool hovered)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const auto toWorld = [&](ImVec2 s) {
        return me::WorldPoint{camera_.left() + (s.x - origin.x) / camera_.zoom, camera_.top() + (s.y - origin.y) / camera_.zoom};
    };
    const me::WorldPoint w = toWorld(m);

    // Zoom about the mouse (1/8 to 16 screen pixels a world unit, as the viewer).
    if (hovered && io.MouseWheel != 0) {
        const float z = std::clamp(camera_.zoom * std::pow(1.25f, io.MouseWheel), 1.0f / 8, 16.0f);
        camera_.x = w.x - (m.x - origin.x - panelW_ / 2) / z;
        camera_.y = w.y - (m.y - origin.y - panelH_ / 2) / z;
        camera_.zoom = z;
    }
    // Pan: the middle button, or the right one dragged (a right click without a drag is the menu).
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        drag_ = Drag::Pan;
    }
    if (drag_ == Drag::Pan) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            camera_.x -= io.MouseDelta.x / camera_.zoom;
            camera_.y -= io.MouseDelta.y / camera_.zoom;
        } else {
            drag_ = Drag::None;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        rightDownX_ = m.x;
        rightDownY_ = m.y;
        rightDragging_ = false;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && moveDragging()) {
        cancelDrags();     // (a right click mid-drag cancels it: everything back where it was)
        rightDownX_ = -1; // (and is nothing else: no menu, no tool's right click)
    }
    if (rightDownX_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        if (std::hypot(m.x - rightDownX_, m.y - rightDownY_) > 4) {
            rightDragging_ = true;
        }
        if (rightDragging_) {
            camera_.x -= io.MouseDelta.x / camera_.zoom;
            camera_.y -= io.MouseDelta.y / camera_.zoom;
        }
    }
    if (rightDownX_ >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && pick_) {
        if (!rightDragging_) cancelPick(); // (a right click cancels a pick in view)
        rightDownX_ = -1;
    }
    if (rightDownX_ >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !rightDragging_ && hovered) {
        if (const LiveEntity* e = liveAt(origin, m)) {
            liveMenu_ = *e; // (a live player's menu: Follow, Jump in here)
            rightDownX_ = -1;
        }
    }
    if (rightDownX_ >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (!rightDragging_ && hovered && (tool_ == Tool::Wall || tool_ == Tool::Floor || tool_ == Tool::Edge || tool_ == Tool::Object ||
                                           tool_ == Tool::Waypoint || tool_ == Tool::Polygon) && !areaCopyArmed_) {
            toolRightClick_ = true; // (the tool's: remove what's under the mouse)
        } else if (!rightDragging_ && hovered) {
            contextAt_ = w;
            // A right click on something unselected selects it first, as most editors do.
            const me::Selection pick = me::withoutLocked(doc_, me::pickAt(doc_, w.x, w.y, clickRadius(), pickFilter()), locks_, self_);
            if (!pick.empty() && !overlaps(pick, selection_)) {
                setSelection(pick);
            }
            ImGui::OpenPopup("##mapmenu");
        }
        rightDownX_ = -1;
    }
    if (liveInput(origin, hovered)) {
        return; // (a live player clicked: followed)
    }
    camera_.x = std::clamp(camera_.x, 0.0f, me::kWorldSize);
    camera_.y = std::clamp(camera_.y, 0.0f, me::kWorldSize);

    if (pickInput(w, hovered)) {
        return; // (a field's pick in view has the left button)
    }
    if (tool_ == Tool::Select && !areaCopyArmed_ && props_->gizmoInput(w, hovered)) {
        return; // (the move gizmo or the rotate ring is being dragged)
    }
    if (tool_ != Tool::Select && !areaCopyArmed_) {
        toolInput(w, hovered);
        return;
    }

    // Left button: select, box / lasso, move, area copy.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragStart_ = w;
        dragStartSx_ = m.x;
        dragStartSy_ = m.y;
        dragMoved_ = false;
        dragMods_ = (io.KeyShift ? 1 : 0) | (io.KeyCtrl ? 2 : 0);
        if (areaCopyArmed_) {
            drag_ = Drag::AreaCopy;
            region_ = {me::Region::Shape::Box, {w, w}};
        } else {
            std::size_t skipped = 0;
            const me::Selection pick = me::withoutLocked(doc_, me::pickAt(doc_, w.x, w.y, clickRadius(), pickFilter()), locks_, self_, &skipped);
            if (skipped > 0) {
                status("Being edited by another editor");
            }
            dragPick_ = pick;
            if (!pick.empty() && dragMods_ == 0) {
                if (!overlaps(pick, selection_)) {
                    setSelection(pick);
                }
                drag_ = Drag::Move;
                moveDx_ = moveDy_ = 0;
                moveDoor_ = false;
                // The item grabbed is what snapping lines up.
                moveAnchor_ = w;
                if (!pick.objects.empty()) {
                    const auto& h = doc_.objects.at(*pick.objects.begin()).header;
                    moveAnchor_ = {h.x, h.y};
                    // (A door grabbed moves on the grid, as the old editor kept doors.)
                    moveDoor_ = me::snapsAsDoor(doc_.objects.at(*pick.objects.begin()));
                } else if (!pick.waypoints.empty()) {
                    const auto it = std::ranges::find(doc_.waypoints.waypoints, *pick.waypoints.begin(), &nox::map::Waypoint::id);
                    if (it != doc_.waypoints.waypoints.end()) moveAnchor_ = {it->x, it->y};
                }
            } else if (!pick.empty()) {
                me::Selection s = selection_;
                if (dragMods_ & 1) {
                    s.toggle(pick); // Shift adds or toggles
                } else {
                    s.remove(pick); // Ctrl removes
                }
                setSelection(std::move(s));
                drag_ = Drag::None;
            } else {
                drag_ = Drag::Region;
                region_ = {static_cast<me::Region::Shape>(settings_.regionShape), {w, w}};
            }
        }
    }
    if ((drag_ == Drag::Region || drag_ == Drag::AreaCopy || drag_ == Drag::Move) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (std::hypot(m.x - dragStartSx_, m.y - dragStartSy_) > 3) {
            dragMoved_ = true;
        }
        if (drag_ == Drag::Move) {
            const auto snapMove = [&](float v) { return moveDoor_ ? std::round(v / me::kWallCellSize) * me::kWallCellSize : snapValue(v); };
            const float tx = snapMove(moveAnchor_.x + (w.x - dragStart_.x));
            const float ty = snapMove(moveAnchor_.y + (w.y - dragStart_.y));
            moveDx_ = dragMoved_ ? tx - moveAnchor_.x : 0;
            moveDy_ = dragMoved_ ? ty - moveAnchor_.y : 0;
            // Live: the move the release will make, shown in place (snapped the same).
            previewDrag({moveDx_, moveDy_, 0, 0}, [this](std::optional<me::Selection>& moved) {
                const auto [cx, cy] = me::cellStep(doc_, selection_, moveDx_, moveDy_);
                moveCx_ = cx;
                moveCy_ = cy;
                me::Selection after;
                me::Op op = me::moveOp(doc_, selection_, moveDx_, moveDy_, &after);
                moved = std::move(after);
                return op;
            });
        } else if (region_.shape == me::Region::Shape::Lasso && drag_ == Drag::Region) {
            const me::WorldPoint last = region_.points.back();
            if (std::hypot((w.x - last.x) * camera_.zoom, (w.y - last.y) * camera_.zoom) > 4) {
                region_.points.push_back(w);
            }
        } else {
            region_.points.back() = w;
        }
    }
    if ((drag_ == Drag::Region || drag_ == Drag::AreaCopy || drag_ == Drag::Move) && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const Drag was = drag_;
        drag_ = Drag::None;
        if (was == Drag::Region) {
            if (!dragMoved_) {
                if (dragMods_ == 0) {
                    setSelection({}, true); // a click on nothing clears
                }
            } else {
                me::Selection s = me::selectIn(doc_, region_, pickFilter());
                if (dragMods_ & 1) {
                    me::Selection all = selection_;
                    all.add(s);
                    s = std::move(all);
                } else if (dragMods_ & 2) {
                    me::Selection all = selection_;
                    all.remove(s);
                    s = std::move(all);
                }
                setSelection(std::move(s));
            }
        } else if (was == Drag::AreaCopy) {
            areaCopyArmed_ = false;
            std::size_t skipped = 0;
            const me::Selection s = me::withoutLocked(doc_, me::selectIn(doc_, region_, me::KindFilter{}), locks_, self_, &skipped);
            const me::Clip clip = me::copySelection(doc_, s, settings_.copyLayers);
            const std::size_t n = clip.walls.size() + clip.tiles.size() + clip.objects.size() + clip.waypoints.size();
            if (n == 0) {
                status("Copy area: nothing of the ticked layers in it");
            } else {
                clip_ = clip;
                SDL_SetClipboardText(me::clipToText(clip, things_).c_str());
                status("Copied an area: " + std::to_string(clip.walls.size()) + " walls, " + std::to_string(clip.tiles.size()) + " tiles, " +
                       std::to_string(clip.objects.size()) + " objects, " + std::to_string(clip.waypoints.size()) + " waypoints" +
                       (skipped ? " (" + std::to_string(skipped) + " skipped: being edited by others)" : ""));
            }
        } else if (was == Drag::Move) {
            endPreview(); // (the document as it was: the drag's one op from it)
            if (dragMoved_ && (moveDx_ != 0 || moveDy_ != 0)) {
                me::Selection moved;
                if (perform(me::moveOp(doc_, selection_, moveDx_, moveDy_, &moved))) {
                    setSelection(std::move(moved), true);
                }
            } else if (!dragMoved_) {
                setSelection(dragPick_, true); // a click on a selected item selects just it
            }
            moveDx_ = moveDy_ = 0;
        }
    }
}

void MapEditor::mapContextMenu()
{
    if (!ImGui::BeginPopup("##mapmenu")) {
        return;
    }
    menuItem("edit.cut");
    menuItem("edit.copy");
    if (ImGui::MenuItem("Paste Here", commands_.shortcut("edit.paste").text().c_str())) {
        paste(me::wallCellAt(contextAt_.x, contextAt_.y));
    }
    menuItem("edit.duplicate");
    menuItem("edit.delete");
    ImGui::Separator();
    menuItem("select.sameType");
    menuItem("view.frame");
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Coordinates Here")) {
        char text[64];
        std::snprintf(text, sizeof text, "%.1f, %.1f", contextAt_.x, contextAt_.y);
        SDL_SetClipboardText(text);
        status(std::string("Copied ") + text);
    }
    menuItem("edit.copyExtents");
    menuItem("edit.copyWaypoints");
    if (ctx_.playTest.available()) {
        ImGui::Separator();
        // Jump in at the cursor: the play-test from the point clicked (or a running one's player put there).
        if (ImGui::MenuItem("Play From Here", commands_.shortcut("play.here").text().c_str())) {
            scriptPanel_->playFrom(SDL_FPoint{contextAt_.x, contextAt_.y});
        }
    }
    ImGui::EndPopup();
}

// --- Overlays ---------------------------------------------------------------------------------

const MapEditor::TypeExtent& MapEditor::typeExtent(const std::string& type)
{
    if (const auto it = typeExtents_.find(type); it != typeExtents_.end()) {
        return it->second;
    }
    TypeExtent t;
    if (const nox::things::ThingDef* def = things_.findThing(type)) {
        t.known = true;
        if (const auto f = def->lastAttribute("FLAGS")) {
            t.colliding = f->find("NO_COLLIDE") == std::string_view::npos;
        }
        if (const auto e = def->lastAttribute("EXTENT")) {
            // "CIRCLE r" or "BOX w h" (as the server reads them).
            const std::string s(*e);
            char* end = nullptr;
            t.circle = s.starts_with("CIRCLE");
            t.box = s.starts_with("BOX");
            const char* p0 = s.c_str() + (t.circle ? 6 : t.box ? 3 : s.size());
            t.a = std::strtof(p0, &end);
            t.haveA = end != p0;
            const char* p1 = end;
            t.b = t.haveA ? std::strtof(p1, &end) : 0;
            t.haveB = t.haveA && end != p1;
        }
        if (const auto zs = def->lastAttribute("ZSIZE")) {
            const std::string zt(*zs);
            char* ze = nullptr;
            std::strtof(zt.c_str(), &ze);
            t.zTop = std::strtof(ze, nullptr);
        }
    }
    return typeExtents_.emplace(type, t).first->second;
}

void MapEditor::drawOverlays(ImDrawList* dl, const ImVec2& origin, const ImVec2& size)
{
    const float z = camera_.zoom;
    const float left = camera_.left(), top = camera_.top();
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - left) * z, origin.y + (y - top) * z); };
    const float x0 = left, y0 = top, x1 = left + size.x / z, y1 = top + size.y / z;
    const auto visible = [&](float x, float y, float margin = 60) {
        return x >= x0 - margin && x <= x1 + margin && y >= y0 - margin && y <= y1 + margin;
    };
    const editor::Layers& L = settings_.layers;
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // Grid: wall cells (23-unit squares), or the floor tiles' diamonds (lines x + y = 24 + 46k and
    // x - y = 46k, from the tiles' centres at (23 gx + 12, 23 gy + 35)).
    if (L.grid == 1) {
        float step = me::kWallCellSize;
        while (step * z < 6) step *= 2;
        const ImU32 c = col(255, 255, 255, 26);
        for (float x = std::floor(x0 / step) * step; x <= x1; x += step) dl->AddLine(S(x, y0), S(x, y1), c);
        for (float y = std::floor(y0 / step) * step; y <= y1; y += step) dl->AddLine(S(x0, y), S(x1, y), c);
    } else if (L.grid == 2) {
        float step = 46;
        while (step * z < 8) step *= 2;
        const ImU32 c = col(255, 255, 255, 26);
        for (float k = std::floor((x0 + y0 - 24) / step) * step + 24; k <= x1 + y1; k += step) dl->AddLine(S(k - y0, y0), S(k - y1, y1), c);
        for (float k = std::floor((x0 - y1) / step) * step; k <= x1 - y0; k += step) dl->AddLine(S(k + y0, y0), S(k + y1, y1), c);
    }

    // Schematic: walls as flat cells by kind, objects as discs (the old editor's wireframe view).
    if (L.schematic) {
        for (const auto& [c, w] : doc_.walls) {
            const float wx = c.x * me::kWallCellSize, wy = c.y * me::kWallCellSize;
            if (!visible(wx, wy)) continue;
            const auto st = me::wallState(doc_, c);
            const ImU32 fill = st->secret ? col(80, 140, 255, 110) : st->destructible ? col(255, 90, 70, 110) : st->window ? col(90, 230, 230, 110)
                                                                                                                        : col(200, 200, 200, 80);
            dl->AddRectFilled(S(wx, wy), S(wx + 23, wy + 23), fill);
        }
        for (const auto& [id, o] : doc_.objects) {
            if (o.parent == 0 && visible(o.header.x, o.header.y)) dl->AddCircleFilled(S(o.header.x, o.header.y), std::max(2.5f, 4 * z), col(255, 230, 120, 140));
        }
    }

    // Polygons (outlines, names, the selected one, the ambient preview: marker_tools.cpp); shown
    // with the layer, and always while the Polygon tool, the Polygons panel or a selected polygon
    // needs them.
    if (L.polygons || tool_ == Tool::Polygon || !selection_.polygons.empty() || markers_->showPolygons) {
        markers_->drawPolygons(dl, origin);
    }

    // Waypoints and their links (one way, both ways, the selection's: marker_tools.cpp).
    if (L.waypoints) {
        markers_->drawWaypoints(dl, origin, size);
    }

    // Objects' extents, facing, labels and the disabled mark.
    const bool anyObjectLayer = L.extents != 0 || L.facing || L.labelTypes || L.labelScriptNames || L.labelExtents || L.markDisabled;
    if (anyObjectLayer && L.objects) {
        for (const auto& [id, o] : doc_.objects) {
            if (o.parent != 0 || !visible(o.header.x, o.header.y)) continue;
            const ImVec2 p = S(o.header.x, o.header.y);
            if (const TypeExtent* te = L.extents != 0 ? &typeExtent(o.type) : nullptr; te && te->known) {
                const bool colliding = o.header.details ? (o.header.details->flags & kNoCollideFlag) == 0 : te->colliding;
                if ((te->circle || te->box) && (L.extents == 2 || colliding)) {
                    const bool circle = te->circle, box = te->box, haveA = te->haveA, haveB = te->haveB;
                    const float a = te->a, b = te->b;
                    const ImU32 c = colliding ? col(255, 120, 60, 200) : col(160, 160, 160, 160);
                    // 3D: a raised copy at the type's height (ZSIZE's top), joined by uprights.
                    float lift = 0;
                    if (L.extents3d && te->zTop) {
                        lift = std::max(0.0f, *te->zTop) * z;
                    }
                    if (circle && haveA) {
                        dl->AddCircle(p, a * z, c, 0, 1.5f);
                        if (lift > 0) {
                            dl->AddCircle(ImVec2(p.x, p.y - lift), a * z, c, 0, 1.0f);
                            dl->AddLine(ImVec2(p.x - a * z, p.y), ImVec2(p.x - a * z, p.y - lift), c);
                            dl->AddLine(ImVec2(p.x + a * z, p.y), ImVec2(p.x + a * z, p.y - lift), c);
                        }
                    } else if (box && haveB) {
                        // A rectangle turned 45 degrees: width along (1, 1), height along (-1, 1).
                        const float ha = a * kBoxHalf, hb = b * kBoxHalf;
                        const ImVec2 q[4] = {S(o.header.x + hb - ha, o.header.y - ha - hb), S(o.header.x + ha + hb, o.header.y + ha - hb),
                                             S(o.header.x + ha - hb, o.header.y + ha + hb), S(o.header.x - ha - hb, o.header.y + hb - ha)};
                        dl->AddPolyline(q, 4, c, ImDrawFlags_Closed, 1.5f);
                        if (lift > 0) {
                            ImVec2 up[4];
                            for (int k = 0; k < 4; ++k) {
                                up[k] = ImVec2(q[k].x, q[k].y - lift);
                                dl->AddLine(q[k], up[k], c);
                            }
                            dl->AddPolyline(up, 4, c, ImDrawFlags_Closed, 1.0f);
                        }
                    }
                }
            }
            if (L.facing) {
                const auto dir = std::visit([](const auto& v) -> std::optional<std::pair<float, float>> {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, nox::map::xfer::Monster> || std::is_same_v<T, nox::map::xfer::Npc>) {
                        return std::pair{static_cast<float>(v.direction[0]), static_cast<float>(v.direction[1])};
                    } else {
                        return std::nullopt;
                    }
                }, o.data);
                if (dir && (dir->first != 0 || dir->second != 0)) {
                    const float len = std::hypot(dir->first, dir->second);
                    const ImVec2 tip(p.x + dir->first / len * 22, p.y + dir->second / len * 22);
                    dl->AddLine(p, tip, col(255, 230, 90, 230), 2);
                    dl->AddCircleFilled(tip, 3, col(255, 230, 90, 230));
                }
            }
            if (L.markDisabled && o.header.details && (o.header.details->flags & kEnabledFlag) == 0) {
                dl->AddCircle(p, 7, col(150, 150, 150, 220), 0, 2);
                dl->AddLine(ImVec2(p.x - 5, p.y + 5), ImVec2(p.x + 5, p.y - 5), col(150, 150, 150, 220), 2);
            }
            if (z >= 0.5f) {
                float ty = p.y + 6;
                const auto label = [&](const std::string& text, ImU32 c) {
                    if (text.empty()) return;
                    const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
                    dl->AddRectFilled(ImVec2(p.x - sz.x / 2 - 2, ty), ImVec2(p.x + sz.x / 2 + 2, ty + sz.y), col(0, 0, 0, 140));
                    dl->AddText(ImVec2(p.x - sz.x / 2, ty), c, text.c_str());
                    ty += sz.y;
                };
                if (L.labelTypes) label(o.type, col(230, 230, 230));
                if (L.labelScriptNames && o.header.details) label(o.header.details->scriptName, col(140, 220, 255));
                if (L.labelExtents) label("#" + std::to_string(o.header.extent), col(255, 210, 120));
            }
        }
    }

    // A selection's outlines (offset: a move being dragged).
    const auto outline = [&](const me::Selection& s, float ox, float oy, int cx, int cy, ImU32 c, float thick) {
        const float r = std::max(7.0f, 11 * z);
        for (const ObjId id : s.objects) {
            const auto it = doc_.objects.find(id);
            if (it == doc_.objects.end()) continue;
            const float x = it->second.header.x + ox, y = it->second.header.y + oy;
            if (visible(x, y)) dl->AddCircle(S(x, y), r, c, 0, thick);
        }
        for (const auto& wp : doc_.waypoints.waypoints) {
            if (s.waypoints.contains(wp.id) && visible(wp.x + ox, wp.y + oy)) dl->AddCircle(S(wp.x + ox, wp.y + oy), 8, c, 0, thick);
        }
        for (const auto cell : s.walls) {
            const float x = (cell.x + cx) * me::kWallCellSize, y = (cell.y + cy) * me::kWallCellSize;
            if (visible(x, y)) dl->AddRect(S(x, y), S(x + 23, y + 23), c, 0, 0, thick);
        }
        for (const auto t : s.tiles) {
            const me::WorldPoint p = me::tileCentre({static_cast<std::int16_t>(t.gx + cx), static_cast<std::int16_t>(t.gy + cy)});
            if (!visible(p.x, p.y)) continue;
            const ImVec2 q[4] = {S(p.x, p.y - 23), S(p.x + 23, p.y), S(p.x, p.y + 23), S(p.x - 23, p.y)};
            dl->AddPolyline(q, 4, c, ImDrawFlags_Closed, thick);
        }
    };
    // Other editors' locks: their colour and name.
    for (const auto& [k, h] : locks_.all()) {
        if (h.peer == self_) continue;
        me::Selection one;
        if (k.kind == me::ElementKey::Kind::Object) one.objects.insert(static_cast<ObjId>(k.a));
        else if (k.kind == me::ElementKey::Kind::Wall) one.walls.insert({static_cast<std::uint8_t>(k.a), static_cast<std::uint8_t>(k.b)});
        else if (k.kind == me::ElementKey::Kind::Tile) one.tiles.insert({static_cast<std::int16_t>(k.a), static_cast<std::int16_t>(k.b)});
        else if (k.kind == me::ElementKey::Kind::Waypoint) one.waypoints.insert(static_cast<std::uint32_t>(k.a));
        else if (k.kind == me::ElementKey::Kind::Polygon) {
            // A polygon: its outline in the holder's colour.
            one.polygons.insert(static_cast<std::uint32_t>(k.a));
            if (const auto pi = doc_.polygonIndex(static_cast<std::uint32_t>(k.a))) {
                std::vector<ImVec2> q;
                for (const auto& c : me::polygonCorners(doc_, *pi)) q.push_back(S(c.x, c.y));
                if (q.size() >= 2) dl->AddPolyline(q.data(), static_cast<int>(q.size()), peerColour(h.peer), ImDrawFlags_Closed, 3.0f);
            }
        }
        outline(one, 0, 0, 0, 0, peerColour(h.peer), 2.5f);
        if (const auto b = me::bounds(doc_, one)) {
            dl->AddText(S(b->first.x, b->first.y - 14 / z), peerColour(h.peer), peerName(h.peer).c_str());
        }
    }
    // The selection in the theme's colour (gold; Dark: orange).
    const editor::ThemeColours& tc = editor::themeColours();
    if (drag_ == Drag::Move && dragMoved_ && preview_.active()) {
        // Shown moved already (the preview): where it came from faint, where it is bright.
        outline(selection_, -moveDx_, -moveDy_, -moveCx_, -moveCy_, tc.viewSelectionFaint, 1.5f);
        outline(selection_, 0, 0, 0, 0, tc.viewSelectionHot, 2.0f);
    } else if (drag_ == Drag::Move && dragMoved_) {
        outline(selection_, 0, 0, 0, 0, tc.viewSelectionFaint, 1.5f);
        const auto [cx, cy] = me::cellStep(doc_, selection_, moveDx_, moveDy_);
        outline(selection_, moveDx_, moveDy_, cx, cy, tc.viewSelectionHot, 2.0f);
    } else {
        outline(selection_, 0, 0, 0, 0, tc.viewSelection, 2.0f);
    }
    // What a click would take.
    if (mapHovered_ && drag_ == Drag::None && tool_ == Tool::Select) {
        if (const auto w = mouseWorld()) {
            const me::Selection under = me::pickAt(doc_, w->x, w->y, clickRadius(), pickFilter());
            outline(under, 0, 0, 0, 0, col(255, 255, 255, 140), 1.0f);
        }
    }
    // The region being dragged.
    if ((drag_ == Drag::Region || drag_ == Drag::AreaCopy) && dragMoved_) {
        std::vector<ImVec2> q;
        for (const auto& p : region_.outline()) q.push_back(S(p.x, p.y));
        const ImU32 c = drag_ == Drag::AreaCopy ? col(120, 220, 255, 255) : col(255, 220, 100, 255);
        if (q.size() >= 3) {
            dl->AddConvexPolyFilled(q.data(), static_cast<int>(q.size()), drag_ == Drag::AreaCopy ? col(120, 220, 255, 30) : col(255, 220, 100, 25));
        }
        if (q.size() >= 2) dl->AddPolyline(q.data(), static_cast<int>(q.size()), c, ImDrawFlags_Closed, 1.5f);
    }
    // A located item's flash: a ring shrinking onto it.
    if (flash_) {
        const float t = static_cast<float>(SDL_GetTicks() - flash_->startMs) / 700.0f;
        if (t >= 1) {
            flash_.reset();
        } else {
            dl->AddCircle(S(flash_->at.x, flash_->at.y), 80 * (1 - t) + 10, col(255, 255, 255, static_cast<int>(255 * (1 - t))), 0, 3);
        }
    }
    if (areaCopyArmed_ && drag_ == Drag::None) {
        dl->AddText(ImVec2(origin.x + 10, origin.y + 10), col(120, 220, 255), "Copy area: drag a box (Escape cancels)");
    }
    drawToolOverlay(dl, origin);
    props_->drawOverlay(dl, origin); // (the link lines, the gizmos)
    if (session_) drawSession(dl, origin, size); // (pending edits, other editors' cursors)
    drawLive(dl, origin);            // (a session's players and creatures, view only)
    drawPick(dl, origin);
    dl->PopClipRect();
}

// --- The live layer -------------------------------------------------------------------------

void MapEditor::setLiveEntities(std::vector<LiveEntity> entities)
{
    // Each one glides from where it's drawn now to its new place over about the time between
    // updates (measured), as the game view moves its drawables between snapshots.
    const std::uint64_t now = SDL_GetTicks();
    if (liveLastMs_ != 0) {
        const float gap = static_cast<float>(now - liveLastMs_);
        liveIntervalMs_ = std::clamp(liveIntervalMs_ * 0.7f + gap * 0.3f, 40.0f, 400.0f);
    }
    liveLastMs_ = now;
    const double frame = static_cast<double>(now) * 30 / 1000;
    std::map<std::uint16_t, LiveTrack> tracks;
    for (const auto& e : entities) {
        LiveTrack t;
        if (const auto it = liveTracks_.find(e.id); it != liveTracks_.end()) {
            t = it->second;
            std::tie(t.fromX, t.fromY) = livePos(e); // (its glide so far: the old track's)
            // (A jump across the map, a respawn or a teleport: there at once.)
            if (std::abs(e.x - t.fromX) > 250 || std::abs(e.y - t.fromY) > 250) {
                t.fromX = e.x;
                t.fromY = e.y;
            }
            if (t.state != e.animState) {
                t.state = e.animState;
                t.animStart = frame;
            }
        } else {
            t.fromX = e.x;
            t.fromY = e.y;
            t.state = e.animState;
            t.animStart = frame;
        }
        t.toX = e.x;
        t.toY = e.y;
        t.atMs = now;
        tracks[e.id] = t;
    }
    liveTracks_ = std::move(tracks);
    live_ = std::move(entities);
}

std::pair<float, float> MapEditor::livePos(const LiveEntity& e) const
{
    const auto it = liveTracks_.find(e.id);
    if (it == liveTracks_.end()) return {e.x, e.y};
    const LiveTrack& t = it->second;
    const float k = std::clamp(static_cast<float>(SDL_GetTicks() - t.atMs) / liveIntervalMs_, 0.0f, 1.0f);
    return {t.fromX + (t.toX - t.fromX) * k, t.fromY + (t.toY - t.fromY) * k};
}

std::vector<MapView::SceneSprite> MapEditor::liveSprites()
{
    std::vector<MapView::SceneSprite> out;
    const editor::Layers& L = settings_.layers;
    for (auto& [id, t] : liveTracks_) t.drawn = false;
    liveOver_.clear();
    if (live_.empty() || (!L.livePlayers && !L.liveCreatures) || !ctx_.art || !ctx_.sprites) return out;
    // Only what's in view (and a body's height round it): nothing is made for the rest.
    const float z = camera_.zoom;
    const float x0 = camera_.left() - 80, y0 = camera_.top() - 40;
    const float x1 = camera_.left() + panelW_ / z + 80, y1 = camera_.top() + panelH_ / z + 160;
    const double time = static_cast<double>(SDL_GetTicks()) * 30 / 1000;
    const auto frame = static_cast<std::uint32_t>(time);
    const nox::client::World* world = sessionHooks_.gameWorld ? sessionHooks_.gameWorld() : nullptr;
    for (const auto& e : live_) {
        if (e.player ? !L.livePlayers : !L.liveCreatures) continue;
        if (e.typeId == 0 || (e.player && e.observer)) continue; // (the marker alone)
        const auto [px, py] = livePos(e);
        if (px < x0 || px > x1 || py < y0 || py > y1) continue;
        const TypeArt* a = ctx_.art->get(e.typeId);
        if (!a) continue;
        LiveTrack& track = liveTracks_[e.id];
        // The pose the game's live draw gives it (main.cpp liveSprites): its facing, its state
        // since it began, a creature's frame from the server.
        Pose pose;
        pose.direction = e.direction;
        pose.animState = e.animState;
        pose.variant = e.frame.value_or(static_cast<std::uint8_t>(e.id));
        pose.netCode = e.id;
        pose.time = time;
        pose.animStart = track.animStart;
        // Its images, back to front, with the colours each is drawn in.
        std::vector<std::pair<nox::things::ImageRef, std::optional<SpriteCache::Materials>>> images;
        if (a->draw.kind == nox::things::DrawKind::Player) {
            // PlayerDraw's stack: the body, armour and weapon layers (player_art.hpp), in the
            // player's colours and their worn things' (our game's player list, when it has them).
            const nox::client::PlayerEntry* player = nullptr;
            if (world) {
                if (const auto p = world->players().find(e.id); p != world->players().end()) player = &p->second;
            }
            const PlayerLook look{e.armorEquip, e.weaponEquip, 0};
            const auto layers = playerLayers(a->draw, pose, look);
            const auto materials = ctx_.modifiers ? layerMaterials(layers, player, *ctx_.modifiers, 0, frame)
                                                  : std::vector<SpriteCache::Materials>{};
            for (std::size_t i = 0; i < layers.size(); ++i) {
                images.emplace_back(layers[i].image, i < materials.size() ? std::optional(materials[i]) : std::nullopt);
            }
        } else if (a->drawFn != DrawFn::NPC) {
            if (const auto img = pickImage(*a, pose)) images.emplace_back(*img, std::nullopt);
        }
        const float x = px - a->halfWidth;
        const float y = py - a->halfHeight - a->z;
        float top = py;
        for (const auto& [image, colours] : images) {
            const Sprite* s = colours ? ctx_.sprites->tinted(image, *colours) : ctx_.sprites->get(image);
            if (!s) continue;
            MapView::SceneSprite g;
            g.sortY = py;
            g.x = x;
            g.y = y;
            g.sprite = s;
            g.lightX = px;
            g.lightY = py;
            out.push_back(std::move(g));
            liveOver_.push_back({image, colours, x, y}); // (drawn faint over everything after: renderMap)
            top = std::min(top, y + static_cast<float>(s->offsetY));
            track.drawn = true;
        }
        if (track.drawn) track.headY = py - top;
    }
    return out;
}

void MapEditor::drawLive(ImDrawList* dl, const ImVec2& origin)
{
    const editor::Layers& L = settings_.layers;
    if (live_.empty() || (!L.livePlayers && !L.liveCreatures)) return;
    const float z = camera_.zoom;
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - camera_.left()) * z, origin.y + (y - camera_.top()) * z); };
    static constexpr ImU32 kTeams[] = {IM_COL32(230, 230, 230, 255), IM_COL32(230, 40, 40, 255),  IM_COL32(50, 90, 240, 255),
                                       IM_COL32(40, 200, 60, 255),   IM_COL32(40, 220, 220, 255), IM_COL32(240, 230, 50, 255),
                                       IM_COL32(170, 70, 230, 255),  IM_COL32(30, 30, 30, 255),   IM_COL32(240, 240, 240, 255),
                                       IM_COL32(250, 150, 40, 255)};
    const float x0 = camera_.left() - 200, y0 = camera_.top() - 60;
    const float x1 = camera_.left() + panelW_ / z + 200, y1 = camera_.top() + panelH_ / z + 200;
    for (const auto& e : live_) {
        if (e.player ? !L.livePlayers : !L.liveCreatures) continue;
        const auto [wx, wy] = livePos(e);
        if (wx < x0 || wx > x1 || wy < y0 || wy > y1) continue;
        const ImVec2 p = S(wx, wy);
        const ImU32 c = kTeams[e.team < std::size(kTeams) ? e.team : 0];
        const auto t = liveTracks_.find(e.id);
        const bool sprite = t != liveTracks_.end() && t->second.drawn; // (drawn as the game draws it, in the scene)
        if (e.player) {
            float labelY = p.y - 8;
            if (sprite) {
                // Their character is drawn in the map: a small team-coloured mark at their feet,
                // their name over their head.
                dl->AddEllipse(p, ImVec2(std::max(6.0f, 12 * z), std::max(3.0f, 6 * z)), c, 0, 0, 1.5f);
                labelY = p.y - t->second.headY * z - ImGui::GetTextLineHeight() - 4;
            } else {
                // A diamond (never a disc: placed objects are discs and rings), name and client kind.
                const float r = 7;
                const ImVec2 q[4] = {ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y)};
                dl->AddConvexPolyFilled(q, 4, e.observer ? (c & 0x80FFFFFFu) : c);
                dl->AddPolyline(q, 4, col(0, 0, 0, 220), ImDrawFlags_Closed, 1.5f);
            }
            if (follow_ && *follow_ == e.id) {
                dl->AddCircle(p, sprite ? std::max(16.0f, 20 * z) : 13.0f, col(255, 220, 90, 230), 0, 2.0f); // (the one the camera follows)
            }
            // (Who's editing) An editor: the pencil before the name, in their colour, and what
            // they're doing (a parked editor observes: "editing" / "idle" says why).
            const auto* ed = session_ ? session_->editorByNetCode(e.id) : nullptr;
            const char* doing = ed ? editor::activityText(ed->activity) : nullptr;
            const std::string label = e.name + (ed ? std::string("  (") + doing + ")" : e.observer ? "  (observing)" : "") +
                                      (e.stockClient ? "  [stock]" : "  [NoX Core]");
            const ImVec2 sz = ImGui::CalcTextSize(label.c_str());
            const float pencil = ed ? std::round(sz.y * 1.3f) : 0;
            const float lx = (sprite ? p.x - (sz.x + pencil) / 2 : p.x + 9) + pencil;
            dl->AddRectFilled(ImVec2(lx - 2 - pencil, labelY - 1), ImVec2(lx + 2 + sz.x, labelY + 1 + sz.y), col(0, 0, 0, 170), 3);
            if (ed) {
                editor::pencilMark(dl, ImVec2(lx - pencil / 2 - 1, labelY + sz.y / 2), pencil, ed->peer, ed->activity == ss::Activity::Idle);
            }
            dl->AddText(ImVec2(lx, labelY), c, label.c_str());
        } else if (!sprite) {
            dl->AddTriangleFilled(ImVec2(p.x, p.y - 5), ImVec2(p.x + 5, p.y + 4), ImVec2(p.x - 5, p.y + 4), col(255, 120, 60, 200));
        }
    }
}

// --- Pick in view ------------------------------------------------------------------------------

namespace {

/// What a pick request of `kind` would take at (x, y): the result, and whether it found what it asks for.
std::pair<MapEditor::PickResult, bool> pickUnder(const me::MapDocument& doc, MapEditor::PickRequest::Kind kind, float x, float y, float radius)
{
    using Kind = MapEditor::PickRequest::Kind;
    MapEditor::PickResult r;
    r.at = {x, y};
    me::KindFilter f{.objects = kind == Kind::Object || kind == Kind::Any, .walls = kind == Kind::Wall || kind == Kind::Any, .tiles = false,
                     .waypoints = kind == Kind::Waypoint || kind == Kind::Any};
    const me::Selection s = me::pickAt(doc, x, y, radius, f);
    if (!s.objects.empty()) r.object = *s.objects.begin();
    if (!s.waypoints.empty()) r.waypoint = *s.waypoints.begin();
    if (!s.walls.empty()) r.wall = *s.walls.begin();
    const bool found = kind == Kind::Point || (kind == Kind::Object && r.object) || (kind == Kind::Waypoint && r.waypoint) ||
                       (kind == Kind::Wall && r.wall) || (kind == Kind::Any && (r.object || r.waypoint || r.wall));
    return {r, found};
}

} // namespace

bool MapEditor::pickInput(const me::WorldPoint& w, bool hovered)
{
    if (!pick_) return false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto [r, found] = pickUnder(doc_, pick_->kind, w.x, w.y, clickRadius());
        if (pick_->kind == PickRequest::Kind::Point && pick_->snap) r.at = {snapValue(w.x), snapValue(w.y)};
        r.shift = ImGui::GetIO().KeyShift;
        if (!found) {
            status("Nothing of that kind there: " + pick_->prompt);
            return true;
        }
        PickRequest req = *pick_;
        if (!req.repeat) pick_.reset();
        if (req.done) req.done(r);
    }
    return true;
}

void MapEditor::drawPick(ImDrawList* dl, const ImVec2& origin)
{
    if (!pick_) return;
    const float z = camera_.zoom;
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - camera_.left()) * z, origin.y + (y - camera_.top()) * z); };
    const char* hint = "  (right click or Escape cancels)";
    const std::string text = pick_->prompt + hint;
    const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
    dl->AddRectFilled(ImVec2(origin.x + 6, origin.y + 6), ImVec2(origin.x + 14 + sz.x, origin.y + 10 + sz.y), col(20, 30, 50, 220), 4);
    dl->AddText(ImVec2(origin.x + 10, origin.y + 8), col(140, 220, 255), text.c_str());
    const auto m = mouseWorld();
    if (!m) return;
    auto [r, found] = pickUnder(doc_, pick_->kind, m->x, m->y, clickRadius());
    me::WorldPoint target = *m;
    if (pick_->kind == PickRequest::Kind::Point && pick_->snap) target = {snapValue(m->x), snapValue(m->y)};
    if (r.object) {
        const auto& h = doc_.objects.at(*r.object).header;
        target = {h.x, h.y};
        dl->AddCircle(S(h.x, h.y), std::max(9.0f, 13 * z), col(120, 220, 255), 0, 2.5f);
    } else if (r.waypoint) {
        if (const auto* wp = me::findWaypoint(doc_, *r.waypoint)) {
            target = {wp->x, wp->y};
            dl->AddCircle(S(wp->x, wp->y), 10, col(120, 220, 255), 0, 2.5f);
        }
    } else if (r.wall) {
        const float x = r.wall->x * me::kWallCellSize, y = r.wall->y * me::kWallCellSize;
        dl->AddRect(S(x, y), S(x + 23, y + 23), col(120, 220, 255), 0, 0, 2.5f);
    }
    if (pick_->kind == PickRequest::Kind::Point) {
        const ImVec2 p = S(target.x, target.y);
        dl->AddLine(ImVec2(p.x - 8, p.y), ImVec2(p.x + 8, p.y), col(120, 220, 255), 2);
        dl->AddLine(ImVec2(p.x, p.y - 8), ImVec2(p.x, p.y + 8), col(120, 220, 255), 2);
    }
    if (pick_->from) {
        dl->AddLine(S(pick_->from->x, pick_->from->y), S(target.x, target.y), found ? col(120, 220, 255, 220) : col(160, 160, 160, 160), 2);
    }
}

std::string MapEditor::underCursor(float x, float y) const
{
    std::string out;
    const auto add = [&](const std::string& s) {
        out += (out.empty() ? "" : "  |  ") + s;
    };
    const float r = clickRadius();
    const me::Selection p = me::pickAt(doc_, x, y, r, me::KindFilter{.objects = true, .walls = false, .tiles = false, .waypoints = true});
    for (const auto id : p.waypoints) {
        const auto it = std::ranges::find(doc_.waypoints.waypoints, id, &nox::map::Waypoint::id);
        add("waypoint " + std::to_string(id) + (it->name.empty() ? "" : " \"" + it->name + "\"") + ((it->flags & 1) ? "" : " (off)") + ", " +
            std::to_string(it->links.size()) + " links");
    }
    for (const ObjId id : p.objects) {
        const auto& o = doc_.objects.at(id);
        std::string s = o.type + " #" + std::to_string(o.header.extent);
        if (o.header.details && !o.header.details->scriptName.empty()) s += " \"" + o.header.details->scriptName + "\"";
        add(s);
    }
    if (const auto c = me::wallCellAt(x, y)) {
        if (const auto st = me::wallState(doc_, *c)) {
            const auto& w = st->wall;
            std::string s = "wall " + (w.material < things_.walls.size() ? things_.walls[w.material].name : std::to_string(w.material)) +
                            " dir " + std::to_string(w.direction & 0x7F) + " var " + std::to_string(w.variant);
            if (st->secret) s += " secret";
            if (st->destructible) s += " breakable";
            if (st->window) s += " window";
            add(s);
        }
    }
    if (const auto t = me::tileAt(x, y)) {
        if (const auto* tile = doc_.floor.tile(t->gx, t->gy)) {
            std::string s = "floor " + (tile->image < things_.floors.size() ? things_.floors[tile->image].name : std::to_string(tile->image)) +
                            " var " + std::to_string(tile->variant);
            if (!tile->edges.empty()) {
                s += ", edges:";
                for (const auto& e : tile->edges) {
                    s += " " + (e.edge < things_.edges.size() ? things_.edges[e.edge].name : std::to_string(e.edge)) + "/" + std::to_string(e.direction);
                }
            }
            add(s);
        }
    }
    return out;
}

} // namespace noxsdl
