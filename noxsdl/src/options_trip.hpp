// Options from the shell menus and back (issue #19), kept apart from their SDL windows so the tests
// can drive it:
//
//   the menus' screen stack (Menus)  Options' button pushes Options (300) over the screen up once
//                                    that screen has slid out, its choices kept (KeptScreen). Options
//                                    gone, Options and the screen under it come off and that screen
//                                    is entered again, its choices put back instead of its defaults.
//   the Options window (OptionsWindow) from the menus it slides in, and slides out before it's gone
//                                    (nox.cfg saved as it starts out, its input ignored meanwhile, a
//                                    second close nothing); in a game it goes at once. Gone, it goes
//                                    on to what the close was for: Input Config's key bindings (Options
//                                    staying the menus' screen, opened again when they close), or,
//                                    dismissed, the menus told (Menus::optionsClosed).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace noxsdl {

/// The shell's screens by the game's state codes (menus.hpp lists them).
enum class MenuState {
    MainMenu = 100,
    Options = 300,
    Online = 400,
    SelChar = 500,
    SelClass = 600,
    SelColor = 700,
    Login = 1700,
    Chat = 1900,
    Browser = 10000
};

namespace optionstrip {

/// Options' button: Options opens over the screen up; not with none, not over itself, and not while
/// a screen is sliding out (a second press before it's up).
inline bool mayOpen(const std::vector<MenuState>& stack, bool leaving)
{
    return !stack.empty() && stack.back() != MenuState::Options && !leaving;
}

/// Options is the screen up (so its window going brings the one under it back).
inline bool optionsUp(const std::vector<MenuState>& stack)
{
    return !stack.empty() && stack.back() == MenuState::Options;
}

/// Back from the screen on top, and Options gone: that screen comes off, and so does the one under
/// it, which is returned to be entered again (entering pushes it back). The stack holds two or more.
inline MenuState dropToPrevious(std::vector<MenuState>& stack)
{
    stack.pop_back();
    const MenuState previous = stack.back();
    stack.pop_back();
    return previous;
}

/// SelColor's choices: its colour buttons' data, the facial hair switched on or off, the name.
struct SelColorKept {
    std::vector<std::pair<std::int32_t, std::uint32_t>> colors;
    std::vector<std::pair<std::int32_t, bool>> labelsOn;
    std::u16string name;
};
/// SelClass's class: its portrait's window id.
struct SelClassKept {
    std::int32_t chosen = -1;
};
/// SelChar's character: the line picked in its three lists and how far each is scrolled.
struct SelCharKept {
    struct List {
        std::int32_t id = 0;
        int line = -1;
        int scroll = 0;
    };
    std::vector<List> lists;
};
/// The login's fields: the name and password as they stand, Save This Login, the saved login picked
/// and how far the list is scrolled.
struct LoginKept {
    std::u16string name;
    std::u16string password;
    bool save = false;
    int slot = 0;
    int scroll = 0;
};

/// A screen's choices kept while Options is up from it, so the screen comes back with them rather
/// than its defaults. Taken once, by the screen it was kept for; any other screen entered first
/// drops it (a choice never comes back on a later visit).
class KeptScreen {
public:
    template <class T>
    void keep(T kept)
    {
        kept_ = std::move(kept);
    }
    /// What was kept for screen T, once; nothing if it wasn't kept for T (and then it's dropped).
    template <class T>
    std::optional<T> take()
    {
        std::optional<T> out;
        if (T* k = std::get_if<T>(&kept_)) {
            out = std::move(*k);
        }
        kept_ = std::monostate{};
        return out;
    }
    void clear() { kept_ = std::monostate{}; }
    bool empty() const { return std::holds_alternative<std::monostate>(kept_); }

private:
    std::variant<std::monostate, SelColorKept, SelClassKept, SelCharKept, LoginKept> kept_;
};

/// The Options window's open and close, without its windows.
class WindowLife {
public:
    /// What the window goes on to once it's gone.
    enum class After : std::uint8_t {
        Dismissed,   ///< gone for good (Escape, Close, the menus' Back): the menus told (onDismissed)
        InputConfig, ///< on to the key bindings (onInputConfig); the menus keep Options up
    };
    /// How a close goes.
    enum class Close : std::uint8_t {
        Nothing,  ///< not open, or sliding out already
        AtOnce,   ///< gone now (in a game; gone() at once)
        SlideOut, ///< sliding out (from the menus): gone() once it's out
    };

    /// Opened; false if it was open already (nothing done).
    bool open()
    {
        if (open_) {
            return false;
        }
        open_ = true;
        closing_ = false;
        return true;
    }
    /// A close asked for, to go on to `after`; `slides`: it slid in (from the menus) and slides out.
    Close close(bool slides, After after)
    {
        if (!open_ || closing_) {
            return Close::Nothing;
        }
        after_ = after;
        if (!slides) {
            return Close::AtOnce;
        }
        closing_ = true;
        return Close::SlideOut;
    }
    /// Gone (at once, or slid out): what it goes on to.
    After gone()
    {
        open_ = false;
        closing_ = false;
        return after_;
    }
    /// Open, or still sliding out.
    bool isOpen() const { return open_; }
    /// Sliding out: its input ignored.
    bool closing() const { return closing_; }

private:
    bool open_ = false;
    bool closing_ = false;
    After after_ = After::Dismissed;
};

} // namespace optionstrip
} // namespace noxsdl
