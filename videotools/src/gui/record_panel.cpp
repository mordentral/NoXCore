// The record editor (panels.hpp): for now the record as its file holds it, read only.
#include "gui/panels.hpp"

#include "editor/theme_colours.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>

#include <format>
#include <system_error>

namespace fs = std::filesystem;

namespace videotools::gui {

using content::RecordList;

namespace {

/// The loose file a File record stands for.
const fs::path* fileOf(const content::GameFiles& files, const content::RecordRef& ref)
{
    const std::vector<fs::path>* list = nullptr;
    switch (ref.kind) {
    case content::FileKind::Dialog: list = &files.dialog(); break;
    case content::FileKind::Windows: list = &files.windows(); break;
    case content::FileKind::Maps: list = &files.maps(); break;
    case content::FileKind::Rules: list = &files.rules(); break;
    case content::FileKind::Characters: list = &files.characters(); break;
    case content::FileKind::Mods: list = &files.mods(); break;
    case content::FileKind::Text: list = &files.texts(); break;
    case content::FileKind::Fonts: list = &files.fonts(); break;
    case content::FileKind::Palette: list = &files.palettes(); break;
    default: return nullptr;
    }
    return ref.index >= 0 && std::size_t(ref.index) < list->size() ? &(*list)[std::size_t(ref.index)] : nullptr;
}

bool isText(const fs::path& p)
{
    const std::string ext = nox::util::lowered(p.extension().string());
    return ext == ".txt" || ext == ".rul" || ext == ".wnd" || ext == ".json" || ext == ".luau";
}

} // namespace

const fs::path* recordFile(const content::GameFiles& files, const content::RecordRef& ref)
{
    return fileOf(files, ref);
}

void RecordPanel::form(const std::vector<content::Field>& fields, bool fit)
{
    if (fields.empty()) {
        ImGui::TextDisabled("This record has no fields to show.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    const float height = fit ? float(fields.size() + 1) * ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().CellPadding.y * 2 : 0.0f;
    if (!ImGui::BeginTable("fields", 2, flags, ImVec2(0, height))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("MELEE_ATTACK_POISON_STRENGTH").x + 8);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(f.label.c_str());
        if (!f.note.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", f.note.c_str());
        }
        ImGui::TableNextColumn();
        // Read only, but selectable: values can be copied. A sound gets Play beside it.
        const bool event = f.plays == content::Field::Plays::SoundEvent && !f.value.empty() && !nox::util::equalsNoCase(f.value, "NULL");
        const float playW = ImGui::CalcTextSize("Play").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(event ? -playW : -FLT_MIN);
        std::string v = f.value;
        ImGui::InputText("##v", v.data(), v.size() + 1, ImGuiInputTextFlags_ReadOnly);
        if (!f.note.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", f.note.c_str());
        }
        if (event) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Play")) {
                playSoundEvent(ws_, f.value);
            }
            ImGui::SetItemTooltip("One of %s's files (the game picks one at random; this plays them in turn).", f.value.c_str());
        }
        for (std::size_t s = 0; s < f.samples.size(); ++s) {
            ImGui::PushID(int(s));
            if (s > 0) {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(("Play " + f.samples[s]).c_str())) {
                playSample(ws_, f.samples[s]);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void RecordPanel::fileView(const content::Record& r)
{
    const fs::path* p = fileOf(ws_.files, r.ref);
    if (!p) {
        return;
    }
    std::error_code ec;
    const auto size = fs::file_size(*p, ec);
    ImGui::TextDisabled("%s", utf8(*p).c_str());
    if (!ec) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%ju bytes)", static_cast<std::uintmax_t>(size));
    }
    if (ImGui::SmallButton("Show in folder")) {
        SDL_OpenURL(("file:///" + utf8(p->parent_path().generic_u8string())).c_str());
    }
    if (!fileText_.empty()) {
        ImGui::InputTextMultiline("##file", fileText_.data(), fileText_.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
    }
}

void RecordPanel::draw()
{
    const content::Record* r = ws_.selected ? ws_.index.find(*ws_.selected) : nullptr;
    if (!r) {
        ImGui::TextDisabled("Pick a record in Content (or search for one).");
        return;
    }
    if (shown_ != r->ref) {
        shown_ = r->ref;
        fields_ = content::describe(ws_.files, r->ref);
        text_ = content::rawText(ws_.files, r->ref);
        fileText_.clear();
        if (r->ref.list == RecordList::File) {
            if (const fs::path* p = fileOf(ws_.files, r->ref); p && isText(*p)) {
                try {
                    const auto bytes = nox::readFileBytes(*p);
                    fileText_.assign(bytes.begin(), bytes.end());
                } catch (const std::exception& ex) {
                    fileText_ = std::string("(can't read it: ") + ex.what() + ")";
                }
            }
        }
    }
    // The path: file > section > record.
    const char* file = content::fileKindFile(r->ref.kind);
    ImGui::TextDisabled("%s  >", file);
    ImGui::SameLine();
    ImGui::TextUnformatted(r->name.c_str());
    if (!r->detail.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", r->detail.c_str());
    }
    if (ws_.audio.playing()) { // (on this line: nothing below moves when a sound starts)
        ImGui::SameLine();
        playingBar(ws_);
    }
    ImGui::Separator();
    if (r->ref.list == RecordList::File) {
        if (!soundForm(*r) && !characterForm(*r) && !textForm(*r) && !fontForm(*r) && !modForm(*r)) {
            fileView(*r);
        }
        return;
    }
    if (ImGui::BeginTabBar("recordTabs")) {
        if (ImGui::BeginTabItem("Form")) {
            if (!tableForm(*r) && !thingForm(*r) && !stringForm(*r) && !soundForm(*r)) {
                form(fields_);
            }
            ImGui::EndTabItem();
        }
        // The record as its file writes it (a text table's: as the pack has it).
        std::string text = tableText(*r);
        if (text.empty()) {
            text = text_;
        }
        if (!text.empty() && ImGui::BeginTabItem("Text")) {
            ImGui::InputTextMultiline("##text", text.data(), text.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace videotools::gui
