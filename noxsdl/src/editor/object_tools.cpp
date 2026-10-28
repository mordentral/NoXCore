// The map editor's objects (object_tools.hpp): placement, the scatter brush, prefabs, the
// object layers, the summary, commands, test steps and the saved state. The panels are in
// content_browser.cpp and object_list.cpp.
#include "editor/object_tools.hpp"

#include "editor/editor_text.hpp"
#include "editor/map_editor.hpp"
#include "editor/property_panel.hpp"
#include "sprite_cache.hpp"
#include "thing_art.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/mapedit/object_edit.hpp"
#include "noxcore/mapedit/paint.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <numbers>
#include <sstream>

namespace fs = std::filesystem;
namespace me = nox::mapedit;
namespace xf = nox::map::xfer;

namespace noxsdl {

namespace {

using editor::numbersAfter;

constexpr float kBoxHalf = 0.35355339f; // a BOX's corners: 1 / (2 sqrt 2) of its sides (turned 45 degrees)

/// The creature's AnimDir (the 3 x 3 grid index the draw picks a facing by) for its grid step.
std::uint8_t animDir(const std::array<std::int32_t, 2>& d)
{
    return static_cast<std::uint8_t>((std::clamp(d[1], -1, 1) + 1) * 3 + (std::clamp(d[0], -1, 1) + 1));
}

} // namespace

ObjectTools::ObjectTools(MapEditor& editor)
    : ed_(editor)
{
    const auto& c = ed_.ctx_;
    readDir_ = c.userDir.empty() ? c.prefDir : c.userDir;
    writeDir_ = !c.userDir.empty() ? c.userDir : (c.screenshotRun ? fs::path() : c.prefDir);
    if (!c.gameDir.empty()) {
        try {
            // The solo table: every stock map's creatures carry its numbers (placement.hpp).
            monsters_ = nox::things::loadMonsterDb(c.gameDir / "monster.bin", true);
        } catch (const std::exception& ex) {
            SDL_Log("map editor: no monster.bin (%s): new creatures take their type's numbers", ex.what());
        }
    }
    factory_ = std::make_unique<me::ObjectFactory>(ed_.things_, c.modifiers, monsters_ ? &*monsters_ : nullptr, c.gameData);
    catalog_ = me::ContentCatalog::build(ed_.things_);
    catalog_.setDisplayNames(ed_.things_, c.strings, c.modifiers); // (the game's names: search finds them too)
    if (!c.gameDir.empty()) {
        // The thumbnails of the types the game draws in colours (NPCs, maidens, weapons, armour,
        // flags) in their fresh records' colours: what a new one looks like in the game.
        std::map<std::size_t, RecordLook> looks;
        if (c.modifiers && c.art) {
            for (std::size_t i = 0; i < ed_.things_.things.size(); ++i) {
                const auto& def = ed_.things_.things[i];
                const TypeArt* a = me::placeableType(def) ? c.art->get(def.name) : nullptr;
                if (!a || (a->drawFn != DrawFn::NPC && a->drawFn != DrawFn::Maiden && a->itemColours == ItemColours::None)) continue;
                try {
                    if (auto look = lookOf(factory_->make(def.name, 0, 0), {}); look.kind != RecordLook::Kind::Plain) looks.emplace(i, std::move(look));
                } catch (const std::exception&) {
                    // (drawn as it is)
                }
            }
        }
        thumbs_ = std::make_unique<editor::ThingThumbnails>(ed_.things_, c.gameDir, c.imagePack, writeDir_, c.modifiers, std::move(looks));
    }
    loadState();
    loadUserCategories();
    reloadPrefabs();
}

ObjectTools::~ObjectTools()
{
    saveState();
}

// --- Commands --------------------------------------------------------------------------------------

void ObjectTools::registerCommands()
{
    using editor::Command;
    using editor::Shortcut;
    auto& reg = ed_.commands_;
    const auto add = [&](const char* id, const char* label, const char* category, const char* shortcut, const char* help, std::function<void()> run,
                         std::function<bool()> enabled = {}, std::function<bool()> checked = {}) {
        reg.add(Command{id, label, category, Shortcut::parse(shortcut).value_or(Shortcut{}), help, std::move(run), std::move(enabled), std::move(checked)});
    };
    add("window.browser", "Content Browser", "Window", "Ctrl+Shift+B", "Every object type by folder with thumbnails; floors, walls and edges.",
        [this] { showBrowser = !showBrowser; }, {}, [this] { return showBrowser; });
    add("window.objectList", "Object List", "Window", "Ctrl+Shift+J", "Every object on the map: search, sort, select, go to.",
        [this] { showList = !showList; }, {}, [this] { return showList; });
    add("object.rotateLeft", "Rotate Left", "Tools", "Q",
        "The object tool's facing (or the selected objects') a step anticlockwise: doors a quarter, creatures an eighth, sentries 5 degrees.",
        [this] { rotate(-1); });
    add("object.rotateRight", "Rotate Right", "Tools", "E", "The object tool's facing (or the selected objects') a step clockwise.", [this] { rotate(1); });
    add("object.scatter", "Scatter Brush", "Tools", "", "The object tool paints objects from a weighted list (density, spacing, random facing).",
        [this] {
            mode_ = mode_ == Mode::Scatter ? Mode::Place : Mode::Scatter;
            ed_.tool_ = MapEditor::Tool::Object;
        },
        {}, [this] { return mode_ == Mode::Scatter; });
    add("object.random", "Random From Folder", "Tools", "", "Each object placed is a random type from the browser's folder (the old editor's Randomize).",
        [this] { randomFromFolder_ = !randomFromFolder_; }, {}, [this] { return randomFromFolder_; });
    add("prefab.save", "Save Selection as Prefab...", "Edit", "", "The selected objects (and walls, floor, waypoints) saved under a name, to place again.",
        [this] {
            prefabName_[0] = 0;
            prefabOpen_ = true;
        },
        [this] { return !ed_.selection_.empty(); });
    const auto layer = [&](const char* id, const char* label, bool editor::Layers::*field) {
        add(id, label, "View", "", "Shows or hides this layer.", [this, field] { ed_.settings_.layers.*field = !(ed_.settings_.layers.*field); }, {},
            [this, field] { return ed_.settings_.layers.*field; });
    };
    layer("view.triggers", "Trigger and Plate Boxes", &editor::Layers::triggers);
    layer("view.lightRings", "Light Radius Rings", &editor::Layers::lightRings);
}

// --- Placement -------------------------------------------------------------------------------------

me::WorldPoint ObjectTools::placePoint(float wx, float wy) const
{
    static constexpr me::Snap kSnaps[] = {me::Snap::None, me::Snap::Grid, me::Snap::HalfGrid, me::Snap::Custom};
    const me::Snap snap = kSnaps[std::clamp(ed_.settings_.snap, 0, 3)];
    const auto* e = catalog_.find(type_);
    const bool door = prefab_ < 0 && e && e->xfer == "DoorXfer"; // (a door always on the grid, as the old editor placed them)
    me::WorldPoint p = me::snapPoint({wx, wy}, snap, ed_.settings_.snapCustom, door);
    p.x = std::clamp(p.x, 1.0f, me::kWorldSize - 1);
    p.y = std::clamp(p.y, 1.0f, me::kWorldSize - 1);
    return p;
}

std::optional<me::DocObject> ObjectTools::makeAt(const std::string& type, me::WorldPoint at)
{
    try {
        me::DocObject o = factory_->make(type, at.x, at.y);
        me::setFacingDegrees(o, facing_);
        return o;
    } catch (const std::exception& ex) {
        ed_.status("Can't place " + type + ": " + ex.what());
        return std::nullopt;
    }
}

std::string ObjectTools::randomType()
{
    std::vector<const me::ContentEntry*> pool;
    for (const auto* e : catalog_.inFolder(folder_, true)) {
        if (e->placeable) pool.push_back(e);
    }
    if (pool.empty()) return type_;
    const float r = me::scatterRandom(0x5EED0000u + ed_.strokeSeed_, placements_);
    return pool[std::min(pool.size() - 1, static_cast<std::size_t>(r * static_cast<float>(pool.size())))]->name;
}

bool ObjectTools::placeAt(me::WorldPoint at, std::string type)
{
    me::Selection placed;
    if (type.empty() && prefab_ >= 0 && prefab_ < static_cast<int>(prefabs_.size())) {
        const me::Prefab& p = prefabs_[static_cast<std::size_t>(prefab_)];
        if (!ed_.perform(me::placePrefabOp(ed_.doc_, p, at, &placed))) return false;
        ed_.setSelection(std::move(placed), true);
        ++placements_;
        return true;
    }
    if (type.empty()) type = randomFromFolder_ ? randomType() : type_;
    if (type.empty()) {
        ed_.status("Pick something to place in the Content Browser");
        return false;
    }
    const auto o = makeAt(type, at);
    if (!o) return false;
    if (!ed_.perform(me::placeObjectsOp(ed_.doc_, {*o}, &placed))) return false;
    ed_.setSelection(std::move(placed), true);
    noteRecent(type);
    ++placements_;
    return true;
}

void ObjectTools::rotate(int steps)
{
    if (ed_.tool_ == MapEditor::Tool::Object) {
        const auto* e = catalog_.find(type_);
        const float step = e && e->xfer == "DoorXfer" ? 90.0f : e && e->xfer == "SentryXfer" ? 5.0f : 45.0f;
        facing_ = std::fmod(facing_ + static_cast<float>(steps) * step + 3600.0f, 360.0f);
        ed_.status("Facing " + std::to_string(static_cast<int>(std::lround(facing_))) + " degrees");
        return;
    }
    std::vector<std::pair<me::ObjId, me::DocObject>> turned;
    for (const me::ObjId id : ed_.selection_.objects) {
        me::DocObject o = ed_.doc_.objects.at(id);
        if (me::rotateFacing(o, steps)) turned.emplace_back(id, std::move(o));
    }
    if (turned.empty()) {
        ed_.status("Nothing selected turns (doors, creatures, NPCs and sentries do)");
        return;
    }
    ed_.perform(me::setObjectsOp(ed_.doc_, turned, "Rotate " + std::to_string(turned.size()) + (turned.size() == 1 ? " object" : " objects")));
}

void ObjectTools::deleteAt(float wx, float wy)
{
    const me::Selection pick = me::pickAt(ed_.doc_, wx, wy, ed_.clickRadius(), me::KindFilter{.objects = true, .walls = false, .tiles = false, .waypoints = false});
    if (pick.objects.empty()) return;
    ed_.perform(me::deleteOp(ed_.doc_, pick));
}

void ObjectTools::noteRecent(const std::string& type)
{
    std::erase(recent_, type);
    recent_.push_front(type);
    while (recent_.size() > kRecent) recent_.pop_back();
}

void ObjectTools::input(float wx, float wy, bool hovered, bool rightClick)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (rightClick && rightDeletes_) {
        deleteAt(wx, wy);
        return;
    }
    if (mode_ == Mode::Scatter) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            me::ScatterBrush b = scatter_;
            if (b.types.empty() && !type_.empty()) b.types = {{type_, 1}};
            b.seed = 0x5CA7u + ed_.strokeSeed_;
            if (b.types.empty()) {
                ed_.status("Scatter: add types to the list (or pick one in the browser)");
                return;
            }
            stroke_.emplace(ed_.doc_, *factory_, b);
            stroke_->dab({wx, wy});
            lastDab_ = {wx, wy};
            return;
        }
        if (!stroke_) return;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float d = std::hypot(wx - lastDab_.x, wy - lastDab_.y);
            if (d >= std::max(8.0f, scatter_.radius * 0.5f)) {
                stroke_->dab({wx, wy});
                lastDab_ = {wx, wy};
            }
        } else {
            me::Selection placed;
            const me::Op op = stroke_->finish(&placed);
            stroke_.reset();
            ++ed_.strokeSeed_;
            if (op.empty()) {
                ed_.status("Scatter: nothing fitted (spacing, walls or holes)");
            } else if (ed_.perform(op)) {
                ed_.setSelection(std::move(placed), true);
            }
        }
        return;
    }
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (io.KeyAlt) {
        // The eyedropper: the type under the mouse into the tool.
        const me::Selection pick = me::pickAt(ed_.doc_, wx, wy, ed_.clickRadius(), me::KindFilter{.objects = true, .walls = false, .tiles = false, .waypoints = false});
        if (!pick.objects.empty()) {
            const me::DocObject& o = ed_.doc_.objects.at(*pick.objects.begin());
            pickType(o.type);
            if (const auto f = me::facingDegrees(o)) facing_ = *f;
            ed_.status("Eyedropper: " + o.type);
        }
        return;
    }
    placeAt(placePoint(wx, wy));
}

