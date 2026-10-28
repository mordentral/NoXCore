// The Record panel's forms for the text tables' records (monster.bin, soundset.bin, modifier.bin,
// gamedata.bin): a row per field of the schema, the value as the pack has it (SOLO and ARENA side
// by side where the file has them), who reads it, the game's own value and a revert.
#include "gui/panels.hpp"

#include "editor/theme_colours.hpp"
#include "gui/textures.hpp"

#include "noxcore/audio/pcm.hpp"
#include "noxcore/audio/sound_names.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

namespace fs = std::filesystem;

namespace videotools::gui {

using content::FieldSpec;
using content::FieldType;
using content::GameKind;
using content::TableFormat;

namespace {

/// Which table and record a browser record edits (nullopt: not a table record).
struct TableTarget {
    TableFormat format;
    std::string section;
    std::string record;
};

std::optional<TableTarget> targetOf(const Workspace& ws, const content::Record& r)
{
    using content::RecordList;
    switch (r.ref.list) {
    case RecordList::Monster: return TableTarget{TableFormat::Monster, {}, r.name};
    case RecordList::SoundSet: return TableTarget{TableFormat::SoundSet, {}, r.name};
    case RecordList::Weapon: return TableTarget{TableFormat::Modifier, "WEAPON_DEFINITIONS", r.name};
    case RecordList::Armor: return TableTarget{TableFormat::Modifier, "ARMOR_DEFINITIONS", r.name};
    case RecordList::Modifier:
        if (const auto* md = ws.files.modifiers(); md && std::size_t(r.ref.index) < md->modifiers.size()) {
            return TableTarget{TableFormat::Modifier, md->modifiers[std::size_t(r.ref.index)].section, r.name};
        }
        return std::nullopt;
    case RecordList::Tuning: return TableTarget{TableFormat::GameData, {}, {}};
    default: return std::nullopt;
    }
}

ImVec4 readByColour(content::ReadBy r)
{
    const auto& t = noxsdl::editor::themeColours();
    switch (r) {
    case content::ReadBy::Server: return t.info;
    case content::ReadBy::Client: return t.ok;
    case content::ReadBy::Both: return t.caution;
    default: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
}

/// The sound names, once (for the pickers).
const std::vector<std::string>& soundNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (std::uint32_t i = 1; i < nox::audio::kSoundCount; ++i) {
            const auto n = nox::audio::soundName(i);
            if (!n.empty()) {
                v.emplace_back(n);
            }
        }
        std::ranges::sort(v, [](const std::string& a, const std::string& b) { return nox::util::compareNoCase(a, b) < 0; });
        return v;
    }();
    return names;
}

/// One value's editor. Returns true with `out` set when an edit was committed.
bool valueEditor(const FieldSpec* spec, const std::string& value, std::string& out, float width)
{
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    ImGui::SetNextItemWidth(width);
    const FieldType type = spec ? spec->type : FieldType::Text;
    if (type == FieldType::Enum && spec) {
        bool changed = false;
        if (ImGui::BeginCombo("##v", value.empty() ? "(none)" : value.c_str())) {
            for (const auto n : spec->names) {
                if (ImGui::Selectable(std::string(n).c_str(), nox::util::equalsNoCase(n, value))) {
                    out = std::string(n);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    if (type == FieldType::Sound) {
        bool changed = false;
        if (ImGui::BeginCombo("##v", value.empty() ? "(none)" : value.c_str(), ImGuiComboFlags_HeightLarge)) {
            static char filter[64] = {};
            if (ImGui::IsWindowAppearing()) {
                filter[0] = 0;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##f", "filter", filter, sizeof filter);
            const std::string f = nox::util::lowered(filter);
            if (ImGui::Selectable("NULL (none)", value == "NULL")) {
                out = "NULL";
                changed = true;
            }
            for (const auto& n : soundNames()) {
                if (!f.empty() && nox::util::lowered(n).find(f) == std::string::npos) {
                    continue;
                }
                if (ImGui::Selectable(n.c_str(), n == value)) {
                    out = n;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    if (type == FieldType::Rgb) {
        int c[3] = {};
        std::sscanf(value.c_str(), "%d %d %d", &c[0], &c[1], &c[2]);
        float col[3] = {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f};
        ImGui::SetNextItemWidth(width);
        if (ImGui::ColorEdit3("##v", col, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB)) {
            // (committed as it's dragged: each step is one edit of the field)
            out = std::format("{} {} {}", int(col[0] * 255 + 0.5f), int(col[1] * 255 + 0.5f), int(col[2] * 255 + 0.5f));
            return out != value;
        }
        return false;
    }
    if (type == FieldType::Slot) {
        bool changed = false;
        if (ImGui::BeginCombo("##v", value.c_str())) {
            for (int s = 1; s <= 6; ++s) {
                const std::string n = std::format("COLOR{}", s);
                if (ImGui::Selectable(n.c_str(), n == value)) {
                    out = n;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    ImGui::InputText("##v", buf, sizeof buf, type == FieldType::Int || type == FieldType::Float ? ImGuiInputTextFlags_CharsScientific : 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        out = buf;
        return std::string(nox::util::trim(out)) != value;
    }
    if (type == FieldType::Flags && spec && !spec->names.empty()) {
        // The names to tick, beside the text.
        ImGui::SameLine(0, 2);
        if (ImGui::SmallButton("...")) {
            ImGui::OpenPopup("flags");
        }
        if (ImGui::BeginPopup("flags")) {
            std::vector<std::string> on;
            std::string_view rest = value;
            while (!rest.empty()) {
                const auto plus = rest.find('+');
                const auto w = nox::util::trim(rest.substr(0, plus));
                if (!w.empty() && !nox::util::equalsNoCase(w, "NULL")) on.emplace_back(w);
                rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
            }
            bool changed = false;
            for (const auto n : spec->names) {
                bool set = std::ranges::any_of(on, [&](const std::string& s) { return nox::util::equalsNoCase(s, n); });
                if (ImGui::Checkbox(std::string(n).c_str(), &set)) {
                    changed = true;
                    if (set) {
                        on.emplace_back(n);
                    } else {
                        std::erase_if(on, [&](const std::string& s) { return nox::util::equalsNoCase(s, n); });
                    }
                }
            }
            ImGui::EndPopup();
            if (changed) {
                std::string joined;
                for (const auto& s : on) {
                    joined += (joined.empty() ? "" : "+") + s;
                }
                out = joined.empty() ? "NULL" : joined;
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::string RecordPanel::tableText(const content::Record& r) const
{
    if (r.ref.list == content::RecordList::Thing) {
        // A type's lines as the pack has them (the binary ones as their summary).
        const auto* db = ws_.pack.workingThings();
        if (!db || std::size_t(r.ref.index) >= db->things.size()) {
            return {};
        }
        const auto& t = db->things[std::size_t(r.ref.index)];
        std::string out = t.name + "\n";
        for (const auto& a : t.attributes) {
            out += "    " + a + "\n";
        }
        if (t.draw) {
            out += "    DRAW " + t.draw->type + std::format(" ({} bytes)\n", t.draw->data.size());
        }
        if (t.menuIcon) {
            out += "    MENUICON " + nox::content::thingValue(t, "MENUICON").value_or("") + "\n";
        }
        if (t.prettyImage) {
            out += "    PRETTYIMAGE " + nox::content::thingValue(t, "PRETTYIMAGE").value_or("") + "\n";
        }
        return out;
    }
    const auto target = targetOf(ws_, r);
    const content::TextTable* work = target ? ws_.pack.working(target->format) : nullptr;
    if (!work) {
        return {};
    }
    if (target->format == TableFormat::GameData) {
        // Every line of this name.
        std::string out;
        for (const auto& f : work->records().front().fields) {
            if (nox::util::equalsNoCase(f.key, r.name)) {
                out += work->text().substr(f.whole.begin, f.whole.size()) + "\n";
            }
        }
        return out;
    }
    const auto i = work->find(target->record, target->section);
    if (!i) {
        return {};
    }
    const auto& rec = work->records()[*i];
    return work->text().substr(rec.whole.begin, rec.whole.size());
}

bool RecordPanel::tableForm(const content::Record& r)
{
    const auto target = targetOf(ws_, r);
    if (!target) {
        return false;
    }
    content::PackEditor& pack = ws_.pack;
    const content::TextTable* work = pack.working(target->format);
    const content::TextTable* base = pack.base(target->format);
    if (!work || !base) {
        ImGui::TextColored(noxsdl::editor::themeColours().error, "%s didn't load.", nox::content::tableFileName(target->format));
        return true;
    }
    const auto& theme = noxsdl::editor::themeColours();
    const std::string recordName = target->format == TableFormat::GameData ? std::string() : target->record;
    const auto wr = work->find(recordName, target->section);
    const auto br = base->find(recordName, target->section);
    if (!wr) {
        ImGui::TextDisabled("Not in %s.", nox::content::tableFileName(target->format));
        return true;
    }
    const nox::content::TableRecord& wrec = work->records()[*wr];
    const nox::content::TableRecord* brec = br ? &base->records()[*br] : nullptr;

    // The rows: the schema's fields (gamedata.bin: this one name), then any key the record has
    // that the game doesn't read.
    std::vector<const FieldSpec*> specs;
    std::vector<std::string> keys;
    if (target->format == TableFormat::GameData) {
        keys.push_back(r.name);
        specs.push_back(content::gameDataName(r.name));
    } else {
        for (const FieldSpec& f : content::tableFields(target->format, target->section)) {
            keys.emplace_back(f.key);
            specs.push_back(&f);
        }
        for (const auto& f : wrec.fields) {
            if (!content::tableField(target->format, f.key, target->section) &&
                std::ranges::none_of(keys, [&](const std::string& k) { return nox::util::equalsNoCase(k, f.key); })) {
                keys.push_back(f.key);
                specs.push_back(nullptr);
            }
        }
    }
    const bool kinds = target->format == TableFormat::Monster || target->format == TableFormat::GameData;
    if (!pack.packOpen()) {
        ImGui::TextColored(theme.note, "Changes go into a content pack: pick one at the top (or make one) before you save.");
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    const int columns = kinds ? 5 : 4;
    if (!ImGui::BeginTable("tableForm", columns, flags)) {
        return true;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("MELEE_ATTACK_POISON_STRENGTH").x + 8);
    if (kinds) {
        ImGui::TableSetupColumn("Solo", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Network (arena)", ImGuiTableColumnFlags_WidthStretch);
    } else {
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    }
    ImGui::TableSetupColumn("Read by", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("server").x + 12);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Revert").x + 12);
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const std::string& key = keys[i];
        const FieldSpec* spec = specs[i];
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool edited = std::ranges::any_of(std::initializer_list<GameKind>{GameKind::Both, GameKind::Solo, GameKind::Arena}, [&](GameKind k) {
            return pack.editOf(target->format, target->section, recordName, key, k) != nullptr;
        });
        ImGui::AlignTextToFramePadding();
        if (edited) {
            ImGui::TextColored(theme.highlightText, "%s *", key.c_str());
        } else if (!spec) {
            ImGui::TextColored(theme.unknown, "%s", key.c_str());
        } else {
            ImGui::TextUnformatted(key.c_str());
        }
        if (ImGui::IsItemHovered()) {
            std::string tip = spec ? std::string(spec->docs) : std::string("The game doesn't read this key.");
            if (spec && !spec->readers.empty()) {
                tip += std::format("\nRead in {}.", spec->readers);
            }
            if (spec) {
                tip += std::string("\n\nReach: ") + content::reachLabel(content::reachOf(spec->readBy)) + ". " + content::reachDocs(content::reachOf(spec->readBy));
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        const auto cell = [&](GameKind kind) {
            ImGui::TableNextColumn();
            ImGui::PushID(int(kind));
            const std::string now = wrec.effective(key, kind).value_or("");
            const std::string stock = brec ? brec->effective(key, kind).value_or("") : std::string();
            std::string out;
            const bool sound = spec && spec->type == FieldType::Sound;
            const float playW = sound ? ImGui::CalcTextSize("Play").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x : 0.0f;
            if (valueEditor(spec, now, out, sound ? -playW : -FLT_MIN)) {
                if (!pack.set(target->format, target->section, recordName, key, kind, out)) {
                    ws_.session.error(target->format == nox::content::TableFormat::Monster || target->format == nox::content::TableFormat::SoundSet
                                          ? "A value here is one word (words join only across a '+'): the game would read the next word as a key."
                                          : "That value can't be written so the game reads it back.");
                }
            }
            const bool plainText = !spec || (spec->type != FieldType::Sound && spec->type != FieldType::Flags && spec->type != FieldType::Enum &&
                                             spec->type != FieldType::Rgb && spec->type != FieldType::Slot);
            if (now.empty() && !ImGui::IsItemActive() && plainText) {
                // (an absent key: the game reads 0 or none)
                const ImVec2 a = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + ImGui::GetStyle().FramePadding.x, a.y + ImGui::GetStyle().FramePadding.y),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), "(not set)");
            }
            if (sound && !now.empty() && !nox::util::equalsNoCase(now, "NULL")) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Play")) {
                    playSoundEvent(ws_, now);
                }
            }
            if (ImGui::IsItemHovered() && now != stock) {
                ImGui::SetTooltip("The game's own: %s", stock.empty() ? "(none)" : stock.c_str());
            }
            if (spec && !now.empty() && now != stock) { // (the game's own values aren't questioned)
                if (const std::string why = content::checkValue(*spec, now); !why.empty()) {
                    ImGui::TextColored(theme.warning, "%s", why.c_str());
                }
            }
            ImGui::PopID();
        };
        if (kinds) {
            cell(GameKind::Solo);
            cell(GameKind::Arena);
        } else {
            cell(GameKind::Both);
        }
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (spec && spec->readBy == content::ReadBy::Unknown) {
            ImGui::TextDisabled("nothing");
            ImGui::SetItemTooltip("%s", std::string(spec->docs).c_str());
        } else if (spec) {
            ImGui::TextColored(readByColour(spec->readBy), "%s", content::readByLabel(spec->readBy));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s: %s", content::reachLabel(content::reachOf(spec->readBy)), content::reachDocs(content::reachOf(spec->readBy)));
            }
        } else {
            ImGui::TextDisabled(target->format == TableFormat::GameData ? "mods" : "nothing");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(target->format == TableFormat::GameData ? "Nothing in the game reads this name; a game-mode mod can (nox.gameNumber), on the host."
                                                                          : "The game doesn't read this key.");
            }
        }
        ImGui::TableNextColumn();
        if (edited && ImGui::SmallButton("Revert")) {
            for (const GameKind k : {GameKind::Both, GameKind::Solo, GameKind::Arena}) {
                for (std::size_t e = 0; e < pack.edits().size(); ++e) {
                    const auto& fe = pack.edits()[e];
                    if (fe.format == target->format && fe.section == target->section && fe.record == recordName && nox::util::equalsNoCase(fe.key, key) &&
                        fe.kind == k) {
                        pack.revert(e);
                        break;
                    }
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    return true;
}

bool RecordPanel::thingForm(const content::Record& r)
{
    if (r.ref.list != content::RecordList::Thing) {
        return false;
    }
    content::PackEditor& pack = ws_.pack;
    const nox::things::ThingDb* work = pack.workingThings();
    const nox::things::ThingDb* base = pack.baseThings();
    const auto& theme = noxsdl::editor::themeColours();
    if (!work || !base || std::size_t(r.ref.index) >= work->things.size()) {
        ImGui::TextColored(theme.error, "thing.bin didn't load.");
        return true;
    }
    // (a copy: an edit below rebuilds the working types, which would leave a reference dangling)
    const nox::things::ThingDef t = work->things[std::size_t(r.ref.index)];
    const std::string& name = t.name;
    // (a pack's new type has no thing.bin of its own: its lines are all the pack's)
    const bool fresh = pack.isNewThing(name);
    const nox::things::ThingDef* stockT = std::size_t(r.ref.index) < base->things.size() ? &base->things[std::size_t(r.ref.index)] : nullptr;
    if (fresh) {
        const auto it = std::ranges::find_if(pack.newThings(), [&](const content::NewThing& n) { return n.name == t.name; });
        ImGui::TextColored(theme.warning, "Type %d: new in this pack, made like %s.", r.ref.index + 1, it != pack.newThings().end() ? it->like.c_str() : "?");
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove this new type")) {
            if (const std::string why = pack.removeNewThing(name); !why.empty()) {
                ws_.session.error(why);
            }
            return true; // (its record goes at the next frame's sync)
        }
        ImGui::TextDisabled("It goes after thing.bin's types. Its CLASS, XFER, FLAGS, COLLIDE and MENUICON are its own to choose.\n"
                            "A game hosted with this pack lets in only NoX Core games with the same pack (stock players can't join it).");
        // A monster's numbers (strength, speed, what it casts...) are monster.bin's, by its name.
        if (nox::things::thingClasses(t) & nox::things::ObjectClass::Monster) {
            if (const auto* monsters = pack.working(content::TableFormat::Monster); monsters && !monsters->find(name)) {
                ImGui::TextColored(theme.note, "monster.bin has no %s: as a monster it has only its own lines' numbers (no strength, speed\n"
                                               "or spells of monster.bin's). A record for it in the pack's monster.patch gives it some.",
                                   name.c_str());
            }
        }
    } else {
        ImGui::TextDisabled("Type %d: its number in thing.bin, which maps and the network use (types are never moved).", r.ref.index + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("New type like this...")) {
            std::snprintf(newThingName_, sizeof newThingName_, "%s2", t.name.c_str());
            newThingLike_ = t.name;
            ImGui::OpenPopup("New type");
        }
        ImGui::SetItemTooltip("A new type for this pack: a copy of %s that chooses its own lines, CLASS too.", t.name.c_str());
    }
    newThingPopup();
    if (!pack.packOpen()) {
        ImGui::TextColored(theme.note, "Changes go into a content pack: pick one at the top (or make one) before you save.");
    }
    // The rows: each key the type has, once (a change sets every line of it), then its pictures,
    // then keys removed by the pack.
    std::vector<std::string> keys;
    const auto addKey = [&](std::string_view k) {
        if (std::ranges::none_of(keys, [&](const std::string& o) { return nox::util::equalsNoCase(o, k); })) {
            keys.emplace_back(k);
        }
    };
    for (const auto& a : t.attributes) {
        addKey(nox::util::trim(std::string_view(a).substr(0, a.find('='))));
    }
    addKey("MENUICON");
    addKey("PRETTYIMAGE");
    for (const auto& e : pack.thingEdits()) {
        if (e.type == t.name && !e.value) {
            addKey(e.key);
        }
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("thingForm", 4, flags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f))) {
        return true;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("LIGHTINTENSITY__").x + 8);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Read by", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("server").x + 12);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Revert").x + ImGui::CalcTextSize("Remove").x + 24);
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const std::string& key = keys[i];
        const content::FieldSpec* spec = content::thingField(key);
        const content::ThingEdit* edit = pack.thingEditOf(t.name, key);
        const auto now = nox::content::thingValue(t, key);
        const auto stock = stockT ? nox::content::thingValue(*stockT, key) : std::nullopt;
        const bool locked = nox::content::lockedThingKey(key) && !fresh;
        const bool image = nox::util::equalsNoCase(key, "MENUICON") || nox::util::equalsNoCase(key, "PRETTYIMAGE");
        if (image && !now && !stock && !edit) {
            continue; // (no picture: nothing to show)
        }
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (edit) {
            ImGui::TextColored(theme.highlightText, "%s *", key.c_str());
        } else if (!spec) {
            ImGui::TextColored(theme.unknown, "%s", key.c_str());
        } else {
            ImGui::TextUnformatted(key.c_str());
        }
        if (ImGui::IsItemHovered()) {
            std::string tip = spec ? std::string(spec->docs) : std::string("NoX Core doesn't read this key.");
            if (spec) {
                tip += std::string("\n\nReach: ") + content::reachLabel(content::reachOf(*spec)) + ". " + content::reachDocs(content::reachOf(*spec));
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::TableNextColumn();
        if (edit && !edit->value) {
            ImGui::TextDisabled("(removed; the game's own: %s)", stock.value_or("none").c_str());
        } else if (nox::util::equalsNoCase(key, "DRAW")) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s (%zu bytes of animation data; shown in Preview and Sprites)", t.draw ? t.draw->type.c_str() : "?",
                                t.draw ? t.draw->data.size() : std::size_t(0));
        } else if (locked && !edit) {
            // (thing.bin's own types keep these: only a new one chooses its own)
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(now.value_or("").c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(locked)");
            ImGui::SetItemTooltip("A type's %s never changes: every player's game and the maps read its objects by it.\n"
                                  "A new type (New type like this...) chooses its own.",
                                  key.c_str());
        } else {
            std::string out;
            const bool sound = spec && spec->type == FieldType::Sound;
            const float side = image ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x
                               : sound ? ImGui::CalcTextSize("Play").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x
                                       : 0.0f;
            content::FieldSpec text;
            text.key = key;
            text.type = FieldType::Text;
            if (valueEditor(sound ? spec : &text, now.value_or(""), out, side > 0 ? -side : -FLT_MIN)) {
                if (const std::string why = pack.setThing(t.name, key, out); !why.empty()) {
                    ws_.session.error(why);
                }
            }
            if (ImGui::IsItemHovered() && now != stock) {
                ImGui::SetTooltip("The game's own: %s", stock.value_or("(none)").c_str());
            }
            if (image && now) {
                ImGui::SameLine();
                const float s = ImGui::GetFrameHeight();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(s, s));
                drawFit(ImGui::GetWindowDrawList(), ws_.session.sd().get(std::atoi(now->c_str())), p, ImVec2(s, s));
            }
            if (sound && now && !now->empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Play")) {
                    playSoundEvent(ws_, *now);
                }
            }
            if (spec && now && now != stock) {
                if (const std::string why = content::checkValue(*spec, *now); !why.empty()) {
                    ImGui::TextColored(theme.warning, "%s", why.c_str());
                }
            }
        }
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (spec && spec->readBy == content::ReadBy::Unknown) {
            ImGui::TextDisabled("nothing");
            ImGui::SetItemTooltip("%s", std::string(spec->docs).c_str());
        } else if (spec) {
            const content::Reach reach = content::reachOf(*spec);
            ImGui::TextColored(reach == content::Reach::Structural ? theme.error : readByColour(spec->readBy), "%s",
                               reach == content::Reach::Structural ? "both!" : content::readByLabel(spec->readBy));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s: %s", content::reachLabel(reach), content::reachDocs(reach));
            }
        } else {
            ImGui::TextDisabled("nothing");
        }
        ImGui::TableNextColumn();
        if (edit && ImGui::SmallButton("Revert")) {
            for (std::size_t k = 0; k < pack.thingEdits().size(); ++k) {
                if (&pack.thingEdits()[k] == edit) {
                    pack.revertThing(k);
                    break;
                }
            }
        } else if (!edit && now && !nox::util::equalsNoCase(key, "DRAW") && !locked) {
            if (ImGui::SmallButton("Remove")) {
                if (const std::string why = pack.removeThing(t.name, key); !why.empty()) {
                    ws_.session.error(why);
                }
            }
            ImGui::SetItemTooltip("Removes every %s line of this type", key.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    // A line the type doesn't have yet.
    static char newKey[64] = {};
    static char newValue[256] = {};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("LIGHTINTENSITY__").x);
    if (ImGui::BeginCombo("##newKey", newKey[0] ? newKey : "Add a line...")) {
        for (const auto& f : content::thingFields()) {
            if (std::ranges::any_of(keys, [&](const std::string& k) { return nox::util::equalsNoCase(k, f.key); }) || f.key == "DRAW" ||
                ((f.key == "MENUICON" || nox::content::lockedThingKey(f.key)) && !fresh)) { // (saves keep only the types with one: never added; locked keys)
                continue;
            }
            if (ImGui::Selectable(std::string(f.key).c_str())) {
                std::snprintf(newKey, sizeof newKey, "%s", std::string(f.key).c_str());
            }
            ImGui::SetItemTooltip("%s", std::string(f.docs).c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Add").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    const bool enter = ImGui::InputTextWithHint("##newValue", "value", newValue, sizeof newValue, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!newKey[0]);
    if (ImGui::Button("Add") || (enter && newKey[0])) {
        if (const std::string why = pack.setThing(t.name, newKey, newValue); !why.empty()) {
            ws_.session.error(why); // (kept in the boxes, to be put right)
        } else {
            newKey[0] = 0;
            newValue[0] = 0;
        }
    }
    ImGui::EndDisabled();
    return true;
}
void RecordPanel::newThingPopup()
{
    if (!ImGui::BeginPopupModal("New type", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const auto& theme = noxsdl::editor::themeColours();
    ImGui::Text("A new object type for this pack, made like %s: its lines, animation and pictures, which you then change.", newThingLike_.c_str());
    ImGui::TextColored(theme.warning, "A pack that adds types changes the game's list of types, which games send each other by number:\n"
                                      "a game hosted with it lets in only NoX Core games with the same pack. Stock players, and NoX Core\n"
                                      "players without the pack, can't join it.");
    ImGui::SetNextItemWidth(320 * ws_.session.scale());
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
        newThingError_.clear();
    }
    const bool enter = ImGui::InputText("Name", newThingName_, sizeof newThingName_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("Letters, digits and '_', starting with a letter; a name no type has.");
    if (!newThingError_.empty()) {
        ImGui::TextColored(theme.error, "%s", newThingError_.c_str());
    }
    if (ImGui::Button("Add") || enter) {
        newThingError_ = ws_.pack.addThing(newThingName_, newThingLike_);
        if (newThingError_.empty()) {
            ws_.selectAfterSync = std::string(nox::util::trim(newThingName_)); // (listed, then selected, at the next frame's sync)
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool RecordPanel::stringForm(const content::Record& r)
{
    if (r.ref.list != content::RecordList::String) {
        return false;
    }
    content::PackEditor& pack = ws_.pack;
    const auto* work = pack.workingStrings();
    const auto* base = pack.baseStrings();
    const auto& theme = noxsdl::editor::themeColours();
    if (!work || !base) {
        ImGui::TextColored(theme.error, "nox.csf didn't load.");
        return true;
    }
    const nox::content::CsfEntry* stock = base->find(r.name);
    const nox::content::CsfEntry* now = work->find(r.name);
    const content::StringEdit* edit = pack.stringEditOf(r.name);
    if (!pack.packOpen()) {
        ImGui::TextColored(theme.note, "Changes go into a content pack: pick one at the top (or make one) before you save.");
    }
    ImGui::TextDisabled("Texts the host sends as text (a map script's Print / PrintToAll / Chat, the server's messages) follow the host's pack, for\neveryone; the rest (menus, the HUD, names, lines sent by their label) come from each player's own game: only those who install the\npack see them.");
    if (!now) {
        ImGui::TextDisabled("(removed by the pack; the game's own: %s)", stock && !stock->variants.empty() ? nox::strings::toUtf8(stock->variants.front().shown()).c_str() : "");
        if (edit && ImGui::Button("Revert")) {
            pack.revertString(static_cast<std::size_t>(edit - pack.stringEdits().data()));
        }
        return true;
    }
    // The variants (the game picks one at random each time it shows the label).
    std::vector<std::u16string> texts;
    for (const auto& v : now->variants) {
        texts.push_back(v.text());
    }
    std::optional<std::string> speech = edit ? edit->speech : std::nullopt;
    bool changed = false;
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    for (std::size_t i = 0; i < texts.size(); ++i) {
        ImGui::PushID(int(i));
        ImGui::SeparatorText(texts.size() == 1 ? "Text" : std::format("Variant {}", i + 1).c_str());
        std::string utf8 = nox::strings::toUtf8(texts[i]);
        char buf[4096];
        std::snprintf(buf, sizeof buf, "%s", utf8.c_str());
        const int lines = std::clamp(int(std::ranges::count(utf8, '\n')) + 2, 2, 8);
        ImGui::InputTextMultiline("##t", buf, sizeof buf, ImVec2(-FLT_MIN, lineH * float(lines)));
        if (ImGui::IsItemDeactivatedAfterEdit() && std::string(buf) != utf8) {
            texts[i] = nox::strings::fromUtf8(buf);
            changed = true;
        }
        // Longer than the game's own by much: it may not fit where the game shows it.
        if (stock && i < stock->variants.size()) {
            const std::size_t was = stock->variants[i].shown().size();
            const std::size_t is = texts[i].size();
            if (was > 0 && is > was + std::max<std::size_t>(8, was / 4)) {
                ImGui::TextColored(theme.warning, "%zu characters, the game's own %zu: check that it fits where the game shows it.", is, was);
            }
        }
        if (i < now->variants.size() && now->variants[i].wide()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Speech file:");
            ImGui::SameLine();
            ImGui::TextUnformatted(now->variants[i].speech.c_str());
        }
        if (texts.size() > 1 && ImGui::SmallButton("Remove this variant")) {
            texts.erase(texts.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add a variant")) {
        texts.push_back(u"");
        changed = true;
    }
    ImGui::SameLine();
    if (edit && ImGui::Button("Revert")) {
        pack.revertString(static_cast<std::size_t>(edit - pack.stringEdits().data()));
        return true;
    }
    if (stock) {
        ImGui::SameLine();
        if (ImGui::Button("Remove the label")) {
            pack.removeString(r.name);
            return true;
        }
        ImGui::SetItemTooltip("The game then shows nothing (or the label's name) where it used it.");
    }
    if (changed) {
        if (const std::string why = pack.setStrings(r.name, std::move(texts), speech); !why.empty()) {
            ws_.session.error(why);
        }
    }
    return true;
}

bool RecordPanel::soundForm(const content::Record& r)
{
    const bool sample = r.ref.list == content::RecordList::Sample;
    const bool dialog = r.ref.list == content::RecordList::File && r.ref.kind == content::FileKind::Dialog;
    if (!sample && !dialog) {
        return false;
    }
    content::PackEditor& pack = ws_.pack;
    const auto& theme = noxsdl::editor::themeColours();
    const auto folder = sample ? content::SoundEdit::Audio : content::SoundEdit::Dialog;
    const std::string name = sample ? r.name : fs::path(r.name).stem().string();
    const content::SoundEdit* edit = pack.soundEditOf(folder, name);
    if (dialog) {
        fileView(r);
        ImGui::Separator();
    } else {
        form(fields_, true);
    }
    // The game's own.
    if (ImGui::Button("Play")) {
        if (sample) {
            playSample(ws_, r.name);
        } else if (const fs::path* p = recordFile(ws_.files, r.ref)) {
            playSoundFile(ws_, *p);
        }
    }
    ImGui::SetItemTooltip("The game's own %s.", sample ? "sample, from Audio.bag" : "line, from the dialog folder");
    ImGui::SeparatorText("In the content pack");
    ImGui::TextDisabled("Played by the players' own games (client-visible): only those who install the pack hear it.");
    if (edit) {
        ImGui::TextColored(theme.highlightText, "Replaced by %s%s", utf8(edit->source).c_str(), edit->saved ? "" : " (copied in when the pack is saved)");
        if (ImGui::Button("Play the pack's")) {
            playSoundFile(ws_, edit->source);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            pack.revertSound(static_cast<std::size_t>(edit - pack.soundEdits().data()));
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(edit ? "Replace again..." : "Replace with a .wav...")) {
        static constexpr SDL_DialogFileFilter kWav[] = {{"Wave sound", "wav"}};
        soundTarget_ = {folder == content::SoundEdit::Audio, name};
        ws_.session.dialogs().request(kDialogReplaceSound, Dialogs::Open, ws_.session.window(), ws_.session.settings().extractDir, kWav, 1);
    }
    ImGui::SetItemTooltip("The .wav goes into the pack's %s folder as %s.wav (PCM, IMA ADPCM or MP3 in a WAV; the original game's own\naudio folder plays only PCM ones properly).",
                          sample ? "audio" : "dialog", name.c_str());
    if (const auto picked = ws_.session.dialogs().take(kDialogReplaceSound); picked && soundTarget_) {
        const fs::path source = pathFromUtf8(*picked);
        try {
            const auto pcm = nox::audio::decodeFile(nox::readFileBytes(source)); // (one the game can't play isn't taken)
            pack.setSound(soundTarget_->first ? content::SoundEdit::Audio : content::SoundEdit::Dialog, soundTarget_->second, source);
            ws_.session.setStatus(std::format("{}: {:.1f} s, {} Hz{}", source.filename().string(), double(pcm.frames()) / std::max(1u, pcm.rate), pcm.rate,
                                              pcm.channels == 2 ? " stereo" : ""));
        } catch (const std::exception& ex) {
            ws_.session.error(std::string("That file can't be played: ") + ex.what());
        }
        soundTarget_.reset();
    }
    return true;
}
} // namespace videotools::gui
