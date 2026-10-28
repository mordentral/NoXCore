// The map editor's markers and map-wide panels (marker_tools.hpp): the Waypoint and Polygon tools,
// their overlays and properties, the Waypoints, Polygons, Groups, Map Info, Minimap and Game Texts
// panels, the commands and the test steps.
//
// --editor-run steps (tests and scripted screenshots):
//   waypoint:x,y            a waypoint placed there (and selected)
//   wplink:a,b              a links to b (both ways when the tool's "both ways" is on)
//   wplink2:a,b             a links to b both ways
//   wpunlink:a,b            the link a -> b removed
//   wpsel:a,b,...           those waypoints selected (the first is where links start)
//   wpmode:place|connect    the Waypoint tool's mode
//   wpboth:0|1              the Waypoint tool's "both ways"
//   wpsearch:Q              the Waypoints panel opened, searching Q
//   polygon:x0,y0,x1,y1,... a polygon added with these corners (and selected)
//   polysel:key             that polygon selected (by key)
//   polycorner:key,i        that polygon's corner i made the active corner
//   polymove:key,i,x,y      that corner moved
//   polyinsert:key,after,x,y a corner inserted after `after`
//   polydraw:x,y            a corner added to the polygon being drawn (the tool in Draw mode)
//   ambientpreview:0|1      the polygons' ambient preview fill
//   polygons-open           the Polygons panel opened
//   group-new:name,kind     a group made (kind: objects, waypoints, walls, groups or 0-3) and opened
//   group-open:name         the Groups panel opened on that group
//   group-addsel            the selection added to the open group
//   group-pick              "pick members on the map" armed for the open group
//   mapinfo-open            the Map Info panel opened
//   mapinfo-pictures        the Map Info panel and its quest picture picker opened
//   mapinfo-picfilter:Q     the picker's filter
//   mapinfo-picall:0|1      the picker's "every named image"
//   mapinfo-picture:NAME    the quest picture set, as a click in the picker does
//   mapinfo-version:N       the map info version set (3 stores the quest intro)
//   minimap-open            the Minimap panel opened
//   minimap-zoom:N          the minimap's zoom (2-4 pixels a cell)
//   strings-open:Q          the Game Texts panel opened, searching Q
#include "editor/marker_tools.hpp"

#include "editor/editor_text.hpp"
#include "editor/map_editor.hpp"
#include "editor/panel_textures.hpp"
#include "editor/property_panel.hpp"
#include "editor/script_panel.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"
#include "sprite_cache.hpp"

#include "noxcore/mapedit/locks.hpp"
#include "noxcore/mapedit/quest_pictures.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/video/sprite.hpp"
#include "noxcore/video/video_bag.hpp"

#include <imgui.h>
#include <imgui_internal.h> // (ImGuiItemFlags_MixedValue: a multi-selection's mixed checkboxes)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>

namespace me = nox::mapedit;
namespace nm = nox::map;

namespace noxsdl {

namespace {

using editor::col;

/// Help shown over a disabled field too.
void help(const char* text)
{
    editor::help(text, ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
}

/// The fields of one element under its own ids: an edit still being typed when the selection
/// changes ends with the old element's field (dropped), never written to the new element.
struct IdScope {
    explicit IdScope(const std::string& id) { ImGui::PushID(id.c_str()); }
    ~IdScope() { ImGui::PopID(); }
    IdScope(const IdScope&) = delete;
    IdScope& operator=(const IdScope&) = delete;
};

/// SameLine when a button labelled `next` still fits on the line (narrow docked panels wrap).
void flow(const char* next)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = ImGui::CalcTextSize(next, nullptr, true).x + st.FramePadding.x * 2;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= right) ImGui::SameLine();
}

using editor::hexText;
using editor::numbersAfter;
using nox::util::lowered;

int resizeCallback(ImGuiInputTextCallbackData* d)
{
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<std::size_t>(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}

constexpr nm::Group::Kind kKinds[] = {nm::Group::Kind::Objects, nm::Group::Kind::Waypoints, nm::Group::Kind::Walls, nm::Group::Kind::Unknown3};

using editor::texId;

} // namespace

// --- The quest picture picker's list --------------------------------------------------------------------

/// thing.bin's named images (quest_pictures.hpp) with small thumbnails, read on a worker thread
/// from the game's own video.bag (the stock maps' choices from its maps folder) when the picker
/// first opens; each thumbnail made into a texture the first time it's shown.
class QuestPictures {
public:
    static constexpr int kThumbW = 160; ///< a thumbnail's box (a 612 x 467 picture: 160 x 122)
    static constexpr int kThumbH = 122;

    struct Entry {
        me::QuestPicture info;
        std::vector<std::uint8_t> rgba; ///< the thumbnail (tw x th, straight alpha), empty if none
        int tw = 0, th = 0;
        SDL_Texture* texture = nullptr;
    };

    QuestPictures(const nox::things::ThingDb& things, std::filesystem::path gameDir)
    {
        worker_ = std::thread([this, &things, dir = std::move(gameDir)] { run(things, dir); });
    }
    ~QuestPictures()
    {
        if (worker_.joinable()) worker_.join();
        for (auto& e : entries_) {
            if (e.texture) {
                editor::panelTextureForget(e.texture);
                SDL_DestroyTexture(e.texture);
            }
        }
    }
    QuestPictures(const QuestPictures&) = delete;
    QuestPictures& operator=(const QuestPictures&) = delete;

    bool ready() const { return done_.load(std::memory_order_acquire); }
    /// After ready(): the list (render thread only), and why it's empty if it is.
    std::vector<Entry>& entries() { return entries_; }
    const std::string& error() const { return error_; }
    /// The entry's thumbnail as a texture (made now if it isn't yet), or null.
    SDL_Texture* texture(Entry& e, SDL_Renderer* renderer)
    {
        if (!e.texture && !e.rgba.empty() && renderer) {
            e.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, e.tw, e.th);
            if (e.texture) {
                SDL_SetTextureBlendMode(e.texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(e.texture, SDL_SCALEMODE_LINEAR);
                SDL_UpdateTexture(e.texture, nullptr, e.rgba.data(), e.tw * 4);
                editor::panelTextureSet(e.texture, e.rgba.data(), e.tw, e.th, e.tw * 4); // (for a floating panel's copy)
            }
            e.rgba.clear();
            e.rgba.shrink_to_fit();
        }
        return e.texture;
    }

private:
    /// The image box-filtered down to fit the thumbnail box (never up).
    static void thumbnail(const nox::video::DecodedImage& d, Entry& e)
    {
        const float k = std::min({1.0f, static_cast<float>(kThumbW) / static_cast<float>(d.width), static_cast<float>(kThumbH) / static_cast<float>(d.height)});
        e.tw = std::max(1, static_cast<int>(static_cast<float>(d.width) * k));
        e.th = std::max(1, static_cast<int>(static_cast<float>(d.height) * k));
        std::vector<float> acc(static_cast<std::size_t>(e.tw) * e.th * 5, 0.0f);
        for (int y = 0; y < d.height; ++y) {
            const int ty = std::min(e.th - 1, static_cast<int>(static_cast<float>(y) * k));
            for (int x = 0; x < d.width; ++x) {
                const int tx = std::min(e.tw - 1, static_cast<int>(static_cast<float>(x) * k));
                const std::uint8_t* s = &d.rgba[(static_cast<std::size_t>(y) * d.width + x) * 4];
                float* a = &acc[(static_cast<std::size_t>(ty) * e.tw + tx) * 5];
                const float al = s[3] / 255.0f;
                a[0] += s[0] * al;
                a[1] += s[1] * al;
                a[2] += s[2] * al;
                a[3] += al;
                a[4] += 1;
            }
        }
        e.rgba.assign(static_cast<std::size_t>(e.tw) * e.th * 4, 0);
        for (std::size_t i = 0; i < static_cast<std::size_t>(e.tw) * e.th; ++i) {
            const float* a = &acc[i * 5];
            if (a[3] <= 0 || a[4] <= 0) continue;
            for (int c = 0; c < 3; ++c) e.rgba[i * 4 + c] = static_cast<std::uint8_t>(std::clamp(a[c] / a[3], 0.0f, 255.0f));
            e.rgba[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(a[3] / a[4] * 255.0f, 0.0f, 255.0f));
        }
    }

    void run(const nox::things::ThingDb& things, const std::filesystem::path& dir)
    {
        try {
            nox::video::VideoBag bag(dir / "video.bag", dir / "video.idx");
            const auto used = me::questPicturesOfMaps(dir / "maps");
            std::vector<Entry> out(things.images.size());
            auto list = me::questPictures(things, bag, used, [&](std::size_t i, const nox::video::DecodedImage& d) { thumbnail(d, out[i]); });
            for (std::size_t i = 0; i < list.size() && i < out.size(); ++i) out[i].info = std::move(list[i]);
            out.resize(list.size());
            entries_ = std::move(out);
        } catch (const std::exception& ex) {
            error_ = ex.what();
        }
        done_.store(true, std::memory_order_release);
    }

    std::vector<Entry> entries_;
    std::string error_;
    std::atomic<bool> done_{false};
    std::thread worker_;
};

MarkerTools::MarkerTools(MapEditor& editor)
    : ed_(editor)
{
    if (!ed_.ctx_.gameDir.empty()) {
        try {
            strings_ = std::make_unique<nox::strings::StringTable>(nox::strings::StringTable::load(ed_.ctx_.gameDir / "nox.csf"));
        } catch (const std::exception&) {
            // (no string table: the readable's preview and Game Texts are blank)
        }
    }
}

MarkerTools::~MarkerTools()
{
    if (minimap_) {
        editor::panelTextureForget(minimap_);
        SDL_DestroyTexture(minimap_);
    }
}

// --- Commands --------------------------------------------------------------------------------------

void MarkerTools::registerCommands()
{
    using editor::Command;
    using editor::Shortcut;
    auto& reg = ed_.commands_;
    const auto add = [&](const char* id, const char* label, const char* category, const char* shortcut, const char* helpText, std::function<void()> run,
                         std::function<bool()> enabled = {}, std::function<bool()> checked = {}) {
        reg.add(Command{id, label, category, Shortcut::parse(shortcut).value_or(Shortcut{}), helpText, std::move(run), std::move(enabled), std::move(checked)});
    };
    const auto window = [&](const char* id, const char* label, const char* shortcut, const char* helpText, bool MarkerTools::*field) {
        add(id, label, "Window", shortcut, helpText, [this, field] { this->*field = !(this->*field); }, {}, [this, field] { return this->*field; });
    };
    window("window.mapInfo", "Map Info", "Ctrl+Shift+M", "The map's title, credits, game types, version, players, quest intro, ambient light and intro text.",
           &MarkerTools::showMapInfo);
    window("window.waypoints", "Waypoints", "Ctrl+Shift+W", "Every waypoint: search, sort, select, enable or disable, go to.", &MarkerTools::showWaypoints);
    window("window.polygons", "Polygons", "", "The polygons in their list order (the game's priority): select, reorder, new, delete.",
           &MarkerTools::showPolygons);
    window("window.groups", "Groups", "Ctrl+Shift+G", "Script groups of objects, waypoints, walls or groups: members picked on the map.",
           &MarkerTools::showGroups);
    window("window.minimap", "Minimap", "M", "The whole map at a glance: click or drag to move the view; the symmetry guides.", &MarkerTools::showMinimap);
    window("window.strings", "Game Texts", "", "The game's string table (nox.csf): search labels and texts, copy a label.", &MarkerTools::showStrings);
    add("waypoint.enableAll", "Enable All Waypoints", "Tools", "", "Turns every waypoint on (the old editor's button of this name deleted them all).",
        [this] {
            std::set<std::uint32_t> all;
            for (const auto& w : ed_.doc_.waypoints.waypoints) all.insert(w.id);
            if (!ed_.perform(me::setWaypointsEnabledOp(ed_.doc_, all, true))) ed_.status("Every waypoint is enabled already");
        },
        [this] { return !ed_.doc_.waypoints.waypoints.empty(); });
    add("waypoint.disableAll", "Disable All Waypoints", "Tools", "", "Turns every waypoint off (scripts can turn them on).",
        [this] {
            std::set<std::uint32_t> all;
            for (const auto& w : ed_.doc_.waypoints.waypoints) all.insert(w.id);
            if (!ed_.perform(me::setWaypointsEnabledOp(ed_.doc_, all, false))) ed_.status("Every waypoint is disabled already");
        },
        [this] { return !ed_.doc_.waypoints.waypoints.empty(); });
    add("waypoint.connect", "Waypoint Connect Mode", "Tools", "",
        "The Waypoint tool links: click links the selected waypoint to the one clicked and selects it (chains), Shift+click unlinks.",
        [this] {
            ed_.tool_ = MapEditor::Tool::Waypoint;
            wpMode_ = wpMode_ == WpMode::Connect ? WpMode::Place : WpMode::Connect;
        },
        {}, [this] { return wpMode_ == WpMode::Connect; });
    add("polygon.new", "New Polygon", "Tools", "", "Draw a new polygon: click its corners; Enter, a double click or a click on the first corner closes it.",
        [this] { startDrawing(); });
    add("view.ambientPreview", "Polygons' Ambient Preview", "View", "", "Fills each polygon with its ambient light (darker polygons more opaque).",
        [this] { ambientPreview_ = !ambientPreview_; }, {}, [this] { return ambientPreview_; });

    // Delete and Escape mean the Polygon tool's corner and drawing first (wrapping the editor's own).
    if (const Command* del = reg.find("edit.delete")) {
        Command c = *del;
        auto run = c.run;
        auto enabled = c.enabled;
        c.run = [this, run] {
            if (!deleteActiveCorner() && run) run();
        };
        c.enabled = [this, enabled] {
            return (ed_.tool_ == MapEditor::Tool::Polygon && activeCorner_) || !enabled || enabled();
        };
        reg.add(std::move(c));
    }
    if (const Command* none = reg.find("select.none")) {
        Command c = *none;
        auto run = c.run;
        c.run = [this, run] {
            if (ed_.tool_ == MapEditor::Tool::Polygon && polyMode_ == PolyMode::Draw) {
                cancelDrawing();
            } else if (drag_ != Drag::None) {
                drag_ = Drag::None;
            } else if (run) {
                run();
            }
        };
        reg.add(std::move(c));
    }
}

// --- Geometry helpers --------------------------------------------------------------------------------

float MarkerTools::pickRadius() const
{
    return std::max(4.0f, 10.0f / ed_.camera_.zoom);
}

me::WorldPoint MarkerTools::gridSnap(me::WorldPoint p) const
{
    return {std::clamp(ed_.snapValue(p.x), 0.0f, me::kWorldSize - 1), std::clamp(ed_.snapValue(p.y), 0.0f, me::kWorldSize - 1)};
}

me::WorldPoint MarkerTools::polySnap(me::WorldPoint p, std::optional<std::uint32_t> except) const
{
    if (polygonSnap_) {
        const me::WorldPoint c = me::snapToPolygonCorners(ed_.doc_, p, polygonSnapPx_ / ed_.camera_.zoom, except);
        if (c.x != p.x || c.y != p.y) return c;
    }
    return gridSnap(p);
}

std::optional<std::uint32_t> MarkerTools::waypointAt(float x, float y) const
{
    const me::Selection s = me::pickAt(ed_.doc_, x, y, pickRadius(), me::KindFilter{.objects = false, .walls = false, .tiles = false, .waypoints = true});
    if (s.waypoints.empty()) return std::nullopt;
    return *s.waypoints.begin();
}

std::optional<std::uint32_t> MarkerTools::currentWaypoint() const
{
    const auto& sel = ed_.selection_.waypoints;
    if (lastWp_ && sel.contains(*lastWp_)) return lastWp_;
    if (sel.size() == 1) return *sel.begin();
    return std::nullopt;
}

std::optional<std::uint32_t> MarkerTools::selectedPolygon() const
{
    const auto& sel = ed_.selection_.polygons;
    if (activeCorner_ && sel.contains(activeCorner_->first)) return activeCorner_->first;
    if (sel.size() >= 1) return *sel.begin();
    return std::nullopt;
}

void MarkerTools::selectWaypoints(std::set<std::uint32_t> ids, std::optional<std::uint32_t> current)
{
    me::Selection s;
    s.waypoints = std::move(ids);
    ed_.setSelection(std::move(s), true);
    if (current) lastWp_ = current;
}

void MarkerTools::openGroup(std::uint32_t key)
{
    if (!ed_.doc_.groupIndex(key)) return;
    if (openGroup_ != key && pickingMembers_) ed_.cancelPick();
    openGroup_ = key;
    showGroups = true;
    ImGui::SetWindowFocus("Groups");
}

void MarkerTools::selectPolygon(std::optional<std::uint32_t> key, bool add)
{
    me::Selection s;
    if (add) s.polygons = ed_.selection_.polygons;
    if (key) {
        if (add && s.polygons.contains(*key)) s.polygons.erase(*key);
        else s.polygons.insert(*key);
    }
    ed_.setSelection(std::move(s), true);
    if (activeCorner_ && !ed_.selection_.polygons.contains(activeCorner_->first)) activeCorner_.reset();
}

void MarkerTools::locate(float x, float y, bool zoom)
{
    if (zoom) {
        ed_.frameWorld(x - 46, y - 46, x + 46, y + 46);
        return;
    }
    ed_.camera_.x = x;
    ed_.camera_.y = y;
    ed_.flash_ = MapEditor::Flash{{x, y}, SDL_GetTicks()};
}

void MarkerTools::frameKeys(const std::set<std::uint32_t>& keys)
{
    me::Selection s;
    s.polygons = keys;
    if (const auto b = me::bounds(ed_.doc_, s)) ed_.frameWorld(b->first.x, b->first.y, b->second.x, b->second.y);
}

// --- The Waypoint tool -----------------------------------------------------------------------------

void MarkerTools::placeWaypoint(me::WorldPoint at)
{
    std::uint32_t id = 0;
    const auto from = currentWaypoint();
    const me::Op op = placeLinked_ && from ? me::placeLinkedWaypointOp(ed_.doc_, gridSnap(at), *from, bothWays_, linkFlags_, &id)
                                           : me::placeWaypointOp(ed_.doc_, gridSnap(at), {}, &id);
    if (ed_.perform(op)) selectWaypoints({id}, id);
}

void MarkerTools::linkTo(std::uint32_t target, bool unlink)
{
    const auto from = currentWaypoint();
    if (!from) {
        selectWaypoints({target}, target);
        ed_.status("Links start from waypoint " + std::to_string(target) + ": click the next one");
        return;
    }
    if (*from == target) return;
    const me::Op op = unlink ? me::unlinkWaypointsOp(ed_.doc_, *from, target, bothWays_) : me::linkWaypointsOp(ed_.doc_, *from, target, bothWays_, linkFlags_);
    if (op.empty()) {
        ed_.status(unlink ? "No link from " + std::to_string(*from) + " to " + std::to_string(target) : "Already linked with these flags");
    } else {
        ed_.perform(op);
    }
    selectWaypoints({target}, target); // (the next click links on from here: chains)
}

void MarkerTools::waypointInput(float wx, float wy, bool hovered, bool rightClick)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (rightClick && wpRightDeletes_) {
        if (const auto id = waypointAt(wx, wy)) {
            me::Selection s;
            s.waypoints.insert(*id);
            ed_.perform(me::deleteOp(ed_.doc_, s));
        }
        return;
    }
    if (drag_ == Drag::Waypoints) {
        const ImVec2 m = io.MousePos;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (std::hypot(m.x - dragSx_, m.y - dragSy_) > 3) dragMoved_ = true;
            const me::WorldPoint t = gridSnap({dragAnchor_.x + (wx - dragStart_.x), dragAnchor_.y + (wy - dragStart_.y)});
            dragTo_ = dragMoved_ ? me::WorldPoint{t.x - dragAnchor_.x, t.y - dragAnchor_.y} : me::WorldPoint{};
            // Live: the move the release will make.
            ed_.previewDrag({1, dragTo_.x, dragTo_.y, 0}, [this](std::optional<me::Selection>&) {
                me::Selection s;
                s.waypoints = ed_.selection_.waypoints; // (waypoints keep their ids: the selection stands)
                return me::moveOp(ed_.doc_, s, dragTo_.x, dragTo_.y);
            });
        } else {
            drag_ = Drag::None;
            ed_.endPreview(); // (the document as it was: the drag's one op from it)
            if (dragMoved_ && (dragTo_.x != 0 || dragTo_.y != 0)) {
                me::Selection s;
                s.waypoints = ed_.selection_.waypoints;
                me::Selection moved;
                if (ed_.perform(me::moveOp(ed_.doc_, s, dragTo_.x, dragTo_.y, &moved))) ed_.setSelection(std::move(moved), true);
            }
            dragTo_ = {};
        }
        return;
    }
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    const auto hit = waypointAt(wx, wy);
    if (wpMode_ == WpMode::Connect) {
        if (!hit) {
            ed_.status("Connect: click a waypoint (Shift+click unlinks)");
            return;
        }
        linkTo(*hit, io.KeyShift);
        return;
    }
    if (hit) {
        std::set<std::uint32_t> sel = ed_.selection_.waypoints;
        if (io.KeyShift) {
            if (!sel.erase(*hit)) sel.insert(*hit);
            selectWaypoints(std::move(sel), *hit);
            return;
        }
        if (io.KeyCtrl) {
            sel.erase(*hit);
            selectWaypoints(std::move(sel));
            return;
        }
        if (!sel.contains(*hit) || !ed_.selection_.objects.empty() || !ed_.selection_.walls.empty()) selectWaypoints({*hit}, *hit);
        lastWp_ = *hit;
        // A drag moves the selected waypoints (the grabbed one snapped).
        const nm::Waypoint* w = me::findWaypoint(ed_.doc_, *hit);
        drag_ = Drag::Waypoints;
        dragStart_ = {wx, wy};
        dragAnchor_ = w ? me::WorldPoint{w->x, w->y} : dragStart_;
        dragSx_ = io.MousePos.x;
        dragSy_ = io.MousePos.y;
        dragMoved_ = false;
        dragTo_ = {};
        return;
    }
    if (io.KeyCtrl) return;
    placeWaypoint({wx, wy});
}