std::vector<MapView::SceneSprite> ObjectTools::ghosts()
{
    std::vector<MapView::SceneSprite> out;
    const bool dragging = !dragType_.empty() || dragPrefab_ >= 0;
    if ((ed_.tool_ != MapEditor::Tool::Object && !dragging) || (!ed_.mapHovered_ && !dragging)) return out;
    const auto mw = ed_.mouseWorld();
    std::vector<me::DocObject> objects;
    std::vector<std::vector<const me::DocObject*>> carried; // (each object's inventory: a prefab's NPC's worn things)
    if (stroke_) {
        objects = stroke_->objects();
    } else if (mw) {
        const std::string type = dragging ? dragType_ : type_;
        const int prefab = dragging ? dragPrefab_ : prefab_;
        const auto saved = prefab_;
        const auto savedType = type_;
        prefab_ = prefab;
        type_ = type;
        const me::WorldPoint at = placePoint(mw->x, mw->y);
        prefab_ = saved;
        type_ = savedType;
        if (prefab >= 0 && prefab < static_cast<int>(prefabs_.size())) {
            const auto& p = prefabs_[static_cast<std::size_t>(prefab)];
            const me::WorldPoint c = p.centre();
            for (const auto& e : p.clip.objects) {
                if (e.object.parent != 0) continue;
                me::DocObject o = e.object;
                o.header.x += at.x - c.x;
                o.header.y += at.y - c.y;
                objects.push_back(std::move(o));
                carried.resize(objects.size());
                for (const auto& item : p.clip.objects) {
                    if (item.object.parent == e.id) carried.back().push_back(&item.object);
                }
            }
        } else if (mode_ == Mode::Place && !type.empty()) {
            if (auto o = makeAt(type, at)) objects.push_back(std::move(*o));
        }
    }
    const double time = static_cast<double>(SDL_GetTicks()) * 30 / 1000;
    static const std::vector<const me::DocObject*> kNone;
    for (std::size_t k = 0; k < objects.size(); ++k) {
        const auto& o = objects[k];
        const TypeArt* a = ed_.ctx_.art->get(o.type);
        if (!a) continue;
        std::optional<nox::things::ImageRef> img;
        const auto take = [&](const nox::things::ImageRef* ref) {
            if (ref) img = *ref;
        };
        std::uint8_t dir = 7; // (AnimDir: south)
        if (const auto* d = std::get_if<xf::Door>(&o.data)) {
            take(mapObjectImage(*a, 0, static_cast<std::uint8_t>(d->currentDirection & 31), std::nullopt, true, 0, 0, time));
        } else if (const auto* m = std::get_if<xf::Monster>(&o.data); m && a->drawFn != DrawFn::NPC) {
            // A creature (a maiden too: MaidenDraw is MonsterDraw in her colours) facing its way.
            Pose pose;
            pose.direction = dir = animDir(m->direction);
            pose.time = time;
            take(pickImage(*a, pose));
            if (!img && a->still) img = a->still;
        } else if (a->still) {
            take(mapObjectImage(*a, 0, std::nullopt, std::nullopt, true, 0, 0, time));
        }
        if (const auto* n = std::get_if<xf::Npc>(&o.data)) dir = animDir(n->direction);
        // In the colours the game will draw it in: the record's own (a prefab's, a copy's), else the
        // type's fresh one's (player_art.hpp's recordLook).
        std::vector<LookLayer> layers;
        if (ed_.ctx_.modifiers) {
            layers = lookLayers(*a, lookOf(o, k < carried.size() ? carried[k] : kNone), img, *ed_.ctx_.art, *ed_.ctx_.modifiers, dir, time);
        } else if (img) {
            layers.push_back({*img, std::nullopt});
        }
        if (layers.empty()) {
            // Nothing the scene would draw (an NPC without NewPlayer's art): its menu icon.
            const auto* def = ed_.things_.findThing(o.type);
            if (def && def->menuIcon && def->menuIcon->index >= 0) layers.push_back({*def->menuIcon, std::nullopt});
        }
        for (const auto& layer : layers) {
            const Sprite* s = layer.colours ? ed_.ctx_.sprites->tinted(layer.image, *layer.colours) : ed_.ctx_.sprites->get(layer.image);
            if (!s) continue;
            MapView::SceneSprite g;
            g.sortY = o.header.y;
            g.x = o.header.x - a->halfWidth;
            g.y = o.header.y - a->halfHeight - a->z;
            g.sprite = s;
            g.lightX = o.header.x;
            g.lightY = o.header.y;
            g.alpha = stroke_ ? 200 : 150;
            if (ed_.session_) {
                // In a session: what isn't in the game yet, in the pending look.
                g.alpha = stroke_ ? 255 : 230;
                g.pendingLook = static_cast<std::uint8_t>(nox::mapedit::PendingLook::Pending);
                g.pendingImage = ed_.ctx_.sprites->indexOf(layer.image);
            }
            g.light = std::array<std::uint8_t, 3>{255, 255, 255};
            out.push_back(std::move(g));
        }
    }
    return out;
}

