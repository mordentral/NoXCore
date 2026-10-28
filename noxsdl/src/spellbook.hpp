// The spellbook (guibook.c, bookInit): the Book of Knowledge in the bottom left,
// listing what we know and describing it a page at a time, and where the spell bars are filled
// from. Toggled by the quickbar's book button and ToggleBook; BookOpen / BookClose.
//
//   window     0x11D x 0xA8 at (5, screen height - 0x143) in game pixels (after an award, at
//              (5, a third of the height)); BookOfKnowledge drawn 0x18 left and 0x4C up of it,
//              with the lit tab of the page shown (SpellTabLit, GuideTabLit).
//   tabs       spells (abilities for a warrior) at (0xFD, 0x3D), the field guide at (0x101, 0xF),
//              27 x 40; switching to an empty one says NoCanDo.
//   list       (bookDrawList) the titles centred in two columns at x 0x4E and 199 from y 0x13, a line
//              each (font height + 2), (0x8D / line - 1) to a column; the game's disabled spells
//              greyed. Clicking one turns to its page; dragging it puts it on a bar (bookListWndProc).
//   pages      the title centred over a 0x6C column at x 0x18, y 0x35; for a spell its mana cost,
//              how it's cast (instant, targeted, at a location, hostile) and our level; for a
//              creature its size and picture (a conjurer who can summon it: "Size Small"...);
//              the description in a 0x5C column at x 0x99, centred in 0x8D. The icon at
//              (0x3F, 0x13) can be dragged to a bar, or clicked to cast (bookWndFn).
//   arrows     ArrowW at (0x18, 0x8A), ArrowE at (0xE9, 0x8A): a page back or on, through the
//              list pages and then the entries; PageTurn, and BookPageForward / Backward played
//              over the page (bookWndProc and the book's page turn).
//   contents   (guiSpellSortList) a caster's class spells, in a network game all of them, else those
//              learned (not the trap glyph, not the numbered summons, marks and teleports'
//              general spells), by title; a warrior's abilities; the guide's creatures (the
//              last four only in quests), all of them in a network game, else those met.
//   awards     (Cli_AwardSpell, Cli_AwardGuideCreature, abilityReward, bookRewardCli) a spell,
//              creature or ability learned with the show flag opens the book, moved up to a third
//              of the screen, on its page, sparks bursting from its edges; with the fill flag a
//              spell or ability also goes to the first empty slot of the bars, sparks trailing to
//              it (bookFillAll, in a network game at once).
#pragma once

#include "screen_particles.hpp"
#include "sprite_cache.hpp"
#include "text.hpp"

#include "noxcore/client/world.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class Hud;

/// What the book needs to know about the game this frame.
struct BookContext {
    const nox::client::World* world = nullptr;
    std::uint8_t playerClass = 0; ///< 0 warrior, 1 wizard, 2 conjurer
    bool quest = false;           ///< game flag 0x1000
    /// A network game that isn't a quest (game flag 0x2000 without 0x1000): every spell, ability
    /// and creature shown (guiSpellSortList's test). A solo game's book (0x800 alone, the co-op
    /// campaign's too) shows only those known.
    bool allKnown = false;
    std::uint32_t frame = 0;      ///< the game frame (the page animations)
    /// noxcpp's multiclass, a mode's: every class's spells and the abilities in one book, an
    /// ability's entry tagged (nox::client::kAbilityTag); the guide whatever the class.
    bool multiclass = false;
};

class Spellbook {
public:
    /// What the book asks of the rest of the client.
    struct Actions {
        /// A spell or ability let go at output pixel (x, y): onto a bar slot there, if any
        /// (bookSpellDrop, the spell lit if it goes on its caster).
        std::function<void(std::uint32_t id, float x, float y)> drop;
        /// The page's icon clicked: cast it (guiSpellSetCursor) or use the ability.
        std::function<void(std::uint32_t id)> click;
    };

    Spellbook(const nox::things::ThingDb& things, const nox::strings::StringTable* strings, SpriteCache& sprites,
              Font* font, const Hud& hud);

    void setSound(std::function<void(std::uint32_t)> play) { playSound_ = std::move(play); }
    /// Gui_PrintCenteredMessage, for "the book is empty".
    void setMessage(std::function<void(const std::string&)> say) { say_ = std::move(say); }

