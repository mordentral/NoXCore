// The in-game chat line (chat_entry.hpp): GuiChat.wnd's entry field driven by the window system.
#include "chat_entry.hpp"

#include "noxcore/strings/string_table.hpp"

#include <algorithm>

namespace noxsdl {

namespace {

constexpr std::int32_t kField = 9201; // GuiChat.wnd's entry field

} // namespace

ChatEntry::ChatEntry(gui::Resources res, const std::filesystem::path& windowDir,
                     std::function<void(const std::string& text, bool team)> send)
    : gui_(std::move(res)), send_(std::move(send))
{
    window_ = gui_.load(windowDir / "GuiChat.wnd", nullptr);
    field_ = window_->find(kField);
    window_->show(false);
}

void ChatEntry::open(SDL_Window* window, bool team)
{
    if (!field_) {
        return;
    }
    team_ = team;
    open_ = true;
    field_->entry.clear();
    window_->show(true);
    gui_.setFocus(field_);
    SDL_StartTextInput(window);
}

void ChatEntry::close(SDL_Window* window)
{
    open_ = false;
    window_->show(false);
    gui_.setFocus(nullptr);
    SDL_StopTextInput(window); // the game's keys are keys again
}

void ChatEntry::draw(SDL_Renderer* renderer, int width, int height)
{
    if (!open_) {
        return;
    }
    // The box grows with what's typed, 100 to 320 wide, centred across at two-thirds down, in the
    // menus' 640 x 480 layout.
    gui::Painter p(nullptr, gui_.resources(), width, height);
    const int textWidth = p.textWidth(nox::strings::toUtf8(field_->entry), window_->largeFont);
    const int w = std::clamp(textWidth + 10, 100, 320);
    window_->rect = {(640 - w) / 2, 480 * 2 / 3, w, 20};
    field_->rect = {0, 0, w, 20};
    gui_.draw(renderer, width, height);
}

bool ChatEntry::handleEvent(SDL_Window* window, const SDL_Event& event, int width, int height)
{
    if (!open_) {
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
            const std::string text = nox::strings::toUtf8(field_->entry);
            const bool team = team_;
            close(window);
            // Leading spaces skipped; nothing sent for a blank line (CliNet_SendChatText).
            const auto start = text.find_first_not_of(' ');
            if (start != std::string::npos && send_) {
                send_(text.substr(start), team);
            }
            return true;
        }
        if (event.key.key == SDLK_ESCAPE) {
            close(window);
            return true;
        }
        gui_.handleEvent(event, width, height); // Backspace
        return true;
    }
    if (event.type == SDL_EVENT_TEXT_INPUT) {
        gui_.handleEvent(event, width, height);
        return true;
    }
    return event.type == SDL_EVENT_KEY_UP; // (the keyboard is ours; the mouse still plays)
}

} // namespace noxsdl