RecordLook ObjectTools::lookOf(const me::DocObject& o, const std::vector<const me::DocObject*>& items) const
{
    const TypeArt* a = ed_.ctx_.art->get(o.type);
    if (!a || !ed_.ctx_.modifiers) return {};
    std::vector<RecordItem> list;
    list.reserve(items.size());
    for (const auto* item : items) {
        RecordItem r;
        r.type = item->type;
        r.worn = item->header.details && (item->header.details->flags & me::ObjectFlag::Equipped);
        if (const auto* w = std::get_if<xf::Weapon>(&item->data)) r.modifiers = &w->modifiers;
        else if (const auto* ar = std::get_if<xf::Armor>(&item->data)) r.modifiers = &ar->modifiers;
        else if (const auto* am = std::get_if<xf::Ammo>(&item->data)) r.modifiers = &am->modifiers;
        list.push_back(r);
    }
    return recordLook(*a, o.data, list, ed_.things_, *ed_.ctx_.modifiers);
}

// --- The object layers and the tool's overlay --------------------------------------------------------

void ObjectTools::drawOverlay(ImDrawList* dl, const ImVec2& origin)
{
    const Camera& cam = ed_.camera_;
    const float z = cam.zoom;
    const float left = cam.left(), top = cam.top();
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - left) * z, origin.y + (y - top) * z); };
    const float x0 = left - 200, y0 = top - 200, x1 = left + ed_.panelW_ / z + 200, y1 = top + ed_.panelH_ / z + 200;
    const editor::Layers& L = ed_.settings_.layers;
    constexpr float kPi = std::numbers::pi_v<float>;
    if (L.objects) {
        for (const auto& [id, o] : ed_.doc_.objects) {
            const float x = o.header.x, y = o.header.y;
            if (o.parent != 0 || x < x0 || x > x1 || y < y0 || y > y1) continue;
            const ImVec2 p = S(x, y);
            if (const auto* l = std::get_if<xf::InvisibleLight>(&o.data)) {
                // An invisible light draws nothing: three rings mark it (the old editor's preview mark), in its colour.
                const ImU32 col = IM_COL32(std::clamp(l->color[0], 60, 255), std::clamp(l->color[1], 60, 255), std::clamp(l->color[2], 60, 255), 200);
                for (const float r : {4.0f, 6.0f, 8.0f}) dl->AddCircle(p, r * std::max(0.6f, std::min(z, 2.0f)), col, 0, 1.2f);
            }
            if (L.triggers) {
                if (const auto* t = std::get_if<xf::Trigger>(&o.data)) {
                    // Its box turned 45 degrees (the old editor's trigger and plate outline), in its edge colour.
                    const float ha = static_cast<float>(t->sizeX) * kBoxHalf, hb = static_cast<float>(t->sizeY) * kBoxHalf;
                    const ImVec2 q[4] = {S(x + hb - ha, y - ha - hb), S(x + ha + hb, y + ha - hb), S(x + ha - hb, y + ha + hb), S(x - ha - hb, y + hb - ha)};
                    const auto& c = t->edgeColor;
                    const bool black = (c[0] | c[1] | c[2]) == 0;
                    const ImU32 col = black ? IM_COL32(218, 112, 214, 200) : IM_COL32(c[0], c[1], c[2], 220);
                    dl->AddConvexPolyFilled(q, 4, (col & 0x00FFFFFF) | 0x28000000);
                    dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, 1.5f);
                }
            }
            if (L.lightRings) {
                if (const auto* l = std::get_if<xf::InvisibleLight>(&o.data)) {
                    const ImU32 col = IM_COL32(std::clamp(l->color[0], 0, 255), std::clamp(l->color[1], 0, 255), std::clamp(l->color[2], 0, 255), 210);
                    if (const auto cone = lightConeNow(ed_.doc_, ed_.things_, o, static_cast<std::uint32_t>(SDL_GetTicks() * 30 / 1000))) {
                        // A cone: its kite as the game lights it now (turning, sweeping or aiming).
                        drawLightCone(dl, S, x, y, static_cast<float>(l->radius), *cone, col, false);
                    } else {
                        dl->AddCircle(p, std::max(2.0f, static_cast<float>(l->radius) * z), col, 0, 1.5f);
                    }
                    dl->AddCircleFilled(p, 3, col);
                }
            }
            if (L.facing) {
                // Doors (the way they hang) and sentries (their beam, 80 units); creatures: the view's arrows.
                const auto deg = me::facingDegrees(o);
                const auto kind = me::facingKind(o);
                if (deg && (kind == me::FacingKind::Door || kind == me::FacingKind::Sentry)) {
                    const float r = *deg * kPi / 180.0f;
                    const float len = kind == me::FacingKind::Sentry ? 80.0f : 20.0f;
                    const ImU32 col = kind == me::FacingKind::Sentry ? IM_COL32(190, 120, 255, 220) : IM_COL32(205, 140, 70, 230);
                    const ImVec2 tip = S(x + std::cos(r) * len, y + std::sin(r) * len);
                    dl->AddLine(p, tip, col, 2);
                    dl->AddCircleFilled(tip, 3, col);
                }
            }
        }
    }
    // The tool's cursor: where a click places, the scatter brush's reach.
    if (ed_.tool_ != MapEditor::Tool::Object || !ed_.mapHovered_) return;
    const auto mw = ed_.mouseWorld();
    if (!mw) return;
    if (mode_ == Mode::Scatter) {
        dl->AddCircle(S(mw->x, mw->y), scatter_.radius * z, IM_COL32(255, 230, 120, 170), 0, 1.5f);
        return;
    }
    const me::WorldPoint at = placePoint(mw->x, mw->y);
    const ImVec2 p = S(at.x, at.y);
    dl->AddLine(ImVec2(p.x - 6, p.y), ImVec2(p.x + 6, p.y), IM_COL32(255, 230, 120, 220), 1.5f);
    dl->AddLine(ImVec2(p.x, p.y - 6), ImVec2(p.x, p.y + 6), IM_COL32(255, 230, 120, 220), 1.5f);
    std::string label = prefab_ >= 0 && prefab_ < static_cast<int>(prefabs_.size()) ? "prefab " + prefabs_[static_cast<std::size_t>(prefab_)].name
                        : randomFromFolder_                                         ? "random from " + folder_
                                                                                    : type_;
    if (!label.empty()) {
        const auto* e = catalog_.find(type_);
        if (prefab_ < 0 && e && e->xfer != "DefaultXfer") {
            const float r = facing_ * kPi / 180.0f;
            dl->AddLine(p, ImVec2(p.x + std::cos(r) * 18, p.y + std::sin(r) * 18), IM_COL32(255, 230, 90, 230), 2);
        }
        dl->AddText(ImVec2(p.x + 8, p.y + 6), IM_COL32(255, 240, 180, 230), label.c_str());
    }
}

