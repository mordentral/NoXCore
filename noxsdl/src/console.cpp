// The game's console (console.hpp): its lines, typing, history, and the command table's parse.
#include "console.hpp"

#include "noxcore/util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <utility>

namespace noxsdl {

namespace {

// The lines kept; past this many the oldest are dropped.
constexpr std::size_t kMaxLines = 512;

/// The palette's colours (loadDefColor, in its table's order).
constexpr std::array<SDL_Color, 17> kColors{{
    {0, 0, 0, 255},       {8, 8, 8, 255},       {115, 115, 115, 255}, {212, 212, 212, 255},
    {255, 255, 255, 255}, {100, 0, 0, 255},     {255, 0, 0, 255},     {255, 128, 128, 255},
    {0, 100, 0, 255},     {0, 255, 0, 255},     {128, 255, 128, 255}, {0, 0, 140, 255},
    {0, 0, 255, 255},     {0, 160, 255, 255},   {240, 180, 42, 255},  {255, 255, 0, 255},
    {255, 255, 128, 255},
}};

} // namespace

SDL_Color paletteColor(std::uint8_t index)
{
    return kColors[index < kColors.size() ? index : 4];  // out of range: white
}

namespace {

/// The words of a line: split at spaces, "quoted words" kept whole (server_parseCmdText).
Console::Args words(std::string_view line)
{
    Console::Args out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        if (line[i] == '"') {
            const std::size_t end = line.find('"', i + 1);
            out.emplace_back(line.substr(i + 1, (end == std::string_view::npos ? line.size() : end) - i - 1));
            i = end == std::string_view::npos ? line.size() : end + 1;
            continue;
        }
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        out.emplace_back(line.substr(start, i - start));
    }
    return out;
}

} // namespace

void Console::toggle(SDL_Window* window)
{
    if (open_) {
        close(window);
        return;
    }
    open_ = true;
    scroll_ = 0;
    SDL_StartTextInput(window);
}

void Console::close(SDL_Window* window)
{
    open_ = false;
    SDL_StopTextInput(window); // the game's keys are keys again
}

void Console::print(std::string_view text, ConsoleColor color)
{
    // One line each, as the list box takes them (wndListBoxAddLine drops a trailing newline).
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', start);
        const std::string_view part = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        lines_.push_back({std::string(part), color});
        if (!logPath_.empty()) {
            std::ofstream(logPath_, std::ios::app) << part << "\n";
        }
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    while (lines_.size() > kMaxLines) {
        lines_.pop_front();
    }
}

std::string Console::text(std::string_view id, std::initializer_list<std::string_view> values) const
{
    std::string s(id);
    if (strings_) {
        std::string label(id);
        if (label.find(':') == std::string::npos) {
            // A bare id is looked up in ParseCmd.c's strings.
            label = "ParseCmd.c:" + label;
        }
        if (const auto t = strings_->text(label)) {
            s = nox::strings::toUtf8(*t);
        }
    }
    // %s / %S / %d in order.
    auto value = values.begin();
    for (std::size_t at = s.find('%'); at != std::string::npos && at + 1 < s.size(); at = s.find('%', at)) {
        const char kind = s[at + 1];
        if ((kind == 's' || kind == 'S' || kind == 'd') && value != values.end()) {
            s.replace(at, 2, *value);
            at += value->size();
            ++value;
        } else {
            at += 2;
        }
    }
    return s;
}

void Console::logTo(const std::string& path)
{
    logPath_ = path;
}

// Matches word `at` against `table`, walking into sub-commands while words match; true once
// something was run or a refusal printed, false when no command has that name.
bool Console::dispatch(const std::vector<Command>& table, const Args& w, std::size_t at)
{
    if (at >= w.size()) {
        return false;
    }
    for (const auto& c : table) {
        if (!nox::util::equalsNoCase(c.name, w[at])) {
            continue;
        }
        if (!(c.flags & kNoxsdl) && !(c.flags & (client_ ? kClient : kHost))) {
            // A command for the host's only, or a client's only.
            print(text(client_ ? "ServerOnly" : "ClientOnly"), Red);
            return true;
        }
        if (!c.sub.empty() && at + 1 < w.size() && dispatch(c.sub, w, at + 1)) {
            return true;
        }
        if (c.run) {
            c.run(Args(w.begin() + static_cast<std::ptrdiff_t>(at + 1), w.end()));
            return true;
        }
        // A group without that sub-command: what it has.
        std::string names;
        for (const auto& s : c.sub) {
            names += (names.empty() ? "" : " ") + s.name;
        }
        print(c.name + ": " + names, Red);
        return true;
    }
    return false;
}

