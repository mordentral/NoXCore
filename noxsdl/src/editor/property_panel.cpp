// The map editor's object properties (property_panel.hpp).
#include "editor/property_panel.hpp"

#include "editor/editor_text.hpp"
#include "editor/held_edit.hpp"
#include "editor/map_editor.hpp"
#include "editor/marker_tools.hpp"
#include "editor/name_label.hpp"
#include "editor/object_tools.hpp"
#include "editor/script_panel.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"

#include "noxcore/mapedit/content.hpp"
#include "noxcore/mapedit/markers.hpp"
#include "noxcore/mapedit/object_edit.hpp"
#include "noxcore/mapedit/placement.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/sound_sets.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>
#include <imgui_internal.h> // (ImGuiItemFlags_MixedValue: the multi-select's mixed checkboxes)

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <sstream>
#include <unordered_map>

namespace me = nox::mapedit;
namespace xf = nox::map::xfer;

namespace noxsdl {

namespace {

using ObjId = me::ObjId;
using K = me::FieldKind;
using editor::col;
using editor::hexText;
using nox::util::lowered;

constexpr float kPi = std::numbers::pi_v<float>;

/// Help shown over a disabled field too.
void help(const std::string& text)
{
    editor::help(text, ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
}

/// A colour picker committed once, when its picking ends (held_edit.hpp's PickedColour, as
/// MarkerTools::colourField). Whether `out` is a new colour.
bool colourPick(const char* id, std::array<std::uint8_t, 3> current, std::array<std::uint8_t, 3>& out, ImGuiColorEditFlags flags)
{
    static std::unordered_map<ImGuiID, editor::PickedColour> picks;
    editor::PickedColour& p = picks[ImGui::GetID(id)];
    const bool changed = ImGui::ColorEdit3(id, p.show(current).data(), flags | ImGuiColorEditFlags_Uint8);
    const auto picked = p.after(changed, !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left), current);
    if (picked) out = *picked;
    return picked.has_value();
}

/// A checkbox showing a multi-selection's mixed value as such; a click on a mixed one sets it.
std::optional<bool> mixedCheck(const char* id, bool on, bool isMixed)
{
    bool b = on;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool clicked = ImGui::Checkbox(id, &b);
    if (isMixed) ImGui::PopItemFlag();
    if (clicked) return isMixed ? true : b;
    return std::nullopt;
}

/// A field's value with its number or its text changed (what an edit commits).
me::FieldValue withInt(me::FieldValue v, std::int64_t i)
{
    v.i = i;
    return v;
}
me::FieldValue withText(me::FieldValue v, std::string s)
{
    v.s = std::move(s);
    return v;
}

/// "Pick" beside a field naming something on the map: the next click there answers `done` (`from`:
/// a line drawn from there to the mouse; `owner`: the field asking, whose next ask replaces this).
void pickButton(MapEditor& ed, MapEditor::PickRequest::Kind kind, std::string prompt, std::optional<me::WorldPoint> from, const std::string& owner,
                std::function<void(const MapEditor::PickResult&)> done)
{
    ImGui::SameLine();
    if (ImGui::SmallButton("Pick")) {
        MapEditor::PickRequest r;
        r.kind = kind;
        r.prompt = std::move(prompt);
        r.done = std::move(done);
        r.snap = kind == MapEditor::PickRequest::Kind::Point;
        r.from = from;
        r.owner = owner;
        ed.beginPick(std::move(r));
    }
    help("Pick it on the map (right click or Escape cancels).");
}

/// `f` on an object's data when it's a monster or an NPC (their spells and enchantments are alike).
template <class Data, class F>
void ifCreature(Data& data, F&& f)
{
    std::visit(
        [&](auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, xf::Monster> || std::is_same_v<T, xf::Npc>) f(x);
        },
        data);
}

/// The creatures' 8 grid steps in the field's order (0 east, clockwise on screen).
constexpr std::array<std::array<int, 2>, 8> kSteps = {{{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};

/// A light's key animation at `frame`, as the client runs it (light_cone.hpp's keySegment).
const auto keySeg = lightcone::keySegment;

/// The light's colour now (its colour keys animated; the client steps through as many of the
/// first keys as there are non-zero ones, colorLightClientLoad).
std::array<std::uint8_t, 3> lightColourNow(const xf::InvisibleLight& l, std::uint32_t frame)
{
    std::array<std::uint8_t, 3> c{static_cast<std::uint8_t>(std::clamp(l.color[0], 0, 255)), static_cast<std::uint8_t>(std::clamp(l.color[1], 0, 255)),
                                  static_cast<std::uint8_t>(std::clamp(l.color[2], 0, 255))};
    std::size_t n = 0;
    for (const auto& k : l.colorKeys) n += (k[0] | k[1] | k[2]) != 0 ? 1 : 0;
    if (const auto s = keySeg(n, l.colorStep, (l.animFlags & 1) != 0, frame)) {
        for (std::size_t k = 0; k < 3; ++k) {
            const float a = l.colorKeys[s->from][k], b = l.colorKeys[s->to][k];
            c[k] = static_cast<std::uint8_t>(std::clamp(a + (b - a) * s->along, 0.0f, 255.0f));
        }
    }
    return c;
}

float lightIntensityNow(const xf::InvisibleLight& l, std::uint32_t frame)
{
    std::size_t n = 0;
    for (const auto k : l.intensityKeys) n += k != 0 ? 1 : 0;
    if (const auto s = keySeg(n, l.intensityStep, (l.animFlags & 4) != 0, frame)) {
        const float a = l.intensityKeys[s->from], b = l.intensityKeys[s->to];
        return a + (b - a) * s->along;
    }
    return l.intensity;
}

/// The radius the game works out from an intensity (lightRadiusFromIntensity).
float radiusOf(float intensity)
{
    const float a = std::fabs(intensity);
    return a <= 1 ? 0 : std::sqrt(((4 + std::min(a, 31.0f)) / 5 + 1) * a * a / 0.961f);
}

} // namespace

std::optional<lightcone::Cone> lightConeNow(const me::MapDocument& doc, const nox::things::ThingDb& things, const me::DocObject& o,
                                            std::uint32_t frame)
{
    const auto* l = std::get_if<xf::InvisibleLight>(&o.data);
    if (!l) return std::nullopt;
    auto a = lightcone::coneAnim(*l);
    if ((a.flags & 0x40) != 0) {
        // Its target: a static (client-loaded) object with that extent (Cli_GetStaticDrawableByNetCode).
        constexpr std::uint32_t kStatic = nox::things::ObjectClass::ClientPersist | nox::things::ObjectClass::Immobile;
        for (const auto& [id, t] : doc.objects) {
            if (t.parent != 0 || t.header.extent != a.target) continue;
            const auto* def = things.findThing(t.type);
            if (def && (nox::things::thingClasses(*def) & kStatic) != 0) {
                a.targetAt = std::array<float, 2>{t.header.x, t.header.y};
                break;
            }
        }
    }
    return lightcone::coneAt(a, o.header.x, o.header.y, frame);
}

void drawLightCone(ImDrawList* dl, const std::function<ImVec2(float, float)>& toScreen, float x, float y, float radius, lightcone::Cone cone,
                   std::uint32_t colour, bool filled)
{
    const auto k = lightcone::kite(x, y, radius, cone);
    const auto P = [&](std::size_t i) { return toScreen(k[i][0], k[i][1]); };
    // Each of the kite's halves the game fills (both, for a cone under 180 degrees each side).
    const auto lit = [&](std::size_t a, std::size_t b, std::size_t c) {
        return (k[b][0] - k[a][0]) * (k[c][1] - k[a][1]) - (k[b][1] - k[a][1]) * (k[c][0] - k[a][0]) < 0;
    };
    const bool first = lit(0, 2, 1), second = lit(0, 1, 3);
    if (filled) {
        if (first) dl->AddTriangleFilled(P(0), P(2), P(1), (colour & 0x00FFFFFFu) | 0x2D000000u);
        if (second) dl->AddTriangleFilled(P(0), P(1), P(3), (colour & 0x00FFFFFFu) | 0x2D000000u);
    }
    if (first && second) {
        const ImVec2 q[4] = {P(0), P(3), P(1), P(2)};
        dl->AddPolyline(q, 4, colour, ImDrawFlags_Closed, 1.5f);
    }
    // Where it points.
    dl->AddLine(P(0), P(1), (colour & 0x00FFFFFFu) | 0x80000000u, 1.0f);
}

PropertyPanel::PropertyPanel(MapEditor& editor)
    : ed_(editor)
{
    ctx_.things = &ed_.things_;
    ctx_.modifiers = ed_.ctx_.modifiers;
    if (!ed_.ctx_.gameDir.empty()) {
        try {
            sounds_ = std::make_unique<nox::things::SoundSets>(nox::things::loadSoundSets(ed_.ctx_.gameDir / "soundset.bin"));
            ctx_.soundSets = sounds_.get();
        } catch (const std::exception&) {
            // (no sound sets: voices are typed)
        }
    }
}

PropertyPanel::~PropertyPanel() = default;

void PropertyPanel::registerCommands()
{
    auto& c = ed_.commands_;
    c.add(editor::Command{"object.resetData", "Reset Type Data", "Tools", {},
          "The selected objects' type data back to what the game gives a new one (monster.bin's numbers...): the old editor's "
          "Load Defaults.",
          [this] {
              if (!ed_.selection_.objects.empty()) ed_.perform(me::resetTypeDataOp(ed_.doc_, ed_.objects_->factory(), ed_.selection_.objects));
          },
          [this] { return !ed_.selection_.objects.empty(); }, {}});
    c.add(editor::Command{"object.changeType", "Change Type to the Browser's", "Tools", {},
          "Changes the selected objects to the Content Browser's type, with the new type's fresh data.",
          [this] {
              const std::string t = ed_.objects_->browserType();
              if (t.empty() || ed_.selection_.objects.empty()) return;
              try {
                  ed_.perform(me::changeTypeOp(ed_.doc_, ed_.objects_->factory(), ed_.selection_.objects, t));
              } catch (const std::exception& ex) {
                  ed_.status(std::string("Change type: ") + ex.what());
              }
          },
          [this] { return !ed_.selection_.objects.empty() && !ed_.objects_->browserType().empty(); }, {}});
    c.add(editor::Command{"extents.reorder", "Reorder Extents", "Tools", {},
          "Renumbers every object 1, 2, 3... in record order, rewriting every link (teleporters, elevators, movers) and object "
          "group member to match (the old command broke groups).",
          [this] { ed_.perform(me::reorderExtentsOp(ed_.doc_, 1)); }, {}, {}});
}

std::string PropertyPanel::label(const me::DocObject& o) const
{
    std::string s = o.type + " #" + std::to_string(o.header.extent);
    if (o.header.details && !o.header.details->scriptName.empty()) s += " \"" + me::fromMapText(o.header.details->scriptName) + "\"";
    return s;
}

// --- Links into the selection ------------------------------------------------------------------------------

const me::LinkIndex& PropertyPanel::links()
{
    if (linksRevision_ != ed_.history_.revision()) {
        links_.build(ed_.doc_); // (once per change: the panel and the waypoint panel look links up)
        linksRevision_ = ed_.history_.revision();
    }
    return links_;
}

std::string PropertyPanel::linkSource(const me::Link& l) const
{
    std::string s;
    if (l.source == me::Link::Source::Group) {
        const auto gi = ed_.doc_.groupIndex(l.from);
        if (!gi) return "?";
        s = "Group \"" + me::fromMapText(ed_.doc_.groups.groups[*gi].name) + "\"";
    } else if (l.source == me::Link::Source::Waypoint) {
        const auto* w = me::findWaypoint(ed_.doc_, l.from);
        s = "Waypoint " + std::to_string(l.from);
        if (w && !w->name.empty()) s += " \"" + me::fromMapText(w->name) + "\"";
    } else {
        const auto it = ed_.doc_.objects.find(l.from);
        if (it == ed_.doc_.objects.end()) return "?";
        s = label(it->second);
        if (const auto owner = ed_.doc_.objects.find(it->second.parent); owner != ed_.doc_.objects.end()) s += " in " + label(owner->second);
    }
    return s + " (" + l.label + ")";
}

void PropertyPanel::goToSource(const me::Link& l, bool add)
{
    if (l.source == me::Link::Source::Group) {
        // Groups aren't selected on the map: the Groups panel opens on it, its members highlighted.
        ed_.markers_->openGroup(l.from);
        return;
    }
    me::Selection s;
    if (add) s = ed_.selection_;
    float x = 0, y = 0;
    if (l.source == me::Link::Source::Waypoint) {
        const auto* w = me::findWaypoint(ed_.doc_, l.from);
        if (!w) return;
        s.waypoints.insert(w->id);
        x = w->x;
        y = w->y;
    } else {
        // An item's link: its owner in the world is what's selected and shown.
        ObjId root = l.from;
        while (ed_.doc_.objects.contains(root) && ed_.doc_.objects.at(root).parent != 0) root = ed_.doc_.objects.at(root).parent;
        const auto it = ed_.doc_.objects.find(root);
        if (it == ed_.doc_.objects.end()) return;
        s.objects.insert(root);
        x = it->second.header.x;
        y = it->second.header.y;
    }
    ed_.setSelection(std::move(s));
    ed_.camera_.x = x; // (centred, the zoom kept)
    ed_.camera_.y = y;
    ed_.flash_ = MapEditor::Flash{{x, y}, SDL_GetTicks()};
}

void PropertyPanel::linkedFrom(const std::vector<me::Link>& in, const char* id)
{
    if (in.empty()) return;
    if (!section("Linked from (" + std::to_string(in.size()) + ")", id, ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("from", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("go", ImGuiTableColumnFlags_WidthFixed);
    for (std::size_t k = 0; k < in.size(); ++k) {
        const me::Link& l = in[k];
        ImGui::PushID(static_cast<int>(k));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped("%s", linkSource(l).c_str());
        const bool group = l.source == me::Link::Source::Group;
        help(group ? std::string("This one is a member of the group.") : std::string("Its \"") + l.label + "\" names this one (" + l.field + ").");
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Go to")) goToSource(l, ImGui::GetIO().KeyShift);
        help(group ? "Open the group in the Groups panel (its members highlighted on the map)."
                   : "Select it and centre the view on it (Shift: add it to the selection).");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

std::vector<std::string> PropertyPanel::typesOfClass(std::uint32_t classes) const
{
    auto it = typesByClass_.find(classes);
    if (it != typesByClass_.end()) return it->second;
    std::vector<std::string> out;
    for (const auto& e : ed_.objects_->catalog().entries) {
        if (!e.placeable) continue;
        const auto* t = ed_.things_.findThing(e.name);
        if (!t) continue;
        if (classes == 0 || (nox::things::thingClasses(*t) & classes)) out.push_back(e.name);
    }
    std::ranges::sort(out);
    return typesByClass_.emplace(classes, std::move(out)).first->second;
}

const std::vector<std::string>& PropertyPanel::carriedTypes() const
{
    if (!carried_) carried_ = me::carriedTypes(ed_.things_);
    return *carried_;
}

const std::string& PropertyPanel::displayLower(const std::string& type) const
{
    if (displayLower_.empty()) {
        for (const auto& e : ed_.objects_->catalog().entries) {
            displayLower_.emplace(e.name, lowered(e.displayName));
        }
        displayLower_.emplace(std::string(), std::string()); // (built: never empty again)
    }
    static const std::string kNone;
    const auto it = displayLower_.find(type);
    return it == displayLower_.end() ? kNone : it->second;
}

bool PropertyPanel::nameCombo(const char* id, std::string& value, const std::vector<std::string>& names, bool allowNone, bool thumbs)
{
    bool changed = false;
    const std::string preview = value.empty() ? std::string("(none)") : value;
    bool opened = false;
    if (!openCombo_.empty() && openCombo_ == id) {
        // (A test's props-combo: step: the picker opened as a click opens it, BeginCombo's popup.)
        ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID(id)));
        openCombo_.clear();
        opened = true;
    }
    if (ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            filter_[0] = 0;
            if (opened || !comboFilter_.empty()) {
                std::snprintf(filter_, sizeof filter_, "%s", comboFilter_.c_str());
                comboFilter_.clear();
            }
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "filter", filter_, sizeof filter_);
        const std::string f = lowered(filter_);
        if (allowNone && ImGui::Selectable("(none)", value.empty())) {
            value.clear();
            changed = true;
        }
        // What the filter keeps, then only the rows in sight drawn (a list of a thousand types,
        // each with its icon, costs only what shows).
        std::vector<int> shown;
        shown.reserve(names.size());
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            const std::string& n = names[static_cast<std::size_t>(i)];
            if (!f.empty()) {
                const std::string l = lowered(n);
                // (A type's game name matches too, as the Content Browser's search: "potion" finds RedPotion.)
                if (l.find(f) == std::string::npos && displayLower(n).find(f) == std::string::npos) continue;
            }
            shown.push_back(i);
        }
        if (shown.empty() && !names.empty()) ImGui::TextDisabled("Nothing matches \"%s\"", filter_);
        const float icon = 20 * ed_.imgui_->scale();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(shown.size()), thumbs ? std::max(icon, ImGui::GetTextLineHeight()) + ImGui::GetStyle().ItemSpacing.y : -1.0f);
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                const int i = shown[static_cast<std::size_t>(row)];
                const std::string& n = names[static_cast<std::size_t>(i)];
                if (!thumbs) {
                    if (editor::verbatimSelectable(i, n, n == value)) { // (a map's name can hold '#')
                        value = n;
                        changed = true;
                    }
                    continue;
                }
                // The inventory rows' mini icon, the name, the game's name greyed after it.
                ImGui::PushID(i);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", n == value, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, icon))) {
                    value = n;
                    changed = true;
                }
                ImGui::SetCursorScreenPos(at);
                ed_.objects_->drawThumbnail(n, icon);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(n.c_str());
                if (const auto* e = ed_.objects_->catalog().find(n); e && !e->displayName.empty() && e->displayName != n) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", e->displayName.c_str());
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::optional<int> PropertyPanel::intOnRelease(const char* id, int value, float width)
{
    ImGui::SetNextItemWidth(width * ed_.imgui_->scale());
    ImGui::InputInt(id, &value, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) return value;
    return std::nullopt;
}

std::optional<std::string> PropertyPanel::droppedThing(const char* label)
{
    ImGui::Button(label, ImVec2(-FLT_MIN, 0));
    std::optional<std::string> name;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NOX_THING"); p && p->DataSize == sizeof(int)) {
            const int thing = *static_cast<const int*>(p->Data);
            if (thing >= 0 && static_cast<std::size_t>(thing) < ed_.things_.things.size()) name = ed_.things_.things[static_cast<std::size_t>(thing)].name;
        }
        ImGui::EndDragDropTarget();
    }
    return name;
}

void PropertyPanel::addItemRow(const std::function<void(const std::string&)>& add)
{
    const std::string browser = ed_.objects_->browserType();
    if (!browser.empty() && ImGui::Button(("Add " + browser).c_str())) add(browser);
    help("Adds the Content Browser's selected type (or drag a thumbnail onto the list).");
    if (!browser.empty()) ImGui::SameLine();
    std::string pick;
    ImGui::SetNextItemWidth(200 * ed_.imgui_->scale());
    if (nameCombo("##additem", pick, carriedTypes(), false, true) && !pick.empty()) add(pick);
    help("Add an item by name.");
    if (const auto dropped = droppedThing("drop an item here")) add(*dropped);
}

bool PropertyPanel::section(const std::string& title, const char* id, int flags)
{
    // (The theme's section colours: on Dark grey headers, the blue kept for what's selected.)
    const editor::ThemeColours& tc = editor::themeColours();
    ImGui::PushStyleColor(ImGuiCol_Header, tc.section);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, tc.sectionHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, tc.sectionActive);
    const bool open = ImGui::CollapsingHeader((title + "###" + id).c_str(), flags);
    ImGui::PopStyleColor(3);
    if (!scrollTo_.empty() && title.starts_with(scrollTo_)) {
        ImGui::SetScrollHereY(0.0f); // (a test's step: this section at the panel's top)
        scrollTo_.clear();
    }
    return open;
}