// --- The summary in Properties -------------------------------------------------------------------------

void ObjectTools::summary(me::ObjId id)
{
    const auto it = ed_.doc_.objects.find(id);
    if (it == ed_.doc_.objects.end()) return;
    const me::DocObject& o = it->second;
    const auto* e = catalog_.find(o.type);
    const auto row = [&](const char* label, const std::string& value) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(90 * ed_.imgui_->scale());
        ImGui::TextUnformatted(value.c_str());
    };
    const auto f = [](std::uint32_t bits) {
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", std::bit_cast<float>(bits));
        return std::string(b);
    };
    ImGui::SeparatorText("Type");
    if (e) {
        row("Folder", e->folder);
        row("Xfer", e->xfer + " v0x" + [&] {
            char b[8];
            std::snprintf(b, sizeof b, "%X", o.xferVersion);
            return std::string(b);
        }());
    }
    if (const auto deg = me::facingDegrees(o)) row("Facing", std::to_string(static_cast<int>(std::lround(*deg))) + " degrees (Q / E turn it)");
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, xf::Door>) {
                static constexpr const char* kLocks[] = {"none", "silver", "gold", "ruby", "sapphire", "mechanism"};
                row("Lock", v.lock < 6 ? kLocks[v.lock] : std::to_string(v.lock));
                row("Direction", std::to_string(v.direction));
            } else if constexpr (std::is_same_v<T, xf::Monster>) {
                row("Health", std::to_string(v.health));
                row("Sight", f(v.sightRange));
                row("Aggression", f(v.aggression));
                row("Action", v.action);
                row("Spells", std::to_string(v.spells.size()) + (v.unknown337[3] ? " (its type's)" : ""));
                if (v.shop) row("Shop", std::to_string(v.shop->items.size()) + " items");
            } else if constexpr (std::is_same_v<T, xf::Npc>) {
                row("Health", std::to_string(v.healthCurrent) + " / " + std::to_string(v.healthMax));
                row("Voice", v.voiceSet);
                row("Action", v.action);
            } else if constexpr (std::is_same_v<T, xf::Trigger>) {
                row("Size", std::to_string(v.sizeX) + " x " + std::to_string(v.sizeY));
                row("Pressed", v.onPressed.function.empty() ? "-" : v.onPressed.function);
            } else if constexpr (std::is_same_v<T, xf::InvisibleLight>) {
                row("Radius", std::to_string(v.radius));
                char b[48];
                std::snprintf(b, sizeof b, "%.2f, colour %d %d %d", v.intensity, v.color[0], v.color[1], v.color[2]);
                row("Intensity", b);
            } else if constexpr (std::is_same_v<T, xf::Exit>) {
                row("Map", v.mapName.substr(0, v.mapName.find('\0')).empty() ? "-" : v.mapName.substr(0, v.mapName.find('\0')));
            } else if constexpr (std::is_same_v<T, xf::Transporter>) {
                row("Links to", v.targetExtent ? "#" + std::to_string(v.targetExtent) : "-");
            } else if constexpr (std::is_same_v<T, xf::Elevator> || std::is_same_v<T, xf::ElevatorShaft>) {
                row("Links to", v.linkedExtent ? "#" + std::to_string(v.linkedExtent) : "-");
            } else if constexpr (std::is_same_v<T, xf::Gold>) {
                row("Amount", std::to_string(v.amount));
            } else if constexpr (std::is_same_v<T, xf::Obelisk>) {
                row("Mana", std::to_string(v.manaPool));
            } else if constexpr (std::is_same_v<T, xf::Readable>) {
                row("Text", v.text.substr(0, v.text.find('\0')).empty() ? "-" : v.text.substr(0, v.text.find('\0')));
            } else if constexpr (std::is_same_v<T, xf::Weapon> || std::is_same_v<T, xf::Armor>) {
                row("Durability", std::to_string(v.health));
                std::string mods;
                for (const auto& m : v.modifiers) {
                    if (!m.empty()) mods += (mods.empty() ? "" : ", ") + m;
                }
                row("Modifiers", mods.empty() ? "-" : mods);
            } else if constexpr (std::is_same_v<T, xf::Sentry>) {
                char b[32];
                std::snprintf(b, sizeof b, "%.2f rad, speed %.3f", v.basePosition, v.rotateSpeed);
                row("Beam", b);
            } else if constexpr (std::is_same_v<T, xf::MonsterGenerator>) {
                std::size_t n = 0;
                for (const auto& g : v.groups) n += g.size();
                row("Templates", std::to_string(n));
            } else if constexpr (std::is_same_v<T, xf::RewardMarker>) {
                char b[16];
                std::snprintf(b, sizeof b, "0x%X", v.rewardType);
                row("Kinds", b);
            }
        },
        o.data);
}

