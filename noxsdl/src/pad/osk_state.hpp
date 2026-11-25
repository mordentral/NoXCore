// (noxcpp) The program's on-screen keyboard (App::osk; pad/osk_game.cpp): the keyboard, what it
// types into, the quick-chat lines, the pad's held direction.
#pragma once

#include "gui.hpp"
#include "pad/osk.hpp"
#include "pad/pad_bindings.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl::pad {

struct OskState {
    /// What it types into: an entry field the pad's A gave the typing focus (its Gui's), the chat
    /// line, the console.
    enum class Target : std::uint8_t { None, Field, Chat, Console };
    Osk osk;
    Target target = Target::None;
    gui::Gui* gui = nullptr;
    gui::Window* field = nullptr;
    bool chatWas = false;    ///< the chat line was open last frame
    bool consoleWas = false; ///< the console was
    /// A held D-pad direction's repeat, and the left stick's way.
    std::optional<PadButton> held;
    OskInput heldInput = OskInput::Up;
    std::uint64_t nextMs = 0;
    std::optional<OskInput> stick;
    /// The quick-chat lines (in a game's chat): a line said, or a control (Taunt, Laugh, Point).
    struct QuickLine {
        std::string text;
        std::string control;
    };
    std::vector<QuickLine> quick;
    /// The texts drawn, made once when it opens (OskKeyKind's, the hints, the quick list's title).
    std::array<std::string, 8> keyNames;
    std::string hints[2];
    std::string quickTitle;
    /// The text of the typing events it sent (kept till SDL has passed them on).
    std::array<std::string, 32> sent;
    std::size_t sentAt = 0;
    std::optional<gui::Resources> res;
};

} // namespace noxsdl::pad