    bool isOpen() const { return open_; }
    /// ToggleBook.
    void toggle(const BookContext& ctx);
    /// Shows the book (bookShowMB): NoCanDo and EmptyBook if there's nothing in it.
    void show(const BookContext& ctx);
    /// Hides it (bookHideMB); `reset` turns back to the first list page.
    void hide(bool reset = false);
    /// Turns to entry `i`'s page, as clicking it in the list does.
    void turnTo(int i, const BookContext& ctx)
    {
        if (i >= 0 && i < static_cast<int>(entries_.size())) {
            moveToPage(i, ctx);
        }
    }
    /// An award or revoke (World::takeBookEvents). Returns the spell or ability to fly to the
    /// bars' first empty slot, if any.
    std::optional<std::uint32_t> event(const nox::client::BookEvent& e, const BookContext& ctx);
    /// The book's icon in game pixels, where an award's trail starts.
    SDL_FPoint iconPosition(int h) const;

    /// Mouse in output pixels on a w x h screen. Whether the book took it.
    bool mouseDown(float x, float y, int w, int h, const BookContext& ctx);
    bool mouseUp(float x, float y, int w, int h, const BookContext& ctx, const Actions& actions);
    /// The mouse moved with the book held (pressed on itself, not on a tab, arrow, its icon or a
    /// spell): it follows, as the GUI moves a DRAGABLE window (cursorUpdate; the book's status 0x4AC).
    void mouseMove(float x, float y, int w, int h);
    bool contains(float x, float y, int w, int h) const;
    /// The tabs' tooltips: ToolTipSpellTab (a warrior's ToolTipAbilityTab),
    /// ToolTipGuideTab; empty elsewhere.
    std::string tooltip(float x, float y, int w, int h, const BookContext& ctx) const;
    /// The spell (a warrior: ability) being dragged out of the book, 0 if none.
    std::uint32_t dragging() const { return drag_; }
    void cancelDrag() { drag_ = 0; pressed_.reset(); }

    void draw(SDL_Renderer* renderer, const BookContext& ctx, int w, int h);

private:
    enum class Anim { None, Forward, Backward };

    /// guiSpellSortList: fills entries_ for the tab; false if it's empty.
    bool sort(const BookContext& ctx);
    int linesPerColumn() const;
    int perPage() const { return linesPerColumn() * 2; }
    int pageCount() const;
    /// bookMoveToPage: turn to entry `i`'s page.
    void moveToPage(int i, const BookContext& ctx);
    void turn(Anim a, const BookContext& ctx);
    /// Whether a conjurer can drag summons from the guide (knows Summon Creature, or a network game).
    bool canSummon(const BookContext& ctx) const;
    /// The class an entry reads as (`cls` below): our class's, but in noxcpp's multiclass 0 for a
    /// tagged ability and a caster's for a spell.
    static std::uint8_t entryClass(std::uint32_t e, const BookContext& ctx)
    {
        if (!ctx.multiclass) {
            return ctx.playerClass;
        }
        return (e & 0x10000) != 0 ? std::uint8_t{0} : ctx.playerClass == 0 ? std::uint8_t{2} : ctx.playerClass;
    }
    /// An entry's title and description: a spell's, an ability's (`cls` 0) or a guide creature's.
    std::string title(std::uint32_t id, bool guide, std::uint8_t cls) const;
    std::string description(std::uint32_t id, bool guide, std::uint8_t cls) const;
    std::string text(const std::string& id) const;
    const nox::things::ImageDef* image(const std::string& name) const;
    SDL_FPoint origin(int h) const;
    void sound(std::uint32_t id) const
    {
        if (playSound_) {
            playSound_(id);
        }
    }
    /// The award's sparks round the book's edges (its draw, six calls in bookRewardCli).
    void burst(int kind, int h);

public:
    ScreenParticles particles; ///< the book's and the bars' sparks

private:
    const nox::things::ThingDb& things_;
    const nox::strings::StringTable* strings_;
    SpriteCache& sprites_;
    Font* font_;
    const Hud& hud_;
    std::map<std::string, const nox::things::ImageDef*, std::less<>> images_;
    std::function<void(std::uint32_t)> playSound_;
    std::function<void(const std::string&)> say_;

    bool open_ = false;
    bool listMode_ = true;
    int listPage_ = 0;     ///< the list page shown
    int entry_ = 0;        ///< the entry turned to
    bool guide_ = false;   ///< the field guide's tab
    Anim anim_ = Anim::None;
    std::uint32_t animStart_ = 0;
    bool raised_ = false; ///< moved up by an award
    std::optional<SDL_FPoint> moved_; ///< where it was dragged to (its window's position), game pixels
    std::optional<SDL_FPoint> grab_;  ///< the pointer while the book itself is held
    int lastHeight_ = 600; ///< the screen's height in game pixels when last drawn
    std::vector<std::uint32_t> entries_;

    std::optional<int> pressed_; ///< the list entry pressed
    SDL_FPoint pressAt_{};
    bool iconPressed_ = false;
    std::uint32_t drag_ = 0;
};

} // namespace noxsdl