// --- Prefabs -------------------------------------------------------------------------------------------

void ObjectTools::reloadPrefabs()
{
    prefabs_ = me::loadPrefabs(readDir_ / "prefabs", ed_.things_);
    if (prefab_ >= static_cast<int>(prefabs_.size())) prefab_ = -1;
}

void ObjectTools::savePrefab(const std::string& name)
{
    if (ed_.selection_.empty()) {
        ed_.status("Select what the prefab holds first");
        return;
    }
    const me::Prefab p = me::makePrefab(ed_.doc_, ed_.selection_, name);
    if (writeDir_.empty()) {
        // (a test's run with no user folder: kept for this session only)
        prefabs_.push_back(p);
        ed_.status("Prefab " + name + " kept for this session (no user folder to save it in)");
        return;
    }
    try {
        const fs::path file = me::savePrefab(writeDir_ / "prefabs", p, ed_.things_);
        reloadPrefabs();
        ed_.status("Saved prefab " + name + " (" + std::to_string(p.objectCount()) + " objects) to " + file.string());
    } catch (const std::exception& ex) {
        ed_.status(std::string("Prefab not saved: ") + ex.what());
    }
}

void ObjectTools::prefabDialog()
{
    if (prefabOpen_) {
        ImGui::OpenPopup("Save Prefab");
        prefabOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Save Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("%zu selected: they're saved with their places relative to each other,", ed_.selection_.size());
    ImGui::TextUnformatted("their links between each other (teleporters, elevators, movers, waypoints) kept.");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##prefab", "name", prefabName_, sizeof prefabName_);
    const bool ok = prefabName_[0] != 0;
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button("Save") || (ok && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        savePrefab(prefabName_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// --- Saved state -----------------------------------------------------------------------------------

void ObjectTools::loadState()
{
    std::ifstream in(readDir_ / "content.txt");
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "favourite" && catalog_.find(v)) favourites_.insert(v);
        else if (k == "recent" && catalog_.find(v) && recent_.size() < kRecent) recent_.push_back(v);
        else if (k == "cell") cell_ = std::clamp(static_cast<float>(std::atof(v.c_str())), 40.0f, 160.0f);
    }
}

void ObjectTools::saveState()
{
    if (writeDir_.empty()) return;
    std::ostringstream out;
    for (const auto& f : favourites_) out << "favourite=" << f << "\n";
    for (const auto& r : recent_) out << "recent=" << r << "\n";
    out << "cell=" << cell_ << "\n";
    std::error_code ec;
    fs::create_directories(writeDir_, ec);
    std::ofstream(writeDir_ / "content.txt", std::ios::trunc) << out.str();
}

void ObjectTools::loadUserCategories()
{
    userCats_.clear();
    try {
        const auto bytes = nox::readFileBytes(readDir_ / "categories.xml");
        userCats_ = me::parseCategoriesXml(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    } catch (const std::exception&) {
        // (none yet)
    }
}

// --- Test steps --------------------------------------------------------------------------------------

bool ObjectTools::runStep(const std::string& step)
{
    if (step.starts_with("object:")) {
        pickType(step.substr(7));
        return true;
    }
    if (const auto n = numbersAfter(step, "facing:", 1)) {
        facing_ = (*n)[0];
        return true;
    }
    if (const auto n = numbersAfter(step, "place:", 2)) {
        placeAt(placePoint((*n)[0], (*n)[1]));
        return true;
    }
    if (step.starts_with("browse:")) {
        browse(step.substr(7));
        return true;
    }
    if (step.starts_with("search:")) {
        std::snprintf(search_, sizeof search_, "%s", step.substr(7).c_str());
        return true;
    }
    if (step.starts_with("showall:")) {
        showAll_ = step.substr(8) == "1";
        return true;
    }
    if (step.starts_with("randomfolder:")) {
        randomFromFolder_ = step.substr(13) == "1";
        return true;
    }
    if (step.starts_with("scattertypes:")) {
        // "A=2,B=1": the weighted list.
        scatter_.types.clear();
        std::istringstream in(step.substr(13));
        for (std::string part; std::getline(in, part, ',');) {
            const auto eq = part.find('=');
            scatter_.types.push_back({part.substr(0, eq), eq == std::string::npos ? 1.0f : static_cast<float>(std::atof(part.c_str() + eq + 1))});
        }
        return true;
    }
    if (const auto n = numbersAfter(step, "scatter:", 3)) {
        scatter_.radius = (*n)[0];
        scatter_.density = (*n)[1];
        scatter_.spacing = (*n)[2];
        mode_ = Mode::Scatter;
        ed_.tool_ = MapEditor::Tool::Object;
        return true;
    }
    if (const auto n = numbersAfter(step, "scatterstroke:", 5)) {
        // A whole stroke from (x0, y0) to (x1, y1) in n dabs, as one op.
        me::ScatterBrush b = scatter_;
        if (b.types.empty() && !type_.empty()) b.types = {{type_, 1}};
        b.seed = 0x5CA7u + ed_.strokeSeed_;
        me::ScatterStroke s(ed_.doc_, *factory_, b);
        const int count = std::max(1, static_cast<int>((*n)[4]));
        for (int i = 0; i < count; ++i) {
            const float t = count == 1 ? 0 : static_cast<float>(i) / static_cast<float>(count - 1);
            s.dab({(*n)[0] + ((*n)[2] - (*n)[0]) * t, (*n)[1] + ((*n)[3] - (*n)[1]) * t});
        }
        me::Selection placed;
        if (ed_.perform(s.finish(&placed))) ed_.setSelection(std::move(placed), true);
        ++ed_.strokeSeed_;
        return true;
    }
    if (step.starts_with("mode:")) {
        mode_ = step.substr(5) == "scatter" ? Mode::Scatter : Mode::Place;
        return true;
    }
    if (step.starts_with("prefab-save:")) {
        savePrefab(step.substr(12));
        return true;
    }
    if (step.starts_with("prefab-place:")) {
        // "name,x,y"
        const std::string rest = step.substr(13);
        const auto comma = rest.find(',');
        const std::string name = rest.substr(0, comma);
        const auto n = numbersAfter(rest.substr(comma == std::string::npos ? rest.size() : comma), "", 2);
        for (std::size_t i = 0; i < prefabs_.size(); ++i) {
            if (prefabs_[i].name == name && n) {
                prefab_ = static_cast<int>(i);
                placeAt(placePoint((*n)[0], (*n)[1]));
                return true;
            }
        }
        ed_.status("No prefab " + name);
        return true;
    }
    if (step.starts_with("prefab:")) {
        const std::string name = step.substr(7);
        for (std::size_t i = 0; i < prefabs_.size(); ++i) {
            if (prefabs_[i].name == name) {
                prefab_ = static_cast<int>(i);
                ed_.tool_ = MapEditor::Tool::Object;
            }
        }
        return true;
    }
    if (step.starts_with("equip-item:")) {
        // The selected object's Nth inventory item worn (EQUIPPED), as the inventory editor's check sets it.
        if (ed_.selection_.objects.size() == 1) {
            const auto items = ed_.doc_.childrenOf(*ed_.selection_.objects.begin());
            const auto n = static_cast<std::size_t>(std::atoi(step.c_str() + 11));
            if (n < items.size()) ed_.perform(me::setEquippedOp(ed_.doc_, items[n], true, factory_.get()));
        }
        return true;
    }
    if (step.starts_with("objlist-search:")) {
        std::snprintf(listSearch_, sizeof listSearch_, "%s", step.substr(15).c_str());
        showList = true;
        return true;
    }
    if (step.starts_with("categories:")) {
        // A categories.xml read in as the user's categories (and kept in the user folder).
        try {
            const auto bytes = nox::readFileBytes(fs::path(reinterpret_cast<const char8_t*>(step.c_str() + 11)));
            const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            userCats_ = me::parseCategoriesXml(text);
            if (!writeDir_.empty()) std::ofstream(writeDir_ / "categories.xml", std::ios::binary | std::ios::trunc) << text;
            ed_.status("Read " + std::to_string(userCats_.size()) + " categories");
        } catch (const std::exception& ex) {
            ed_.status(std::string("categories.xml: ") + ex.what());
        }
        return true;
    }
    if (step.starts_with("category-rule:")) {
        // (tests) "category-rule:NAME,KIND,PATTERN": a rule added to the user's category NAME (made if
        // it's new), as the categories editor adds one.
        const std::string rest = step.substr(14);
        const auto c1 = rest.find(',');
        const auto c2 = c1 == std::string::npos ? std::string::npos : rest.find(',', c1 + 1);
        if (c2 != std::string::npos) {
            const std::string name = rest.substr(0, c1);
            auto it = std::ranges::find(userCats_, name, &me::UserCategory::name);
            if (it == userCats_.end()) {
                userCats_.push_back({name, {}});
                it = userCats_.end() - 1;
            }
            it->rules.push_back({true, rest.substr(c1 + 1, c2 - c1 - 1), rest.substr(c2 + 1)});
            saveUserCategories();
        }
        return true;
    }
    return false;
}

} // namespace noxsdl