// --- The Polygon tool ------------------------------------------------------------------------------

void MarkerTools::startDrawing()
{
    ed_.tool_ = MapEditor::Tool::Polygon;
    polyMode_ = PolyMode::Draw;
    drawing_.clear();
    activeCorner_.reset();
    ed_.status("New polygon: click its corners; Enter, a double click or the first corner closes it; Escape cancels");
}

void MarkerTools::finishDrawing()
{
    if (drawing_.size() < 3) {
        ed_.status("A polygon needs at least 3 corners");
        return;
    }
    std::uint32_t key = 0;
    const me::Op op = me::addPolygonOp(ed_.doc_, drawing_, me::defaultPolygon(ed_.doc_), &key);
    drawing_.clear();
    polyMode_ = PolyMode::Edit;
    if (ed_.perform(op)) selectPolygon(key);
}

void MarkerTools::cancelDrawing()
{
    drawing_.clear();
    polyMode_ = PolyMode::Edit;
    ed_.status("Polygon cancelled");
}

bool MarkerTools::deleteActiveCorner()
{
    if (ed_.tool_ != MapEditor::Tool::Polygon || !activeCorner_) return false;
    const auto i = ed_.doc_.polygonIndex(activeCorner_->first);
    if (!i || activeCorner_->second >= ed_.doc_.polygons.polygons[*i].points.size()) {
        activeCorner_.reset();
        return false;
    }
    if (ed_.doc_.polygons.polygons[*i].points.size() <= 3) {
        ed_.status("A polygon keeps at least 3 corners (Delete with no corner picked deletes the polygon)");
        activeCorner_.reset();
        return true;
    }
    ed_.perform(me::deletePolygonCornerOp(ed_.doc_, activeCorner_->first, activeCorner_->second));
    activeCorner_.reset();
    return true;
}

void MarkerTools::polygonInput(float wx, float wy, bool hovered, bool rightClick)
{
    const ImGuiIO& io = ImGui::GetIO();
    const float r = pickRadius();
    if (polyMode_ == PolyMode::Draw) {
        if (!io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
            finishDrawing();
            return;
        }
        if (rightClick) {
            if (!drawing_.empty()) drawing_.pop_back(); // (the last corner taken back)
            else polyMode_ = PolyMode::Edit;
            return;
        }
        if (!hovered) return;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (drawing_.size() >= 3) finishDrawing();
            return;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (drawing_.size() >= 3) {
                const float d = std::hypot(wx - drawing_.front().x, wy - drawing_.front().y) * ed_.camera_.zoom;
                if (d <= 8) {
                    finishDrawing(); // (a click on the first corner closes it)
                    return;
                }
            }
            drawing_.push_back(polySnap({wx, wy}, std::nullopt));
        }
        return;
    }
    if (drag_ == Drag::Corner || drag_ == Drag::Polygon) {
        const ImVec2 m = io.MousePos;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (std::hypot(m.x - dragSx_, m.y - dragSy_) > 3) dragMoved_ = true;
            if (drag_ == Drag::Corner) {
                dragTo_ = polySnap({dragAnchor_.x + (wx - dragStart_.x), dragAnchor_.y + (wy - dragStart_.y)}, dragKey_);
            } else {
                const me::WorldPoint t = gridSnap({dragAnchor_.x + (wx - dragStart_.x), dragAnchor_.y + (wy - dragStart_.y)});
                dragTo_ = {t.x - dragAnchor_.x, t.y - dragAnchor_.y};
            }
            // Live: the corner or the polygons where the release will put them, snapped the same
            // (polySnap leaves out the polygon being dragged, the only one the preview moves).
            if (dragMoved_) {
                const bool corner = drag_ == Drag::Corner;
                const float cornerIndex = corner && activeCorner_ ? static_cast<float>(activeCorner_->second) : -1;
                ed_.previewDrag({corner ? 2.0f : 3.0f, dragTo_.x, dragTo_.y, cornerIndex}, [this, corner](std::optional<me::Selection>&) {
                    if (corner) {
                        return activeCorner_ ? me::movePolygonCornerOp(ed_.doc_, dragKey_, activeCorner_->second, dragTo_) : me::Op{};
                    }
                    std::set<std::uint32_t> keys = ed_.selection_.polygons;
                    keys.insert(dragKey_);
                    return me::movePolygonsOp(ed_.doc_, keys, dragTo_.x, dragTo_.y);
                });
            }
            return;
        }
        const Drag was = drag_;
        drag_ = Drag::None;
        ed_.endPreview(); // (the document as it was: the drag's one op from it)
        if (!dragMoved_) return;
        if (was == Drag::Corner && activeCorner_) {
            ed_.perform(me::movePolygonCornerOp(ed_.doc_, dragKey_, activeCorner_->second, dragTo_));
        } else if (was == Drag::Polygon && (dragTo_.x != 0 || dragTo_.y != 0)) {
            std::set<std::uint32_t> keys = ed_.selection_.polygons;
            keys.insert(dragKey_);
            ed_.perform(me::movePolygonsOp(ed_.doc_, keys, dragTo_.x, dragTo_.y));
        }
        return;
    }
    const auto sel = selectedPolygon();
    if (rightClick) {
        if (!polyRightDeletes_) return;
        auto corner = sel ? me::polygonCornerAt(ed_.doc_, wx, wy, r, sel) : std::nullopt;
        if (!corner) corner = me::polygonCornerAt(ed_.doc_, wx, wy, r);
        if (corner) {
            selectPolygon(corner->first);
            activeCorner_ = corner;
            deleteActiveCorner();
        }
        return;
    }
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    const auto beginDrag = [&](Drag kind, std::uint32_t key, me::WorldPoint anchor) {
        drag_ = kind;
        dragKey_ = key;
        dragStart_ = {wx, wy};
        dragAnchor_ = anchor;
        dragTo_ = kind == Drag::Corner ? anchor : me::WorldPoint{};
        dragSx_ = io.MousePos.x;
        dragSy_ = io.MousePos.y;
        dragMoved_ = false;
    };
    if (io.KeyShift) {
        // Shift+click on an edge: a corner inserted there (the selected polygon's edges first).
        auto edge = sel ? me::polygonEdgeAt(ed_.doc_, wx, wy, r, sel) : std::nullopt;
        if (!edge) edge = me::polygonEdgeAt(ed_.doc_, wx, wy, r);
        if (!edge) {
            ed_.status("Shift+click on a polygon's edge to add a corner there");
            return;
        }
        const me::WorldPoint at = polySnap(edge->at, edge->key);
        if (ed_.perform(me::insertPolygonCornerOp(ed_.doc_, edge->key, edge->after, at))) {
            selectPolygon(edge->key);
            activeCorner_ = std::pair{edge->key, edge->after + 1};
            beginDrag(Drag::Corner, edge->key, at);
        }
        return;
    }
    auto corner = sel ? me::polygonCornerAt(ed_.doc_, wx, wy, r, sel) : std::nullopt;
    if (!corner) corner = me::polygonCornerAt(ed_.doc_, wx, wy, r);
    if (corner) {
        if (!ed_.selection_.polygons.contains(corner->first)) selectPolygon(corner->first);
        if (!ed_.selection_.polygons.contains(corner->first)) return; // (another editor holds it)
        activeCorner_ = corner;
        const auto i = ed_.doc_.polygonIndex(corner->first);
        const auto c = me::polygonCorners(ed_.doc_, *i);
        beginDrag(Drag::Corner, corner->first, c[corner->second]);
        return;
    }
    const auto anchorOf = [&](std::uint32_t key) {
        const auto c = me::polygonCorners(ed_.doc_, *ed_.doc_.polygonIndex(key));
        return c.empty() ? me::WorldPoint{wx, wy} : c.front();
    };
    if (sel && me::polygonContains(ed_.doc_, *ed_.doc_.polygonIndex(*sel), wx, wy) && !io.KeyCtrl) {
        activeCorner_.reset();
        beginDrag(Drag::Polygon, *sel, anchorOf(*sel)); // (inside the selected polygon: a drag moves it whole)
        return;
    }
    if (const auto k = me::polygonAt(ed_.doc_, wx, wy)) {
        selectPolygon(*k, io.KeyCtrl);
        activeCorner_.reset();
        if (!io.KeyCtrl && ed_.selection_.polygons.contains(*k)) beginDrag(Drag::Polygon, *k, anchorOf(*k));
        return;
    }
    if (!io.KeyCtrl) selectPolygon(std::nullopt);
}

void MarkerTools::input(float wx, float wy, bool hovered, bool rightClick)
{
    if (ed_.tool_ == MapEditor::Tool::Waypoint) waypointInput(wx, wy, hovered, rightClick);
    else if (ed_.tool_ == MapEditor::Tool::Polygon) polygonInput(wx, wy, hovered, rightClick);
}

// --- Overlays ----------------------------------------------------------------------------------

void MarkerTools::drawPolygons(ImDrawList* dl, const ImVec2& origin)
{
    const auto& cam = ed_.camera_;
    const float z = cam.zoom;
    const float left = cam.left(), top = cam.top();
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - left) * z, origin.y + (y - top) * z); };
    const auto& doc = ed_.doc_;
    std::optional<std::uint32_t> hover;
    if (ed_.tool_ == MapEditor::Tool::Polygon && ed_.mapHovered_ && drag_ == Drag::None && polyMode_ == PolyMode::Edit) {
        if (const auto m = ed_.mouseWorld()) hover = me::polygonAt(doc, m->x, m->y);
    }
    for (std::size_t i = 0; i < doc.polygons.polygons.size(); ++i) {
        const auto& poly = doc.polygons.polygons[i];
        const std::uint32_t key = i < doc.polygonKeys.size() ? doc.polygonKeys[i] : 0;
        std::vector<ImVec2> outline;
        float cx = 0, cy = 0;
        for (const auto& c : me::polygonCorners(doc, i)) {
            outline.push_back(S(c.x, c.y));
            cx += c.x;
            cy += c.y;
        }
        if (outline.size() < 2) continue;
        const bool selected = ed_.selection_.polygons.contains(key);
        if (ambientPreview_ && outline.size() >= 3) {
            // The ambient light it gives, as a fill: dark polygons more opaque (the old editor's preview).
            const auto& a = poly.ambientLight;
            const int bright = (a[0] + a[1] + a[2]) / 3;
            const int alpha = std::clamp((255 - bright) * 3 / 4, 24, 200);
            dl->AddConcavePolyFilled(outline.data(), static_cast<int>(outline.size()), col(a[0], a[1], a[2], alpha));
        } else if (selected && outline.size() >= 3) {
            dl->AddConcavePolyFilled(outline.data(), static_cast<int>(outline.size()), col(255, 210, 60, 28));
        }
        const ImU32 line = selected ? col(255, 210, 60) : hover == key ? col(255, 255, 255, 230) : col(120, 200, 255, 200);
        dl->AddPolyline(outline.data(), static_cast<int>(outline.size()), line, ImDrawFlags_Closed, selected ? 2.5f : 1.5f);
        if (z >= 0.2f && !poly.name.empty()) {
            const float n = static_cast<float>(outline.size());
            const std::string name = me::fromMapText(poly.name);
            const ImVec2 sz = ImGui::CalcTextSize(name.c_str());
            const ImVec2 p = S(cx / n, cy / n);
            dl->AddText(ImVec2(p.x - sz.x / 2, p.y - sz.y / 2), selected ? col(255, 230, 140) : col(160, 220, 255, 220), name.c_str());
        }
    }
}