bool PropertyPanel::edit(ObjId id, const std::string& label, const std::function<void(me::DocObject&)>& change)
{
    const auto it = ed_.doc_.objects.find(id);
    if (it == ed_.doc_.objects.end()) return false;
    me::DocObject c = it->second;
    change(c);
    try {
        return ed_.perform(me::editObjectsOp(ed_.doc_, {{id, std::move(c)}}, label));
    } catch (const std::exception& ex) {
        ed_.status(label + ": " + ex.what());
        return false;
    }
}

PropertyPanel::Apply PropertyPanel::selectionApply(const std::vector<ObjId>& ids)
{
    return [this, ids](const me::FieldDesc& f, const me::FieldValue& v, std::uint64_t mask) {
        try {
            switch (f.special) {
            case me::FieldDesc::Special::Type:
                ed_.perform(me::changeTypeOp(ed_.doc_, ed_.objects_->factory(), std::set<ObjId>(ids.begin(), ids.end()), v.s));
                return;
            case me::FieldDesc::Special::Extent:
                if (ids.size() == 1) ed_.perform(me::setExtentOp(ed_.doc_, ids.front(), static_cast<std::uint32_t>(v.i)));
                return;
            case me::FieldDesc::Special::Details: {
                me::Op op;
                op.label = v.i ? "Add header details" : "Remove header details";
                for (const ObjId id : ids) {
                    const me::Op one = me::setDetailsOp(ed_.doc_, id, v.i != 0, &ed_.objects_->factory());
                    for (const auto& p : one.parts) op.parts.push_back(p);
                }
                ed_.perform(op);
                return;
            }
            case me::FieldDesc::Special::None: break;
            }
            ed_.perform(me::setFieldOp(ed_.doc_, ids, f.id, v, ctx_, mask));
        } catch (const std::exception& ex) {
            ed_.status(f.label + ": " + ex.what());
        }
    };
}

// --- The panel -------------------------------------------------------------------------------------------

void PropertyPanel::objects(const std::set<ObjId>& set)
{
    std::vector<ObjId> ids;
    for (const ObjId id : set) {
        if (ed_.doc_.objects.contains(id)) ids.push_back(id);
    }
    if (ids.empty()) return;
    header(ids);
    const me::DocObject& first = ed_.doc_.objects.at(ids.front());
    const bool multi = ids.size() > 1;
    if (std::holds_alternative<xf::Raw>(first.data)) {
        ImGui::TextWrapped("This record is older than the game's typed layouts (xfer version 0x%X): its type data is kept as stored "
                           "bytes and can't be edited.",
                           first.xferVersion);
    }
    const auto shared = me::sharedFields(ed_.doc_, ids, ctx_);
    fieldList(shared, selectionApply(ids), first.type, multi ? std::nullopt : std::optional(me::WorldPoint{first.header.x, first.header.y}), multi,
              "sel");
    if (!multi) {
        linkedFrom(links().toObject(ed_.doc_, ids.front()), "linkedFrom");
        lists(ids.front());
        inventory(ids.front());
    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("Lists (inventory, spells, shop, templates...) edit one object at a time.");
    }
}