void Console::run(std::string_view line)
{
    print("> " + std::string(line), White); // (gui_console_Enter echoes the line first)
    currentLine_ = std::string(line);
    const Args w = words(line);
    if (!w.empty() && !dispatch(commands_, w, 0)) {
        print(text("typehelp"), Red);
    }
}

void Console::draw(SDL_Renderer* renderer, int width, int height, float scale, bool translucent)
{
    if (!open_ || !font_) {
        return;
    }
    // Across the top half: the lines, then the line being typed in the bottom 20 (gui_console_Create).
    const float h = std::floor(static_cast<float>(height) / 2);
    const float entry = 20 * scale;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, translucent ? 160 : 255);
    const SDL_FRect back{0, 0, static_cast<float>(width), h};
    SDL_RenderFillRect(renderer, &back);
    SDL_SetRenderDrawColor(renderer, 115, 115, 115, 255);
    SDL_RenderLine(renderer, 0, h - entry, static_cast<float>(width), h - entry);

    const int size = std::max(10, static_cast<int>(std::lround(11 * scale)));
    const float lineH = font_->lineHeight(size);
    const float margin = 4 * scale;
    float y = h - entry - lineH - 2 * scale;
    // From the newest line (less the scroll) upwards until the top is reached.
    const int first = static_cast<int>(lines_.size()) - 1 - scroll_;
    for (int i = first; i >= 0 && y > -lineH; --i, y -= lineH) {
        font_->draw(lines_[static_cast<std::size_t>(i)].text, margin, y, size, kColors[lines_[static_cast<std::size_t>(i)].color]);
    }
    // The typed line and a blinking caret.
    const float ty = h - entry + (entry - lineH) / 2;
    font_->draw(input_, margin, ty, size, kColors[White]);
    if ((SDL_GetTicks() / 400) % 2 == 0) {
        const float cx = margin + font_->measure(input_, size) + 1;
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        const SDL_FRect caret{cx, ty + 2, std::max(1.0f, scale), lineH - 4};
        SDL_RenderFillRect(renderer, &caret);
    }
}

bool Console::handleEvent(SDL_Window* window, const SDL_Event& event)
{
    if (!open_) {
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_TEXT_INPUT:
        // Typed text, capped at 250 bytes.
        if (event.text.text && input_.size() < 250) {
            input_ += event.text.text;
        }
        return true;
    case SDL_EVENT_KEY_DOWN:
        switch (event.key.key) {
        case SDLK_ESCAPE:
        case SDLK_F1:
            close(window); // (consoleEsc; F1 toggles)
            return true;
        case SDLK_RETURN:
        // Run the line and remember it for the history; the view jumps back to the newest line.
        case SDLK_KP_ENTER: {
            const std::string line = std::exchange(input_, {});
            scroll_ = 0;
            if (!line.empty()) {
                history_.push_back(line);
                historyAt_ = history_.size();
                run(line);
            }
            return true;
        }
        case SDLK_BACKSPACE:
            // A whole UTF-8 character off the end.
            while (!input_.empty()) {
                const auto c = static_cast<unsigned char>(input_.back());
                input_.pop_back();
                if ((c & 0xC0) != 0x80) {  // stop once a lead byte (not a 10xxxxxx continuation byte) is gone
                    break;
                }
            }
            return true;
        // Up/Down walk the history; past the newest entry the line is emptied again.
        case SDLK_UP:
            if (historyAt_ > 0) {
                input_ = history_[--historyAt_];
            }
            return true;
        case SDLK_DOWN:
            if (historyAt_ + 1 < history_.size()) {
                input_ = history_[++historyAt_];
            } else {
                historyAt_ = history_.size();
                input_.clear();
            }
            return true;
        // Page Up/Down scroll the lines ten at a time; `scroll_` counts lines back from the newest.
        case SDLK_PAGEUP:
            scroll_ = std::min(scroll_ + 10, std::max(0, static_cast<int>(lines_.size()) - 1));
            return true;
        case SDLK_PAGEDOWN:
            scroll_ = std::max(scroll_ - 10, 0);
            return true;
        // Every other key is swallowed so it doesn't reach the game.
        default:
            return true;
        }
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        scroll_ = std::clamp(scroll_ + (event.wheel.y > 0 ? 3 : -3), 0, std::max(0, static_cast<int>(lines_.size()) - 1));
        return true;
    default:
        return false; // (the mouse still plays)
    }
}

} // namespace noxsdl