void MarkerTools::drawWaypoints(ImDrawList* dl, const ImVec2& origin, const ImVec2& size)
{
    const auto& cam = ed_.camera_;
    const float z = cam.zoom;
    const float left = cam.left(), top = cam.top();
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - left) * z, origin.y + (y - top) * z); };
    const float x0 = left, y0 = top, x1 = left + size.x / z, y1 = top + size.y / z;
    const auto visible = [&](float x, float y, float margin) { return x >= x0 - margin && x <= x1 + margin && y >= y0 - margin && y <= y1 + margin; };
    const auto& doc = ed_.doc_;
    std::map<std::uint32_t, const nm::Waypoint*> at;
    for (const auto& wp : doc.waypoints.waypoints) at[wp.id] = &wp;
    const auto& sel = ed_.selection_.waypoints;
    // Links: one way green with an arrowhead, both ways orange, from a selected waypoint aqua; a
    // link without the default class purple.
    for (const auto& wp : doc.waypoints.waypoints) {
        for (const auto& l : wp.links) {
            const auto it = at.find(l.target);
            if (it == at.end()) continue;
            const nm::Waypoint& t = *it->second;
            if (!visible(wp.x, wp.y, 400) && !visible(t.x, t.y, 400)) continue;
            const bool back = std::ranges::any_of(t.links, [&](const nm::WaypointLink& b) { return b.target == wp.id; });
            if (back && t.id < wp.id && !sel.contains(wp.id)) continue; // (a two-way pair drawn once)
            ImU32 c = back ? col(255, 170, 60, 200) : col(120, 255, 160, 170);
            if ((l.flags & me::kDefaultLinkFlags) == 0) c = col(200, 140, 255, 210);
            if (sel.contains(wp.id)) c = col(80, 240, 255, 230);
            const ImVec2 a = S(wp.x, wp.y), b = S(t.x, t.y);
            dl->AddLine(a, b, c, sel.contains(wp.id) ? 2.0f : 1.5f);
            const float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
            if (!back && len > 20) {
                const ImVec2 tip(a.x + dx * 0.66f, a.y + dy * 0.66f);
                const float ux = dx / len, uy = dy / len;
                dl->AddTriangleFilled(tip, ImVec2(tip.x - ux * 8 - uy * 4, tip.y - uy * 8 + ux * 4), ImVec2(tip.x - ux * 8 + uy * 4, tip.y - uy * 8 - ux * 4), c);
            }
            if (z >= 1.0f && l.flags != me::kDefaultLinkFlags && len > 40) {
                const std::string f = hexText(l.flags);
                dl->AddText(ImVec2(a.x + dx * 0.5f + 3, a.y + dy * 0.5f), c, f.c_str());
            }
        }
    }
    for (const auto& wp : doc.waypoints.waypoints) {
        if (!visible(wp.x, wp.y, 60)) continue;
        const ImVec2 p = S(wp.x, wp.y);
        const bool on = (wp.flags & me::kWaypointEnabled) != 0;
        dl->AddCircleFilled(p, 5, on ? col(60, 220, 120, 230) : col(130, 130, 130, 230));
        dl->AddCircle(p, 5, col(0, 0, 0, 200));
        if (wp.flags & me::kWaypointRegistered) dl->AddCircle(p, 8, col(255, 80, 60, 230), 0, 2); // (the run-time bit stored: never found)
        if (z >= 0.5f) {
            const std::string label = wp.name.empty() ? std::to_string(wp.id) : std::to_string(wp.id) + " " + me::fromMapText(wp.name);
            dl->AddText(ImVec2(p.x + 7, p.y - 7), on ? col(190, 255, 210, 220) : col(190, 190, 190, 200), label.c_str());
        }
    }
}

void MarkerTools::drawOverlay(ImDrawList* dl, const ImVec2& origin)
{
    const auto& cam = ed_.camera_;
    const float z = cam.zoom;
    const float left = cam.left(), top = cam.top();
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - left) * z, origin.y + (y - top) * z); };
    const auto& doc = ed_.doc_;

    // The open group's members, magenta.
    if (showGroups && openGroup_) {
        if (const auto gi = doc.groupIndex(*openGroup_)) {
            const nm::Group& g = doc.groups.groups[*gi];
            const ImU32 c = col(255, 80, 230, 240);
            const me::Selection s = me::groupSelection(doc, g);
            for (const auto id : s.objects) {
                const auto& h = doc.objects.at(id).header;
                dl->AddCircle(S(h.x, h.y), std::max(9.0f, 13 * z), c, 0, 2.5f);
            }
            for (const auto id : s.waypoints) {
                if (const auto* w = me::findWaypoint(doc, id)) dl->AddCircle(S(w->x, w->y), 10, c, 0, 2.5f);
            }
            for (const auto cell : s.walls) {
                const float x = cell.x * me::kWallCellSize, y = cell.y * me::kWallCellSize;
                dl->AddRect(S(x, y), S(x + 23, y + 23), c, 0, 0, 2.5f);
                dl->AddRectFilled(S(x + 4, y + 4), S(x + 19, y + 19), col(255, 80, 230, 60));
            }
            if (g.kind == nm::Group::Kind::Unknown3) {
                // A group of groups: its member groups' members.
                for (const auto m : g.members) {
                    if (const nm::Group* sub = me::findGroupById(doc, m)) {
                        const me::Selection t = me::groupSelection(doc, *sub);
                        for (const auto id : t.objects) {
                            const auto& h = doc.objects.at(id).header;
                            dl->AddCircle(S(h.x, h.y), std::max(9.0f, 13 * z), col(255, 140, 240, 160), 0, 1.5f);
                        }
                        for (const auto id : t.waypoints) {
                            if (const auto* w = me::findWaypoint(doc, id)) dl->AddCircle(S(w->x, w->y), 10, col(255, 140, 240, 160), 0, 1.5f);
                        }
                    }
                }
            }
        }
    }

    // The Polygon tool: the selected polygon's corners as handles, drags, the polygon being drawn.
    if (ed_.tool_ == MapEditor::Tool::Polygon) {
        const auto m = ed_.mouseWorld();
        if (polyMode_ == PolyMode::Draw) {
            std::vector<ImVec2> pts;
            for (const auto& p : drawing_) pts.push_back(S(p.x, p.y));
            std::optional<me::WorldPoint> next;
            if (m && ed_.mapHovered_) next = polySnap(*m, std::nullopt);
            if (next) pts.push_back(S(next->x, next->y));
            if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), col(255, 230, 120, 30));
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col(255, 230, 120), static_cast<ImDrawFlags>(drawing_.size() >= 2 ? ImDrawFlags_Closed : ImDrawFlags_None), 2.0f);
            for (std::size_t k = 0; k < drawing_.size(); ++k) {
                const ImVec2 p = S(drawing_[k].x, drawing_[k].y);
                const bool closer = k == 0 && drawing_.size() >= 3 && m && std::hypot(m->x - drawing_[0].x, m->y - drawing_[0].y) * z <= 8;
                dl->AddCircleFilled(p, closer ? 7.0f : 4.0f, closer ? col(120, 255, 160) : col(255, 230, 120));
            }
            if (next) {
                const ImVec2 p = S(next->x, next->y);
                dl->AddCircle(p, 5, col(255, 255, 255), 0, 1.5f);
            }
            const std::string hint = drawing_.empty() ? "Draw a polygon: click its first corner"
                                                      : std::to_string(drawing_.size()) + " corners: Enter, a double click or the first corner closes it; right click takes one back; Escape cancels";
            dl->AddText(ImVec2(origin.x + 10, origin.y + 10), col(255, 230, 140), hint.c_str());
        } else {
            for (const auto key : ed_.selection_.polygons) {
                const auto i = doc.polygonIndex(key);
                if (!i) continue;
                auto c = me::polygonCorners(doc, *i);
                if (drag_ == Drag::Corner && dragMoved_ && key == dragKey_ && activeCorner_ && activeCorner_->second < c.size()) {
                    c[activeCorner_->second] = dragTo_;
                }
                if (drag_ == Drag::Polygon && dragMoved_ && !ed_.preview_.active() && (key == dragKey_ || ed_.selection_.polygons.contains(key))) {
                    for (auto& p : c) {
                        p.x += dragTo_.x;
                        p.y += dragTo_.y;
                    }
                }
                std::vector<ImVec2> pts;
                for (const auto& p : c) pts.push_back(S(p.x, p.y));
                if (dragMoved_ && drag_ != Drag::None && pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col(255, 255, 255, 220), ImDrawFlags_Closed, 1.5f);
                for (std::size_t k = 0; k < pts.size(); ++k) {
                    const bool active = activeCorner_ && activeCorner_->first == key && activeCorner_->second == k;
                    const bool hot = m && std::hypot(m->x - c[k].x, m->y - c[k].y) <= pickRadius();
                    const float hs = active ? 5.5f : 4.0f;
                    dl->AddRectFilled(ImVec2(pts[k].x - hs, pts[k].y - hs), ImVec2(pts[k].x + hs, pts[k].y + hs),
                                      active ? col(80, 160, 255) : hot ? col(255, 255, 255) : col(255, 90, 160));
                    dl->AddRect(ImVec2(pts[k].x - hs, pts[k].y - hs), ImVec2(pts[k].x + hs, pts[k].y + hs), col(0, 0, 0, 200));
                }
            }
            // The snap target while a corner is dragged.
            if (drag_ == Drag::Corner && dragMoved_) dl->AddCircle(S(dragTo_.x, dragTo_.y), 9, col(120, 255, 160), 0, 2);
            // Shift: where a corner would be inserted.
            if (m && ed_.mapHovered_ && ImGui::GetIO().KeyShift && drag_ == Drag::None) {
                const auto sel = selectedPolygon();
                auto edge = sel ? me::polygonEdgeAt(doc, m->x, m->y, pickRadius(), sel) : std::nullopt;
                if (!edge) edge = me::polygonEdgeAt(doc, m->x, m->y, pickRadius());
                if (edge) {
                    const me::WorldPoint at = polySnap(edge->at, edge->key);
                    dl->AddCircleFilled(S(at.x, at.y), 5, col(120, 255, 160));
                    dl->AddText(ImVec2(S(at.x, at.y).x + 8, S(at.x, at.y).y - 8), col(160, 255, 190), "+ corner");
                }
            }
        }
    }

    // The Waypoint tool: where a click places, the link a click would make, a drag's preview.
    if (ed_.tool_ == MapEditor::Tool::Waypoint) {
        if (drag_ == Drag::Waypoints && dragMoved_) {
            // (Shown moved already while the preview is up.)
            const me::WorldPoint off = ed_.preview_.active() ? me::WorldPoint{} : dragTo_;
            for (const auto id : ed_.selection_.waypoints) {
                if (const auto* w = me::findWaypoint(doc, id)) {
                    dl->AddCircle(S(w->x + off.x, w->y + off.y), 8, col(255, 230, 120), 0, 2);
                }
            }
        }
        const auto m = ed_.mouseWorld();
        if (m && ed_.mapHovered_ && drag_ == Drag::None) {
            const auto hit = waypointAt(m->x, m->y);
            if (wpMode_ == WpMode::Connect) {
                if (const auto from = currentWaypoint()) {
                    if (const auto* fw = me::findWaypoint(doc, *from)) {
                        const bool unlink = ImGui::GetIO().KeyShift;
                        me::WorldPoint to = *m;
                        if (hit) {
                            if (const auto* hw = me::findWaypoint(doc, *hit)) to = {hw->x, hw->y};
                        }
                        dl->AddLine(S(fw->x, fw->y), S(to.x, to.y), unlink ? col(255, 90, 80, 230) : hit ? col(80, 240, 255, 230) : col(160, 160, 160, 150), 2);
                        if (hit) dl->AddCircle(S(to.x, to.y), 10, unlink ? col(255, 90, 80) : col(80, 240, 255), 0, 2.5f);
                    }
                }
            } else if (!hit) {
                const me::WorldPoint p = gridSnap(*m);
                const ImVec2 s = S(p.x, p.y);
                dl->AddCircle(s, 5, col(255, 230, 120, 220), 0, 1.5f);
                dl->AddLine(ImVec2(s.x - 8, s.y), ImVec2(s.x + 8, s.y), col(255, 230, 120, 200));
                dl->AddLine(ImVec2(s.x, s.y - 8), ImVec2(s.x, s.y + 8), col(255, 230, 120, 200));
                if (placeLinked_) {
                    if (const auto from = currentWaypoint()) {
                        if (const auto* fw = me::findWaypoint(doc, *from)) dl->AddLine(S(fw->x, fw->y), s, col(120, 255, 160, 150), 1.5f);
                    }
                }
            }
        }
    }
}

// --- Fields ----------------------------------------------------------------------------------------

float MarkerTools::labelWidth() const
{
    return 104 * ed_.imgui_->scale();
}

void MarkerTools::row(const char* label, const char* helpText)
{
    editor::labelRow(label, labelWidth(), helpText, ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
}

MarkerTools::Edit& MarkerTools::editState(const char* id)
{
    Edit& e = edits_[ImGui::GetID(id)];
    const int now = ImGui::GetFrameCount();
    if (e.frame != now - 1 && e.frame != now) {
        e.active = false;
        e.colour.reset();
    }
    e.frame = now;
    return e;
}

bool MarkerTools::textField(const char* id, const std::string& current, std::size_t maxBytes, std::string& out, bool multiline, float height)
{
    Edit& e = editState(id);
    if (!e.active) e.text = current;
    // The count of the 8-bit text the map stores, against the field's room.
    bool lossy = false;
    const std::size_t bytes = me::toMapText(e.text, &lossy).size();
    char count[48];
    std::snprintf(count, sizeof count, "%zu / %zu", bytes, maxBytes);
    const float countW = ImGui::CalcTextSize("0000 / 0000").x + ImGui::GetStyle().ItemSpacing.x;
    const float w = ImGui::CalcItemWidth();
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    bool done = false;
    if (multiline) {
        ImGui::InputTextMultiline(id, e.text.data(), e.text.capacity() + 1, ImVec2(-FLT_MIN, height), flags | ImGuiInputTextFlags_WordWrap, resizeCallback, &e.text);
    } else {
        ImGui::SetNextItemWidth(std::max(60.0f, w - countW));
        ImGui::InputText(id, e.text.data(), e.text.capacity() + 1, flags, resizeCallback, &e.text);
    }
    e.active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        bool cut = false;
        out = me::fitMapText(e.text, maxBytes, &lossy, &cut);
        if (cut) ed_.status("Cut to " + std::to_string(maxBytes) + " bytes (the field's room in the map)");
        else if (lossy) ed_.status("Characters the map's 8-bit text can't hold became '?'");
        done = out != me::toMapText(current); // (`current` is UTF-8, `out` the map's 8-bit text)
        e.active = false;
    }
    if (!multiline) ImGui::SameLine();
    if (bytes > maxBytes) {
        ImGui::TextColored(editor::themeColours().error, "%s", count);
        help("Longer than the field's room in the map: it's cut when the edit ends.");
    } else {
        ImGui::TextDisabled("%s", count);
        help("Bytes of the map's 8-bit (Windows-1252) text, against the field's room.");
    }
    if (lossy) {
        ImGui::SameLine();
        ImGui::TextColored(editor::themeColours().warning, "(?)");
        help("Some characters aren't in the map's 8-bit text: they're stored as '?'.");
    }
    return done;
}

bool MarkerTools::floatField(const char* id, float current, float& out, float speed, float lo, float hi)
{
    Edit& e = editState(id);
    if (!e.active) e.f = current;
    ImGui::DragFloat(id, &e.f, speed, lo, hi, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    e.active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        e.active = false;
        out = e.f;
        return out != current;
    }
    return false;
}

bool MarkerTools::intField(const char* id, int current, int& out, int lo, int hi)
{
    // Typed into or its - / + held (the group's IsItemActive covers the buttons too): its own count
    // till let go, then one op (HeldValue).
    Edit& e = editState(id);
    if (!e.active) e.held.reset(); // (not drawn last frame: from the document again)
    const auto done = editor::heldInputInt(e.held, id, current, lo, hi, false);
    e.active = e.held.held();
    if (done) out = *done;
    return done.has_value();
}

bool MarkerTools::colourField(const char* id, std::array<std::uint8_t, 3> current, std::array<std::uint8_t, 3>& out)
{
    Edit& e = editState(id);
    const bool changed = ImGui::ColorEdit3(id, e.colour.show(current).data(), ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB);
    // One op when the picking ends (the picker dragged, a number typed), not one a frame.
    const auto picked = e.colour.after(changed, !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left), current);
    if (picked) out = *picked;
    return picked.has_value();
}

bool MarkerTools::bitsField(const char* id, const std::vector<me::FlagBit>& bits, std::uint32_t value, std::uint32_t mixed, std::uint32_t& bit, bool& on)
{
    ImGui::PushID(id);
    bool clicked = false;
    std::uint32_t known = 0;
    for (const auto& b : bits) {
        known |= b.bit;
        bool v = (value & b.bit) != 0;
        const bool mix = (mixed & b.bit) != 0;
        if (mix) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        char label[96];
        std::snprintf(label, sizeof label, "%s (%s)", b.name, hexText(b.bit).c_str());
        if (ImGui::Checkbox(label, &v)) {
            clicked = true;
            bit = b.bit;
            on = mix ? true : v;
        }
        if (mix) ImGui::PopItemFlag();
        help(b.help);
    }
    if (const std::uint32_t other = value & ~known) {
        ImGui::TextDisabled("Other bits: %s (the game doesn't read them; kept as stored)", hexText(other).c_str());
    }
    ImGui::PopID();
    return clicked;
}

