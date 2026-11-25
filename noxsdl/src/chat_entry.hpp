// Typing a chat line in a game (GuiChat.wnd; client_chatStart): Chat
// (Enter) or TeamChat (Backspace) opens the entry field across the screen at two-thirds of its
// height; it has the keyboard while open. Enter sends the line (MSG_TEXT_MESSAGE, to the team
// for TeamChat; leading spaces skipped and a blank line not sent, as CliNet_SendChatText
// CliNet_SendChatText) and closes it; Escape closes it without sending. The box grows with the text, 100 to
// 320 wide.
#pragma once

#include "gui.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <string>

namespace noxsdl {

class ChatEntry {
public:
    /// `send` gets the typed line (UTF-8) and whether it's for the team.
    ChatEntry(gui::Resources res, const std::filesystem::path& windowDir,
              std::function<void(const std::string& text, bool team)> send);

    void open(SDL_Window* window, bool team);
    void close(SDL_Window* window);
    bool isOpen() const { return open_; }
    /// (noxcpp) The line typed so far (UTF-8), and whether it goes to the team (the pad's keyboard
    /// shows it).
    std::string text() const;
    bool team() const { return team_; }
    /// (noxcpp) `line` sent in place of what's typed, as Enter sends a line (the pad's quick chat).
    void sendLine(SDL_Window* window, const std::string& line);

    void draw(SDL_Renderer* renderer, int width, int height);
    /// While open it takes the keyboard (and typing); returns whether it took the event.
    bool handleEvent(SDL_Window* window, const SDL_Event& event, int width, int height);

private:
    void enter(SDL_Window* window);

    gui::Gui gui_;
    std::function<void(const std::string&, bool)> send_;
    gui::Window* window_ = nullptr;
    gui::Window* field_ = nullptr;
    bool open_ = false;
    bool team_ = false;
};

} // namespace noxsdl