void PropertyPanel::header(const std::vector<ObjId>& ids)
{
    const me::DocObject& o = ed_.doc_.objects.at(ids.front());
    if (ids.size() == 1) {
        ed_.objects_->drawThumbnail(o.type, 40 * ed_.imgui_->scale());
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(label(o).c_str());
        const auto* e = ed_.objects_->catalog().find(o.type);
        ImGui::TextDisabled("%s  |  %s v0x%X  |  header v0x%X", e ? e->folder.c_str() : "?", e ? e->xfer.c_str() : "?", o.xferVersion, o.header.version);
        ImGui::EndGroup();
    } else {
        std::map<std::string, int> types;
        for (const ObjId id : ids) ++types[ed_.doc_.objects.at(id).type];
        std::string s;
        for (const auto& [t, n] : types) s += (s.empty() ? "" : ", ") + std::to_string(n) + " " + t;
        ImGui::Text("%zu objects", ids.size());
        ImGui::TextWrapped("%s", s.c_str());
        if (types.size() > 1) {
            // Keep one type of them (their own fields show when they're all one kind).
            for (const auto& [t, n] : types) {
                if (ImGui::SmallButton(("Only " + t).c_str())) {
                    me::Selection only;
                    for (const ObjId id : ids) {
                        if (ed_.doc_.objects.at(id).type == t) only.objects.insert(id);
                    }
                    ed_.setSelection(std::move(only));
                    return;
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        ImGui::TextDisabled("Fields they share; a mixed value shows \"-\". An edit applies to all as one op.");
    }
    // Records older than the current versions: the fields the newer ones add aren't stored.
    bool old = false;
    for (const ObjId id : ids) {
        const auto& x = ed_.doc_.objects.at(id);
        const auto* e = ed_.objects_->catalog().find(x.type);
        if (!std::holds_alternative<xf::Raw>(x.data) && e && (x.xferVersion < me::currentXferVersion(e->xfer) || x.header.version < 0x40)) old = true;
    }
    if (old) {
        if (ImGui::SmallButton("Bring up to date")) {
            std::vector<std::pair<ObjId, me::DocObject>> up;
            for (const ObjId id : ids) {
                const auto& x = ed_.doc_.objects.at(id);
                if (std::holds_alternative<xf::Raw>(x.data)) continue;
                try {
                    up.emplace_back(id, me::upgradedRecord(x, ed_.things_));
                } catch (const std::exception&) {
                }
            }
            ed_.perform(me::editObjectsOp(ed_.doc_, up, "Bring records up to date"));
        }
        help("The record is an older version: the fields newer versions add (shown only when stored) aren't in it. This writes it at "
             "the versions the game writes now, the new fields at their fresh values.");
    }
}

void PropertyPanel::fieldList(const std::vector<me::SharedField>& fields, const Apply& apply, const std::string& itemType,
                              std::optional<me::WorldPoint> origin, bool multi, const std::string& scope)
{
    ImGui::PushID(scope.c_str());
    // Sections in their first field's order; advanced fields under each section's "Advanced".
    std::vector<std::string> groups;
    for (const auto& f : fields) {
        if (std::ranges::find(groups, f.desc.group) == groups.end()) groups.push_back(f.desc.group);
    }
    for (const auto& g : groups) {
        const bool open = section(g, (g + scope).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (!open) continue;
        std::vector<const me::SharedField*> normal, advanced;
        for (const auto& f : fields) {
            if (f.desc.group != g) continue;
            if (multi && (f.desc.id == "header.position" || f.desc.id == "header.extent")) continue; // (one object's)
            (f.desc.advanced ? advanced : normal).push_back(&f);
        }
        const auto table = [&](const std::vector<const me::SharedField*>& list, const char* tid) {
            if (list.empty()) return;
            if (!ImGui::BeginTable(tid, 2, ImGuiTableFlags_SizingStretchProp)) return;
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 118 * ed_.imgui_->scale());
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            for (const auto* f : list) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (f->mixed) ImGui::TextColored(editor::themeColours().warning, "%s", f->desc.label.c_str());
                else if (f->desc.readOnly) ImGui::TextDisabled("%s", f->desc.label.c_str());
                else ImGui::TextUnformatted(f->desc.label.c_str());
                help(f->desc.help + (f->mixed ? "\n(The selected objects differ here.)" : ""));
                ImGui::TableNextColumn();
                ImGui::PushID(f->desc.id.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::BeginDisabled(f->desc.readOnly);
                fieldWidget(*f, apply, itemType, origin);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        };
        table(normal, ("##t" + g).c_str());
        if (!advanced.empty()) {
            ImGui::Indent();
            if (ImGui::TreeNode(("Advanced##" + g).c_str())) {
                table(advanced, ("##a" + g).c_str());
                ImGui::TreePop();
            }
            help("Fields whose meaning is partly known, or running state the game keeps (shown as stored; edit with care).");
            ImGui::Unindent();
        }
    }
    ImGui::PopID();
}

void PropertyPanel::focusIfAsked(const std::string& fieldId)
{
    if (!focusField_.empty() && focusField_ == fieldId) {
        ImGui::SetKeyboardFocusHere(); // (a test's props-focus: step)
        focusField_.clear();
    }
}

std::optional<std::string> PropertyPanel::textInput(const char* id, const me::SharedField& sf, const std::string& now, std::size_t maxBytes, bool multiline)
{
    const ImGuiID key = ImGui::GetID(id);
    std::string& buf = textEdits_[key];
    if (ImGui::GetActiveID() != key) buf = sf.mixed ? std::string() : now;
    char b[1100];
    std::snprintf(b, sizeof b, "%s", buf.c_str());
    const std::size_t cap = std::min<std::size_t>(sizeof b, maxBytes * 2 + 1);
    focusIfAsked(sf.desc.id);
    if (multiline) {
        ImGui::InputTextMultiline(id, b, cap, ImVec2(-FLT_MIN, 70 * ed_.imgui_->scale()));
    } else {
        ImGui::InputTextWithHint(id, sf.mixed ? "-" : "", b, cap);
    }
    buf = b;
    if (ImGui::IsItemDeactivatedAfterEdit()) return buf;
    return std::nullopt;
}

std::optional<double> PropertyPanel::numberInput(const char* id, const me::SharedField& sf, double now, bool integer, double lo, double hi)
{
    const ImGuiID key = ImGui::GetID(id);
    double& val = numberEdits_[key];
    // (Held: its - / + button held down, repeating. Read back each frame, the repeats were
    // lost: held for a second, it still went up by one.)
    if (ImGui::GetActiveID() != key && heldNumber_ != key) val = now;
    focusIfAsked(sf.desc.id);
    if (sf.mixed && ImGui::GetActiveID() != key) {
        char b[40] = "";
        ImGui::InputTextWithHint(id, "-", b, sizeof b, ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit() && b[0]) {
            const double typed = std::atof(b);
            return std::clamp(integer ? std::round(typed) : typed, lo, hi);
        }
        return std::nullopt;
    }
    if (integer) ImGui::InputDouble(id, &val, 1, 10, "%.0f");
    else ImGui::InputDouble(id, &val, 0, 0, "%.7g"); // (a float's digits: the text typed into is this)
    if (ImGui::IsItemActive()) heldNumber_ = key; // (the group's: the text or a button)
    else if (heldNumber_ == key) heldNumber_ = 0;
    if (ImGui::IsItemDeactivatedAfterEdit()) return std::clamp(integer ? std::round(val) : val, lo, hi);
    return std::nullopt;
}

void PropertyPanel::fieldWidget(const me::SharedField& sf, const Apply& apply, const std::string& itemType, std::optional<me::WorldPoint> origin)
{
    const me::FieldDesc& f = sf.desc;
    const me::FieldValue& v = sf.value;
    const bool mixed = sf.mixed;
    const auto commit = [&](me::FieldValue nv) { apply(f, nv, ~0ull); };

    switch (f.kind) {
    case K::Int: {
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(w);
        if (const auto n = numberInput("##v", sf, static_cast<double>(v.i), true, f.min, f.max)) commit(withInt(v, static_cast<std::int64_t>(*n)));
        break;
    }
    case K::Float: {
        if (const auto n = numberInput("##v", sf, v.f, false, f.min, f.max)) {
            me::FieldValue nv = v;
            nv.f = *n;
            commit(nv);
        }
        break;
    }
    case K::Bool:
        if (const auto b = mixedCheck("##v", v.i != 0, mixed)) commit(withInt(v, *b ? 1 : 0));
        break;
    case K::Flags: flagsField(sf, apply); break;
    case K::Enum: {
        std::string preview = mixed ? "-" : "value " + std::to_string(v.i);
        for (const auto& n : f.names) {
            if (n.value == v.i && !mixed) preview = n.name;
        }
        if (ImGui::BeginCombo("##v", preview.c_str())) {
            for (const auto& n : f.names) {
                if (ImGui::Selectable(n.name.c_str(), !mixed && n.value == v.i)) commit(withInt(v, n.value));
                help(n.help);
            }
            ImGui::EndCombo();
        }
        break;
    }
    case K::Text: {
        const std::string now = me::fromMapText(v.s);
        if (const auto t = textInput("##v", sf, now, static_cast<std::size_t>(f.max), false)) {
            bool lossy = false;
            std::string text = me::toMapText(*t, &lossy);
            if (text.size() > static_cast<std::size_t>(f.max)) {
                text.resize(static_cast<std::size_t>(f.max));
                ed_.status(f.label + ": cut to " + std::to_string(static_cast<int>(f.max)) + " bytes");
            }
            if (lossy) ed_.status(f.label + ": characters the game's text can't hold became '?'");
            commit(withText(v, text));
        }
        break;
    }
    case K::Function: {
        std::string fn = mixed ? std::string() : v.s;
        if (ed_.scriptPanel_->functionPicker(f.label.c_str(), fn)) commit(withText(v, fn));
        break;
    }
    case K::Thing: {
        // The type (header): a searchable list of the placeable types.
        if (mixed) ImGui::TextDisabled("(mixed types: a pick changes them all)");
        std::string t = v.s;
        if (nameCombo("##v", t, typesOfClass(0), false)) commit(withText(v, t));
        if (!ed_.objects_->browserType().empty() && ed_.objects_->browserType() != v.s) {
            if (ImGui::SmallButton(("Use the browser's: " + ed_.objects_->browserType()).c_str())) commit(withText(v, ed_.objects_->browserType()));
        }
        break;
    }
    case K::Spell:
    case K::Ability:
    case K::Creature:
    case K::Action:
    case K::Voice:
    case K::Modifier: {
        const auto names = me::namesFor(f.kind, ctx_, itemType, f.modifierSlot);
        std::string s2 = mixed ? std::string() : v.s;
        if (names.empty()) {
            if (const auto t = textInput("##v", sf, s2, 255, false)) commit(withText(v, *t));
        } else if (nameCombo("##v", s2, names, f.kind != K::Action)) {
            commit(withText(v, s2));
        }
        // (SPELL_INVALID is spell 0's name, what the game's save writes for none: not a stray name.)
        if (!mixed && !v.s.empty() && std::ranges::find(names, v.s) == names.end() && !names.empty() && !(f.kind == K::Spell && v.s == "SPELL_INVALID")) {
            ImGui::TextColored(editor::themeColours().unknown, "\"%s\" isn't a known name: the game ignores it", v.s.c_str());
        }
        break;
    }
    case K::MapName: {
        if (ed_.mapList_.empty()) ed_.refreshMapList();
        std::vector<std::string> names;
        for (const auto& m : ed_.mapList_) {
            if (std::ranges::find(names, m.name) == names.end()) names.push_back(m.name);
        }
        const std::string now = v.s.substr(0, v.s.find('\0'));
        std::string m = mixed ? std::string() : now;
        if (nameCombo("##v", m, names)) commit(withText(v, m));
        if (const auto t = textInput("##typed", sf, now, static_cast<std::size_t>(f.max), false)) {
            commit(withText(v, me::toMapText(*t).substr(0, static_cast<std::size_t>(f.max))));
        }
        help("Or type a map's name (one in the maps folder; the game's buffer holds 79 bytes).");
        break;
    }
    case K::StringKey: stringKeyField(sf, apply); break;
    case K::Color: {
        if (me::FieldValue nv = v; colourPick("##v", v.rgb, nv.rgb, ImGuiColorEditFlags_DisplayRGB)) {
            commit(nv);
        }
        if (mixed) ImGui::TextDisabled("(mixed)");
        break;
    }
    case K::Extent: extentField(sf, apply, origin); break;
    case K::Waypoint: waypointField(sf, apply, origin); break;
    case K::Point: pointField(sf, apply, origin); break;
    case K::Angle: angleField(sf, apply); break;
    case K::Direction: directionField(sf, apply); break;
    }
}

void PropertyPanel::flagsField(const me::SharedField& sf, const Apply& apply)
{
    const me::FieldDesc& f = sf.desc;
    const auto bits = static_cast<std::uint64_t>(sf.value.i);
    std::uint64_t named = 0;
    const bool split = f.id == "header.flags"; // (the authoring bits apart from the runtime-only ones)
    bool inTable = ImGui::BeginTable("##bits", 2, ImGuiTableFlags_SizingStretchSame);
    for (std::size_t i = 0; i < f.names.size(); ++i) {
        const auto& n = f.names[i];
        named |= static_cast<std::uint64_t>(n.value);
        if (split && i == 8) {
            if (inTable) ImGui::EndTable();
            inTable = false;
            ImGui::Checkbox("Show runtime-only bits", &showRuntimeFlags_);
            help("Bits the game sets while it plays and never saves. A map that stores one has it set on load (the load ORs the "
                 "stored word in).");
            if (!showRuntimeFlags_) break;
            inTable = ImGui::BeginTable("##bits2", 2, ImGuiTableFlags_SizingStretchSame);
        }
        const auto bit = static_cast<std::uint64_t>(n.value);
        if (inTable) ImGui::TableNextColumn();
        const bool isMixed = (sf.mixedBits & bit) != 0;
        ImGui::PushID(static_cast<int>(i));
        if (const auto b = mixedCheck(n.name.c_str(), (bits & bit) != 0, isMixed)) {
            apply(f, withInt(sf.value, static_cast<std::int64_t>(*b ? (bits | bit) : (bits & ~bit))), bit);
        }
        help(n.help + "  (" + hexText(bit) + ")");
        ImGui::PopID();
    }
    if (inTable) ImGui::EndTable();
    if (split && !showRuntimeFlags_) {
        for (const auto& n : f.names) named |= static_cast<std::uint64_t>(n.value);
    }
    const std::uint64_t other = bits & ~named;
    ImGui::TextDisabled("%s%s", hexText(bits).c_str(), other ? ("  (unnamed bits " + hexText(other) + " kept)").c_str() : "");
    if (split && !showRuntimeFlags_) {
        const std::uint64_t runtime = bits & ~static_cast<std::uint64_t>(me::kSavedObjectFlags);
        if (runtime) {
            ImGui::SameLine();
            ImGui::TextColored(editor::themeColours().unknown, "runtime bits set: %s", hexText(runtime).c_str());
        }
    }
}

void PropertyPanel::stringKeyField(const me::SharedField& sf, const Apply& apply)
{
    const me::FieldDesc& f = sf.desc;
    const me::FieldValue& v = sf.value;
    if (const auto t = textInput("##v", sf, v.s, static_cast<std::size_t>(f.max), false)) apply(f, withText(v, *t), ~0ull);
    ImGui::SameLine();
    if (ImGui::SmallButton("...")) {
        ed_.markers_->browseStrings(v.s, [a = apply, copy = f, base = v](const std::string& key) { a(copy, withText(base, key), ~0ull); });
    }
    help("Browse the game's strings (nox.csf).");
    // The text it shows, from the string table.
    if (!sf.mixed && ed_.markers_->strings()) {
        const std::string key = v.s.substr(0, v.s.find('\0'));
        if (const auto text = ed_.markers_->strings()->text(key)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(editor::themeColours().note, "%s", nox::strings::toUtf8(*text).c_str());
            ImGui::PopTextWrapPos();
        } else if (!key.empty()) {
            ImGui::TextDisabled("(no such key: the game shows the key itself)");
        }
    }
}

void PropertyPanel::extentField(const me::SharedField& sf, const Apply& apply, std::optional<me::WorldPoint> origin)
{
    const me::FieldDesc& f = sf.desc;
    const me::FieldValue& v = sf.value;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110 * ed_.imgui_->scale());
    if (const auto n = numberInput("##v", sf, static_cast<double>(v.i), true, f.min, f.max)) apply(f, withInt(v, static_cast<std::int64_t>(*n)), ~0ull);
    pickButton(ed_, MapEditor::PickRequest::Kind::Object, "Click the object for " + f.label, origin, f.id,
               [this, a = apply, copy = f, base = v](const MapEditor::PickResult& r) {
                   if (r.object) a(copy, withInt(base, ed_.doc_.objects.at(*r.object).header.extent), ~0ull);
               });
    ImGui::SameLine();
    if (ImGui::SmallButton("Go") && v.i != 0) {
        const auto ids = me::objectsWithExtent(ed_.doc_, static_cast<std::uint32_t>(v.i));
        if (!ids.empty()) {
            const auto& h = ed_.doc_.objects.at(ids.front()).header;
            ed_.frameWorld(h.x, h.y, h.x, h.y);
        }
    }
    if (!sf.mixed && v.i != 0) {
        const auto ids = me::objectsWithExtent(ed_.doc_, static_cast<std::uint32_t>(v.i));
        if (ids.empty()) ImGui::TextColored(editor::themeColours().error, "no object has extent %lld", static_cast<long long>(v.i));
        else ImGui::TextDisabled("%s", label(ed_.doc_.objects.at(ids.front())).c_str());
    }
}

void PropertyPanel::waypointField(const me::SharedField& sf, const Apply& apply, std::optional<me::WorldPoint> origin)
{
    const me::FieldDesc& f = sf.desc;
    const me::FieldValue& v = sf.value;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110 * ed_.imgui_->scale());
    if (const auto n = numberInput("##v", sf, static_cast<double>(v.i), true, f.min, f.max)) apply(f, withInt(v, static_cast<std::int64_t>(*n)), ~0ull);
    pickButton(ed_, MapEditor::PickRequest::Kind::Waypoint, "Click the waypoint for " + f.label, origin, f.id,
               [a = apply, copy = f, base = v](const MapEditor::PickResult& r) {
                   if (r.waypoint) a(copy, withInt(base, *r.waypoint), ~0ull);
               });
    ImGui::SameLine();
    if (ImGui::SmallButton("Go") && v.i != 0) {
        if (const auto* w = me::findWaypoint(ed_.doc_, static_cast<std::uint32_t>(v.i))) ed_.frameWorld(w->x, w->y, w->x, w->y);
    }
    if (!sf.mixed && v.i != 0) {
        const auto* w = me::findWaypoint(ed_.doc_, static_cast<std::uint32_t>(v.i));
        if (!w) ImGui::TextColored(editor::themeColours().error, "no waypoint %lld", static_cast<long long>(v.i));
        else if (!w->name.empty()) ImGui::TextDisabled("\"%s\"", w->name.c_str());
    }
}

void PropertyPanel::pointField(const me::SharedField& sf, const Apply& apply, std::optional<me::WorldPoint> origin)
{
    const me::FieldDesc& f = sf.desc;
    const me::FieldValue& v = sf.value;
    float p[2] = {static_cast<float>(v.f), static_cast<float>(v.g)};
    const ImGuiID key = ImGui::GetID("##v");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80 * ed_.imgui_->scale());
    static float editing[2];
    if (ImGui::GetActiveID() == key || ImGui::GetActiveID() == key + 1) {
        p[0] = editing[0];
        p[1] = editing[1];
    }
    ImGui::InputFloat2("##v", p, "%.1f");
    editing[0] = p[0];
    editing[1] = p[1];
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        me::FieldValue nv = v;
        nv.f = std::clamp(static_cast<double>(p[0]), 0.0, static_cast<double>(me::kWorldSize - 1));
        nv.g = std::clamp(static_cast<double>(p[1]), 0.0, static_cast<double>(me::kWorldSize - 1));
        apply(f, nv, ~0ull);
    }
    pickButton(ed_, MapEditor::PickRequest::Kind::Point, "Click the point for " + f.label, origin, f.id,
               [a = apply, copy = f, base = v](const MapEditor::PickResult& r) {
                   me::FieldValue nv = base;
                   nv.f = r.at.x;
                   nv.g = r.at.y;
                   a(copy, nv, ~0ull);
               });
    ImGui::SameLine();
    if (ImGui::SmallButton("Go")) ed_.frameWorld(static_cast<float>(v.f), static_cast<float>(v.g), static_cast<float>(v.f), static_cast<float>(v.g));
}

