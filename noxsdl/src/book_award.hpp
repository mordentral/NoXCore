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
//   showing    the award shows the book twice over: Cli_AwardSpell (Cli_AwardGuideCreature,
//              abilityReward) shuts it (bookHideMB: BookClose, only if it's open and no award
//              flies), turns the page (bookMoveToPage: PageTurn) and shows it (bookShow: BookOpen,
//              open already or not), then bookRewardCli's bookSetForward shows it again
//              (bookShowMB, then bookShow: BookOpen), moves it up and turns the page again
//              (PageTurn), and bookRewardCli throws the sparks and fills the bars. So an open book
//              plays BookClose, PageTurn, BookOpen, BookOpen, PageTurn; a shut one the same without
//              the BookClose. bookShow refuses with the Escape menu, the in-game Options or the key
//              bindings up, and the first one also on the level-up moment (the server we host: a
//              solo level-up's or award flight's pause); bookShowMB refuses under a cinematic's
//              widescreen (guiCursor) or with a solo game's player dying or dead (playerAnimCheck).
//              A refused showing plays no BookOpen; refused both times, the book stays shut, but
//              the page still turns, the book still moves up, the sparks still fly and so does the
//              icon (showing()).
//   repeat     bookRewardCli's first test: in a solo game (game flag 0x800) an award less than two
//              client frames after the last one that got past it (now - last < 2, unsigned) does
//              nothing of its own: no second showing, no move, no second page turn, no sparks, no
//              fill. The award's own shut, page turn and first showing still happen. The last
//              frame starts at 0 with each game (a new Spellbook).
#pragma once

#include "noxcore/client/spell_bars.hpp"
#include "noxcore/client/world.hpp"

#include <array>
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

/// playerAnimCheck: our player's drawable's animation state (-1 without one) is dying or dead
/// (1, 2 or 0x33), or there's no drawable.
inline bool dyingOrDead(int animState)
{
    return animState < 0 || animState == 1 || animState == 2 || animState == 0x33;
}

/// What an award's showing of the book looks at.
struct ShowCheck {
    /// The Escape menu, the in-game Options or the key bindings up (gui_xxx_check, the options'
    /// and InputCfg's windows shown).
    bool menuUp = false;
    /// The level-up moment on (the server we host's: a solo level-up's or an award flight's pause;
    /// never in another's game).
    bool moment = false;
    /// A cinematic's widescreen (guiCursor).
    bool wideScreen = false;
    /// dyingOrDead(our animation state).
    bool dyingOrDead = false;
    /// Game flag 0x800.
    bool solo = false;
};

/// The book's sounds in an award.
constexpr std::uint32_t kBookOpen = 0x312;
constexpr std::uint32_t kBookClose = 0x313;
constexpr std::uint32_t kPageTurn = 0x314;

/// One step of an award's showing, in the order the book takes them.
enum class Step : std::uint8_t {
    Close,  ///< shut (bookHideMB): BookClose
    Turn,   ///< turned to the entry's page (bookMoveToPage): PageTurn
    Show,   ///< shown on its page (bookShow): BookOpen
    Raise,  ///< moved a third of the way down (bookSetForward's window_setPos)
    Sparks, ///< the sparks off its edges (bookRewardCli)
};

/// The sound a step plays, 0 for none.
inline std::uint32_t stepSound(Step s)
{
    switch (s) {
    case Step::Close:
        return kBookClose;
    case Step::Turn:
        return kPageTurn;
    case Step::Show:
        return kBookOpen;
    default:
        return 0;
    }
}

/// bookRewardCli's first test: in a solo game an award at client frame `now` less than two frames
/// after `last` (the last award that got past it) does nothing of its own.
inline bool rewardSkipped(bool solo, std::uint32_t now, std::uint32_t last)
{
    return solo && now - last < 2u;
}

/// What an award with the show flag does with the book.
struct Showing {
    std::array<Step, 8> steps{};
    std::size_t count = 0;
    bool shown = false;  ///< shown by the award (either showing): open on its page
    bool reward = false; ///< bookRewardCli ran past its first test (its frame is the new `last`)
    bool flies = false;  ///< the icon to the bars' first empty slot (bookFillAll)

    void add(Step s) { steps[count++] = s; }
    bool has(Step s) const
    {
        for (std::size_t i = 0; i < count; ++i) {
            if (steps[i] == s) {
                return true;
            }
        }
        return false;
    }
};

/// The showing of an award (the show flag set; `fill` its fill flag) with the book `open` or not
/// and an award `flying` or not, at client frame `now`, the last award past bookRewardCli's first
/// test at `last`. Whatever refuses the showing, the page, the move, the sparks and the flight go
/// ahead; an award too soon after the last in a solo game has only the first three steps.
inline Showing showing(const ShowCheck& c, nox::client::BookEvent::Kind kind, bool fill, bool open, bool flying,
                       std::uint32_t now, std::uint32_t last)
{
    using Kind = nox::client::BookEvent::Kind;
    Showing s;
    // The award handler: bookHideMB(0) (an open book, no award flying), bookMoveToPage, bookShow(0)
    // (no menu up, not the moment).
    if (open && !flying) {
        s.add(Step::Close);
    }
    s.add(Step::Turn);
    const bool first = !c.menuUp && !c.moment;
    if (first) {
        s.add(Step::Show);
    }
    s.shown = first;
    // bookRewardCli (abilityReward calls it only with its flag, which is the show flag too).
    if (kind == Kind::Ability && !fill) {
        return s;
    }
    if (rewardSkipped(c.solo, now, last)) {
        return s;
    }
    s.reward = true;
    // bookSetForward: bookShowMB(1) (no widescreen, no solo death, then bookShow(1): no menu up),
    // window_setPos, bookMoveToPage; then the sparks and bookFillAll.
    const bool second = !c.wideScreen && !(c.solo && c.dyingOrDead) && !c.menuUp;
    if (second) {
        s.add(Step::Show);
    }
    s.shown = s.shown || second;
    s.add(Step::Raise);
    s.add(Step::Turn);
    s.add(Step::Sparks);
    s.flies = fill && kind != Kind::Creature;
    return s;
}

} // namespace noxsdl::bookaward