bool MarkerTools::linkFlagsButton(const char* id, std::uint8_t value, std::uint8_t& out)
{
    ImGui::PushID(id);
    const std::string label = hexText(value) + "##lf";
    if (ImGui::SmallButton(label.c_str())) ImGui::OpenPopup("##linkflags");
    help("The link's route classes: click to change them.");
    bool changed = false;
    if (ImGui::BeginPopup("##linkflags")) {
        ImGui::TextDisabled("Route classes of this link");
        for (const auto& b : me::waypointLinkFlagBits()) {
            bool v = (value & b.bit) != 0;
            if (ImGui::Checkbox(b.name, &v)) {
                out = static_cast<std::uint8_t>(v ? (value | b.bit) : (value & ~b.bit));
                changed = true;
            }
            help(b.help);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// --- The tool options -----------------------------------------------------------------------------

void MarkerTools::toolPanel()
{
    if (ed_.tool_ == MapEditor::Tool::Waypoint) {
        ImGui::SeparatorText("Waypoint");
        int mode = wpMode_ == WpMode::Place ? 0 : 1;
        ImGui::RadioButton("Place / select", &mode, 0);
        help("Click on empty map places a waypoint (enabled, the lowest free number); click one selects it, drag moves the selection. "
             "Shift+click adds or removes one, Ctrl+click removes.");
        ImGui::SameLine();
        ImGui::RadioButton("Connect", &mode, 1);
        help("Click a waypoint: the selected one links to it and it becomes the selected one, so clicks chain. Shift+click removes the link "
             "from the selected one to the one clicked.");
        wpMode_ = mode == 0 ? WpMode::Place : WpMode::Connect;
        ImGui::Checkbox("Both ways", &bothWays_);
        help("Links (and unlinks) go both ways: each waypoint links to the other.");
        ImGui::Checkbox("Place linked", &placeLinked_);
        help("Each waypoint placed is linked from the selected one (a path laid out click by click).");
        ImGui::Checkbox("Right click deletes", &wpRightDeletes_);
        help("A right click on a waypoint deletes it (else the right click opens the map's menu).");
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("New links' classes");
        ImGui::SameLine();
        std::uint8_t f = linkFlags_;
        if (linkFlagsButton("newlinks", linkFlags_, f)) linkFlags_ = f;
        ImGui::SameLine();
        if (ImGui::SmallButton("Default")) linkFlags_ = me::kDefaultLinkFlags;
        help("0x80: the class roaming monsters and Blink use (every stock link has it).");
        ImGui::Spacing();
        ImGui::TextWrapped("Snap: the toolbar's. Delete removes the selected waypoints (and the links to them).");
        if (const auto from = currentWaypoint()) ImGui::TextDisabled("Links start from waypoint %u.", *from);
        ImGui::Spacing();
        if (ImGui::Button("Waypoints...")) showWaypoints = true;
        flow("Enable All");
        if (ImGui::Button("Enable All")) ed_.commands_.run("waypoint.enableAll");
        help("Turns every waypoint on.");
        return;
    }
    ImGui::SeparatorText("Polygon");
    int mode = polyMode_ == PolyMode::Edit ? 0 : 1;
    if (ImGui::RadioButton("Edit", &mode, 0)) polyMode_ = PolyMode::Edit;
    help("Click inside a polygon selects it (the first in the list where they overlap, as the game picks); drag a corner to move it, drag "
         "inside the selected polygon to move it whole; Shift+click an edge adds a corner; right click or Delete removes the corner picked.");
    ImGui::SameLine();
    if (ImGui::RadioButton("Draw new", &mode, 1) && polyMode_ != PolyMode::Draw) startDrawing();
    help("Click the corners; Enter, a double click or a click on the first corner closes it (3 or more). It goes at the END of the list.");
    ImGui::Checkbox("Snap to other polygons' corners", &polygonSnap_);
    help("A corner dragged or drawn near another polygon's corner lands on it (so neighbours share edges with no gap). Else the toolbar's grid snap.");
    if (polygonSnap_) {
        ImGui::SetNextItemWidth(120 * ed_.imgui_->scale());
        ImGui::SliderFloat("Snap radius (pixels)", &polygonSnapPx_, 4, 40, "%.0f");
    }
    ImGui::Checkbox("Right click deletes a corner", &polyRightDeletes_);
    ImGui::Checkbox("Ambient preview", &ambientPreview_);
    help("Each polygon filled with its ambient light: dark ones more opaque.");
    ImGui::Spacing();
    if (polyMode_ == PolyMode::Draw) {
        ImGui::Text("%zu corners drawn", drawing_.size());
        ImGui::BeginDisabled(drawing_.size() < 3);
        if (ImGui::Button("Close Polygon")) finishDrawing();
        ImGui::EndDisabled();
        flow("Cancel");
        if (ImGui::Button("Cancel")) cancelDrawing();
    } else if (const auto key = selectedPolygon()) {
        const auto i = ed_.doc_.polygonIndex(*key);
        if (i) {
            ImGui::Text("%s: %zu corners, position %zu of %zu", me::fromMapText(ed_.doc_.polygons.polygons[*i].name).c_str(),
                        ed_.doc_.polygons.polygons[*i].points.size(), *i + 1, ed_.doc_.polygons.polygons.size());
        }
        if (activeCorner_) ImGui::TextDisabled("Corner %zu picked (Delete removes it).", activeCorner_->second + 1);
    }
    if (ImGui::Button("Polygons...")) showPolygons = true;
}

// --- Waypoint properties ---------------------------------------------------------------------------

void MarkerTools::waypointProperties()
{
    // (Copies: an op applied below replaces the document's waypoints under any reference.)
    auto& doc = ed_.doc_;
    const std::set<std::uint32_t> sel = ed_.selection_.waypoints;
    std::vector<nm::Waypoint> copies;
    for (const auto id : sel) {
        if (const auto* w = me::findWaypoint(doc, id)) copies.push_back(*w);
    }
    if (copies.empty()) return;
    std::vector<const nm::Waypoint*> ws;
    for (const auto& w : copies) ws.push_back(&w);
    const IdScope scope("wp" + std::to_string(*sel.begin()) + "." + std::to_string(sel.size()));
    if (ws.size() > 1) {
        ImGui::SeparatorText((std::to_string(ws.size()) + " waypoints").c_str());
        std::uint32_t all = ~0u, any = 0;
        for (const auto* w : ws) {
            all &= w->flags;
            any |= w->flags;
        }
        if (ImGui::Button("Enable")) ed_.perform(me::setWaypointsEnabledOp(doc, sel, true));
        flow("Disable");
        if (ImGui::Button("Disable")) ed_.perform(me::setWaypointsEnabledOp(doc, sel, false));
        flow("Delete");
        if (ImGui::Button("Delete")) ed_.deleteSelection();
        ImGui::SeparatorText("Flags (on all)");
        std::uint32_t bit = 0;
        bool on = false;
        if (bitsField("wpflags", me::waypointFlagBits(), any, any & ~all, bit, on)) ed_.perform(me::setWaypointFlagOp(doc, sel, bit, on));
        ImGui::SeparatorText("Links");
        std::size_t links = 0;
        for (const auto* w : ws) links += w->links.size();
        ImGui::Text("%zu links from these waypoints", links);
        if (ImGui::Button("Link them in a chain")) {
            // In id order, one way (or both): one op.
            me::Op op;
            op.label = "Link " + std::to_string(ws.size()) + " waypoints in a chain";
            me::MapDocument scratch = doc;
            for (std::size_t k = 0; k + 1 < ws.size(); ++k) {
                const me::Op step = me::linkWaypointsOp(scratch, ws[k]->id, ws[k + 1]->id, bothWays_, linkFlags_);
                if (step.empty()) continue;
                me::applyOp(scratch, step);
                op.parts.insert(op.parts.end(), step.parts.begin(), step.parts.end());
            }
            ed_.perform(op);
        }
        help("Each links to the next in id order (both ways when the tool's Both ways is on), with the tool's link classes.");
        return;
    }
    const nm::Waypoint w = *ws.front();
    ImGui::SeparatorText(("Waypoint " + std::to_string(w.id)).c_str());
    std::string text;
    row("Name", "Scripts find waypoints by name (Waypoint(\"name\")). The map stores it as 8-bit text.");
    if (textField("##wpname", me::fromMapText(w.name), me::kMarkerNameMax, text)) {
        nm::Waypoint c = w;
        c.name = text;
        ed_.perform(me::setWaypointOp(doc, c, "Rename waypoint " + std::to_string(w.id)));
    }
    row("Enabled", "Creatures, movers and scripts' waypoint searches use only enabled waypoints.");
    bool on = (w.flags & me::kWaypointEnabled) != 0;
    if (ImGui::Checkbox("##wpon", &on)) ed_.perform(me::setWaypointsEnabledOp(doc, {w.id}, on));
    row("Position");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    float v = 0;
    ImGui::SetNextItemWidth(half);
    if (floatField("##wpx", w.x, v, 1, 0, me::kWorldSize - 1)) {
        nm::Waypoint c = w;
        c.x = v;
        ed_.perform(me::setWaypointOp(doc, c, "Move waypoint " + std::to_string(w.id)));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    if (floatField("##wpy", w.y, v, 1, 0, me::kWorldSize - 1)) {
        nm::Waypoint c = w;
        c.y = v;
        ed_.perform(me::setWaypointOp(doc, c, "Move waypoint " + std::to_string(w.id)));
    }
    ImGui::SeparatorText("Flags");
    std::uint32_t bit = 0;
    bool set = false;
    if (bitsField("wpflags", me::waypointFlagBits(), w.flags, 0, bit, set)) ed_.perform(me::setWaypointFlagOp(doc, {w.id}, bit, set));
    if (w.flags & me::kWaypointRegistered) {
        ImGui::TextColored(editor::themeColours().error, "Flag 0x2 is the game's run-time mark: stored, it keeps the waypoint out of the searches.");
        if (ImGui::Button("Clear 0x2")) ed_.perform(me::setWaypointFlagOp(doc, {w.id}, me::kWaypointRegistered, false));
    }
    const std::uint8_t mask = me::waypointRouteMask(doc, w.id);
    ImGui::TextDisabled("Route classes it offers (its links and links to it): %s", hexText(mask).c_str());
    help("The game gives each waypoint the classes of its own links and of the links pointing at it; a search takes a waypoint when its classes "
         "meet the ones wanted (roaming monsters and Blink want 0x80).");

    ImGui::SeparatorText(("Links (" + std::to_string(w.links.size()) + ")").c_str());
    if (ImGui::BeginTable("##wplinks", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Classes");
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("");
        for (std::size_t k = 0; k < w.links.size(); ++k) {
            const auto& l = w.links[k];
            ImGui::PushID(static_cast<int>(k));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const auto* t = me::findWaypoint(doc, l.target);
            const bool back = t && std::ranges::any_of(t->links, [&](const nm::WaypointLink& b) { return b.target == w.id; });
            const std::string label = std::to_string(l.target) + (t && !t->name.empty() ? " " + me::fromMapText(t->name) : "") + (back ? "  (both ways)" : "");
            ImGui::TextUnformatted(label.c_str());
            ImGui::TableNextColumn();
            std::uint8_t f = l.flags;
            if (linkFlagsButton("lf", l.flags, f)) ed_.perform(me::setLinkFlagsOp(doc, w.id, l.target, f));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Go to") && t) {
                locate(t->x, t->y);
                selectWaypoints({t->id}, t->id);
            }
            help("Select the waypoint it leads to and show it.");
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) ed_.perform(me::unlinkWaypointsOp(doc, w.id, l.target, false));
            help("Removes this link (one way: the link back, if any, stays).");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const auto from = w.id;
    const me::WorldPoint fromAt{w.x, w.y};
    if (ImGui::Button("Add Link...")) {
        MapEditor::PickRequest req;
        req.kind = MapEditor::PickRequest::Kind::Waypoint;
        req.prompt = "Click the waypoint " + std::to_string(from) + " links to (Shift: both ways)";
        req.from = fromAt;
        req.owner = "waypoint.addLink";
        req.done = [this, from](const MapEditor::PickResult& r) {
            if (r.waypoint) ed_.perform(me::linkWaypointsOp(ed_.doc_, from, *r.waypoint, bothWays_ || r.shift, linkFlags_));
        };
        ed_.beginPick(std::move(req));
    }
    help("Pick the waypoint on the map. The new link gets the Waypoint tool's link classes.");
    const auto in = me::incomingLinks(doc, w.id);
    if (!in.empty()) {
        ImGui::TextDisabled("Linked from:");
        for (const auto& [id, f] : in) {
            ImGui::SameLine();
            const std::string b = std::to_string(id) + "##in" + std::to_string(id);
            if (ImGui::SmallButton(b.c_str())) {
                if (const auto* s = me::findWaypoint(doc, id)) {
                    locate(s->x, s->y);
                    selectWaypoints({id}, id);
                }
            }
            help(("A link from waypoint " + std::to_string(id) + " (classes " + hexText(f) + "): click to go to it.").c_str());
        }
    }
    // Objects naming it (a mover's waypoints) and waypoint groups with it: the Properties panel's
    // "Linked from" (the waypoints' links are listed above).
    std::vector<me::Link> objectsIn;
    for (const auto& l : ed_.props_->links().toWaypoint(w.id)) {
        if (l.source != me::Link::Source::Waypoint) objectsIn.push_back(l);
    }
    ed_.props_->linkedFrom(objectsIn, "wpLinkedFrom");
}

// --- Polygon properties ------------------------------------------------------------------------------

void MarkerTools::polygonProperties()
{
    auto& doc = ed_.doc_;
    std::vector<std::uint32_t> keys;
    for (const auto k : ed_.selection_.polygons) {
        if (doc.polygonIndex(k)) keys.push_back(k);
    }
    if (keys.empty()) return;
    if (keys.size() > 1) {
        ImGui::SeparatorText((std::to_string(keys.size()) + " polygons").c_str());
        if (ImGui::Button("Delete")) ed_.deleteSelection();
        flow("Frame");
        if (ImGui::Button("Frame")) frameKeys(ed_.selection_.polygons);
        ImGui::TextWrapped("Select one polygon to edit its name, light, group, handlers and flags.");
        for (const auto k : keys) {
            const auto i = *doc.polygonIndex(k);
            ImGui::BulletText("%zu. %s", i + 1, me::fromMapText(doc.polygons.polygons[i].name).c_str());
        }
        return;
    }
    const std::uint32_t key = keys.front();
    const std::size_t i = *doc.polygonIndex(key);
    const nm::Polygon p = doc.polygons.polygons[i]; // (a copy: ops below replace the list)
    const IdScope scope("poly" + std::to_string(key));
    ImGui::SeparatorText(("Polygon: " + me::fromMapText(p.name)).c_str());
    const auto commit = [&](nm::Polygon n, const std::string& label) { ed_.perform(me::setPolygonPropsOp(doc, key, n, label)); };
    std::string text;
    row("Name", "Its name (shown in the editor; scripts don't name polygons).");
    if (textField("##polyname", me::fromMapText(p.name), me::kMarkerNameMax, text)) {
        nm::Polygon n = p;
        n.name = text;
        commit(n, "Rename polygon " + text);
    }
    row("Ambient light",
        "Its ambient light: while the view's centre (the player) is inside it, the game lights the whole view with this colour, "
        "switching at once on entering (no fade). Where polygons overlap, the one entered first keeps the light; otherwise the "
        "first in the list. Outside every polygon the map's own ambient light (Map Info) applies.");
    std::array<std::uint8_t, 3> c{};
    if (colourField("##polyambient", p.ambientLight, c)) {
        nm::Polygon n = p;
        n.ambientLight = c;
        commit(n, "Polygon ambient light");
    }
    row("Minimap group", "The minimap group of the walls inside it (0-255; 100: none). The game's minimap shows a group's walls once one is seen.");
    int g = 0;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 70 * ed_.imgui_->scale()));
    if (intField("##polygroup", p.minimapGroup, g, 0, 255)) {
        nm::Polygon n = p;
        n.minimapGroup = static_cast<std::uint8_t>(g);
        commit(n, "Polygon minimap group " + std::to_string(g));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(p.minimapGroup == me::kNoMinimapGroup);
    if (ImGui::SmallButton("None")) {
        nm::Polygon n = p;
        n.minimapGroup = me::kNoMinimapGroup;
        commit(n, "Polygon minimap group none");
    }
    ImGui::EndDisabled();
    help("100: no group (what walls outside every polygon get).");
    ImGui::SeparatorText("Handlers");
    row("Player enters", "The script function called when a player walks in.");
    std::string fn = p.playerEnter.function;
    if (ed_.scriptPanel_->functionPicker("Player enters", fn)) {
        nm::Polygon n = p;
        n.playerEnter.function = fn;
        commit(n, "Polygon player-enter handler " + (fn.empty() ? std::string("none") : fn));
    }
    help("The script function called when a player walks in.");
    row("Monster enters", "The script function called when a monster walks in (monsters look only at polygons with a handler).");
    fn = p.monsterEnter.function;
    if (ed_.scriptPanel_->functionPicker("Monster enters", fn)) {
        nm::Polygon n = p;
        n.monsterEnter.function = fn;
        commit(n, "Polygon monster-enter handler " + (fn.empty() ? std::string("none") : fn));
    }
    help("The script function called when a monster walks in (monsters look only at polygons with a handler).");
    ImGui::SeparatorText("Flags");
    std::uint32_t bit = 0;
    bool on = false;
    if (bitsField("polyflags", me::polygonFlagBits(), p.flags, 0, bit, on)) {
        nm::Polygon n = p;
        n.flags = on ? (p.flags | bit) : (p.flags & ~bit);
        commit(n, std::string(on ? "Set" : "Clear") + " polygon flag " + hexText(bit));
    }
    ImGui::SeparatorText("Order");
    ImGui::Text("Position %zu of %zu", i + 1, doc.polygons.polygons.size());
    help("The game takes the FIRST polygon in the list that holds a point: where polygons overlap, the earlier one wins.");
    ImGui::SameLine();
    ImGui::BeginDisabled(i == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) ed_.perform(me::reorderPolygonOp(doc, key, i - 1));
    ImGui::EndDisabled();
    help("Earlier in the list: it wins over the ones after it where they overlap.");
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 >= doc.polygons.polygons.size());
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) ed_.perform(me::reorderPolygonOp(doc, key, i + 1));
    ImGui::EndDisabled();
    flow("First");
    if (ImGui::SmallButton("First")) ed_.perform(me::reorderPolygonOp(doc, key, 0));
    flow("Last");
    if (ImGui::SmallButton("Last")) ed_.perform(me::reorderPolygonOp(doc, key, doc.polygons.polygons.size() - 1));
    const auto now = doc.polygonIndex(key); // (it may have moved in the list above)
    if (!now) return;
    const auto corners = me::polygonCorners(doc, *now);
    ImGui::SeparatorText(("Corners (" + std::to_string(corners.size()) + ")").c_str());
    ImGui::TextDisabled("Drag them with the Polygon tool; Shift+click an edge adds one.");
    if (ImGui::BeginTable("##corners", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::min(10.0f, static_cast<float>(corners.size()) + 1.5f) * ImGui::GetTextLineHeightWithSpacing()))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30 * ed_.imgui_->scale());
        ImGui::TableSetupColumn("X");
        ImGui::TableSetupColumn("Y");
        ImGui::TableHeadersRow();
        for (std::size_t k = 0; k < corners.size(); ++k) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool active = activeCorner_ && activeCorner_->first == key && activeCorner_->second == k;
            const std::string l = std::to_string(k + 1) + "##c" + std::to_string(k);
            if (ImGui::Selectable(l.c_str(), active, ImGuiSelectableFlags_SpanAllColumns)) {
                activeCorner_ = std::pair{key, k};
                locate(corners[k].x, corners[k].y);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", corners[k].x);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", corners[k].y);
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Frame")) frameKeys({key});
    flow("Edit Corners");
    if (ImGui::Button("Edit Corners")) ed_.tool_ = MapEditor::Tool::Polygon;
    help("The Polygon tool: drag corners, Shift+click edges, right click deletes a corner.");
}

// --- Panels ------------------------------------------------------------------------------------------

void MarkerTools::syncHolds()
{
    // The open group and the map info are held while their panels show them.
    for (const auto& k : std::set<me::ElementKey>(ed_.panelLocks_)) {
        const bool want = (k.kind == me::ElementKey::Kind::Group && showGroups && openGroup_ && k == me::ElementKey::group(*openGroup_)) ||
                          (k.kind == me::ElementKey::Kind::MapInfo && showMapInfo);
        if (!want && (k.kind == me::ElementKey::Kind::Group || k.kind == me::ElementKey::Kind::MapInfo)) ed_.holdPanel(k, false);
    }
    if (openGroup_ && !ed_.doc_.groupIndex(*openGroup_)) openGroup_.reset();
    if (showGroups && openGroup_ && !ed_.holdPanel(me::ElementKey::group(*openGroup_), true)) openGroup_.reset();
    if (showMapInfo) ed_.holdPanel(me::ElementKey::mapInfo(), true);
    if (!showGroups && pickingMembers_) {
        pickingMembers_ = false;
        if (const auto* r = ed_.pickRequest(); r && r->owner == "group.members") ed_.cancelPick();
    }
}

void MarkerTools::panels()
{
    if (ed_.tool_ != MapEditor::Tool::Polygon && polyMode_ == PolyMode::Draw) {
        drawing_.clear();
        polyMode_ = PolyMode::Edit;
    }
    if (ed_.tool_ != MapEditor::Tool::Waypoint && ed_.tool_ != MapEditor::Tool::Polygon) drag_ = Drag::None;
    if (activeCorner_ && !ed_.selection_.polygons.contains(activeCorner_->first)) activeCorner_.reset();
    if (pickingMembers_ && !ed_.picking()) pickingMembers_ = false;
    if (edits_.size() > 2048) std::erase_if(edits_, [](const auto& kv) { return !kv.second.active && !kv.second.colour.picking(); });
    syncHolds();
    // A panel just opened comes to the front of its dock (it may share one with others): focused
    // for its first few frames (a window new to a dock node joins its tab bar a frame late).
    const auto panel = [&](bool show, int& focus, void (MarkerTools::*draw)()) {
        if (!show) {
            focus = -1;
            return;
        }
        if (focus < 0) focus = 3;
        if (focus > 0) {
            ImGui::SetNextWindowFocus();
            --focus;
        }
        (this->*draw)();
    };
    panel(showWaypoints, focus_[0], &MarkerTools::waypointsPanel);
    panel(showPolygons, focus_[1], &MarkerTools::polygonsPanel);
    panel(showGroups, focus_[2], &MarkerTools::groupsPanel);
    panel(showMapInfo, focus_[3], &MarkerTools::mapInfoPanel);
    if (showPictures_ && showMapInfo) picturePicker();
    panel(showMinimap, focus_[4], &MarkerTools::minimapPanel);
    panel(showStrings, focus_[5], &MarkerTools::stringsPanel);
}

void MarkerTools::waypointsPanel()
{
    ImGui::SetNextWindowSize(ImVec2(360 * ed_.imgui_->scale(), 420 * ed_.imgui_->scale()), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Waypoints", &showWaypoints)) {
        ImGui::End();
        return;
    }
    auto& doc = ed_.doc_;
    const auto& list = doc.waypoints.waypoints;
    std::size_t enabled = 0, links = 0;
    for (const auto& w : list) {
        enabled += (w.flags & me::kWaypointEnabled) ? 1 : 0;
        links += w.links.size();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##wpsearch", "Search by number or name", wpSearch_, sizeof wpSearch_);
    const std::string q = lowered(wpSearch_);
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (q.empty() || std::to_string(list[i].id).find(q) != std::string::npos || lowered(me::fromMapText(list[i].name)).find(q) != std::string::npos) rows.push_back(i);
    }
    ImGui::Text("%zu waypoints, %zu enabled, %zu links", list.size(), enabled, links);
    if (!q.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu listed)", rows.size());
    }
    std::set<std::uint32_t> listed;
    for (const auto r : rows) listed.insert(list[r].id);
    bool changed = false;
    ImGui::BeginDisabled(listed.empty());
    if (ImGui::Button("Enable All")) changed = ed_.perform(me::setWaypointsEnabledOp(doc, listed, true));
    help("Turns every listed waypoint on (with no search: every waypoint). The old editor's button of this name deleted them all.");
    flow("Disable All");
    if (ImGui::Button("Disable All")) changed = ed_.perform(me::setWaypointsEnabledOp(doc, listed, false));
    help("Turns every listed waypoint off.");
    flow("Select Listed");
    if (ImGui::Button("Select Listed")) selectWaypoints(listed);
    flow("Delete Listed");
    if (ImGui::Button("Delete Listed")) {
        me::Selection s;
        s.waypoints = listed;
        changed = ed_.perform(me::deleteOp(doc, s));
        if (changed) ed_.setSelection({}, true);
    }
    help("Deletes every listed waypoint and the links to them (undoable).");
    ImGui::EndDisabled();
    if (changed) {
        ImGui::End(); // (the list is drawn again next frame, from the changed waypoints)
        return;
    }
    if (ImGui::BeginTable("##wps", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_DefaultSort, 0.55f);
        ImGui::TableSetupColumn("Name", 0, 1.6f);
        ImGui::TableSetupColumn("X", 0, 0.65f);
        ImGui::TableSetupColumn("Y", 0, 0.65f);
        ImGui::TableSetupColumn("On", 0, 0.4f);
        ImGui::TableSetupColumn("Links", 0, 0.5f);
        ImGui::TableHeadersRow();
        if (const ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
            const int c = ss->Specs[0].ColumnIndex;
            const bool asc = ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            std::ranges::stable_sort(rows, [&](std::size_t a, std::size_t b) {
                const auto& A = list[a];
                const auto& B = list[b];
                bool lt = false, gt = false;
                switch (c) {
                case 1: lt = A.name < B.name; gt = B.name < A.name; break;
                case 2: lt = A.x < B.x; gt = B.x < A.x; break;
                case 3: lt = A.y < B.y; gt = B.y < A.y; break;
                case 4: lt = (A.flags & 1) < (B.flags & 1); gt = (B.flags & 1) < (A.flags & 1); break;
                case 5: lt = A.links.size() < B.links.size(); gt = B.links.size() < A.links.size(); break;
                default: lt = A.id < B.id; gt = B.id < A.id; break;
                }
                return asc ? lt : gt;
            });
        }
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(rows.size()));
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const auto& w = list[rows[static_cast<std::size_t>(r)]];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = ed_.selection_.waypoints.contains(w.id);
                const std::string label = std::to_string(w.id) + "##wp" + std::to_string(w.id);
                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    const ImGuiIO& io = ImGui::GetIO();
                    std::set<std::uint32_t> s = ed_.selection_.waypoints;
                    if (io.KeyShift && wpAnchorRow_ && *wpAnchorRow_ < rows.size()) {
                        const auto [a, b] = std::minmax(*wpAnchorRow_, static_cast<std::size_t>(r));
                        for (std::size_t k = a; k <= b; ++k) s.insert(list[rows[k]].id);
                        selectWaypoints(std::move(s));
                    } else if (io.KeyCtrl) {
                        if (!s.erase(w.id)) s.insert(w.id);
                        selectWaypoints(std::move(s), w.id);
                        wpAnchorRow_ = static_cast<std::size_t>(r);
                    } else {
                        selectWaypoints({w.id}, w.id);
                        wpAnchorRow_ = static_cast<std::size_t>(r);
                    }
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) locate(w.x, w.y, true);
                }
                editor::help("Double click to go to it; Ctrl / Shift to select several.");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(me::fromMapText(w.name).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", w.x);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", w.y);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((w.flags & me::kWaypointEnabled) ? "on" : "-");
                ImGui::TableNextColumn();
                ImGui::Text("%zu", w.links.size());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void MarkerTools::polygonsPanel()
{
    ImGui::SetNextWindowSize(ImVec2(360 * ed_.imgui_->scale(), 420 * ed_.imgui_->scale()), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Polygons", &showPolygons)) {
        ImGui::End();
        return;
    }
    auto& doc = ed_.doc_;
    const auto& list = doc.polygons.polygons;
    ImGui::TextDisabled("%zu polygons, in the game's order", list.size());
    help("The list order is the game's priority: it takes the FIRST polygon that holds a point, so where polygons overlap the earlier one "
         "wins (its light, its handlers, its minimap group).");
    if (ImGui::Button("New")) startDrawing();
    help("Draw a new polygon on the map (the Polygon tool's Draw mode). It goes at the end of the list.");
    const auto sel = selectedPolygon();
    const auto si = sel ? doc.polygonIndex(*sel) : std::nullopt;
    flow("Delete");
    ImGui::BeginDisabled(ed_.selection_.polygons.empty());
    if (ImGui::Button("Delete")) {
        if (ed_.perform(me::deletePolygonsOp(doc, ed_.selection_.polygons))) ed_.setSelection({}, true);
    }
    help("Deletes the selected polygons (undoable).");
    ImGui::EndDisabled();
    flow("WW");
    ImGui::BeginDisabled(!si || *si == 0);
    if (ImGui::ArrowButton("##pup", ImGuiDir_Up)) ed_.perform(me::reorderPolygonOp(doc, *sel, *si - 1));
    ImGui::EndDisabled();
    help("Move the selected polygon up: earlier polygons win where they overlap.");
    flow("WW");
    ImGui::BeginDisabled(!si || *si + 1 >= list.size());
    if (ImGui::ArrowButton("##pdown", ImGuiDir_Down)) ed_.perform(me::reorderPolygonOp(doc, *sel, *si + 1));
    ImGui::EndDisabled();
    help("Move the selected polygon down.");
    ImGui::Checkbox("Ambient preview", &ambientPreview_);
    help("Fill each polygon with its ambient light on the map.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##polysearch", "Search by name or handler", polySearch_, sizeof polySearch_);
    const std::string q = lowered(polySearch_);
    if (ImGui::BeginTable("##polys", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", 0, 0.35f);
        ImGui::TableSetupColumn("Name", 0, 1.6f);
        ImGui::TableSetupColumn("Light", 0, 0.5f);
        ImGui::TableSetupColumn("Pts", 0, 0.4f);
        ImGui::TableSetupColumn("Grp", 0, 0.4f);
        ImGui::TableSetupColumn("Handlers", 0, 1.2f);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& p = list[i];
            const std::string name = me::fromMapText(p.name);
            if (!q.empty() && lowered(name).find(q) == std::string::npos && lowered(p.playerEnter.function).find(q) == std::string::npos &&
                lowered(p.monsterEnter.function).find(q) == std::string::npos) {
                continue;
            }
            const std::uint32_t key = i < doc.polygonKeys.size() ? doc.polygonKeys[i] : 0;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool selected = ed_.selection_.polygons.contains(key);
            const std::string label = std::to_string(i + 1) + "##poly" + std::to_string(key);
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                selectPolygon(key, ImGui::GetIO().KeyCtrl);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frameKeys({key});
            }
            editor::help("Click selects it on the map; double click frames it.");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name.c_str());
            if (p.flags & 1) {
                ImGui::SameLine();
                ImGui::TextColored(editor::themeColours().warning, "secret");
            }
            ImGui::TableNextColumn();
            const ImVec2 sw(ImGui::GetTextLineHeight() * 1.6f, ImGui::GetTextLineHeight());
            const ImVec2 c0 = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(c0, ImVec2(c0.x + sw.x, c0.y + sw.y), col(p.ambientLight[0], p.ambientLight[1], p.ambientLight[2]));
            ImGui::Dummy(sw);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", p.points.size());
            ImGui::TableNextColumn();
            if (p.minimapGroup == me::kNoMinimapGroup) ImGui::TextDisabled("-");
            else ImGui::Text("%d", p.minimapGroup);
            ImGui::TableNextColumn();
            std::string h;
            if (!p.playerEnter.function.empty()) h += "player: " + p.playerEnter.function;
            if (!p.monsterEnter.function.empty()) h += (h.empty() ? "" : ", ") + std::string("monster: ") + p.monsterEnter.function;
            ImGui::TextUnformatted(h.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// --- Groups ------------------------------------------------------------------------------------------

std::string MarkerTools::memberLabel(const nm::Group& g, std::size_t i) const
{
    const auto& doc = ed_.doc_;
    switch (g.kind) {
    case nm::Group::Kind::Objects: {
        const auto ext = static_cast<std::uint32_t>(g.members[i]);
        for (const auto& [id, o] : doc.objects) {
            if (o.header.extent == ext) {
                std::string s = o.type + " #" + std::to_string(ext);
                if (o.header.details && !o.header.details->scriptName.empty()) s += " \"" + o.header.details->scriptName + "\"";
                return s;
            }
        }
        return "#" + std::to_string(ext) + " (no object has it)";
    }
    case nm::Group::Kind::Waypoints: {
        const auto* w = me::findWaypoint(doc, static_cast<std::uint32_t>(g.members[i]));
        if (!w) return "Waypoint " + std::to_string(g.members[i]) + " (missing)";
        return "Waypoint " + std::to_string(w->id) + (w->name.empty() ? "" : " \"" + me::fromMapText(w->name) + "\"");
    }
    case nm::Group::Kind::Walls: {
        const auto& p = g.walls[i];
        std::string s = "Wall " + std::to_string(p.x) + "," + std::to_string(p.y);
        if (p.x >= 0 && p.x < 256 && p.y >= 0 && p.y < 256) {
            const auto it = doc.walls.find(me::Cell{static_cast<std::uint8_t>(p.x), static_cast<std::uint8_t>(p.y)});
            if (it == doc.walls.end()) s += " (no wall)";
            else if (it->second.material < ed_.things_.walls.size()) s += " " + ed_.things_.walls[it->second.material].name;
        }
        return s;
    }
    case nm::Group::Kind::Unknown3: {
        const auto* sub = me::findGroupById(doc, g.members[i]);
        return "Group " + std::to_string(g.members[i]) + (sub ? " \"" + me::fromMapText(sub->name) + "\" (" + me::groupKindName(sub->kind) + ")" : " (missing)");
    }
    }
    return "?";
}

std::optional<me::WorldPoint> MarkerTools::memberPlace(const nm::Group& g, std::size_t i) const
{
    const auto& doc = ed_.doc_;
    switch (g.kind) {
    case nm::Group::Kind::Objects:
        for (const auto& [id, o] : doc.objects) {
            if (o.parent == 0 && o.header.extent == static_cast<std::uint32_t>(g.members[i])) return me::WorldPoint{o.header.x, o.header.y};
        }
        return std::nullopt;
    case nm::Group::Kind::Waypoints:
        if (const auto* w = me::findWaypoint(doc, static_cast<std::uint32_t>(g.members[i]))) return me::WorldPoint{w->x, w->y};
        return std::nullopt;
    case nm::Group::Kind::Walls: return me::WorldPoint{g.walls[i].x * me::kWallCellSize + 11.5f, g.walls[i].y * me::kWallCellSize + 11.5f};
    case nm::Group::Kind::Unknown3: return std::nullopt;
    }
    return std::nullopt;
}

void MarkerTools::pickMembers(std::uint32_t key)
{
    const auto gi = ed_.doc_.groupIndex(key);
    if (!gi) return;
    const nm::Group& g = ed_.doc_.groups.groups[*gi];
    MapEditor::PickRequest req;
    req.kind = g.kind == nm::Group::Kind::Objects ? MapEditor::PickRequest::Kind::Object
             : g.kind == nm::Group::Kind::Waypoints ? MapEditor::PickRequest::Kind::Waypoint
                                                    : MapEditor::PickRequest::Kind::Wall;
    req.prompt = "Click " + std::string(g.kind == nm::Group::Kind::Objects ? "objects" : g.kind == nm::Group::Kind::Waypoints ? "waypoints" : "walls") +
                 " to add to " + me::fromMapText(g.name) + " (Shift+click removes)";
    req.repeat = true;
    req.owner = "group.members";
    req.done = [this, key](const MapEditor::PickResult& r) {
        me::Selection s;
        if (r.object) s.objects.insert(*r.object);
        if (r.waypoint) s.waypoints.insert(*r.waypoint);
        if (r.wall) s.walls.insert(*r.wall);
        const me::Op op = r.shift ? me::removeGroupMembersOp(ed_.doc_, key, s) : me::addGroupMembersOp(ed_.doc_, key, s);
        if (op.empty()) ed_.status(r.shift ? "Not a member" : "Already a member");
        else ed_.perform(op);
    };
    ed_.beginPick(std::move(req));
    pickingMembers_ = true;
}

void MarkerTools::groupsPanel()
{
    ImGui::SetNextWindowSize(ImVec2(380 * ed_.imgui_->scale(), 520 * ed_.imgui_->scale()), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Groups", &showGroups)) {
        ImGui::End();
        return;
    }
    auto& doc = ed_.doc_;
    const auto& list = doc.groups.groups;
    // A new group.
    ImGui::SetNextItemWidth(160 * ed_.imgui_->scale());
    ImGui::InputTextWithHint("##newgroup", "New group's name", newGroupName_, sizeof newGroupName_);
    help("Scripts find a group by its name (ObjectGroup(\"name\"), WaypointGroup, WallGroup).");
    flow("Waypoints  WW");
    ImGui::SetNextItemWidth(110 * ed_.imgui_->scale());
    const char* kinds[] = {"Objects", "Waypoints", "Walls", "Groups"};
    ImGui::Combo("##newkind", &newGroupKind_, kinds, 4);
    help("What its members are: objects (by extent), waypoints, walls, or other groups (a group of groups: the scripts' for-each walks each).");
    flow("Create");
    const std::string newName = me::toMapText(newGroupName_);
    ImGui::BeginDisabled(newName.empty() || newName.size() > me::kGroupNameMax);
    if (ImGui::Button("Create")) {
        std::uint32_t key = 0;
        if (ed_.perform(me::addGroupOp(doc, newName, kKinds[newGroupKind_], &key))) {
            openGroup_ = key;
            newGroupName_[0] = 0;
        }
    }
    ImGui::EndDisabled();
    if (newName.size() > me::kGroupNameMax) ImGui::TextColored(editor::themeColours().error, "Longer than the game's 75-byte name buffer.");

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##groupsearch", "Search groups", groupSearch_, sizeof groupSearch_);
    const std::string q = lowered(groupSearch_);
    const float listH = openGroup_ ? std::min(9.0f, static_cast<float>(list.size()) + 1.5f) * ImGui::GetTextLineHeightWithSpacing() : 0.0f;
    if (ImGui::BeginTable("##groups", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70 * ed_.imgui_->scale());
        ImGui::TableSetupColumn("Members", ImGuiTableColumnFlags_WidthFixed, 56 * ed_.imgui_->scale());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 16 * ed_.imgui_->scale());
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& g = list[i];
            const std::string name = me::fromMapText(g.name);
            if (!q.empty() && lowered(name).find(q) == std::string::npos) continue;
            const std::uint32_t key = i < doc.groupKeys.size() ? doc.groupKeys[i] : 0;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = (name.empty() ? std::string("(no name)") : name) + "##g" + std::to_string(key);
            if (ImGui::Selectable(label.c_str(), openGroup_ == key, ImGuiSelectableFlags_SpanAllColumns)) {
                if (openGroup_ != key) {
                    if (pickingMembers_) ed_.cancelPick();
                    openGroup_ = key;
                }
            }
            if (const auto h = ed_.otherHolder(me::ElementKey::group(key))) {
                ImGui::SameLine();
                ImGui::TextColored(editor::themeColours().caution, "(being edited by %s)", ed_.peerName(*h).c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(me::groupKindName(g.kind));
            ImGui::TableNextColumn();
            ImGui::Text("%zu", g.kind == nm::Group::Kind::Walls ? g.walls.size() : g.members.size());
            ImGui::TableNextColumn();
            const auto problems = me::groupProblems(doc, i);
            if (!problems.empty()) {
                ImGui::TextColored(editor::themeColours().warning, "!");
                if (ImGui::IsItemHovered()) {
                    std::string t;
                    for (const auto& p : problems) t += (t.empty() ? "" : "\n") + p;
                    ImGui::SetTooltip("%s", t.c_str());
                }
            }
        }
        ImGui::EndTable();
    }
    if (list.empty()) ImGui::TextDisabled("No groups yet.");
    if (openGroup_) {
        if (const auto gi = doc.groupIndex(*openGroup_)) groupDetails(*gi);
    }
    // The kind change's confirmation (members are cleared: a destructive change).
    if (kindChange_) ImGui::OpenPopup("Change group kind?");
    if (ImGui::BeginPopupModal("Change group kind?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto gi = kindChange_ ? doc.groupIndex(kindChange_->first) : std::nullopt;
        if (!gi) {
            kindChange_.reset();
            ImGui::CloseCurrentPopup();
        } else {
            const nm::Group& g = doc.groups.groups[*gi];
            ImGui::Text("%s has %zu members. Changing its kind to %s clears them.", me::fromMapText(g.name).c_str(),
                        g.kind == nm::Group::Kind::Walls ? g.walls.size() : g.members.size(), me::groupKindName(kKinds[kindChange_->second]));
            if (ImGui::Button("Change and Clear")) {
                nm::Group n = g;
                n.kind = kKinds[kindChange_->second];
                n.members.clear();
                n.walls.clear();
                ed_.perform(me::setGroupOp(doc, kindChange_->first, n, "Group " + me::fromMapText(g.name) + " kind " + me::groupKindName(n.kind)));
                kindChange_.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                kindChange_.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void MarkerTools::groupDetails(std::size_t index)
{
    auto& doc = ed_.doc_;
    const nm::Group g = doc.groups.groups[index]; // (a copy: ops below replace the list)
    const std::uint32_t key = doc.groupKeys[index];
    const IdScope scope("group" + std::to_string(key));
    ImGui::SeparatorText(("Group: " + me::fromMapText(g.name)).c_str());
    std::string text;
    row("Name", "Scripts find the group by this name (compiled into its id).");
    if (textField("##gname", me::fromMapText(g.name), me::kGroupNameMax, text)) {
        nm::Group n = g;
        n.name = text;
        n.terminated = true;
        ed_.perform(me::setGroupOp(doc, key, n, "Rename group " + text));
    }
    row("Kind", "What its members are. Changing it clears the members (asks first).");
    int kind = static_cast<int>(g.kind);
    const char* kinds[] = {"Objects", "Waypoints", "Walls", "Groups (kind 3)"};
    if (ImGui::Combo("##gkind", &kind, kinds, 4) && kind != static_cast<int>(g.kind)) {
        const std::size_t members = g.kind == nm::Group::Kind::Walls ? g.walls.size() : g.members.size();
        if (members == 0) {
            nm::Group n = g;
            n.kind = kKinds[kind];
            ed_.perform(me::setGroupOp(doc, key, n, "Group " + me::fromMapText(g.name) + " kind " + me::groupKindName(n.kind)));
        } else {
            kindChange_ = std::pair{key, kind};
        }
    }
    row("Id", "The number scripts reach the group by (the compiler turns the name into it). It must be unique.");
    ImGui::Text("%d", g.id);
    if (std::ranges::count_if(doc.groups.groups, [&](const nm::Group& o) { return o.id == g.id; }) > 1) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Give it a fresh id")) {
            nm::Group n = g;
            n.id = me::freeGroupId(doc);
            ed_.perform(me::setGroupOp(doc, key, n, "Group " + me::fromMapText(g.name) + " id " + std::to_string(n.id)));
        }
        help("Another group has the same id: scripts reach only the last of them.");
    }
    for (const auto& p : me::groupProblems(doc, index)) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(editor::themeColours().warning, "! %s", p.c_str());
        ImGui::PopTextWrapPos();
    }
    if (g.kind == nm::Group::Kind::Unknown3) {
        // Members are other groups, picked from the list (never one leading back to this one).
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##addgroup", "Add a group...")) {
            for (const auto& o : doc.groups.groups) {
                if (o.id == g.id || me::groupCycle(doc, g.id, o.id) || std::ranges::find(g.members, o.id) != g.members.end()) continue;
                const std::string l = me::fromMapText(o.name) + " (" + me::groupKindName(o.kind) + ", id " + std::to_string(o.id) + ")";
                if (ImGui::Selectable(l.c_str())) ed_.perform(me::addGroupMemberIdsOp(doc, key, {o.id}));
            }
            ImGui::EndCombo();
        }
        help("The scripts' for-each over this group walks each member group in turn.");
    } else {
        const bool armed = pickingMembers_ && ed_.picking();
        if (ImGui::Button(armed ? "Picking... (click to stop)" : "Pick on the Map")) {
            if (armed) ed_.cancelPick();
            else pickMembers(key);
        }
        help("Click members on the map one after another; Shift+click removes one. Right click or Escape stops.");
        flow("Add Selection");
        ImGui::BeginDisabled(ed_.selection_.empty());
        if (ImGui::Button("Add Selection")) {
            if (!ed_.perform(me::addGroupMembersOp(doc, key, ed_.selection_))) ed_.status("Nothing selected fits this group's kind (or it's in already)");
        }
        help("The selected objects, waypoints or walls (those of the group's kind) become members.");
        flow("Remove Selection");
        if (ImGui::Button("Remove Selection")) ed_.perform(me::removeGroupMembersOp(doc, key, ed_.selection_));
        ImGui::EndDisabled();
        flow("Select Members");
        if (ImGui::Button("Select Members")) {
            const me::Selection s = me::groupSelection(doc, g);
            if (const auto b = me::bounds(doc, s)) ed_.frameWorld(b->first.x, b->first.y, b->second.x, b->second.y);
            ed_.setSelection(s);
        }
        help("Selects the members on the map (and frames them).");
    }
    if (ImGui::Button("Delete Group")) {
        if (pickingMembers_) ed_.cancelPick();
        ed_.perform(me::deleteGroupOp(doc, key));
        openGroup_.reset();
        return;
    }
    help("Deletes the group (undoable); its members stay on the map.");
    const std::size_t count = g.kind == nm::Group::Kind::Walls ? g.walls.size() : g.members.size();
    ImGui::SeparatorText(("Members (" + std::to_string(count) + ")").c_str());
    ImGui::BeginChild("##members", ImVec2(0, 0), ImGuiChildFlags_Borders);
    std::optional<std::size_t> drop;
    for (std::size_t k = 0; k < count; ++k) {
        ImGui::PushID(static_cast<int>(k));
        if (ImGui::SmallButton("x")) drop = k;
        help("Remove this member.");
        ImGui::SameLine();
        const std::string l = memberLabel(g, k);
        if (ImGui::Selectable(l.c_str())) {
            if (const auto at = memberPlace(g, k)) locate(at->x, at->y);
        }
        editor::help("Click to show it on the map.");
        ImGui::PopID();
    }
    if (count == 0) ImGui::TextDisabled(g.kind == nm::Group::Kind::Unknown3 ? "No member groups." : "No members: pick them on the map.");
    ImGui::EndChild();
    if (drop) {
        nm::Group n = g;
        if (n.kind == nm::Group::Kind::Walls) n.walls.erase(n.walls.begin() + static_cast<std::ptrdiff_t>(*drop));
        else n.members.erase(n.members.begin() + static_cast<std::ptrdiff_t>(*drop));
        ed_.perform(me::setGroupOp(doc, key, n, "Remove a member from group " + me::fromMapText(g.name)));
    }
}

// --- Map info ----------------------------------------------------------------------------------------

void MarkerTools::mapInfoPanel()
{
    ImGui::SetNextWindowSize(ImVec2(420 * ed_.imgui_->scale(), 640 * ed_.imgui_->scale()), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Map Info", &showMapInfo)) {
        ImGui::End();
        return;
    }
    auto& doc = ed_.doc_;
    const bool held = ed_.panelLocks_.contains(me::ElementKey::mapInfo());
    if (!held) {
        const auto h = ed_.locks_.holder(me::ElementKey::mapInfo());
        ImGui::TextColored(editor::themeColours().caution, "Being edited by %s.", h ? ed_.peerName(*h).c_str() : "another editor");
    }
    ImGui::BeginDisabled(!held);
    const nm::MapInfo& cur = doc.info;
    const auto commit = [&](const nm::MapInfo& n, const std::string& label) { ed_.perform(me::Op{label, {me::SetMapInfo{n}}}); };
    std::string text;
    // A fixed text field: N-1 bytes of 8-bit text and the NUL.
    const auto fixed = [&](const char* label, auto member, const char* tip, bool multiline = false) {
        constexpr std::size_t kN = sizeof((cur.*member).raw);
        row(label, tip);
        const std::string id = std::string("##mi") + label;
        if (textField(id.c_str(), me::fromMapText((cur.*member).view()), kN - 1, text, multiline, multiline ? 90 * ed_.imgui_->scale() : 0)) {
            nm::MapInfo n = cur;
            (n.*member).set(text);
            commit(n, std::string("Map info: ") + label);
        }
    };
    ImGui::SeparatorText("About the map");
    fixed("Title", &nm::MapInfo::title, "The name the game's map lists show (63 bytes).");
    fixed("Description", &nm::MapInfo::description, "Shown with the map in the game's lists (511 bytes).", true);
    fixed("Version", &nm::MapInfo::mapVersion, "The map's own version text (15 bytes).");
    fixed("Date", &nm::MapInfo::date, "Free text (31 bytes).");
    fixed("Copyright", &nm::MapInfo::copyright, "127 bytes.");
    ImGui::SeparatorText("Authors");
    fixed("Author", &nm::MapInfo::author, "63 bytes.");
    fixed("E-mail", &nm::MapInfo::email, "63 bytes.");
    fixed("Second author", &nm::MapInfo::author2, "127 bytes.");
    fixed("Second e-mail", &nm::MapInfo::email2, "127 bytes.");

    ImGui::SeparatorText("Game types");
    help("The modes the map offers (a bitfield: any number of them). The game's map lists filter by these bits.");
    std::uint32_t bit = 0;
    bool on = false;
    if (bitsField("gametypes", me::gameTypeBits(), cur.gameFlags, 0, bit, on)) {
        nm::MapInfo n = cur;
        n.gameFlags = on ? (cur.gameFlags | bit) : (cur.gameFlags & ~bit);
        commit(n, std::string("Map info: ") + (on ? "add" : "remove") + " game type " + hexText(bit));
    }

    ImGui::SeparatorText("Version and players");
    row("Info version", "Version 2 stores the recommended players; version 3 stores the quest intro title and picture instead (the game's own save "
                        "writes 3). Changing it keeps the other fields.");
    const std::string vlabel = cur.version == 2 ? "2: recommended players" : cur.version == 3 ? "3: quest intro" : std::to_string(cur.version) + " (as stored)";
    if (ImGui::BeginCombo("##miversion", vlabel.c_str())) {
        for (const int v : {2, 3}) {
            const std::string l = v == 2 ? "2: recommended players" : "3: quest intro";
            if (ImGui::Selectable(l.c_str(), cur.version == v) && cur.version != v) {
                nm::MapInfo n = cur;
                n.version = static_cast<std::uint16_t>(v);
                commit(n, "Map info: version " + std::to_string(v));
            }
        }
        ImGui::EndCombo();
    }
    if (cur.version == 2) {
        int v = 0;
        row("Min players", "Recommended, shown in the game's lists (stored only in version 2).");
        if (intField("##minp", cur.minPlayers, v, 0, 255)) {
            nm::MapInfo n = cur;
            n.minPlayers = static_cast<std::uint8_t>(v);
            commit(n, "Map info: min players " + std::to_string(v));
        }
        row("Max players", "Recommended (stored only in version 2).");
        if (intField("##maxp", cur.maxPlayers, v, 0, 255)) {
            nm::MapInfo n = cur;
            n.maxPlayers = static_cast<std::uint8_t>(v);
            commit(n, "Map info: max players " + std::to_string(v));
        }
        if (cur.minPlayers > cur.maxPlayers) ImGui::TextColored(editor::themeColours().warning, "The minimum is above the maximum.");
    } else {
        ImGui::TextDisabled("Recommended players are stored only in version 2 (the game reads 2 / 16 here).");
    }
    if (cur.version == 3) {
        row("Quest intro", "The quest intro's title (31 bytes: the game reads it into a 32-byte buffer without a check).");
        if (textField("##qintro", me::fromMapText(cur.questIntro), me::kQuestIntroMax, text)) {
            nm::MapInfo n = cur;
            n.questIntro = text;
            commit(n, "Map info: quest intro");
        }
        flow("Texts...");
        if (ImGui::SmallButton("Texts...")) {
            browseStrings(cur.questIntro, [this](const std::string& label) {
                nm::MapInfo n = ed_.doc_.info;
                n.questIntro = me::fitMapText(label, me::kQuestIntroMax);
                ed_.perform(me::Op{"Map info: quest intro", {me::SetMapInfo{n}}});
            });
        }
        help("Pick a label from the game's texts.");
        row("Quest picture", "The quest intro's picture: one of thing.bin's named images, by name (31 bytes: the game reads it into a "
                             "32-byte buffer without a check). A name the game hasn't shows WarriorChapterBegin8.");
        if (textField("##qgraphic", me::fromMapText(cur.questGraphic), me::kQuestIntroMax, text)) {
            nm::MapInfo n = cur;
            n.questGraphic = text;
            commit(n, "Map info: quest picture");
        }
        flow("Pictures...");
        if (ImGui::SmallButton("Pictures...")) showPictures_ = true;
        help("Browse the game's pictures with thumbnails (a click sets the field).");
        // (gLoadImg compares the names exactly.)
        const std::string graphic = cur.questGraphic.substr(0, cur.questGraphic.find('\0'));
        if (!graphic.empty() && std::ranges::none_of(ed_.things_.images, [&](const nox::things::ImageDef& d) { return d.name == graphic; })) {
            ImGui::TextColored(editor::themeColours().warning, "Not one of the game's named images: it shows WarriorChapterBegin8.");
        }
    } else {
        ImGui::TextDisabled("The quest intro title and picture are stored only in version 3.");
    }

    ImGui::SeparatorText("Light and intro");
    row("Ambient light",
        "The map's light where the view's centre (the player) is outside every polygon (AmbientData). Inside a polygon the game "
        "lights the whole view with that polygon's own ambient light instead, so on a map whose polygons cover the play area "
        "(most stock maps) this one never shows. The Lighting Preview layer (L) shows the light the game would use where the "
        "view's centre is.");
    std::array<std::uint8_t, 3> amb{static_cast<std::uint8_t>(std::min<std::uint32_t>(doc.ambient.r, 255)), static_cast<std::uint8_t>(std::min<std::uint32_t>(doc.ambient.g, 255)),
                                    static_cast<std::uint8_t>(std::min<std::uint32_t>(doc.ambient.b, 255))};
    std::array<std::uint8_t, 3> na{};
    if (colourField("##miambient", amb, na)) {
        nm::AmbientData a = doc.ambient;
        a.r = na[0];
        a.g = na[1];
        a.b = na[2];
        ed_.perform(me::Op{"Map ambient light", {me::SetAmbient{a}}});
    }
    // Where it shows: the polygon the preview's light comes from (the game's rule: ambientAt).
    if (!ed_.settings_.layers.lighting) {
        ImGui::TextDisabled("Turn on the Lighting Preview layer (L) to see it on the map.");
    } else if (const std::uint32_t in = ed_.view_ ? ed_.view_->ambientPolygon() : 0; in != 0 && in <= doc.polygons.polygons.size()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(editor::themeColours().warning, "The view's centre is in polygon %u (%s): its ambient light shows there, not this one.", in,
                           me::fromMapText(doc.polygons.polygons[in - 1].name).c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::TextDisabled("Intro text (shown when the map starts)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (textField("##intro", me::fromMapText(doc.intro.text), 0xFFFF, text, true, 110 * ed_.imgui_->scale())) {
        nm::MapIntro in = doc.intro;
        in.text = text;
        ed_.perform(me::Op{"Map intro text", {me::SetIntro{in}}});
    }
    ImGui::PushStyleColor(ImGuiCol_Header, editor::themeColours().section); // (as Properties' sections)
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editor::themeColours().sectionHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, editor::themeColours().sectionActive);
    const bool advanced = ImGui::CollapsingHeader("Advanced");
    ImGui::PopStyleColor(3);
    if (advanced) {
        const auto& raw = cur.unknown.raw;
        const auto set = std::ranges::count_if(raw, [](char c) { return c != 0; });
        ImGui::TextWrapped("The 256-byte field after the second e-mail: the game reads it and never uses it. Kept as stored (%td bytes set).", set);
        if (set > 0) ImGui::TextDisabled("Text: %s", me::fromMapText(cur.unknown.view()).c_str());
        ImGui::TextDisabled("Raw flags: %s", hexText(cur.gameFlags).c_str());
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// --- The quest picture picker ---------------------------------------------------------------------------

bool MarkerTools::setQuestPicture(const std::string& name)
{
    if (name.size() > me::kQuestIntroMax) {
        ed_.status("\"" + name + "\" is " + std::to_string(name.size()) + " bytes: the quest picture's name holds 31 (the game's 32-byte buffer)");
        return false;
    }
    nm::MapInfo n = ed_.doc_.info;
    if (n.version != 3 || n.questGraphic == name) return false;
    n.questGraphic = name;
    return ed_.perform(me::Op{"Map info: quest picture " + name, {me::SetMapInfo{n}}});
}

void MarkerTools::picturePicker()
{
    const float s = ed_.imgui_->scale();
    ImGui::SetNextWindowSize(ImVec2(760 * s, 600 * s), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Quest Pictures", &showPictures_)) {
        ImGui::End();
        return;
    }
    if (!pictures_) {
        if (ed_.ctx_.gameDir.empty()) {
            ImGui::TextDisabled("No game folder: no pictures to show. The name can still be typed in Map Info.");
            ImGui::End();
            return;
        }
        pictures_ = std::make_unique<QuestPictures>(ed_.things_, ed_.ctx_.gameDir);
    }
    if (!pictures_->ready()) {
        ImGui::TextDisabled("Reading the game's pictures...");
        ImGui::End();
        return;
    }
    if (pictures_->entries().empty()) {
        ImGui::TextDisabled("No pictures (%s). The name can still be typed in Map Info.", pictures_->error().c_str());
        ImGui::End();
        return;
    }
    const bool held = ed_.panelLocks_.contains(me::ElementKey::mapInfo());
    const nm::MapInfo& info = ed_.doc_.info;
    const std::string current = info.questGraphic.substr(0, info.questGraphic.find('\0'));
    ImGui::TextWrapped("The picture the quest's screens show for this map (the game loads it by name from thing.bin's named images). "
                       "A click sets the Quest picture field; typing it there still works.");
    if (info.version != 3) ImGui::TextColored(editor::themeColours().warning, "Only map info version 3 stores a quest picture (Map Info: Info version).");
    ImGui::SetNextItemWidth(220 * s);
    ImGui::InputTextWithHint("##picfilter", "Filter by name", pictureFilter_, sizeof pictureFilter_);
    ImGui::SameLine();
    ImGui::Checkbox("Every named image", &picturesAll_);
    help("Off: the full-screen pictures (the chapters' briefings, the quest's own, the menus' backgrounds) and any a stock map uses. "
         "On: all of thing.bin's named images (any works, drawn from the screen's top left).");
    ImGui::SameLine();
    ImGui::TextDisabled("Now: %s", current.empty() ? "(none)" : current.c_str());
    const std::string filter = lowered(pictureFilter_);
    auto& entries = pictures_->entries();
    std::vector<QuestPictures::Entry*> shown;
    for (auto& e : entries) {
        const bool wanted = picturesAll_ || e.info.fullScreen || !e.info.usedBy.empty() || e.info.name == current;
        if (!wanted || (!filter.empty() && lowered(e.info.name).find(filter) == std::string::npos)) continue;
        shown.push_back(&e);
    }
    // The stock maps' choices first, then thing.bin's order.
    std::ranges::stable_sort(shown, [](const auto* a, const auto* b) { return !a->info.usedBy.empty() && b->info.usedBy.empty(); });
    ImGui::TextDisabled("%zu pictures", shown.size());
    ImGui::BeginChild("##pictures", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const float cellW = QuestPictures::kThumbW * s, cellH = QuestPictures::kThumbH * s;
    const float pad = ImGui::GetStyle().ItemSpacing.x;
    const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + pad) / (cellW + pad + 8 * s)));
    std::string picked;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        auto& e = *shown[i];
        ImGui::PushID(static_cast<int>(i));
        if (i % static_cast<std::size_t>(perRow) != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        const bool fits = e.info.name.size() <= me::kQuestIntroMax;
        const bool isCurrent = e.info.name == current;
        ImGui::BeginDisabled(!held || !fits || info.version != 3);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##cell", isCurrent, ImGuiSelectableFlags_None, ImVec2(cellW + 8 * s, cellH + ImGui::GetTextLineHeightWithSpacing() * 2 + 8 * s))) picked = e.info.name;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            std::string tip = e.info.name + "\n" + std::to_string(e.info.width) + " x " + std::to_string(e.info.height);
            if (!e.info.usedBy.empty()) {
                tip += "\nUsed by the stock quest maps:";
                for (const auto& m : e.info.usedBy) tip += " " + m;
            }
            if (!fits) tip += "\n" + std::to_string(e.info.name.size()) + " bytes: the quest picture's name holds 31 (the game's 32-byte buffer).";
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (SDL_Texture* t = pictures_->texture(e, ed_.ctx_.renderer)) {
            float tw = 0, th = 0;
            SDL_GetTextureSize(t, &tw, &th);
            float k = std::min(cellW / std::max(1.0f, tw), cellH / std::max(1.0f, th));
            if (!e.info.fullScreen) k = std::min(k, 2.0f * s); // (an icon not blown up past twice)
            const float w = tw * k, h = th * k;
            const ImVec2 p0(at.x + 4 * s + (cellW - w) / 2, at.y + 4 * s + (cellH - h) / 2);
            dl->AddImage(texId(t), p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1), fits ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 90));
        } else {
            dl->AddText(ImVec2(at.x + 6 * s, at.y + cellH / 2), editor::onPanelU32(col(150, 150, 150)), "(no image)");
        }
        const ImU32 textCol = fits ? (isCurrent ? ImGui::ColorConvertFloat4ToU32(editor::themeColours().highlightText) : ImGui::GetColorU32(ImGuiCol_Text)) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const ImVec2 tp(at.x + 4 * s, at.y + cellH + 6 * s);
        dl->PushClipRect(tp, ImVec2(at.x + cellW + 4 * s, tp.y + ImGui::GetTextLineHeightWithSpacing() * 2), true);
        dl->AddText(tp, textCol, e.info.name.c_str());
        const std::string sub = !fits ? "too long (" + std::to_string(e.info.name.size()) + " bytes)"
                                      : !e.info.usedBy.empty() ? "used by " + std::to_string(e.info.usedBy.size()) + (e.info.usedBy.size() == 1 ? " stock map" : " stock maps")
                                                               : std::to_string(e.info.width) + " x " + std::to_string(e.info.height);
        dl->AddText(ImVec2(tp.x, tp.y + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        dl->PopClipRect();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (shown.empty()) ImGui::TextDisabled("No picture's name has \"%s\".", pictureFilter_);
    ImGui::EndChild();
    ImGui::End();
    if (!picked.empty()) setQuestPicture(picked);
}

// --- Minimap -----------------------------------------------------------------------------------------

std::uint32_t MarkerTools::floorColour(int floor)
{
    if (const auto it = floorColours_.find(floor); it != floorColours_.end()) return it->second;
    std::uint32_t out = 0xFF404040u;
    const auto& things = ed_.things_;
    if (floor >= 0 && static_cast<std::size_t>(floor) < things.floors.size() && !things.floors[static_cast<std::size_t>(floor)].images.empty() && ed_.ctx_.sprites) {
        // The floor's first image's average colour (its opaque pixels).
        if (const auto* d = ed_.ctx_.sprites->decoded(ed_.ctx_.sprites->indexOf(things.floors[static_cast<std::size_t>(floor)].images[0]))) {
            std::uint64_t r = 0, g = 0, b = 0, n = 0;
            for (std::size_t k = 0; k + 3 < d->rgba.size(); k += 4) {
                if (d->rgba[k + 3] < 128) continue;
                r += d->rgba[k];
                g += d->rgba[k + 1];
                b += d->rgba[k + 2];
                ++n;
            }
            if (n > 0) out = 0xFF000000u | static_cast<std::uint32_t>(b / n) << 16 | static_cast<std::uint32_t>(g / n) << 8 | static_cast<std::uint32_t>(r / n);
        }
    }
    floorColours_[floor] = out;
    return out;
}

void MarkerTools::rebuildMinimap()
{
    // One pixel a wall cell (256 x 256): the floor tile's colour at the cell's centre, walls over it.
    constexpr int kN = 256;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(kN * kN), 0xFF0C0C0Eu); // (ABGR in memory: RGBA bytes)
    const auto& doc = ed_.doc_;
    for (int y = 0; y < kN; ++y) {
        for (int x = 0; x < kN; ++x) {
            const auto t = me::tileAt(x * me::kWallCellSize + 11.5f, y * me::kWallCellSize + 11.5f);
            if (!t) continue;
            if (const auto* tile = doc.floor.tile(t->gx, t->gy)) px[static_cast<std::size_t>(y * kN + x)] = floorColour(tile->image);
        }
    }
    std::set<std::pair<int, int>> secret, window, breakable;
    for (const auto& s : doc.secretWalls.walls) secret.insert({s.pos.x, s.pos.y});
    for (const auto& p : doc.windowWalls.walls) window.insert({p.x, p.y});
    for (const auto& p : doc.destructableWalls.walls) breakable.insert({p.x, p.y});
    for (const auto& [c, w] : doc.walls) {
        const std::pair<int, int> k{c.x, c.y};
        std::uint32_t v = 0xFFE6E6E6u;
        if (secret.contains(k)) v = 0xFFFF9050u;
        else if (breakable.contains(k)) v = 0xFF4A5AFFu;
        else if (window.contains(k)) v = 0xFFE6E65Au;
        px[static_cast<std::size_t>(c.y) * kN + c.x] = v;
    }
    // A session's pending walls as the view's plan has them: in "Hidden" the published walls (the
    // working copy's new ones left out, its removed ones back); the pending and removed looks are
    // drawn over the image in the panel (red and dark red), so they stand out.
    const auto& plan = ed_.pendingPlan_;
    for (const auto& c : plan.hideWalls) {
        std::uint32_t v = 0xFF0C0C0Eu;
        if (const auto t = me::tileAt(c.x * me::kWallCellSize + 11.5f, c.y * me::kWallCellSize + 11.5f)) {
            if (const auto* tile = doc.floor.tile(t->gx, t->gy)) v = floorColour(tile->image);
        }
        px[static_cast<std::size_t>(c.y) * kN + c.x] = v;
    }
    for (const auto& w : plan.walls) {
        if (w.inBase || w.look != me::PendingLook::Normal) continue;
        px[static_cast<std::size_t>(w.wall.y) * kN + w.wall.x] = 0xFFE6E6E6u;
    }
    minimapPendingSerial_ = ed_.pendingPlanSerial_;
    if (!minimap_) {
        minimap_ = SDL_CreateTexture(ed_.ctx_.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, kN, kN);
        if (minimap_) SDL_SetTextureScaleMode(minimap_, SDL_SCALEMODE_NEAREST);
    }
    if (minimap_) {
        SDL_UpdateTexture(minimap_, nullptr, px.data(), kN * 4);
        editor::panelTextureSet(minimap_, px.data(), kN, kN, kN * 4, SDL_PIXELFORMAT_ABGR8888); // (for a floating Minimap's copy)
    }
    minimapRevision_ = ed_.history_.revision();
}

void MarkerTools::minimapPanel()
{
    ImGui::SetNextWindowSize(ImVec2(560 * ed_.imgui_->scale(), 600 * ed_.imgui_->scale()), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Minimap", &showMinimap)) {
        ImGui::End();
        return;
    }
    // Built again only when the document changed (the history's revision: every op, undo, redo and
    // a map opened bump it).
    if (!minimap_ || minimapRevision_ != ed_.history_.revision() || minimapPendingSerial_ != ed_.pendingPlanSerial_) rebuildMinimap();
    if (focus_[4] > 0 || minimapShownZoom_ != minimapZoom_) minimapCentre_ = true; // (opened or zoomed: the view's place in sight)
    minimapShownZoom_ = minimapZoom_;
    ImGui::TextUnformatted("Zoom");
    ImGui::SameLine();
    for (const int zv : {2, 3, 4}) {
        ImGui::SameLine();
        ImGui::RadioButton((std::to_string(zv) + "x").c_str(), &minimapZoom_, zv);
        help("Pixels a wall cell.");
    }
    flow("Go to Centre");
    if (ImGui::Button("Go to Centre")) {
        ed_.camera_.x = me::kWorldSize / 2;
        ed_.camera_.y = me::kWorldSize / 2;
        minimapCentre_ = true;
    }
    help("The main view to the map's centre (where the symmetry guides cross).");
    ImGui::Checkbox("Guides", &minimapGuides_);
    help("The two diagonals (the old editor's Divide1 / Divide2): the axes Spin / mirror copies across.");
    flow("WW Polygons");
    ImGui::Checkbox("Polygons", &minimapPolygons_);
    flow("WW Objects");
    ImGui::Checkbox("Objects", &minimapObjects_);
    flow("WW Waypoints");
    ImGui::Checkbox("Waypoints", &minimapWaypoints_);
    // A session's walls not yet published (the Pending changes setting's plan): counted for the key
    // under the options.
    const auto& plan = ed_.pendingPlan_;
    std::size_t pendingWalls = 0, removedWalls = 0;
    for (const auto& w : plan.walls) {
        if (w.look == me::PendingLook::Pending) ++pendingWalls;
        else if (w.look == me::PendingLook::Ghost) ++removedWalls;
    }
    if (pendingWalls + removedWalls > 0) {
        const float sw = ImGui::GetTextLineHeight() * 0.8f;
        const auto key = [&](ImU32 c, bool hatch, const std::string& text, const char* tip) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(sw, ImGui::GetTextLineHeight()));
            const ImVec2 a(at.x, at.y + (ImGui::GetTextLineHeight() - sw) / 2), b(a.x + sw, a.y + sw);
            ImDrawList* kd = ImGui::GetWindowDrawList();
            kd->AddRectFilled(a, b, c);
            if (hatch) kd->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, a.y), col(230, 90, 90), 1.5f);
            ImGui::SameLine();
            ImGui::TextUnformatted(text.c_str());
            help(tip);
        };
        if (pendingWalls > 0) {
            key(col(255, 48, 48), false, std::to_string(pendingWalls) + (pendingWalls == 1 ? " pending wall" : " pending walls"),
                "New or changed walls not published yet: only NoX Core players see them, and nothing in the game is blocked by them.");
            if (removedWalls > 0) ImGui::SameLine(0, 16);
        }
        if (removedWalls > 0) {
            key(col(120, 14, 14), true, std::to_string(removedWalls) + " removed, still in the game",
                "Walls the working copy removed: the game keeps them (they still block) until a publish.");
        }
    }
    const float zoom = static_cast<float>(minimapZoom_);
    const float side = 256 * zoom;
    ImGui::BeginChild("##mm", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (minimapCentre_) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetScrollX(std::max(0.0f, ed_.camera_.x / me::kWallCellSize * zoom - avail.x / 2));
        ImGui::SetScrollY(std::max(0.0f, ed_.camera_.y / me::kWallCellSize * zoom - avail.y / 2));
        minimapCentre_ = false;
    }
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##mmmap", ImVec2(side, side));
    const bool active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto P = [&](float wx, float wy) { return ImVec2(o.x + wx / me::kWallCellSize * zoom, o.y + wy / me::kWallCellSize * zoom); };
    if (minimap_) dl->AddImage(texId(minimap_), o, ImVec2(o.x + side, o.y + side));
    // Pending walls in a clear red, a cell's border wider so a lone one shows at 2x; removed ones
    // (still in the game) dark red with a hatch stroke.
    for (const auto& w : plan.walls) {
        if (w.look == me::PendingLook::Normal) continue;
        const ImVec2 a(o.x + w.wall.x * zoom, o.y + w.wall.y * zoom), b(a.x + zoom, a.y + zoom);
        if (w.look == me::PendingLook::Pending) {
            dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), col(255, 48, 48));
        } else {
            dl->AddRectFilled(a, b, col(120, 14, 14));
            dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, a.y), col(230, 90, 90), 1.0f);
        }
    }
    const auto& doc = ed_.doc_;
    if (minimapPolygons_) {
        for (std::size_t i = 0; i < doc.polygons.polygons.size(); ++i) {
            std::vector<ImVec2> pts;
            for (const auto& c : me::polygonCorners(doc, i)) pts.push_back(P(c.x, c.y));
            const bool sel = i < doc.polygonKeys.size() && ed_.selection_.polygons.contains(doc.polygonKeys[i]);
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), sel ? col(255, 210, 60) : col(120, 200, 255, 170), ImDrawFlags_Closed, sel ? 2.0f : 1.0f);
        }
    }
    if (minimapObjects_) {
        for (const auto& [id, ob] : doc.objects) {
            if (ob.parent == 0) dl->AddRectFilled(P(ob.header.x, ob.header.y), ImVec2(P(ob.header.x, ob.header.y).x + 1.5f, P(ob.header.x, ob.header.y).y + 1.5f), col(255, 230, 120, 200));
        }
    }
    if (minimapWaypoints_) {
        for (const auto& w : doc.waypoints.waypoints) dl->AddCircleFilled(P(w.x, w.y), 1.8f, (w.flags & 1) ? col(60, 220, 120) : col(140, 140, 140));
    }
    if (minimapGuides_) {
        // The two diagonals of the whole map, at this zoom.
        dl->AddLine(o, ImVec2(o.x + side, o.y + side), col(255, 120, 200, 170), 1.5f);
        dl->AddLine(ImVec2(o.x + side, o.y), ImVec2(o.x, o.y + side), col(255, 120, 200, 170), 1.5f);
        dl->AddCircle(ImVec2(o.x + side / 2, o.y + side / 2), 4, col(255, 120, 200, 200));
    }
    // The main view's rectangle.
    const auto& cam = ed_.camera_;
    const ImVec2 a = P(cam.left(), cam.top());
    const ImVec2 b = P(cam.left() + ed_.panelW_ / cam.zoom, cam.top() + ed_.panelH_ / cam.zoom);
    dl->AddRect(a, b, col(255, 255, 255, 230), 0, 0, 1.5f);
    if (active) {
        // A click or a drag moves the main view there.
        const ImVec2 m = ImGui::GetIO().MousePos;
        ed_.camera_.x = std::clamp((m.x - o.x) / zoom * me::kWallCellSize, 0.0f, me::kWorldSize);
        ed_.camera_.y = std::clamp((m.y - o.y) / zoom * me::kWallCellSize, 0.0f, me::kWorldSize);
    }
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        ImGui::SetTooltip("Cell %d, %d (click or drag to move the view)", static_cast<int>((m.x - o.x) / zoom), static_cast<int>((m.y - o.y) / zoom));
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- Game texts --------------------------------------------------------------------------------------

