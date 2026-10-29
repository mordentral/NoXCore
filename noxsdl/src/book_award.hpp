// The spellbook award's moment (bookRewardCli, bookSetForward, bookMoveToPage, bookFillAll,
// bookDrawFn): the book opened on the new entry's page, the page turning, the page with its full
// information, then the icon popping off it and flying to the bars.
//
//   clock      the client's own frame counter (the exe's input sequence, one a client frame,
//              30 a second; noxsdl's clockFrame), never the game's: a solo game is paused for the
//              whole moment (the flight's start pauses it, its landing unpauses it), so a clock
//              of game frames would stop with the page still turning.
//   turn       the page turn (BookPageForward, 5 frames, delay 0) begins at the award's frame;
//              frame (now - start) / (delay + 1), and once it reaches the last the turn is over
//              (its callback): the last frame drawn once, the page from the next frame on.
//   hold       the icon leaves the page kHoldFrames after the award (gameFPS * 2), so the page
//              shows from frame 5 to frame 60 before SpellPopOffBook and the flight.
#pragma once

#include "noxcore/client/spell_bars.hpp"
#include "noxcore/client/world.hpp"

#include <cstddef>
#include <cstdint>

namespace noxsdl::bookaward {

/// Client frames from the award to the icon leaving the page (bookDrawFn: gameFPS * 2).
constexpr std::uint32_t kHoldFrames = 60;

/// One frame of a page turn: the turning page's frame to draw, and whether the turn is over after
/// it (the page shows from the next frame).
struct TurnStep {
    std::size_t frame = 0;
    bool over = false;
};

/// The page turn begun at client frame `start`, at client frame `now`, for an animation of
/// `frames` frames a frame each `delay + 1` (the one-shot animated image's frame).
inline TurnStep turnStep(std::uint32_t now, std::uint32_t start, std::size_t frames, std::uint8_t delay)
{
    if (frames == 0) {
        return {0, true};
    }
    const std::size_t i = (now - start) / (static_cast<std::uint32_t>(delay) + 1u);
    if (i >= frames - 1) {
        return {frames - 1, true};
    }
    return {i, false};
}

/// The book entry an award turns to: the spell, creature or ability id, an ability's tagged in
/// noxcpp's multiclass (nox::client::kAbilityTag), as the book lists it.
inline std::uint32_t awardEntry(nox::client::BookEvent::Kind kind, std::uint8_t id, bool multiclass)
{
    const auto e = static_cast<std::uint32_t>(id);
    return multiclass && kind == nox::client::BookEvent::Kind::Ability ? nox::client::kAbilityTag | e : e;
}

} // namespace noxsdl::bookaward
