// The game's console (F1, ToggleConsole; guicon.c: gui_console_Create): across the
// top half of the screen, the lines printed so far over a translucent black (TranslucentConsole),
// and the line being typed at its bottom. Enter runs the line (echoed first) through the command
// table (server_parseCmdText over the game's command table): words matched one by one
// against the commands and their sub-commands, case-insensitively; an unknown command prints
// ParseCmd.c:typehelp. A command the host alone may run prints ParseCmd.c:ServerOnly on a client.
// Escape or F1 closes it. Lines take one of the 17 colours of the game's palette.
//
// The commands and what they do are given by main.cpp (Command); the console keeps the lines,
// the typing (with Up / Down through what was typed before, Page Up / Page Down and the wheel to
// scroll) and the parsing.
#pragma once

#include "text.hpp"

#include "noxcore/strings/string_table.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

/// The game's 17 console colours (the colours loadDefColor makes, in order).
enum ConsoleColor : std::uint8_t {
    Black,
    DarkGrey,
    Grey,
    LightGrey,
    White,
    DarkRed,
    Red,
    LightRed,
    DarkGreen,
    Green,
    LightGreen,
    DarkBlue,
    Blue,
    LightBlue,
    DarkYellow,
    Yellow,
    LightYellow,
};
/// A palette colour's RGB (the console's, the scoreboard's).
SDL_Color paletteColor(std::uint8_t index);

class Console {
public:
    using Args = std::vector<std::string>;
    struct Command {
        std::string name;
        /// Who may run it (the table's flags): 1 the host, 2 a client joined to a server; 0x10 a
        /// cheat; 0x100 noxsdl's own (not the game's).
        unsigned flags = 3;
        std::string help; ///< a ParseCmd.c string id, or the text itself for noxsdl's
        std::function<void(const Args& args)> run; ///< the words after the command
        std::vector<Command> sub;
    };
    static constexpr unsigned kHost = 1, kClient = 2, kCheat = 0x10, kNoxsdl = 0x100;

    Console(Font* font, const nox::strings::StringTable* strings) : font_(font), strings_(strings) {}

    void setCommands(std::vector<Command> commands) { commands_ = std::move(commands); }
    const std::vector<Command>& commands() const { return commands_; }
    /// Whether commands run as the host (in the viewer) or as a client joined to a server.
    void setClient(bool client) { client_ = client; }
    bool client() const { return client_; }

    void toggle(SDL_Window* window);
    void close(SDL_Window* window);
    bool isOpen() const { return open_; }

    void print(std::string_view text, ConsoleColor color = White);
    /// A string-table text (or the id itself if there's none), with %s / %d filled in order.
    std::string text(std::string_view id, std::initializer_list<std::string_view> values = {}) const;
    void clear() { lines_.clear(); scroll_ = 0; }
    /// Runs a line as if typed (echoed first).
    void run(std::string_view line);
    /// The line being run (for a command passed whole to the server we host).
    const std::string& currentLine() const { return currentLine_; }
    /// Writes every line printed from now on to `path` too (empty: stop).
    void logTo(const std::string& path);

    void draw(SDL_Renderer* renderer, int width, int height, float scale, bool translucent);
    /// While open it takes the keyboard and typing; returns whether it took the event.
    bool handleEvent(SDL_Window* window, const SDL_Event& event);

private:
    struct Line {
        std::string text;
        ConsoleColor color;
    };
    bool dispatch(const std::vector<Command>& table, const Args& words, std::size_t at);

    Font* font_;
    const nox::strings::StringTable* strings_;
    std::vector<Command> commands_;
    std::deque<Line> lines_;
    std::string input_;
    std::vector<std::string> history_;
    std::size_t historyAt_ = 0;
    int scroll_ = 0; ///< lines scrolled up from the newest
    bool open_ = false;
    bool client_ = true;
    std::string logPath_;
    std::string currentLine_;
};

} // namespace noxsdl