void PropertyPanel::angleField(const me::SharedField& sf, const Apply& apply)
{
    const me::FieldDesc& f = sf.desc;
    const me::FieldValue& v = sf.value;
    const float s = ed_.imgui_->scale();
    // A dial: drag the needle (5-degree steps with Shift off... Ctrl for free), or type degrees.
    const float r = 26 * s;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##dial", ImVec2(r * 2 + 4, r * 2 + 4));
    const ImVec2 c(at.x + r + 2, at.y + r + 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r, col(30, 30, 36));
    dl->AddCircle(c, r, col(140, 140, 150), 0, 1.5f);
    for (int k = 0; k < 8; ++k) {
        const float a = static_cast<float>(k) * kPi / 4;
        dl->AddLine(ImVec2(c.x + std::cos(a) * (r - 4), c.y + std::sin(a) * (r - 4)), ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), col(140, 140, 150));
    }
    float angle = static_cast<float>(v.f);
    static float dragging = 0;
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        float a = std::atan2(m.y - c.y, m.x - c.x);
        if (!ImGui::GetIO().KeyCtrl) a = std::round(a / (kPi / 36)) * (kPi / 36);
        dragging = a < 0 ? a + 2 * kPi : a;
        angle = dragging;
    }
    if (ImGui::IsItemDeactivated()) {
        me::FieldValue nv = v;
        nv.f = dragging;
        apply(f, nv, ~0ull);
    }
    if (!sf.mixed) dl->AddLine(c, ImVec2(c.x + std::cos(angle) * (r - 2), c.y + std::sin(angle) * (r - 2)), col(255, 210, 90), 2.5f);
    help("Drag the needle (5-degree steps; Ctrl: free). 0 points east, a quarter turn south (the world's y grows down).");
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(90 * s);
    if (const auto n = numberInput("##deg", sf, v.f * 180.0 / std::numbers::pi, false, f.min * 180.0 / std::numbers::pi, f.max * 180.0 / std::numbers::pi)) {
        // (Degrees typed: the range in degrees. Clamped to the field's radians, 90 became 6.28.)
        me::FieldValue nv = v;
        nv.f = *n * std::numbers::pi / 180.0;
        apply(f, nv, ~0ull);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("degrees");
    ImGui::TextDisabled("%.4f rad", v.f);
    ImGui::EndGroup();
}

void PropertyPanel::directionField(const me::SharedField& sf, const Apply& apply)
{
    // The 8 steps as a 3 x 3 grid of buttons (the middle: none).
    const float b = 24 * ed_.imgui_->scale();
    for (int gy = -1; gy <= 1; ++gy) {
        for (int gx = -1; gx <= 1; ++gx) {
            if (gx != -1) ImGui::SameLine(0, 2);
            if (gx == 0 && gy == 0) {
                ImGui::Dummy(ImVec2(b, b));
                continue;
            }
            int idx = 0;
            for (int k = 0; k < 8; ++k) {
                if (kSteps[static_cast<std::size_t>(k)][0] == gx && kSteps[static_cast<std::size_t>(k)][1] == gy) idx = k;
            }
            static const char* kArrows[8] = {"E", "SE", "S", "SW", "W", "NW", "N", "NE"};
            const bool on = !sf.mixed && sf.value.i == idx;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            ImGui::PushID(idx);
            if (ImGui::Button(kArrows[idx], ImVec2(b, b))) apply(sf.desc, withInt(sf.value, idx), ~0ull);
            ImGui::PopID();
            if (on) ImGui::PopStyleColor();
        }
    }
}

// --- Lists ---------------------------------------------------------------------------------------------------

void PropertyPanel::lists(ObjId id)
{
    const me::DocObject& o = ed_.doc_.objects.at(id);
    linkTools(id);
    if (std::holds_alternative<xf::Monster>(o.data) || std::holds_alternative<xf::Npc>(o.data)) {
        monsterSpells(id);
        buffs(id);
        if (const auto* m = std::get_if<xf::Monster>(&o.data); m && m->shop) shop(id);
    } else if (std::holds_alternative<xf::Glyph>(o.data)) {
        glyphSpells(id);
    } else if (std::holds_alternative<xf::RewardMarker>(o.data)) {
        rewardLists(id);
    } else if (std::holds_alternative<xf::MonsterGenerator>(o.data)) {
        generator(id);
    } else if (std::holds_alternative<xf::InvisibleLight>(o.data)) {
        lightKeys(id);
    }
}

void PropertyPanel::linkTools(ObjId id)
{
    const me::DocObject& o = ed_.doc_.objects.at(id);
    // Two-way linking: a teleporter's partner, an elevator's shaft (and back).
    const bool linkable = std::holds_alternative<xf::Transporter>(o.data) || std::holds_alternative<xf::Elevator>(o.data) ||
                          std::holds_alternative<xf::ElevatorShaft>(o.data);
    if (!linkable) return;
    ImGui::SeparatorText("Link");
    ImGui::Checkbox("Link both ways", &twoWay_);
    help("Picking the partner also links it back to this one (a teleporter pair, an elevator and its shaft).");
    ImGui::SameLine();
    if (ImGui::Button("Pick the partner...")) {
        MapEditor::PickRequest r;
        r.kind = MapEditor::PickRequest::Kind::Object;
        r.prompt = std::holds_alternative<xf::Transporter>(o.data) ? "Click the teleporter it leads to" : std::holds_alternative<xf::Elevator>(o.data)
                                                                                                             ? "Click its elevator shaft"
                                                                                                             : "Click its elevator";
        r.from = me::WorldPoint{o.header.x, o.header.y};
        r.owner = "link";
        r.done = [this, id](const MapEditor::PickResult& p) {
            if (!p.object || *p.object == id) return;
            const auto it = ed_.doc_.objects.find(id);
            const auto pt = ed_.doc_.objects.find(*p.object);
            if (it == ed_.doc_.objects.end() || pt == ed_.doc_.objects.end()) return;
            me::DocObject a = it->second, b = pt->second;
            const auto setLink = [](me::DocObject& x, std::uint32_t extent) {
                if (auto* t = std::get_if<xf::Transporter>(&x.data)) t->targetExtent = extent;
                else if (auto* e = std::get_if<xf::Elevator>(&x.data)) e->linkedExtent = extent;
                else if (auto* sh = std::get_if<xf::ElevatorShaft>(&x.data)) sh->linkedExtent = extent;
                else return false;
                return true;
            };
            if (!setLink(a, b.header.extent)) return;
            std::vector<std::pair<ObjId, me::DocObject>> changed{{id, a}};
            if (twoWay_ && setLink(b, a.header.extent)) changed.emplace_back(*p.object, b);
            ed_.perform(me::editObjectsOp(ed_.doc_, changed, twoWay_ ? "Link both ways" : "Link"));
        };
        ed_.beginPick(std::move(r));
    }
}