void MarkerTools::browseStrings(std::string current, std::function<void(const std::string&)> done)
{
    stringCurrent_ = std::move(current);
    stringPicked_ = std::move(done);
    showStrings = true;
    std::snprintf(stringSearch_, sizeof stringSearch_, "%s", stringCurrent_.c_str());
    stringSel_ = -1;
    stringScroll_ = true;
    if (strings_) {
        const auto& e = strings_->entries();
        for (std::size_t i = 0; i < e.size(); ++i) {
            if (lowered(e[i].label) == lowered(stringCurrent_)) stringSel_ = static_cast<int>(i);
        }
    }
}

void MarkerTools::stringsPanel()
{
    ImGui::SetNextWindowSize(ImVec2(640 * ed_.imgui_->scale(), 420 * ed_.imgui_->scale()), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Game Texts", &showStrings)) {
        ImGui::End();
        return;
    }
    if (!showStrings) stringPicked_ = nullptr;
    if (!strings_) {
        ImGui::TextWrapped("The game's string table (nox.csf) wasn't found in the game folder.");
        ImGui::End();
        return;
    }
    const auto& entries = strings_->entries();
    if (stringIndex_.size() != entries.size()) {
        stringIndex_.clear();
        for (const auto& e : entries) {
            stringIndex_.emplace_back(lowered(e.label), e.variants.empty() ? std::string() : lowered(nox::strings::toUtf8(e.variants[0].text)));
        }
    }
    if (stringPicked_) {
        ImGui::TextColored(editor::themeColours().info, "Pick a text for the field (it stores the label)%s", stringCurrent_.empty() ? "" : (": now " + stringCurrent_).c_str());
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##strsearch", "Search labels and texts", stringSearch_, sizeof stringSearch_)) stringSel_ = -1;
    const std::string q = lowered(stringSearch_);
    std::vector<int> rows;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (q.empty() || stringIndex_[i].first.find(q) != std::string::npos || stringIndex_[i].second.find(q) != std::string::npos) rows.push_back(static_cast<int>(i));
    }
    ImGui::TextDisabled("%zu of %zu texts", rows.size(), entries.size());
    const float leftW = ImGui::GetContentRegionAvail().x * 0.45f;
    ImGui::BeginChild("##strlist", ImVec2(leftW, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(rows.size()));
    if (stringScroll_ && stringSel_ >= 0) {
        const auto it = std::ranges::find(rows, stringSel_);
        if (it != rows.end()) clip.IncludeItemByIndex(static_cast<int>(it - rows.begin()));
    }
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const int i = rows[static_cast<std::size_t>(r)];
            ImGui::PushID(i);
            if (ImGui::Selectable(entries[static_cast<std::size_t>(i)].label.c_str(), stringSel_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                stringSel_ = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && stringPicked_) {
                    auto done = std::move(stringPicked_);
                    stringPicked_ = nullptr;
                    done(entries[static_cast<std::size_t>(i)].label);
                }
            }
            if (stringScroll_ && stringSel_ == i) {
                ImGui::SetScrollHereY();
                stringScroll_ = false;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##strtext", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (stringSel_ >= 0 && stringSel_ < static_cast<int>(entries.size())) {
        const auto& e = entries[static_cast<std::size_t>(stringSel_)];
        ImGui::TextUnformatted(e.label.c_str());
        if (stringPicked_) {
            if (ImGui::Button("Use")) {
                auto done = std::move(stringPicked_);
                stringPicked_ = nullptr;
                done(e.label);
            }
            help("Puts this label in the field the browser was opened from.");
            ImGui::SameLine();
        }
        if (ImGui::Button("Copy Label")) {
            SDL_SetClipboardText(e.label.c_str());
            ed_.status("Copied " + e.label);
        }
        ImGui::SameLine();
        if (!e.variants.empty() && ImGui::Button("Copy Text")) {
            SDL_SetClipboardText(nox::strings::toUtf8(e.variants[0].text).c_str());
            ed_.status("Copied the text of " + e.label);
        }
        ImGui::Separator();
        for (std::size_t v = 0; v < e.variants.size(); ++v) {
            if (e.variants.size() > 1) ImGui::TextDisabled("Variant %zu (the game picks one at random)", v + 1);
            ImGui::PushTextWrapPos();
            ImGui::TextUnformatted(nox::strings::toUtf8(e.variants[v].text).c_str());
            ImGui::PopTextWrapPos();
            if (!e.variants[v].extra.empty()) ImGui::TextDisabled("Speech: %s", e.variants[v].extra.c_str());
        }
    } else {
        ImGui::TextDisabled("Pick a text on the left.");
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- Test steps --------------------------------------------------------------------------------------

bool MarkerTools::runStep(const std::string& step)
{
    auto& doc = ed_.doc_;
    if (const auto n = numbersAfter(step, "waypoint:", 2)) {
        placeWaypoint({(*n)[0], (*n)[1]});
        return true;
    }
    if (const auto n = numbersAfter(step, "wplink:", 2)) {
        ed_.perform(me::linkWaypointsOp(doc, static_cast<std::uint32_t>((*n)[0]), static_cast<std::uint32_t>((*n)[1]), bothWays_, linkFlags_));
        return true;
    }
    if (const auto n = numbersAfter(step, "wplink2:", 2)) {
        ed_.perform(me::linkWaypointsOp(doc, static_cast<std::uint32_t>((*n)[0]), static_cast<std::uint32_t>((*n)[1]), true, linkFlags_));
        return true;
    }
    if (const auto n = numbersAfter(step, "wpunlink:", 2)) {
        ed_.perform(me::unlinkWaypointsOp(doc, static_cast<std::uint32_t>((*n)[0]), static_cast<std::uint32_t>((*n)[1]), false));
        return true;
    }
    if (const auto n = numbersAfter(step, "wpsel:", 1)) {
        std::set<std::uint32_t> ids;
        for (const float v : *n) ids.insert(static_cast<std::uint32_t>(v));
        selectWaypoints(std::move(ids), static_cast<std::uint32_t>((*n)[0]));
        return true;
    }
    if (step.starts_with("wpmode:")) {
        ed_.tool_ = MapEditor::Tool::Waypoint;
        wpMode_ = step.substr(7) == "connect" ? WpMode::Connect : WpMode::Place;
        return true;
    }
    if (step.starts_with("wpboth:")) {
        bothWays_ = step.substr(7) == "1";
        return true;
    }
    if (step.starts_with("wpsearch:")) {
        std::snprintf(wpSearch_, sizeof wpSearch_, "%s", step.substr(9).c_str());
        showWaypoints = true;
        return true;
    }
    if (step == "waypoints-open") {
        showWaypoints = true;
        return true;
    }
    if (const auto n = numbersAfter(step, "polygon:", 6)) {
        std::vector<me::WorldPoint> c;
        for (std::size_t k = 0; k + 1 < n->size(); k += 2) c.push_back({(*n)[k], (*n)[k + 1]});
        std::uint32_t key = 0;
        try {
            if (ed_.perform(me::addPolygonOp(doc, c, me::defaultPolygon(doc), &key))) selectPolygon(key);
        } catch (const std::exception& ex) {
            ed_.status(ex.what());
        }
        return true;
    }
    if (const auto n = numbersAfter(step, "poly-dump:", 1)) {
        // (Tests) the polygon's panel fields, logged.
        if (const auto i = doc.polygonIndex(static_cast<std::uint32_t>((*n)[0]))) {
            const auto& p = doc.polygons.polygons[*i];
            SDL_Log("poly-dump: %u name \"%s\" ambient %d,%d,%d group %d flags 0x%X", static_cast<unsigned>((*n)[0]), p.name.c_str(), p.ambientLight[0],
                    p.ambientLight[1], p.ambientLight[2], p.minimapGroup, p.flags);
        }
        return true;
    }
    if (const auto n = numbersAfter(step, "polysel:", 1)) {
        selectPolygon(static_cast<std::uint32_t>((*n)[0]));
        return true;
    }
    if (const auto n = numbersAfter(step, "polycorner:", 2)) {
        selectPolygon(static_cast<std::uint32_t>((*n)[0]));
        activeCorner_ = std::pair{static_cast<std::uint32_t>((*n)[0]), static_cast<std::size_t>((*n)[1])};
        return true;
    }
    if (const auto n = numbersAfter(step, "polymove:", 4)) {
        ed_.perform(me::movePolygonCornerOp(doc, static_cast<std::uint32_t>((*n)[0]), static_cast<std::size_t>((*n)[1]), {(*n)[2], (*n)[3]}));
        return true;
    }
    if (const auto n = numbersAfter(step, "polyinsert:", 4)) {
        ed_.perform(me::insertPolygonCornerOp(doc, static_cast<std::uint32_t>((*n)[0]), static_cast<std::size_t>((*n)[1]), {(*n)[2], (*n)[3]}));
        return true;
    }
    if (const auto n = numbersAfter(step, "polydraw:", 2)) {
        if (polyMode_ != PolyMode::Draw || ed_.tool_ != MapEditor::Tool::Polygon) startDrawing();
        drawing_.push_back(polySnap({(*n)[0], (*n)[1]}, std::nullopt));
        return true;
    }
    if (step.starts_with("ambientpreview:")) {
        ambientPreview_ = step.substr(15) == "1";
        return true;
    }
    if (step == "polygons-open") {
        showPolygons = true;
        return true;
    }
    if (step.starts_with("group-new:")) {
        const std::string rest = step.substr(10);
        const auto comma = rest.rfind(',');
        const std::string name = comma == std::string::npos ? rest : rest.substr(0, comma);
        const std::string k = comma == std::string::npos ? "objects" : lowered(rest.substr(comma + 1));
        const nm::Group::Kind kind = k == "waypoints" || k == "1" ? nm::Group::Kind::Waypoints
                                   : k == "walls" || k == "2"     ? nm::Group::Kind::Walls
                                   : k == "groups" || k == "3"    ? nm::Group::Kind::Unknown3
                                                                  : nm::Group::Kind::Objects;
        std::uint32_t key = 0;
        if (ed_.perform(me::addGroupOp(doc, me::toMapText(name), kind, &key))) openGroup_ = key;
        showGroups = true;
        return true;
    }
    if (step.starts_with("group-open:")) {
        const std::string name = me::toMapText(step.substr(11));
        for (std::size_t i = 0; i < doc.groups.groups.size(); ++i) {
            if (doc.groups.groups[i].name == name) openGroup_ = doc.groupKeys[i];
        }
        showGroups = true;
        return true;
    }
    if (step == "group-addsel") {
        if (openGroup_) ed_.perform(me::addGroupMembersOp(doc, *openGroup_, ed_.selection_));
        return true;
    }
    if (step == "group-pick") {
        if (openGroup_) pickMembers(*openGroup_);
        return true;
    }
    if (step == "mapinfo-open") {
        showMapInfo = true;
        return true;
    }
    if (step == "mapinfo-pictures") {
        showMapInfo = true;
        showPictures_ = true;
        return true;
    }
    if (step.starts_with("mapinfo-picfilter:")) {
        std::snprintf(pictureFilter_, sizeof pictureFilter_, "%s", step.c_str() + 18);
        return true;
    }
    if (step.starts_with("mapinfo-picall:")) {
        picturesAll_ = step.size() > 15 && step[15] == '1';
        return true;
    }
    if (step.starts_with("mapinfo-picture:")) {
        setQuestPicture(step.substr(16));
        return true;
    }
    if (const auto n = numbersAfter(step, "mapinfo-ambient:", 3)) {
        // Tests: the Map Info panel's ambient light set, as its colour field commits it.
        nm::AmbientData a = doc.ambient;
        a.r = static_cast<std::uint32_t>(std::clamp((*n)[0], 0.0f, 255.0f));
        a.g = static_cast<std::uint32_t>(std::clamp((*n)[1], 0.0f, 255.0f));
        a.b = static_cast<std::uint32_t>(std::clamp((*n)[2], 0.0f, 255.0f));
        ed_.perform(me::Op{"Map ambient light", {me::SetAmbient{a}}});
        return true;
    }
    if (const auto n = numbersAfter(step, "polyambient:", 4)) {
        // Tests: polygon `key`'s ambient light set, as its panel's colour field commits it.
        const auto key = static_cast<std::uint32_t>((*n)[0]);
        if (const auto i = doc.polygonIndex(key)) {
            nm::Polygon p = doc.polygons.polygons[*i];
            for (std::size_t c = 0; c < 3; ++c) p.ambientLight[c] = static_cast<std::uint8_t>(std::clamp((*n)[c + 1], 0.0f, 255.0f));
            ed_.perform(me::setPolygonPropsOp(doc, key, p, "Polygon ambient light"));
        }
        return true;
    }
    if (step.starts_with("mapinfo-version:")) {
        nm::MapInfo n = ed_.doc_.info;
        n.version = static_cast<std::uint16_t>(std::atoi(step.c_str() + 16));
        if (n.version != ed_.doc_.info.version) ed_.perform(me::Op{"Map info: version " + std::to_string(n.version), {me::SetMapInfo{n}}});
        return true;
    }
    if (step == "minimap-open") {
        showMinimap = true;
        return true;
    }
    if (const auto n = numbersAfter(step, "minimap-zoom:", 1)) {
        minimapZoom_ = std::clamp(static_cast<int>((*n)[0]), 2, 4);
        showMinimap = true;
        return true;
    }
    if (step.starts_with("strings-open:") || step == "strings-open") {
        std::snprintf(stringSearch_, sizeof stringSearch_, "%s", step.size() > 13 ? step.substr(13).c_str() : "");
        showStrings = true;
        stringSel_ = -1;
        if (strings_) {
            const std::string q = lowered(stringSearch_);
            const auto& e = strings_->entries();
            for (std::size_t i = 0; i < e.size() && stringSel_ < 0; ++i) {
                if (lowered(e[i].label).find(q) != std::string::npos) stringSel_ = static_cast<int>(i);
            }
        }
        return true;
    }
    return false;
}

} // namespace noxsdl
