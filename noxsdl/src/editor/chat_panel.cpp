// The editor's Chat panel: a session's in-game chat (the model:
// chat_log.hpp), a box to chat into the game as a player does, and its tab marked with the count of
// lines that came while it wasn't seen.
#include "editor/map_editor.hpp"
#include "editor/theme_colours.hpp"

#include <imgui.h>
#include <imgui_internal.h> // (a layout without the panel: docked beside History)

#include <algorithm>
#include <cstring>
#include <string>

namespace noxsdl {

void MapEditor::addChat(editor::ChatLine line)
{
    line.timeMs = static_cast<std::uint32_t>(SDL_GetTicks());
    // Closed (or outside a session, where the panel isn't shown): a short line in the status bar.
    if (!showChat_ || !session_) {
        std::string said = line.speaker.empty() || line.kind == editor::ChatLine::Kind::System ? line.text : line.speaker + ": " + line.text;
        if (said.size() > 90) said = said.substr(0, 87) + "...";
        status("Chat - " + said + (session_ ? "  (Window > Chat)" : ""));
    }
    chat_.add(std::move(line));
}

void MapEditor::chatPanel()
{
    // The title carries the unread count; "###Chat" keeps the window (its dock, its place in the
    // layout) the same whatever the title says.
    const std::size_t unread = chat_.unread();
    const std::string title = unread > 0 ? "Chat (" + std::to_string(unread) + ")###Chat" : std::string("Chat###Chat");
    // The tab of a docked window is drawn in the colours current at its Begin: lines it hasn't
    // shown mark its tab amber (and the dot of ImGuiWindowFlags_UnsavedDocument) till it's seen.
    int pushed = 0;
    if (unread > 0) {
        const editor::ThemeColours& tc = editor::themeColours();
        ImGui::PushStyleColor(ImGuiCol_Tab, tc.chatTab);
        ImGui::PushStyleColor(ImGuiCol_TabDimmed, tc.chatTabDimmed);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, tc.chatTabHovered);
        ImGui::PushStyleColor(ImGuiCol_TitleBg, tc.chatTabDimmed);
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, tc.chatTab);
        pushed = 5;
    }
    if (chatFocus_) {
        chatFocus_ = false;
        ImGui::SetNextWindowFocus();
    }
    // A layout from before the panel (no place of its own in it): tabbed with Session at first.
    if (const ImGuiWindow* session = ImGui::FindWindowByName("Session"); session && session->DockId != 0) {
        ImGui::SetNextWindowDockID(session->DockId, ImGuiCond_FirstUseEver);
    }
    const bool open = ImGui::Begin(title.c_str(), &showChat_, unread > 0 ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None);
    ImGui::PopStyleColor(pushed);
    // Seen while its contents are drawn (its tab in front, not collapsed): what comes is read.
    chat_.setViewing(open && showChat_);
    if (!open) {
        ImGui::End();
        return;
    }
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float inputH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("##chatlines", ImVec2(0, -inputH), ImGuiChildFlags_Borders)) {
        // Follows the end unless scrolled up to read back.
        const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        if (chat_.lines().empty()) {
            ImGui::TextDisabled("The game's chat shows here: what players say (team lines too), who comes and goes.");
        }
        for (const auto& l : chat_.lines()) {
            switch (l.kind) {
            case editor::ChatLine::Kind::System:
                ImGui::PushStyleColor(ImGuiCol_Text, editor::themeColours().chatSystem);
                ImGui::TextWrapped("%s", l.text.c_str());
                ImGui::PopStyleColor();
                break;
            case editor::ChatLine::Kind::Team:
            case editor::ChatLine::Kind::Chat: {
                const bool team = l.kind == editor::ChatLine::Kind::Team;
                ImGui::TextColored(team ? editor::themeColours().chatTeam : editor::themeColours().chatAll, "%s%s:", team ? "(team) " : "",
                                   l.speaker.empty() ? "Someone" : l.speaker.c_str());
                ImGui::SameLine(0, 6 * scale);
                ImGui::TextWrapped("%s", l.text.c_str());
                break;
            }
            }
        }
        if (chat_.total() != chatSeenTotal_ && atEnd) {
            ImGui::SetScrollHereY(1.0f);
        }
        chatSeenTotal_ = chat_.total();
    }
    ImGui::EndChild();
    // The box: a line into the game as a player chatting sends it (MSG_TEXT_MESSAGE), stock
    // clients included; Team: to our team only, as the game's team chat.
    const bool canSay = static_cast<bool>(sessionHooks_.say);
    ImGui::BeginDisabled(!canSay);
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 120 * scale));
    bool send = ImGui::InputTextWithHint("##chatsay", canSay ? "Say something to the players..." : "Not in the game", chatInput_, sizeof chatInput_,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (send) ImGui::SetKeyboardFocusHere(-1); // (the box keeps the keyboard after Enter)
    ImGui::SameLine();
    ImGui::Checkbox("Team", &chatTeam_);
    ImGui::SameLine();
    send = ImGui::Button("Send") || send;
    ImGui::EndDisabled();
    if (send && canSay && chatInput_[0] != 0) {
        sessionHooks_.say(chatInput_, chatTeam_);
        chatInput_[0] = 0;
    }
    ImGui::End();
}

} // namespace noxsdl