void PropertyPanel::monsterSpells(ObjId id)
{
    const me::DocObject& o = ed_.doc_.objects.at(id);
    const std::vector<xf::MonsterSpell>* spells = nullptr;
    ifCreature(o.data, [&](const auto& x) { spells = &x.spells; });
    if (!spells) return;
    if (!section("Spells (" + std::to_string(spells->size()) + ")", "spells", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const auto mutate = [&](const std::string& lbl, const std::function<void(std::vector<xf::MonsterSpell>&)>& f) {
        edit(id, lbl, [&](me::DocObject& c) { ifCreature(c.data, [&](auto& x) { f(x.spells); }); });
    };
    if (const auto* m = std::get_if<xf::Monster>(&o.data); m && m->unknown337[3]) {
        ImGui::TextColored(editor::themeColours().warning, "\"Spells from its type\" is ticked: the game uses its type's spells, not these.");
    }
    const auto names = me::namesFor(K::Spell, ctx_);
    const auto& bits = me::spellUsageBits();
    if (ImGui::BeginTable("##spells", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Spell", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Used for", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22 * ed_.imgui_->scale());
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < spells->size(); ++i) {
            const auto sp = (*spells)[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            std::string n = sp.name;
            if (nameCombo("##name", n, names, false)) mutate("Set spell", [&](auto& v) { v[i].name = n; });
            ImGui::TableNextColumn();
            for (std::size_t b = 0; b < bits.size(); ++b) {
                if (b) ImGui::SameLine();
                bool on = (sp.flags & bits[b].bit) != 0;
                ImGui::PushID(static_cast<int>(b));
                if (ImGui::Checkbox(bits[b].name, &on)) mutate("Set spell use", [&](auto& v) { v[i].flags = on ? (v[i].flags | bits[b].bit) : (v[i].flags & ~bits[b].bit); });
                help(bits[b].help);
                ImGui::PopID();
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) mutate("Remove spell", [&](auto& v) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(i)); });
            help("Remove it.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    std::string add;
    ImGui::SetNextItemWidth(220 * ed_.imgui_->scale());
    if (nameCombo("##addspell", add, names, false) && !add.empty()) mutate("Add spell " + add, [&](auto& v) { v.push_back({add, 0x20000000u}); });
    ImGui::SameLine();
    ImGui::TextDisabled("add a spell (used for Attack)");
}

void PropertyPanel::buffs(ObjId id)
{
    const me::DocObject& o = ed_.doc_.objects.at(id);
    const xf::Buffs* bf = nullptr;
    ifCreature(o.data, [&](const auto& x) { bf = &x.buffs; });
    if (!bf) return;
    const bool stored = std::holds_alternative<xf::Monster>(o.data) ? o.xferVersion > 0x3D : o.xferVersion > 0x3C;
    if (!stored) return;
    if (!section("Enchantments (" + std::to_string(bf->buffs.size()) + ")", "buffs", 0)) return;
    ImGui::TextDisabled("Enchantments it starts with (the game puts them on at the load's end).");
    const auto mutate = [&](const std::string& lbl, const std::function<void(xf::Buffs&)>& f) {
        edit(id, lbl, [&](me::DocObject& c) { ifCreature(c.data, [&](auto& x) { f(x.buffs); }); });
    };
    const auto names = me::enchantNames();
    for (std::size_t i = 0; i < bf->buffs.size(); ++i) {
        const auto b = bf->buffs[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(170 * ed_.imgui_->scale());
        std::string n = b.enchantment;
        if (nameCombo("##e", n, names, false)) mutate("Set enchantment", [&](auto& v) { v.buffs[i].enchantment = n; });
        ImGui::SameLine();
        const auto power = intOnRelease("##p", b.power, 60);
        help("Its level.");
        if (power) mutate("Set enchantment level", [&](auto& v) { v.buffs[i].power = static_cast<std::uint8_t>(std::clamp(*power, 0, 255)); });
        ImGui::SameLine();
        const auto timer = intOnRelease("##t", static_cast<int>(b.timer), 80);
        help("Frames it lasts (30 a second; 0 for ever).");
        if (timer) mutate("Set enchantment time", [&](auto& v) { v.buffs[i].timer = static_cast<std::uint32_t>(std::max(*timer, 0)); });
        if (b.enchantment == "ENCHANT_SHIELD" && bf->version >= 2) {
            ImGui::SameLine();
            const auto hp = intOnRelease("##h", static_cast<int>(b.shieldHealth), 70);
            help("The shield's health.");
            if (hp) mutate("Set shield health", [&](auto& v) { v.buffs[i].shieldHealth = static_cast<std::uint32_t>(std::max(*hp, 0)); });
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) mutate("Remove enchantment", [&](auto& v) { v.buffs.erase(v.buffs.begin() + static_cast<std::ptrdiff_t>(i)); });
        ImGui::PopID();
    }
    std::string add;
    ImGui::SetNextItemWidth(200 * ed_.imgui_->scale());
    if (nameCombo("##addbuff", add, names, false) && !add.empty()) mutate("Add enchantment " + add, [&](auto& v) { v.buffs.push_back({add, 1, 0, 0}); });
    ImGui::SameLine();
    ImGui::TextDisabled("add");
}

void PropertyPanel::shop(ObjId id)
{
    const auto& m = std::get<xf::Monster>(ed_.doc_.objects.at(id).data);
    if (!m.shop) return;
    const auto& items = m.shop->items;
    if (!section("Shop items (" + std::to_string(items.size()) + ")", "shop", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const auto mutate = [&](const std::string& lbl, const std::function<void(std::vector<xf::Shop::Item>&)>& f) {
        edit(id, lbl, [&](me::DocObject& c) {
            auto& x = std::get<xf::Monster>(c.data);
            if (x.shop) f(x.shop->items);
        });
    };
    const auto& pickups = carriedTypes();
    const float s = ed_.imgui_->scale();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto it = items[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ed_.objects_->drawThumbnail(it.type, 24 * s); // (the inventory rows' mini icon)
        ImGui::SameLine();
        const auto count = intOnRelease("##n", it.count, 46);
        help("How many it has (1-255).");
        if (count) mutate("Set shop count", [&](auto& v) { v[i].count = static_cast<std::uint8_t>(std::clamp(*count, 1, 255)); });
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170 * s);
        std::string type = it.type;
        if (nameCombo("##type", type, pickups, false, true)) {
            mutate("Set shop item", [&](auto& v) {
                v[i].type = type;
                v[i].parameter.clear();
                v[i].modifiers = {};
            });
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) mutate("Move shop item", [&](auto& v) { std::swap(v[i], v[i - 1]); });
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < items.size()) mutate("Move shop item", [&](auto& v) { std::swap(v[i], v[i + 1]); });
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) mutate("Remove shop item", [&](auto& v) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(i)); });
        // A book's spell, ability or creature; an item's modifiers.
        const auto* def = ed_.things_.findThing(it.type);
        const std::string xfer = def ? std::string(def->lastAttribute("XFER").value_or("")) : std::string();
        const K pk = xfer == "SpellRewardXfer" ? K::Spell : xfer == "AbilityRewardXfer" ? K::Ability : xfer == "FieldGuideXfer" ? K::Creature : K::Int;
        if (pk != K::Int) {
            ImGui::Indent();
            ImGui::SetNextItemWidth(200 * s);
            std::string p = it.parameter;
            if (nameCombo("##param", p, me::namesFor(pk, ctx_))) mutate("Set shop book", [&](auto& v) { v[i].parameter = p; });
            ImGui::Unindent();
        }
        if (xfer == "WeaponXfer" || xfer == "ArmorXfer" || xfer == "AmmoXfer") {
            ImGui::Indent();
            static const char* kSlots[4] = {"Effectiveness", "Material", "Enchantment 1", "Enchantment 2"};
            for (int k = 0; k < 4; ++k) {
                ImGui::PushID(k);
                ImGui::SetNextItemWidth(150 * s);
                std::string mod = it.modifiers[static_cast<std::size_t>(k)];
                if (nameCombo("##mod", mod, me::namesFor(K::Modifier, ctx_, it.type, k))) mutate("Set shop item modifier", [&](auto& v) { v[i].modifiers[static_cast<std::size_t>(k)] = mod; });
                help(kSlots[k]);
                if (k % 2 == 0) ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (items.size() < 255) {
        std::string add = ed_.objects_->browserType();
        const bool canBrowser = std::ranges::find(pickups, add) != pickups.end();
        if (canBrowser && ImGui::Button(("Add " + add).c_str())) mutate("Add shop item " + add, [&](auto& v) { v.push_back({1, add, {}, {}}); });
        if (canBrowser) ImGui::SameLine();
        std::string pick;
        ImGui::SetNextItemWidth(200 * s);
        if (nameCombo("##addshop", pick, pickups, false, true) && !pick.empty()) mutate("Add shop item " + pick, [&](auto& v) { v.push_back({1, pick, {}, {}}); });
        help("Add an item to what it sells.");
    }
}

void PropertyPanel::glyphSpells(ObjId id)
{
    const auto& g = std::get<xf::Glyph>(ed_.doc_.objects.at(id).data);
    if (!section("Glyph spells (" + std::to_string(g.spells.size()) + ")", "glyph", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const auto mutate = [&](const std::string& lbl, const std::function<void(std::vector<std::string>&)>& f) {
        edit(id, lbl, [&](me::DocObject& c) { f(std::get<xf::Glyph>(c.data).spells); });
    };
    const auto names = me::namesFor(K::Spell, ctx_);
    for (std::size_t i = 0; i < g.spells.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        std::string n = g.spells[i];
        ImGui::SetNextItemWidth(220 * ed_.imgui_->scale());
        if (nameCombo("##s", n, names, false)) mutate("Set glyph spell", [&](auto& v) { v[i] = n; });
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) mutate("Remove glyph spell", [&](auto& v) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(i)); });
        ImGui::PopID();
    }
    if (g.spells.size() > 5) {
        ImGui::TextColored(editor::themeColours().error, "The game's glyph holds 5 spells: more run over its other data.");
    }
    std::string add;
    ImGui::SetNextItemWidth(220 * ed_.imgui_->scale());
    if (g.spells.size() < 5 && nameCombo("##add", add, names, false) && !add.empty()) mutate("Add glyph spell " + add, [&](auto& v) { v.push_back(add); });
}

void PropertyPanel::rewardLists(ObjId id)
{
    const auto& r = std::get<xf::RewardMarker>(ed_.doc_.objects.at(id).data);
    if (!section("Reward lists", "rlists", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextDisabled("Books come from these when \"Use the map's lists\" ticks their kind.");
    const auto list = [&](const char* title, const std::vector<std::string>& v, K kind, std::vector<std::string> xf::RewardMarker::*m) {
        ImGui::PushID(title);
        ImGui::TextUnformatted(title);
        const auto names = me::namesFor(kind, ctx_);
        for (std::size_t i = 0; i < v.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Bullet();
            ImGui::TextUnformatted(v[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                edit(id, std::string("Remove from ") + title, [&](me::DocObject& c) {
                    auto& l = std::get<xf::RewardMarker>(c.data).*m;
                    l.erase(l.begin() + static_cast<std::ptrdiff_t>(i));
                });
            }
            ImGui::PopID();
        }
        std::string add;
        ImGui::SetNextItemWidth(200 * ed_.imgui_->scale());
        if (nameCombo("##add", add, names, false) && !add.empty() && std::ranges::find(v, add) == v.end()) {
            edit(id, std::string("Add to ") + title, [&](me::DocObject& c) { (std::get<xf::RewardMarker>(c.data).*m).push_back(add); });
        }
        ImGui::PopID();
    };
    list("Spells", r.spells, K::Spell, &xf::RewardMarker::spells);
    list("Abilities", r.abilities, K::Ability, &xf::RewardMarker::abilities);
    list("Field guides", r.creatures, K::Creature, &xf::RewardMarker::creatures);
}

void PropertyPanel::generator(ObjId id)
{
    const auto& g = std::get<xf::MonsterGenerator>(ed_.doc_.objects.at(id).data);
    if (!section("Monsters", "gen", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("Up to 4 monsters a variant, one picked at random each spawn. Outside quests the game uses variant 1; a quest "
                       "picks a variant at random each time the map loads.");
    const auto monsters = typesOfClass(nox::things::ObjectClass::Monster);
    const auto mutate = [&](const std::string& lbl, const std::function<void(xf::MonsterGenerator&)>& f) {
        edit(id, lbl, [&](me::DocObject& c) {
            auto& x = std::get<xf::MonsterGenerator>(c.data);
            f(x);
        });
    };
    if (ImGui::BeginTabBar("##variants")) {
        for (int k = 0; k < 3; ++k) {
            const std::size_t kk = static_cast<std::size_t>(k);
            const std::size_t n = kk < g.groups.size() ? g.groups[kk].size() : 0;
            if (!ImGui::BeginTabItem(("Variant " + std::to_string(k + 1) + " (" + std::to_string(n) + ")").c_str())) continue;
            for (std::size_t i = 0; i < n; ++i) {
                const auto& t = g.groups[kk][i];
                ImGui::PushID(static_cast<int>(i));
                ed_.objects_->drawThumbnail(t.type, 28 * ed_.imgui_->scale());
                ImGui::SameLine();
                std::string type = t.type;
                ImGui::SetNextItemWidth(170 * ed_.imgui_->scale());
                if (nameCombo("##type", type, monsters, false) && type != t.type) {
                    // (A slot whose type changes starts over with the new type's fresh monster.)
                    try {
                        auto fresh = me::freshTemplate(ed_.objects_->factory(), type);
                        mutate("Set generator monster to " + type, [&](auto& x) { x.groups[kk][i] = fresh; });
                    } catch (const std::exception& ex) {
                        ed_.status(ex.what());
                    }
                }
                ImGui::SameLine();
                const bool open = templatesOpen_.contains({k, static_cast<int>(i)});
                if (ImGui::SmallButton(open ? "Hide" : "Fields")) {
                    if (open) templatesOpen_.erase({k, static_cast<int>(i)});
                    else templatesOpen_.insert({k, static_cast<int>(i)});
                }
                help("Its monster's own fields, here (the template the generator copies).");
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) mutate("Remove generator monster", [&](auto& x) { x.groups[kk].erase(x.groups[kk].begin() + static_cast<std::ptrdiff_t>(i)); });
                if (open) {
                    if (const auto obj = me::templateObject(t, ed_.things_)) {
                        me::MapDocument one; // (the template alone, for its shared fields)
                        one.objects.emplace(1, *obj);
                        auto fields = me::sharedFields(one, {1}, ctx_);
                        std::erase_if(fields, [](const me::SharedField& f) { return f.desc.group == "Header"; });
                        const Apply apply = [this, id, kk, i](const me::FieldDesc& f, const me::FieldValue& v, std::uint64_t mask) {
                            const auto& gen = std::get<xf::MonsterGenerator>(ed_.doc_.objects.at(id).data);
                            if (kk >= gen.groups.size() || i >= gen.groups[kk].size()) return;
                            const auto keep = gen.groups[kk][i];
                            auto obj2 = me::templateObject(keep, ed_.things_);
                            if (!obj2) return;
                            me::FieldValue nv = v;
                            if (f.kind == K::Flags) {
                                const auto now = static_cast<std::uint64_t>(f.get(*obj2).i);
                                nv.i = static_cast<std::int64_t>((now & ~mask) | (static_cast<std::uint64_t>(v.i) & mask));
                            }
                            f.set(*obj2, nv);
                            const auto t2 = me::makeTemplate(*obj2, ed_.things_, &keep);
                            edit(id, "Set generator monster's " + f.label, [&](me::DocObject& c) { std::get<xf::MonsterGenerator>(c.data).groups[kk][i] = t2; });
                        };
                        ImGui::Indent();
                        templateInventory(id, kk, i); // (what it carries first: the fields list is long)
                        ImGui::SeparatorText("Fields");
                        fieldList(fields, apply, t.type, std::nullopt, false, "tpl" + std::to_string(k) + "_" + std::to_string(i));
                        ImGui::Unindent();
                    } else {
                        ImGui::TextDisabled("(its record can't be read)");
                    }
                }
                ImGui::PopID();
            }
            if (n < 4) {
                std::string add = ed_.objects_->browserType();
                const bool canBrowser = std::ranges::find(monsters, add) != monsters.end();
                const auto addMonster = [&](const std::string& type) {
                    try {
                        auto fresh = me::freshTemplate(ed_.objects_->factory(), type);
                        mutate("Add generator monster " + type, [&](auto& x) {
                            if (x.groups.size() < 3) x.groups.resize(3);
                            x.groups[kk].push_back(fresh);
                        });
                    } catch (const std::exception& ex) {
                        ed_.status(ex.what());
                    }
                };
                if (canBrowser && ImGui::Button(("Add " + add).c_str())) addMonster(add);
                if (canBrowser) ImGui::SameLine();
                std::string pick;
                ImGui::SetNextItemWidth(200 * ed_.imgui_->scale());
                if (nameCombo("##add", pick, monsters, false) && !pick.empty()) addMonster(pick);
                help("Add a monster to this variant.");
                // A monster dragged from the content browser.
                if (const auto name = droppedThing("drop a monster here"); name && std::ranges::find(monsters, *name) != monsters.end()) addMonster(*name);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void PropertyPanel::templateInventory(ObjId id, std::size_t group, std::size_t slot)
{
    // The template's monster and items as a document of their own (ids by stream position): the
    // ordinary inventory ops on it, each turned into one op on the generator.
    const me::TemplateSlot where{id, group, slot};
    const auto one = me::templateDocument(ed_.doc_, where, ed_.things_);
    const int k = static_cast<int>(group), sl = static_cast<int>(slot);
    if (!one) {
        ImGui::TextDisabled("(its items' records can't be read: they're kept as stored)");
        return;
    }
    const auto items = one->childrenOf(1);
    ImGui::PushID(("tinv" + std::to_string(k) + "_" + std::to_string(sl)).c_str());
    ImGui::SeparatorText(("Inventory (" + std::to_string(items.size()) + ")").c_str());
    help("What this monster carries. Each monster the generator makes gets copies of these (an NPC wears the worn ones).");
    const auto& f = ed_.objects_->factory();
    const float s = ed_.imgui_->scale();
    // Performs an edit of the template's document (made by `inner` from a fresh copy of it).
    const auto perform = [this, where](const std::function<me::Op(const me::MapDocument&)>& inner) {
        try {
            const auto d = me::templateDocument(ed_.doc_, where, ed_.things_);
            if (!d) return false;
            return ed_.perform(me::templateEditOp(ed_.doc_, where, inner(*d), ed_.things_));
        } catch (const std::exception& ex) {
            ed_.status(ex.what());
            return false;
        }
    };
    if (!one->objects.at(1).header.details && items.empty()) ImGui::TextDisabled("(adding an item gives it the header details that hold an inventory)");
    for (std::size_t i = 0; i < items.size(); ++i) {
        const ObjId item = items[i];
        const me::DocObject& it = one->objects.at(item);
        ImGui::PushID(static_cast<int>(item));
        ed_.objects_->drawThumbnail(it.type, 24 * s);
        ImGui::SameLine();
        const auto key = std::tuple(k, sl, static_cast<int>(item));
        const bool open = templateItemsOpen_.contains(key);
        const auto nested = one->childrenOf(item).size();
        const std::string text = label(it) + (nested ? " (+" + std::to_string(nested) + ")" : "") + "##sel";
        if (ImGui::Selectable(text.c_str(), open, ImGuiSelectableFlags_AllowOverlap, ImVec2(std::max(60.0f, ImGui::GetContentRegionAvail().x - 215 * s), 0))) {
            if (open) templateItemsOpen_.erase(key);
            else templateItemsOpen_.insert(key);
        }
        help("Click: its own fields, here.");
        ImGui::SameLine();
        bool done = false;
        bool worn = it.header.details && (it.header.details->flags & me::ObjectFlag::Equipped);
        if (ImGui::Checkbox("Worn", &worn)) done = perform([&](const me::MapDocument& d) { return me::setEquippedOp(d, item, worn, &f); });
        help("Worn (EQUIPPED): the monster wears or wields it (an NPC the generator makes wears it too).");
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) done = perform([&](const me::MapDocument& d) { return me::moveItemOp(d, item, i - 1); });
        ImGui::SameLine();
        if (ImGui::ArrowButton("##dn", ImGuiDir_Down) && i + 1 < items.size()) done = perform([&](const me::MapDocument& d) { return me::moveItemOp(d, item, i + 1); });
        ImGui::SameLine();
        if (ImGui::SmallButton("Clone")) done = perform([&](const me::MapDocument& d) { return me::cloneItemOp(d, item); });
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            done = perform([&](const me::MapDocument&) {
                me::Op op;
                op.label = "Remove " + it.type + " from the inventory";
                op.parts.push_back(me::RemoveObjects{{item}});
                return op;
            });
        }
        if (done) {
            // (Stream positions moved: the open rows start over.)
            std::erase_if(templateItemsOpen_, [&](const auto& e) { return std::get<0>(e) == k && std::get<1>(e) == sl; });
            ImGui::PopID();
            break;
        }
        if (open) {
            ImGui::Indent();
            std::vector<me::SharedField> shown;
            for (const auto& sf : me::sharedFields(*one, {item}, ctx_)) {
                if (sf.desc.id != "header.position") shown.push_back(sf); // (an item is where its owner is)
            }
            const Apply apply = [this, perform, item](const me::FieldDesc& fd, const me::FieldValue& v, std::uint64_t mask) {
                perform([&](const me::MapDocument& d) {
                    switch (fd.special) {
                    case me::FieldDesc::Special::Type: return me::changeTypeOp(d, ed_.objects_->factory(), {item}, v.s);
                    case me::FieldDesc::Special::Extent: return me::setExtentOp(d, item, static_cast<std::uint32_t>(v.i));
                    case me::FieldDesc::Special::Details: return me::setDetailsOp(d, item, v.i != 0, &ed_.objects_->factory());
                    case me::FieldDesc::Special::None: break;
                    }
                    return me::setFieldOp(d, {item}, fd.id, v, ctx_, mask);
                });
            };
            fieldList(shown, apply, it.type, std::nullopt, false, "titem" + std::to_string(k) + "_" + std::to_string(sl) + "_" + std::to_string(item));
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (items.size() < 255) {
        addItemRow([&](const std::string& type) { perform([&](const me::MapDocument& d) { return me::addItemOp(d, f, 1, type); }); });
    }
    ImGui::PopID();
}

void PropertyPanel::lightKeys(ObjId id)
{
    const auto& l = std::get<xf::InvisibleLight>(ed_.doc_.objects.at(id).data);
    if (!section("Light keys", "lkeys", ImGuiTreeNodeFlags_DefaultOpen)) return;
    // A live preview: the colour and intensity as they animate.
    const std::uint32_t frame = static_cast<std::uint32_t>(SDL_GetTicks() * 30 / 1000);
    const auto c = lightColourNow(l, frame);
    const float i = lightIntensityNow(l, frame);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float r = 22 * ed_.imgui_->scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float glow = std::clamp(std::fabs(i) / 31.0f, 0.1f, 1.0f);
    dl->AddRectFilled(at, ImVec2(at.x + r * 2, at.y + r * 2), col(10, 10, 12));
    dl->AddCircleFilled(ImVec2(at.x + r, at.y + r), r * glow, IM_COL32(c[0], c[1], c[2], l.isAnti ? 90 : 220));
    ImGui::Dummy(ImVec2(r * 2, r * 2));
    ImGui::SameLine();
    ImGui::TextDisabled("now: colour %d %d %d, intensity %.1f, reach %.0f%s", c[0], c[1], c[2], i, radiusOf(i), l.isAnti ? " (anti-light)" : "");
    if (const auto cone = lightConeNow(ed_.doc_, ed_.things_, ed_.doc_.objects.at(id), frame)) {
        // (whole degrees, as the game turns them; a cone never darkens: the game's cone path ignores "anti")
        ImGui::TextDisabled("cone: facing %d, %d either side%s", lightcone::degreesFromAngle(cone->direction),
                            lightcone::degreesFromAngle(cone->width), cone->width == 0 || cone->width >= 0x8000 ? " (lights nothing)" : "");
    }
    if ((l.animFlags & 0xC0) != 0 && !lightcone::coneAnim(l).updates) {
        // (ColorLightUpdateDraw drops itself at its first run for a light with no keys at all)
        ImGui::TextColored(editor::themeColours().unknown, "It won't turn or aim without a colour, intensity or cone key.");
    }
    const auto mutate = [&](const std::string& lbl, const std::function<void(xf::InvisibleLight&)>& f) {
        edit(id, lbl, [&](me::DocObject& x) { f(std::get<xf::InvisibleLight>(x.data)); });
    };
    // Colour keys: a black key ends the list (the client counts the keys before it).
    std::size_t n = 0;
    while (n < 16 && (l.colorKeys[n][0] | l.colorKeys[n][1] | l.colorKeys[n][2]) != 0) ++n;
    ImGui::TextUnformatted("Colour sequence");
    help("The colours it fades through, a colour step apart; black ends the list. Fewer than 2: no animation.");
    for (std::size_t k = 0; k < std::min<std::size_t>(n + 1, 16); ++k) {
        ImGui::PushID(static_cast<int>(k));
        if (k % 8) ImGui::SameLine();
        if (std::array<std::uint8_t, 3> v{}; colourPick("##ck", l.colorKeys[k], v, ImGuiColorEditFlags_NoInputs)) {
            if ((v[0] | v[1] | v[2]) == 0) v = {1, 1, 1}; // (black would end the list here)
            mutate("Set light colour key", [&](auto& x) { x.colorKeys[k] = v; });
        }
        help(k < n ? "A key (right click: remove it)" : "Add a key");
        if (k < n && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            mutate("Remove light colour key", [&](auto& x) {
                for (std::size_t j = k; j + 1 < 16; ++j) x.colorKeys[j] = x.colorKeys[j + 1];
                x.colorKeys[15] = {};
            });
        }
        ImGui::PopID();
    }
    // Intensity and cone keys: 0 ends each list.
    const auto keys = [&](const char* title, const char* tip, const std::array<std::uint8_t, 16>& arr, std::array<std::uint8_t, 16> xf::InvisibleLight::*m) {
        ImGui::PushID(title);
        std::size_t len = 0;
        while (len < 16 && arr[len] != 0) ++len;
        ImGui::TextUnformatted(title);
        help(tip);
        for (std::size_t k = 0; k < std::min<std::size_t>(len + 1, 16); ++k) {
            ImGui::PushID(static_cast<int>(k));
            if (k % 8) ImGui::SameLine();
            if (const auto v = intOnRelease("##k", arr[k], 34)) {
                const int nv = std::clamp(*v, 0, 255);
                mutate(std::string("Set ") + title, [&](auto& x) {
                    auto& a = x.*m;
                    a[k] = static_cast<std::uint8_t>(nv);
                    if (nv == 0) {
                        for (std::size_t j = k; j < 16; ++j) a[j] = 0; // (0 ends it: the rest go)
                    }
                });
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    };
    keys("Intensity keys", "Intensities it pulses through, an intensity step apart (0 ends the list).", l.intensityKeys, &xf::InvisibleLight::intensityKeys);
    keys("Cone keys", "Cone widths in degrees it sweeps through, a cone step apart (0 ends the list). (The old editor took these for radius keys.)",
         l.coneKeys, &xf::InvisibleLight::coneKeys);
}

void PropertyPanel::inventory(ObjId owner)
{
    const auto items = ed_.doc_.childrenOf(owner);
    const me::DocObject& o = ed_.doc_.objects.at(owner);
    if (!section("Inventory (" + std::to_string(items.size()) + ")", ("inv" + std::to_string(owner)).c_str(), items.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!o.header.details) ImGui::TextDisabled("(adding an item gives it the header details that hold an inventory)");
    const auto& f = ed_.objects_->factory();
    const float s = ed_.imgui_->scale();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const ObjId item = items[i];
        const me::DocObject& it = ed_.doc_.objects.at(item);
        ImGui::PushID(static_cast<int>(item));
        ed_.objects_->drawThumbnail(it.type, 24 * s);
        ImGui::SameLine();
        const bool open = itemsOpen_.contains(item);
        if (ImGui::Selectable((label(it) + "##sel").c_str(), open, ImGuiSelectableFlags_AllowOverlap, ImVec2(std::max(60.0f, ImGui::GetContentRegionAvail().x - 215 * s), 0))) {
            if (open) itemsOpen_.erase(item);
            else itemsOpen_.insert(item);
        }
        help("Click: its own fields, here.");
        ImGui::SameLine();
        bool worn = it.header.details && (it.header.details->flags & me::ObjectFlag::Equipped);
        if (ImGui::Checkbox("Worn", &worn)) ed_.perform(me::setEquippedOp(ed_.doc_, item, worn, &f));
        help("Worn (EQUIPPED): a creature wears or wields it.");
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) ed_.perform(me::moveItemOp(ed_.doc_, item, i - 1));
        ImGui::SameLine();
        if (ImGui::ArrowButton("##dn", ImGuiDir_Down) && i + 1 < items.size()) ed_.perform(me::moveItemOp(ed_.doc_, item, i + 1));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clone")) {
            try {
                ed_.perform(me::cloneItemOp(ed_.doc_, item));
            } catch (const std::exception& ex) {
                ed_.status(ex.what());
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            me::Op op;
            op.label = "Remove " + it.type + " from the inventory";
            op.parts.push_back(me::RemoveObjects{{item}});
            ed_.perform(op);
            itemsOpen_.erase(item);
            ImGui::PopID();
            break;
        }
        if (open) {
            ImGui::Indent();
            const auto fields = me::sharedFields(ed_.doc_, {item}, ctx_);
            std::vector<me::SharedField> shown;
            for (const auto& sf : fields) {
                if (sf.desc.id != "header.position") shown.push_back(sf); // (an item is where its owner is)
            }
            fieldList(shown, selectionApply({item}), it.type, std::nullopt, false, "item" + std::to_string(item));
            lists(item);
            if (!ed_.doc_.childrenOf(item).empty()) inventory(item); // (what it carries in turn)
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (items.size() < 255) {
        addItemRow([&](const std::string& type) {
            try {
                ed_.perform(me::addItemOp(ed_.doc_, f, owner, type));
            } catch (const std::exception& ex) {
                ed_.status(ex.what());
            }
        });
    }
}

// --- The map's overlays: links, previews, gizmos -----------------------------------------------------------

void PropertyPanel::drawOverlay(ImDrawList* dl, const ImVec2& origin)
{
    const auto& cam = ed_.camera_;
    const float z = cam.zoom;
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - cam.left()) * z, origin.y + (y - cam.top()) * z); };
    const auto& doc = ed_.doc_;
    const auto arrow = [&](ImVec2 a, ImVec2 b, ImU32 c, float thick = 1.5f) {
        dl->AddLine(a, b, c, thick);
        const float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
        if (len < 16) return;
        const float ux = dx / len, uy = dy / len;
        const ImVec2 tip(b.x - ux * 6, b.y - uy * 6);
        dl->AddTriangleFilled(tip, ImVec2(tip.x - ux * 9 - uy * 4.5f, tip.y - uy * 9 + ux * 4.5f), ImVec2(tip.x - ux * 9 + uy * 4.5f, tip.y - uy * 9 - ux * 4.5f), c);
    };
    const auto& sel = ed_.selection_.objects;
    if (ed_.settings_.layers.links && ed_.settings_.layers.objects) {
        std::map<std::uint32_t, std::pair<ObjId, const me::DocObject*>> extents;
        for (const auto& [id, o] : doc.objects) {
            if (o.parent == 0) extents.try_emplace(o.header.extent, id, &o);
        }
        // A link is drawn hot when either end is selected (the selected object's incoming links too).
        bool srcHot = false;
        const me::DocObject* linked = nullptr;
        const auto target = [&](std::uint32_t e) -> const me::DocObject* {
            const auto it = e ? extents.find(e) : extents.end();
            linked = it == extents.end() ? nullptr : it->second.second;
            return linked;
        };
        const auto linkHot = [&](std::uint32_t e) { return srcHot || (e && extents.contains(e) && sel.contains(extents.at(e).first)); };
        for (const auto& [id, o] : doc.objects) {
            if (o.parent != 0) continue;
            srcHot = sel.contains(id);
            const ImVec2 p = S(o.header.x, o.header.y);
            std::visit(
                [&](const auto& x) {
                    using T = std::decay_t<decltype(x)>;
                    const bool hot = srcHot;
                    const int a = hot ? 255 : 150;
                    const auto linkTo = [&](std::uint32_t e, ImU32 c) {
                        if (!target(e)) return;
                        const bool h = linkHot(e);
                        arrow(p, S(linked->header.x, linked->header.y), (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(h ? 255 : 150) << IM_COL32_A_SHIFT), h ? 2.5f : 1.5f);
                    };
                    if constexpr (std::is_same_v<T, xf::Transporter>) {
                        linkTo(x.targetExtent, col(190, 120, 255));
                    } else if constexpr (std::is_same_v<T, xf::Elevator> || std::is_same_v<T, xf::ElevatorShaft>) {
                        linkTo(x.linkedExtent, col(120, 200, 255));
                    } else if constexpr (std::is_same_v<T, xf::InvisibleLight>) {
                        linkTo(x.target, col(255, 240, 150));
                    } else if constexpr (std::is_same_v<T, xf::Mover>) {
                        linkTo(x.movedExtent, col(255, 170, 80));
                        // Its path: the first waypoint, then each one's first link (a preview of where it goes).
                        const auto* w = me::findWaypoint(doc, x.waypoint);
                        const bool pathHot = hot || (w && ed_.selection_.waypoints.contains(w->id));
                        ImVec2 from = p;
                        std::set<std::uint32_t> seen;
                        for (int step = 0; w && step < 64 && seen.insert(w->id).second; ++step) {
                            const ImVec2 to = S(w->x, w->y);
                            arrow(from, to, col(255, 200, 80, pathHot ? 220 : 110), pathHot ? 2.0f : 1.0f);
                            from = to;
                            w = w->links.empty() ? nullptr : me::findWaypoint(doc, w->links.front().target);
                        }
                    } else if constexpr (std::is_same_v<T, xf::Glyph>) {
                        if (x.targetX != 0 || x.targetY != 0) {
                            const ImVec2 t = S(x.targetX, x.targetY);
                            arrow(p, t, col(255, 90, 90, a));
                            dl->AddCircle(t, 4, col(255, 90, 90, a));
                        }
                    } else if constexpr (std::is_same_v<T, xf::Exit>) {
                        if (hot && (x.x != 0 || x.y != 0)) {
                            const ImVec2 t = S(x.x, x.y);
                            arrow(p, t, col(120, 255, 200, a));
                            dl->AddText(ImVec2(t.x + 5, t.y - 7), col(120, 255, 200, a), "return point");
                        }
                    } else if constexpr (std::is_same_v<T, xf::Hole>) {
                        if (x.fallX != 0 || x.fallY != 0) {
                            const ImVec2 t = S(static_cast<float>(x.fallX), static_cast<float>(x.fallY));
                            arrow(p, t, col(200, 160, 110, a));
                            dl->AddCircle(t, 5, col(200, 160, 110, a));
                        }
                    }
                },
                o.data);
        }
    }
    // The selected light's reach and colour, the selected sentry's beam.
    const std::uint32_t frame = static_cast<std::uint32_t>(SDL_GetTicks() * 30 / 1000);
    for (const ObjId id : sel) {
        const auto it = doc.objects.find(id);
        if (it == doc.objects.end()) continue;
        const auto& o = it->second;
        const ImVec2 p = S(o.header.x, o.header.y);
        if (const auto* l = std::get_if<xf::InvisibleLight>(&o.data)) {
            const auto c = lightColourNow(*l, frame);
            const float reach = radiusOf(lightIntensityNow(*l, frame));
            const float r = reach * z;
            if (const auto cone = lightConeNow(doc, ed_.things_, o, frame)) {
                // A cone: the kite the game lights (as it turns, sweeps or aims), and its reach faintly.
                dl->AddCircle(p, r, IM_COL32(c[0], c[1], c[2], 70), 0, 1.0f);
                drawLightCone(dl, S, o.header.x, o.header.y, reach, *cone, IM_COL32(c[0], c[1], c[2], 220), true);
            } else {
                dl->AddCircleFilled(p, r, IM_COL32(c[0], c[1], c[2], l->isAnti ? 30 : 45));
                dl->AddCircle(p, r, IM_COL32(c[0], c[1], c[2], 200), 0, 1.5f);
            }
        } else if (const auto* sn = std::get_if<xf::Sentry>(&o.data)) {
            const float a = sn->basePosition + sn->rotateSpeed * static_cast<float>(frame % 100000);
            dl->AddLine(p, ImVec2(p.x + std::cos(sn->basePosition) * 80 * z, p.y + std::sin(sn->basePosition) * 80 * z), col(255, 80, 200, 220), 2);
            dl->AddLine(p, ImVec2(p.x + std::cos(a) * 80 * z, p.y + std::sin(a) * 80 * z), col(255, 80, 200, 90), 1.5f);
        }
    }

    // The gizmos: arrows along the floor's two diagonals, a centre square, and the ring for facing.
    gizmoVisible_ = false;
    if (ed_.tool_ != MapEditor::Tool::Select || ed_.selection_.empty() || ed_.pick_) return;
    const auto b = me::bounds(doc, ed_.selection_);
    if (!b) return;
    gizmoVisible_ = true;
    gizmoCentre_ = {(b->first.x + b->second.x) / 2, (b->first.y + b->second.y) / 2};
    gizmoTurns_ = false;
    for (const ObjId id : sel) {
        if (const auto it = doc.objects.find(id); it != doc.objects.end() && me::facingKind(it->second) != me::FacingKind::None) gizmoTurns_ = true;
    }
    // (Dragged: the selection is shown moved already while the preview is up, its bounds with it.)
    const bool offset = gizmo_ != Gizmo::None && !ed_.preview_.active();
    const float dx = offset ? gizmoDx_ : 0, dy = offset ? gizmoDy_ : 0;
    const ImVec2 c = S(gizmoCentre_.x + dx, gizmoCentre_.y + dy);
    const float L = 52 * ed_.imgui_->scale();
    const float k = 0.70710678f;
    const ImU32 ca = gizmo_ == Gizmo::MoveA ? col(255, 240, 120) : col(240, 90, 90);
    const ImU32 cb = gizmo_ == Gizmo::MoveB ? col(255, 240, 120) : col(90, 200, 90);
    arrow(c, ImVec2(c.x + L * k, c.y + L * k), ca, 3);
    arrow(c, ImVec2(c.x + L * k, c.y - L * k), cb, 3);
    const float q = 6 * ed_.imgui_->scale();
    dl->AddRectFilled(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), gizmo_ == Gizmo::MoveFree ? col(255, 240, 120) : col(220, 220, 240, 200));
    if (gizmoTurns_) {
        const float R = 74 * ed_.imgui_->scale();
        dl->AddCircle(c, R, gizmo_ == Gizmo::Rotate ? col(255, 240, 120) : col(120, 170, 255, 200), 64, 2.5f);
        if (gizmo_ == Gizmo::Rotate) {
            char t[32];
            std::snprintf(t, sizeof t, "%+.0f deg", gizmoTurn_);
            dl->AddText(ImVec2(c.x + R + 6, c.y - 8), col(255, 240, 120), t);
            const float a0 = std::atan2(gizmoStart_.y - gizmoCentre_.y, gizmoStart_.x - gizmoCentre_.x);
            const float a1 = a0 + gizmoTurn_ * kPi / 180;
            dl->AddLine(c, ImVec2(c.x + std::cos(a0) * R, c.y + std::sin(a0) * R), col(255, 240, 120, 120), 1.5f);
            dl->AddLine(c, ImVec2(c.x + std::cos(a1) * R, c.y + std::sin(a1) * R), col(255, 240, 120), 2.0f);
        }
    }
}

bool PropertyPanel::gizmoInput(const me::WorldPoint& w, bool hovered)
{
    if (!gizmoVisible_ && gizmo_ == Gizmo::None) return false;
    const float z = ed_.camera_.zoom;
    const float s = ed_.imgui_->scale();
    // In screen pixels from the gizmo's centre.
    const float px = (w.x - gizmoCentre_.x) * z, py = (w.y - gizmoCentre_.y) * z;
    const float k = 0.70710678f;
    if (gizmo_ == Gizmo::None) {
        if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return false;
        const float L = 52 * s;
        const auto nearSeg = [&](float ux, float uy) {
            const float t = px * ux + py * uy;
            const float d = std::fabs(px * uy - py * ux);
            return t > 10 * s && t < L + 6 && d < 7 * s;
        };
        Gizmo g = Gizmo::None;
        if (std::fabs(px) <= 7 * s && std::fabs(py) <= 7 * s) g = Gizmo::MoveFree;
        else if (nearSeg(k, k)) g = Gizmo::MoveA;
        else if (nearSeg(k, -k)) g = Gizmo::MoveB;
        else if (gizmoTurns_ && std::fabs(std::hypot(px, py) - 74 * s) < 7 * s) g = Gizmo::Rotate;
        if (g == Gizmo::None) return false;
        gizmo_ = g;
        gizmoStart_ = w;
        gizmoDx_ = gizmoDy_ = gizmoTurn_ = 0;
        return true;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float ddx = w.x - gizmoStart_.x, ddy = w.y - gizmoStart_.y;
        switch (gizmo_) {
        case Gizmo::MoveFree:
            gizmoDx_ = ed_.snapValue(ddx);
            gizmoDy_ = ed_.snapValue(ddy);
            break;
        case Gizmo::MoveA: {
            const float d = ed_.snapValue((ddx + ddy) / 2);
            gizmoDx_ = gizmoDy_ = d;
            break;
        }
        case Gizmo::MoveB: {
            const float d = ed_.snapValue((ddx - ddy) / 2);
            gizmoDx_ = d;
            gizmoDy_ = -d;
            break;
        }
        case Gizmo::Rotate: {
            const float a0 = std::atan2(gizmoStart_.y - gizmoCentre_.y, gizmoStart_.x - gizmoCentre_.x);
            const float a1 = std::atan2(w.y - gizmoCentre_.y, w.x - gizmoCentre_.x);
            float t = (a1 - a0) * 180 / kPi;
            while (t > 180) t -= 360;
            while (t < -180) t += 360;
            gizmoTurn_ = ImGui::GetIO().KeyCtrl ? t : std::round(t / 5) * 5;
            break;
        }
        case Gizmo::None: break;
        }
        // Live: the move or the turn the release will make, snapped the same.
        ed_.previewDrag({gizmo_ == Gizmo::Rotate ? 5.0f : 4.0f, gizmoDx_, gizmoDy_, gizmoTurn_}, [this](std::optional<me::Selection>& moved) {
            return gizmo_ == Gizmo::Rotate ? turnOp() : moveOp(moved);
        });
        return true;
    }
    // Let go: one op.
    const Gizmo was = gizmo_;
    gizmo_ = Gizmo::None;
    ed_.endPreview(); // (the document as it was: the drag's one op from it)
    if (was == Gizmo::Rotate) {
        if (gizmoTurn_ != 0) ed_.perform(turnOp());
    } else if (gizmoDx_ != 0 || gizmoDy_ != 0) {
        std::optional<me::Selection> moved;
        if (ed_.perform(moveOp(moved)) && moved) ed_.setSelection(std::move(*moved), true);
    }
    gizmoDx_ = gizmoDy_ = gizmoTurn_ = 0;
    return true;
}

me::Op PropertyPanel::turnOp() const
{
    // The selection's things that turn, by the ring's angle (nothing for no turn).
    if (gizmoTurn_ == 0) return {};
    std::vector<std::pair<ObjId, me::DocObject>> turned;
    for (const ObjId id : ed_.selection_.objects) {
        me::DocObject o = ed_.doc_.objects.at(id);
        if (const auto deg = me::facingDegrees(o); deg && me::setFacingDegrees(o, *deg + gizmoTurn_)) turned.emplace_back(id, std::move(o));
    }
    char lbl[48];
    std::snprintf(lbl, sizeof lbl, "Turn %+.0f degrees", gizmoTurn_);
    return me::editObjectsOp(ed_.doc_, turned, lbl);
}

me::Op PropertyPanel::moveOp(std::optional<me::Selection>& moved) const
{
    // The selection moved by the arrows' or the square's offset; `moved` the selection after.
    if (gizmoDx_ == 0 && gizmoDy_ == 0) return {};
    me::Selection after;
    me::Op op = me::moveOp(ed_.doc_, ed_.selection_, gizmoDx_, gizmoDy_, &after);
    moved = std::move(after);
    return op;
}

// --- Test steps --------------------------------------------------------------------------------------------

bool PropertyPanel::runStep(const std::string& step)
{
    // "select-extent:N" selects the object with that extent; "prop:FIELD=VALUE" sets a field on the
    // selection (VALUE: a number, a name, "x,y" for a point, "r,g,b" for a colour, "+0xBITS" /
    // "-0xBITS" to set / clear flag bits); "change-type:T"; "add-item:T"; "item-open:N" shows the
    // Nth inventory item's fields; "template-add:V,T"; "template-open:V,S"; "template-item-add:V,S,T",
    // "template-item-open:V,S,N", "template-item-equip:V,S,N" (a template's inventory); "reorder-extents";
    // "linked-from-go:N" ("+N" with Shift) the Nth "Linked from" row's Go to.
    if (step.starts_with("select-extent:")) {
        const auto e = static_cast<std::uint32_t>(std::strtoul(step.c_str() + 14, nullptr, 10));
        me::Selection s;
        for (const auto id : me::objectsWithExtent(ed_.doc_, e)) {
            if (ed_.doc_.objects.at(id).parent == 0) s.objects.insert(id);
        }
        ed_.setSelection(std::move(s));
        return true;
    }
    if (step.starts_with("prop:")) {
        const auto eq = step.find('=');
        if (eq == std::string::npos) return true;
        const std::string field = step.substr(5, eq - 5), value = step.substr(eq + 1);
        const std::vector<ObjId> ids(ed_.selection_.objects.begin(), ed_.selection_.objects.end());
        if (ids.empty()) return true;
        const auto shared = me::sharedFields(ed_.doc_, ids, ctx_);
        const auto f = std::ranges::find(shared, field, [](const me::SharedField& s2) { return s2.desc.id; });
        if (f == shared.end()) {
            ed_.status("No field " + field);
            return true;
        }
        me::FieldValue v = f->value;
        std::uint64_t mask = ~0ull;
        if (f->desc.kind == K::Flags && (value[0] == '+' || value[0] == '-')) {
            mask = std::strtoull(value.c_str() + 1, nullptr, 0);
            v.i = value[0] == '+' ? static_cast<std::int64_t>(mask) : 0;
        } else if (f->desc.kind == K::Point) {
            std::sscanf(value.c_str(), "%lf,%lf", &v.f, &v.g);
        } else if (f->desc.kind == K::Color) {
            int r = 0, g = 0, b = 0;
            std::sscanf(value.c_str(), "%d,%d,%d", &r, &g, &b);
            v.rgb = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b)};
        } else if (f->desc.kind == K::Float || f->desc.kind == K::Angle) {
            v.f = std::atof(value.c_str());
        } else if (f->desc.kind == K::Int || f->desc.kind == K::Enum || f->desc.kind == K::Bool || f->desc.kind == K::Flags || f->desc.kind == K::Extent ||
                   f->desc.kind == K::Waypoint || f->desc.kind == K::Direction) {
            v.i = std::strtoll(value.c_str(), nullptr, 0);
        } else {
            v.s = value;
        }
        selectionApply(ids)(f->desc, v, mask);
        return true;
    }
    if (step.starts_with("light-key:")) {
        // "light-key:KIND,I=V": the selected invisible light's key I of colour ("r g b"), intensity or cone, as the keys' inputs set it.
        char kind[16] = {};
        int index = 0, a = 0, b = 0, c = 0;
        if (ed_.selection_.objects.size() != 1 ||
            std::sscanf(step.c_str() + 10, "%15[^,],%d=%d %d %d", kind, &index, &a, &b, &c) < 3 || index < 0 || index > 15)
            return true;
        const ObjId id = *ed_.selection_.objects.begin();
        if (!std::holds_alternative<xf::InvisibleLight>(ed_.doc_.objects.at(id).data)) return true;
        const std::string k = kind;
        const auto i = static_cast<std::size_t>(index);
        const auto byte = [](int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); };
        edit(id, "Set light key", [&](me::DocObject& x) {
            auto& l = std::get<xf::InvisibleLight>(x.data);
            if (k == "colour") l.colorKeys[i] = {byte(a), byte(b), byte(c)};
            else if (k == "intensity") l.intensityKeys[i] = byte(a);
            else if (k == "cone") l.coneKeys[i] = byte(a);
        });
        return true;
    }
    if (step.starts_with("change-type:")) {
        if (!ed_.selection_.objects.empty()) ed_.perform(me::changeTypeOp(ed_.doc_, ed_.objects_->factory(), ed_.selection_.objects, step.substr(12)));
        return true;
    }
    if (step.starts_with("add-item:")) {
        if (ed_.selection_.objects.size() == 1) ed_.perform(me::addItemOp(ed_.doc_, ed_.objects_->factory(), *ed_.selection_.objects.begin(), step.substr(9)));
        return true;
    }
    if (step.starts_with("item-open:")) {
        if (ed_.selection_.objects.size() == 1) {
            const auto items = ed_.doc_.childrenOf(*ed_.selection_.objects.begin());
            const auto n = static_cast<std::size_t>(std::atoi(step.c_str() + 10));
            if (n < items.size()) itemsOpen_.insert(items[n]);
        }
        return true;
    }
    if (step.starts_with("template-add:")) {
        const auto comma = step.find(',');
        if (ed_.selection_.objects.size() == 1 && comma != std::string::npos) {
            const auto k = static_cast<std::size_t>(std::atoi(step.c_str() + 13));
            const std::string type = step.substr(comma + 1);
            const ObjId id = *ed_.selection_.objects.begin();
            try {
                auto fresh = me::freshTemplate(ed_.objects_->factory(), type);
                edit(id, "Add generator monster " + type, [&](me::DocObject& c) {
                    if (auto* g = std::get_if<xf::MonsterGenerator>(&c.data); g && k < 3) {
                        if (g->groups.size() < 3) g->groups.resize(3);
                        if (g->groups[k].size() < 4) g->groups[k].push_back(fresh);
                    }
                });
            } catch (const std::exception& ex) {
                ed_.status(ex.what());
            }
        }
        return true;
    }
    if (step.starts_with("template-open:")) {
        int k = 0, sl = 0;
        std::sscanf(step.c_str() + 14, "%d,%d", &k, &sl);
        templatesOpen_.insert({k, sl});
        return true;
    }
    if (step.starts_with("template-item-add:") || step.starts_with("template-item-open:") || step.starts_with("template-item-equip:")) {
        // "template-item-add:V,S,T" adds an item of type T to the selected generator's variant V
        // slot S monster; "template-item-open:V,S,N" shows its Nth item's fields;
        // "template-item-equip:V,S,N" marks its Nth item worn.
        const auto colon = step.find(':');
        const std::string verb = step.substr(14, colon - 14), args = step.substr(colon + 1);
        int k = 0, sl = 0;
        std::sscanf(args.c_str(), "%d,%d", &k, &sl);
        const auto second = args.find(',', args.find(',') + 1);
        const std::string last = second == std::string::npos ? std::string() : args.substr(second + 1);
        if (ed_.selection_.objects.size() != 1 || k < 0 || sl < 0) return true;
        const me::TemplateSlot where{*ed_.selection_.objects.begin(), static_cast<std::size_t>(k), static_cast<std::size_t>(sl)};
        const auto one = me::templateDocument(ed_.doc_, where, ed_.things_);
        if (!one) {
            ed_.status("No readable generator template there");
            return true;
        }
        const auto items = one->childrenOf(1);
        const auto n = static_cast<std::size_t>(std::atoi(last.c_str()));
        try {
            if (verb == "add") {
                ed_.perform(me::templateEditOp(ed_.doc_, where, me::addItemOp(*one, ed_.objects_->factory(), 1, last), ed_.things_));
            } else if (verb == "open" && n < items.size()) {
                templatesOpen_.insert({k, sl});
                templateItemsOpen_.insert({k, sl, static_cast<int>(items[n])});
            } else if (verb == "equip" && n < items.size()) {
                ed_.perform(me::templateEditOp(ed_.doc_, where, me::setEquippedOp(*one, items[n], true, &ed_.objects_->factory()), ed_.things_));
            }
        } catch (const std::exception& ex) {
            ed_.status(ex.what());
        }
        return true;
    }
    if (step.starts_with("props-scroll:")) {
        scrollTo_ = step.substr(13);
        return true;
    }
    if (step.starts_with("props-focus:")) {
        // (Tests) "props-focus:FIELD": the field's text box takes the keyboard (as a click in it),
        // for text: and key: steps to type into; a click elsewhere ends the typing.
        focusField_ = step.substr(12);
        return true;
    }
    if (step.starts_with("prop-dump:")) {
        // (Tests) "prop-dump:FIELD": each selected object's value of the field, logged
        // ("prop-dump: Skeleton #3 monster.sight = 321").
        const std::string field = step.substr(10);
        for (const ObjId id : ed_.selection_.objects) {
            const me::DocObject& o = ed_.doc_.objects.at(id);
            for (const auto& fd : me::objectFields(o, ctx_)) {
                if (fd.id != field) continue;
                const me::FieldValue v = fd.get(o);
                char b[96];
                std::snprintf(b, sizeof b, "i %lld f %.6f g %.6f rgb %d,%d,%d", static_cast<long long>(v.i), v.f, v.g, v.rgb[0], v.rgb[1], v.rgb[2]);
                SDL_Log("prop-dump: %s %s = %s s \"%s\"", label(o).c_str(), field.c_str(), b, v.s.c_str());
            }
        }
        return true;
    }
    if (step.starts_with("props-combo:")) {
        // (Tests) a name picker opened, as a click on it: "props-combo:##addshop[,filter]".
        const std::string rest = step.substr(12);
        const auto comma = rest.find(',');
        openCombo_ = rest.substr(0, comma);
        comboFilter_ = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
        return true;
    }
    if (step == "reorder-extents") {
        ed_.perform(me::reorderExtentsOp(ed_.doc_, 1));
        return true;
    }
    if (step.starts_with("linked-from-go:")) {
        // The selected object's Nth "Linked from" row's Go to ("+N": with Shift, added to the selection).
        const bool add = step.size() > 15 && step[15] == '+';
        const auto n = static_cast<std::size_t>(std::atoi(step.c_str() + 15 + (add ? 1 : 0)));
        if (ed_.selection_.objects.size() == 1) {
            const auto& in = links().toObject(ed_.doc_, *ed_.selection_.objects.begin());
            if (n < in.size()) goToSource(me::Link(in[n]), add);
        }
        return true;
    }
    if (step.starts_with("link-pick:")) {
        // The selected object's link picker armed, as its button does ("link-pick:" then "pick:x,y").
        if (ed_.selection_.objects.size() == 1) {
            const ObjId id = *ed_.selection_.objects.begin();
            const auto& o = ed_.doc_.objects.at(id);
            MapEditor::PickRequest r;
            r.kind = MapEditor::PickRequest::Kind::Object;
            r.prompt = "Click the partner";
            r.from = me::WorldPoint{o.header.x, o.header.y};
            r.done = [this, id](const MapEditor::PickResult& p) {
                if (!p.object || *p.object == id) return;
                me::DocObject a = ed_.doc_.objects.at(id), b = ed_.doc_.objects.at(*p.object);
                auto* ta = std::get_if<xf::Transporter>(&a.data);
                auto* tb = std::get_if<xf::Transporter>(&b.data);
                if (!ta) return;
                ta->targetExtent = b.header.extent;
                std::vector<std::pair<ObjId, me::DocObject>> changed{{id, a}};
                if (twoWay_ && tb) {
                    tb->targetExtent = a.header.extent;
                    changed.emplace_back(*p.object, b);
                }
                ed_.perform(me::editObjectsOp(ed_.doc_, changed, "Link both ways"));
            };
            ed_.beginPick(std::move(r));
        }
        return true;
    }
    return false;
}

} // namespace noxsdl
