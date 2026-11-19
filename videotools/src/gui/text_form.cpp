// The host's text files in the record editor (panels.hpp): a rule set, the map cycle, the message
// of the day, a window file (text only) or any other text, edited as text, with what the game's
// own readers make of it beside it. Saved as a new file, never into the game's folders.
#include "gui/panels.hpp"

#include "editor/theme_colours.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>

#include <format>
#include <system_error>

namespace fs = std::filesystem;

namespace videotools::gui {

namespace {

int resizeString(ImGuiInputTextCallbackData* d)
{
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<std::size_t>(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}

bool isTextFile(const fs::path& p)
{
    const std::string ext = nox::util::lowered(p.extension().string());
    return ext == ".txt" || ext == ".rul" || ext == ".wnd";
}

} // namespace

bool RecordPanel::textForm(const content::Record& r)
{
    if (r.ref.list != content::RecordList::File) {
        return false;
    }
    const fs::path* file = recordFile(ws_.files, r.ref);
    if (!file || !isTextFile(*file)) {
        return false;
    }
    textEditor(*file);
    return true;
}

void RecordPanel::textEditor(const fs::path& path)
{
    const fs::path* file = &path;
    const auto& theme = noxsdl::editor::themeColours();
    if (textFile_ != *file) {
        textFile_ = *file;
        textKind_ = content::textKindOf(*file);
        try {
            const auto bytes = nox::readFileBytes(*file);
            textOriginal_.assign(bytes.begin(), bytes.end());
        } catch (const std::exception& ex) {
            textOriginal_.clear();
            ws_.session.setStatus(nox::l10n::format(NOX_TR("content.text_form.cant_read", "Can't read it: {error}"), {{"error", ex.what()}}));
        }
        textEdit_ = textOriginal_;
        textReport_ = content::checkText(textKind_, textEdit_, ws_.files, textFile_);
        textStale_ = false;
    }
    ImGui::TextDisabled("%s (%s)", utf8(*file).c_str(), content::textKindName(textKind_));
    ImGui::SameLine();
    if (ImGui::SmallButton(NOX_TRI("content.common.show_in_folder", "Show in folder"))) {
        SDL_OpenURL(("file:///" + utf8(file->parent_path().generic_u8string())).c_str());
    }

    // The text (top), what the game reads (below).
    const bool checked = textKind_ != content::TextKind::Other;
    const float avail = ImGui::GetContentRegionAvail().y;
    const float editorH = checked ? avail * 0.6f : avail - ImGui::GetFrameHeightWithSpacing() * 2;
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput;
    if (ImGui::InputTextMultiline("##text", textEdit_.data(), textEdit_.capacity() + 1, ImVec2(-FLT_MIN, editorH), flags, resizeString, &textEdit_)) {
        textStale_ = true;
        textEditedAt_ = ImGui::GetTime();
    }
    // Checked again a moment after typing stops (the rule check writes a scratch file).
    if (textStale_ && ImGui::GetTime() - textEditedAt_ > 0.4) {
        textReport_ = content::checkText(textKind_, textEdit_, ws_.files, textFile_);
        textStale_ = false;
    }
    const bool changed = textEdit_ != textOriginal_;
    if (changed) {
        ImGui::TextColored(theme.highlightText, "%s", NOX_TRC("content.text_form.changed", "Changed (not saved)."));
        ImGui::SameLine();
    }
    if (ImGui::Button(NOX_TRI("content.text_form.save_as", "Save as..."))) {
        static const SDL_DialogFileFilter kAll[] = {{NOX_TRC("content.text_form.filter_text", "Text"), "txt;rul;wnd;json"}};
        const std::string start = ws_.session.settings().saveDir.empty() ? std::string() : ws_.session.settings().saveDir + "/" + utf8(file->filename());
        ws_.session.dialogs().request(kDialogSaveText, Dialogs::Save, ws_.session.window(), start, kAll, 1);
    }
    ImGui::SetItemTooltip("%s", NOX_TRC("content.text_form.save_as_tip", "Writes a new file (never over this one, never into the game's folder). To use it, put it in a copy of the game."));
    if (changed) {
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("content.text_form.revert", "Revert"))) {
            textEdit_ = textOriginal_;
            textStale_ = true;
            textEditedAt_ = 0;
        }
    }
    if (const auto picked = ws_.session.dialogs().take(kDialogSaveText)) {
        const fs::path target = pathFromUtf8(*picked);
        if (const std::string why = content::checkSaveOutsideGame(target, *file, ws_.session.gameDir()); !why.empty()) {
            ws_.session.error(nox::l10n::format(NOX_TR("content.common.not_there", "Not there: {why}"), {{"why", why}}));
        } else {
            const auto save = [this, target] {
                try {
                    nox::writeFileBytes(target, std::vector<std::uint8_t>(textEdit_.begin(), textEdit_.end()));
                    ws_.session.settings().saveDir = utf8(target.parent_path());
                    ws_.session.setStatus(nox::l10n::format(NOX_TR("content.common.saved_file", "Saved {file}"), {{"file", utf8(target)}}), target.parent_path());
                } catch (const std::exception& ex) {
                    ws_.session.error(nox::l10n::format(NOX_TR("content.common.cant_save", "Couldn't save it: {error}"), {{"error", ex.what()}}));
                }
            };
            std::error_code ec;
            if (fs::exists(target, ec)) {
                ws_.session.confirm(NOX_TR("content.common.replace_title", "Replace it?"),
                                    nox::l10n::format(NOX_TR("content.common.replace_ask", "{file} is there already. Replace it?"), {{"file", utf8(target)}}), save);
            } else {
                save();
            }
        }
    }
    if (!checked) {
        return;
    }
    ImGui::SeparatorText(NOX_TRI("content.text_form.what_game_reads", "What the game reads"));
    if (ImGui::BeginChild("report", ImVec2(0, 0))) {
        for (const auto& p : textReport_.problems) {
            ImGui::TextColored(theme.warning, "%s", p.c_str());
        }
        if (textReport_.problems.empty()) {
            ImGui::TextDisabled("%s", NOX_TRC("content.text_form.nothing_wrong", "Nothing it would drop or misread."));
        }
        if (!textReport_.facts.empty() && ImGui::BeginTable("facts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("[KING OF THE REALM]__").x);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& [label, value] : textReport_.facts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(label.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

} // namespace videotools::gui
