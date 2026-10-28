// Spellbook (spellbook.hpp): the book's contents, its awards, mouse handling and drawing. Points
// are game pixels from the book's corner (origin), as the exe lays it out.
#include "spellbook.hpp"

#include "hud.hpp"

#include "noxcore/client/spell_bars.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/spell_ids.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace noxsdl {

namespace {

constexpr int kFontHeight = 10; ///< the default font
constexpr int kLine = kFontHeight + 2;
constexpr float kWidth = 0x11D, kHeight = 0xA8;
constexpr std::uint32_t kGlyph = 0x22;  // (the trap glyph, kept on the trap bar)
constexpr std::uint32_t kSummonCreature = 134;  // (Summon Creature)

/// Spell flags the book reads.
constexpr std::uint32_t kInstant = 0x100, kTargeted = 4, kAtLocation = 8, kHostile = 0x20;

constexpr SDL_Color kInk{15, 15, 15, 255};      ///< bookSetColor
constexpr SDL_Color kDisabled{0x73, 100, 100, 255};

bool inside(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

/// Case-insensitive, as the list's sort compares titles.
bool titleLess(const std::string& a, const std::string& b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) < std::tolower(y);
    });
}

} // namespace

Spellbook::Spellbook(const nox::things::ThingDb& things, const nox::strings::StringTable* strings,
                     SpriteCache& sprites, Font* font, const Hud& hud)
    : things_(things), strings_(strings), sprites_(sprites), font_(font), hud_(hud)
{
    for (const auto& i : things_.images) {
        images_.emplace(i.name, &i);
    }
}

const nox::things::ImageDef* Spellbook::image(const std::string& name) const
{
    const auto it = images_.find(name);
    return it == images_.end() ? nullptr : it->second;
}

std::string Spellbook::text(const std::string& id) const
{
    // Str_GetString with guibook.c as the source: its own label first, then the file's.
    if (strings_) {
        if (const auto t = strings_->text("guibook.c:" + id)) {
            return nox::strings::toUtf8(*t);
        }
        if (const auto t = strings_->text(id)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return id;
}

std::string Spellbook::title(std::uint32_t id, bool guide, std::uint8_t cls) const
{
    std::string label;
    if (guide) {
        label = "creature:" + std::string(nox::things::kGuideNames[static_cast<std::size_t>(id - 1)]);
    } else if (cls == 0) {
        label = id >= 1 && id <= things_.abilities.size() ? things_.abilities[id - 1].title : "";
    } else if (const auto* s = things_.findSpell(id)) {
        label = s->title;
    }
    if (strings_) {
        if (const auto t = strings_->text(label)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return label;
}

std::string Spellbook::description(std::uint32_t id, bool guide, std::uint8_t cls) const
{
    std::string label;
    if (guide) {
        label = "creature_desc:" + std::string(nox::things::kGuideNames[static_cast<std::size_t>(id - 1)]);
    } else if (cls == 0) {
        label = id >= 1 && id <= things_.abilities.size() ? things_.abilities[id - 1].description : "";
    } else if (const auto* s = things_.findSpell(id)) {
        label = s->description;
    }
    if (strings_ && !label.empty()) {
        if (const auto t = strings_->text(label)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return {};
}

bool Spellbook::canSummon(const BookContext& ctx) const
{
    if (ctx.playerClass != 2 && !ctx.multiclass) {
        return false;
    }
    const auto& levels = ctx.world->self().spellLevels;
    const auto it = levels.find(static_cast<std::uint8_t>(kSummonCreature));
    return ctx.allKnown || (it != levels.end() && it->second != 0);
}

bool Spellbook::sort(const BookContext& ctx)
{
    entries_.clear();
    const auto& self = ctx.world->self();
    const bool all = ctx.allKnown; // a network game's book has everything (game flag 0x2000, not 0x1000)
    if (guide_) {
        // The guide's creatures listed for this game (the quest's own only in a quest): those met, or all.
        for (int c = 1; c <= nox::things::kGuideCreatures; ++c) {
            if (!nox::things::guideListed(c) || !nox::things::guideShown(c, ctx.quest)) {
                continue;
            }
            const auto it = self.guideLevels.find(static_cast<std::uint8_t>(c));
            if (all || (it != self.guideLevels.end() && it->second != 0)) {
                entries_.push_back(static_cast<std::uint32_t>(c));
            }
        }
    // A warrior's five abilities: those learned, or all.
    } else if (ctx.playerClass == 0 && !ctx.multiclass) {
        for (std::uint32_t a = 1; a <= 5; ++a) {
            if (all || (a < self.abilityLevel.size() && self.abilityLevel[a] != 0)) {
                entries_.push_back(a);
            }
        }
    } else {
        // (noxcpp's multiclass: every class's spells known, then the abilities, tagged.)
        const std::uint32_t classBit = ctx.multiclass ? 0x6000000 : ctx.playerClass == 1 ? 0x2000000 : 0x4000000;
        if (ctx.multiclass) {
            for (std::uint32_t a = 1; a <= 5; ++a) {
                if (a < self.abilityLevel.size() && self.abilityLevel[a] != 0) {
                    entries_.push_back(nox::client::kAbilityTag | a);
                }
            }
        }
        for (std::uint32_t id = 1; id < nox::things::kSpellCount; ++id) {
            const auto* s = things_.findSpell(id);
            if (!s || id == kGlyph || !(s->flags & (0x1000000 | classBit))) {
                continue;
            }
            const auto it = self.spellLevels.find(static_cast<std::uint8_t>(id));
            if (!all && (it == self.spellLevels.end() || it->second == 0)) {
                continue;
            }
            // The numbered summons are the guide's; the general summon, mark and go-to-mark
            // spells (0x15000) come last and the page count leaves them off.
            if (!(s->flags & (0x2000 | 0x15000))) {
                entries_.push_back(id);
            }
        }
    }
    const bool guide = guide_;
    std::stable_sort(entries_.begin(), entries_.end(), [&](std::uint32_t a, std::uint32_t b) {
        return titleLess(title(nox::client::untagged(a), guide, entryClass(a, ctx)), title(nox::client::untagged(b), guide, entryClass(b, ctx)));
    });
    return !entries_.empty();
}

// As many titles as fit in 0x8D, a line each, less one.
int Spellbook::linesPerColumn() const
{
    return 0x8D / kLine - 1;
}

int Spellbook::pageCount() const
{
    // (the exe's count: one more page when the last is exactly full)
    return static_cast<int>(entries_.size()) / perPage() + 1;
}

SDL_FPoint Spellbook::origin(int h) const
{
    // In game pixels, from the screen's bottom (bookInit), or a third down after an award (bookRewardCli).
    if (moved_) {
        return *moved_; // (dragged: mouseMove)
    }
    const float H = static_cast<float>(h);
    return {5, raised_ ? std::floor(H / 3) : H - 0x143};
}

SDL_FPoint Spellbook::iconPosition(int h) const
{
    const SDL_FPoint o = origin(h);
    return {o.x + 0x3F, o.y + 0x13};
}

void Spellbook::turn(Anim a, const BookContext& ctx)
{
    anim_ = a;
    animStart_ = ctx.frame;
}

void Spellbook::moveToPage(int i, const BookContext& ctx)
{
    listMode_ = false;
    entry_ = i;
    listPage_ = 99;
    turn(Anim::Forward, ctx);
    sound(0x314); // PageTurn
}

void Spellbook::toggle(const BookContext& ctx)
{
    if (open_) {
        hide();
    } else {
        show(ctx);
    }
}

void Spellbook::show(const BookContext& ctx)
{
    if (!sort(ctx)) {
        if (say_) {
            say_(text("EmptyBook"));
        }
        sound(0x39D); // NoCanDo
        return;
    }
    open_ = true;
    sound(0x312); // BookOpen
}

void Spellbook::hide(bool reset)
{
    if (!open_) {
        return;
    }
    open_ = false;
    drag_ = 0;
    pressed_.reset();
    iconPressed_ = false;
    sound(0x313); // BookClose
    if (reset) {
        listMode_ = true;
        listPage_ = 0;
    }
}

void Spellbook::burst(int kind, int h)
{
    // bookRewardCli: 100 sparks off each part of the book's edges, flung up and out.
    const SDL_FPoint o = origin(h);
    const int x = static_cast<int>(o.x);
    const int y = static_cast<int>(o.y);
    // along 1: a vertical edge (x fixed), 2: a horizontal one; side 1 flings the sparks left, 2 right.
    const auto edge = [&](int ex, int ey, int ew, int eh, int along, int side) {
        for (int n = 0; n < 100; ++n) {
            const int px = along == 2 ? ex + particles.rand(0, ew) : ex;
            const int py = along == 2 ? ey : ey + particles.rand(0, eh);
            const int size = particles.rand(2, 5);
            const int vy = particles.rand(-0x28, -0x14);
            const int vx = side == 1 ? particles.rand(-0x14, 0) : particles.rand(0, 0x14);
            particles.add(kind, px, py, vx, vy, 1, size, 0, 0, true);
        }
    };
    // The left half's left side, top and bottom; then the right half's right side, top and bottom.
    edge(x, y, 0x10F, 0xA6, 1, 1);
    edge(x, y, 0x87, 0xA6, 2, 1);
    edge(x, y + 0xA6, 0x87, 0xA6, 2, 1);
    edge(x + 0x10F, y, 0x10F, 0xA6, 1, 2);
    edge(x + 0x87, y, 0x87, 0xA6, 2, 2);
    edge(x + 0x87, y + 0xA6, 0x87, 0xA6, 2, 2);
}

std::optional<std::uint32_t> Spellbook::event(const nox::client::BookEvent& e, const BookContext& ctx)
{
    using Kind = nox::client::BookEvent::Kind;
    switch (e.kind) {
    case Kind::RevokeSpell:
    case Kind::RevokeCreature:
    case Kind::RevokeAbility:
        hide(true);
        guide_ = false;
        sort(ctx);
        return std::nullopt;
    default:
        break;
    }
    if (e.kind == Kind::Spell && e.id == kGlyph) {
        sort(ctx);
        return std::nullopt; // the trap bar's (quickbarAddTrap)
    }
    if (e.kind == Kind::Ability && ctx.playerClass != 0 && !ctx.multiclass) {
        return std::nullopt;
    }
    guide_ = e.kind == Kind::Creature;
    sort(ctx);
    if (!e.show) {
        return std::nullopt;
    }
    // (noxcpp's multiclass: an ability's entry, and what flies to the bars, tagged.)
    const std::uint32_t entry = ctx.multiclass && e.kind == Kind::Ability ? nox::client::kAbilityTag | static_cast<std::uint32_t>(e.id)
                                                                          : static_cast<std::uint32_t>(e.id);
    const auto it = std::find(entries_.begin(), entries_.end(), entry);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    // Shut, turned to its page, opened a third of the way down with sparks off its edges.
    hide();
    moveToPage(static_cast<int>(it - entries_.begin()), ctx);
    open_ = true;
    sound(0x312); // BookOpen
    raised_ = true;
    moved_.reset(); // (window_setPos: the award puts it there whatever it was dragged to)
    burst(e.kind == Kind::Spell ? 0 : e.kind == Kind::Ability ? 3 : 2, lastHeight_);
    if (!e.fill || e.kind == Kind::Creature) {
        return std::nullopt;
    }
    return entry;
}

bool Spellbook::contains(float x, float y, int w, int h) const
{
    if (!open_) {
        return false;
    }
    const float s = Hud::scale(w, h);
    const SDL_FPoint o = origin(static_cast<int>(static_cast<float>(h) / s));
    return inside(x / s, y / s, o.x, o.y, kWidth, kHeight);
}

std::string Spellbook::tooltip(float x, float y, int w, int h, const BookContext& ctx) const
{
    if (!open_ || !contains(x, y, w, h)) {
        return {};
    }
    const float s = Hud::scale(w, h);
    const SDL_FPoint o = origin(static_cast<int>(static_cast<float>(h) / s));
    const float gx = x / s - o.x;
    const float gy = y / s - o.y;
    if (inside(gx, gy, 0xFD, 0x3D, 0x1B, 0x28)) {
        return text(ctx.playerClass == 0 && !ctx.multiclass ? "ToolTipAbilityTab" : "ToolTipSpellTab");
    }
    if (inside(gx, gy, 0x101, 0xF, 0x1B, 0x28)) {
        return text("ToolTipGuideTab");
    }
    return {};
}

bool Spellbook::mouseDown(float x, float y, int w, int h, const BookContext& ctx)
{
    if (!open_ || !contains(x, y, w, h)) {
        return false;
    }
    const float s = Hud::scale(w, h);
    const SDL_FPoint o = origin(static_cast<int>(static_cast<float>(h) / s));
    const float gx = x / s - o.x;
    const float gy = y / s - o.y;
    grab_ = SDL_FPoint{x / s, y / s}; // (the book itself took the press: unless a child window did, below)
    if (anim_ != Anim::None) {
        return true;
    }
    const int n = static_cast<int>(entries_.size());
    // The tabs (bookChildWndProcMB).
    const bool spellsTab = inside(gx, gy, 0xFD, 0x3D, 0x1B, 0x28);
    const bool guideTab = inside(gx, gy, 0x101, 0xF, 0x1B, 0x28);
    // A tab switches sides (refused with NoCanDo when that side has no entries) and turns
    // back to the first list page.
    if (spellsTab || guideTab) {
        grab_.reset(); // (a tab's own window)
        const bool toGuide = guideTab;
        bool switched = false;
        if (guide_ != toGuide) {
            guide_ = toGuide;
            if (!sort(ctx)) {
                guide_ = !toGuide;
                sort(ctx);
                sound(0x39D); // NoCanDo
                return true;
            }
            switched = true;
        }
        if (!listMode_ || listPage_ != 0 || switched) {
            listMode_ = true;
            listPage_ = 0;
            sound(0x314);
            turn(toGuide && switched ? Anim::Forward : Anim::Backward, ctx);
            drag_ = 0;
        }
        return true;
    }
    // The arrows (book back, bookWndProc on).
    const bool prevShown = !(listMode_ && listPage_ == 0);
    const bool nextShown = !(!listMode_ && entry_ >= n - 1);
    if (prevShown && inside(gx, gy, 0x18, 0x8A, 0x14, 0x14)) {
        grab_.reset(); // (an arrow's own window)
        // Back: one list page back, one entry back, or from the first entry to the list's last page.
        if (listMode_) {
            --listPage_;
        } else if (entry_ < n) {
            if (entry_ > 0) {
                --entry_;
            } else {
                listMode_ = true;
                listPage_ = pageCount() - 1;
            }
        } else {
            listMode_ = true;
            listPage_ = 0;
        }
        turn(Anim::Backward, ctx);
        sound(0x314);
        return true;
    }
    if (nextShown && inside(gx, gy, 0xE9, 0x8A, 0x14, 0x14)) {
        grab_.reset();
        // Forward: one entry on, one list page on, or from the last list page to the first entry.
        if (!listMode_) {
            ++entry_;
        } else if (listPage_ + 1 >= pageCount()) {
            listMode_ = false;
            entry_ = 0;
        } else {
            ++listPage_;
        }
        turn(Anim::Forward, ctx);
        sound(0x314);
        return true;
    }
    if (!listMode_) {
        // The page's icon (bookWndFn): pressed, it's picked up.
        if (inside(gx, gy, 0x3F, 0x13, 0x1E, 0x1E) && entry_ < n) {
            grab_.reset(); // (the icon's own window, bookWndFn)
            std::uint32_t id = 0;
            const auto e = entries_[static_cast<std::size_t>(entry_)];
            if (guide_) {
                if (canSummon(ctx)) {
                    id = nox::things::guideSummonSpell(static_cast<int>(e));
                }
            } else if (entryClass(e, ctx) == 0 || !(nox::client::spellFlags(things_, e) & 0x15000)) {  // 0x15000: the summon and mark spells, which aren't picked up
                id = e; // (a multiclass ability's tagged)
            }
            if (id != 0) {
                drag_ = id;
                iconPressed_ = true;
                sound(0x319); // SpellPickup
            }
        }
        return true;
    }
    // The list (bookListWndProc).
    const int lines = linesPerColumn();
    const float rowY = gy - 0x13;
    if (rowY < 0) {
        return true;
    }
    // The row under the pointer; past x 0x91, the second column's.
    int row = static_cast<int>(rowY) / kLine;
    if (row >= lines) {
        return true;
    }
    if (gx > 0x91) {
        row += lines;
    }
    // The entry's index: each list page holds two columns of `lines` rows.
    const int index = row + listPage_ * perPage();
    if (index >= n) {
        return true;
    }
    const auto e = entries_[static_cast<std::size_t>(index)];
    if (guide_) {
        if (!canSummon(ctx)) {
            moveToPage(index, ctx); // nothing to drag: straight to its page
            return true;
        }
        drag_ = nox::things::guideSummonSpell(static_cast<int>(e));
    } else {
        drag_ = e;
    }
    pressed_ = index;
    pressAt_ = {x / s, y / s};
    sound(0x319); // SpellPickup
    return true;
}

bool Spellbook::mouseUp(float x, float y, int w, int h, const BookContext& ctx, const Actions& actions)
{
    grab_.reset();
    const float s = Hud::scale(w, h);
    if (pressed_) {
        // A click (within 5) turns to the entry's page; a drag lets it go there.
        const int index = *pressed_;
        const std::uint32_t id = drag_;
        pressed_.reset();
        drag_ = 0;
        if (std::abs(x / s - pressAt_.x) < 5 && std::abs(y / s - pressAt_.y) < 5) {
            moveToPage(index, ctx);
        } else if (actions.drop) {
            actions.drop(id, x, y);
        }
        return true;
    }
    if (iconPressed_) {
        iconPressed_ = false;
        const std::uint32_t id = drag_;
        drag_ = 0;
        const SDL_FPoint o = origin(static_cast<int>(static_cast<float>(h) / s));
        if (inside(x / s - o.x, y / s - o.y, 0x3F, 0x13, 0x1E, 0x1E)) {
            if (actions.click) {
                actions.click(id);
            }
        } else if (actions.drop) {
            actions.drop(id, x, y);
        }
        return true;
    }
    return open_ && contains(x, y, w, h);
}

void Spellbook::mouseMove(float x, float y, int w, int h)
{
    // cursorUpdate with the held window DRAGABLE (status 4) and no spell being dragged from the book
    // moved by the pointer's move, kept within the screen.
    if (!open_ || !grab_ || drag_ != 0) {
        return;
    }
    const float s = Hud::scale(w, h);
    const float W = static_cast<float>(w) / s;
    const float H = static_cast<float>(h) / s;
    const SDL_FPoint o = origin(static_cast<int>(H));
    const SDL_FPoint at{x / s, y / s};
    SDL_FPoint to{o.x + at.x - grab_->x, o.y + at.y - grab_->y};
    to.x = std::clamp(to.x, 0.0f, std::max(0.0f, W - kWidth));
    to.y = std::clamp(to.y, 0.0f, std::max(0.0f, H - kHeight));
    moved_ = to;
    grab_ = at;
}
void Spellbook::draw(SDL_Renderer* renderer, const BookContext& ctx, int w, int h)
{
    const float s = Hud::scale(w, h);
    lastHeight_ = static_cast<int>(static_cast<float>(h) / s);
    particles.draw(renderer, ctx.frame, s, 0, 0, static_cast<float>(w) / s, static_cast<float>(h) / s, true);
    if (!open_) {
        return;
    }
    const SDL_FPoint o = origin(lastHeight_);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const auto drawRef = [&](const nox::things::ImageRef& ref, float gx, float gy) {
        const Sprite* sp = sprites_.get(ref);
        if (!sp) {
            return;
        }
        const SDL_FRect dst{(gx + sp->offsetX) * s, (gy + sp->offsetY) * s, sp->width * s, sp->height * s};
        sp->scale = SDL_SCALEMODE_LINEAR;
        drawSprite(renderer, *sp, nullptr, dst);
    };
    const auto drawNamed = [&](const std::string& name, float gx, float gy, std::size_t frame = 0) {
        const nox::things::ImageDef* def = image(name);
        if (!def) {
            return;
        }
        if (def->image) {
            drawRef(*def->image, gx, gy);
        } else if (def->animation && !def->animation->frames.empty()) {
            drawRef(def->animation->frames[std::min(frame, def->animation->frames.size() - 1)], gx, gy);
        }
    };
    // The font's pixel size for the default font's 10-pixel lines at this scale.
    const int size = font_ ? font_->lineSize(kFontHeight, s)
                           : 0;
    const auto measure = [&](const std::string& t) { return font_ ? font_->measure(t, size) / s : 0.0f; };
    const auto say = [&](const std::string& t, float gx, float gy, SDL_Color c) {
        if (font_ && !t.empty()) {
            font_->draw(t, gx * s, gy * s, size, c);
        }
    };
    const auto wrap = [&](const std::string& t, float width) {
        return font_ ? font_->wrap(t, size, width * s) : std::vector<std::string>{};
    };
    /// A string wrapped in `width`, its widest line and height (bookGetStringSize).
    const auto box = [&](const std::string& t, float width) {
        const auto lines = wrap(t, width);
        float widest = 0;
        for (const auto& l : lines) {
            widest = std::max(widest, measure(l));
        }
        return std::pair{widest, static_cast<float>(std::max<std::size_t>(lines.size(), 1) * kFontHeight)};
    };
    const auto paragraph = [&](const std::string& t, float gx, float gy, float width, SDL_Color c) {
        float y = gy;
        for (const auto& l : wrap(t, width)) {
            say(l, gx, y, c);
            y += kFontHeight;
        }
    };
    /// Centred over the page's 0x6C column at x 0x18.
    const auto centred = [&](const std::string& t, float gy) { say(t, o.x + 0x18 + (0x6C - measure(t)) / 2, gy, kInk); };

    // bookDrawList: the book, its lit tab, then the page, or the turning page over it.
    drawNamed("BookOfKnowledge", o.x - 0x18, o.y - 0x4C);
    drawNamed(guide_ ? "GuideTabLit" : "SpellTabLit", o.x - 0x18, o.y - 0x4C);
    if (anim_ != Anim::None) {
        const auto* def = image(anim_ == Anim::Forward ? "BookPageForward" : "BookPageBackward");
        const std::size_t frames = def && def->animation ? def->animation->frames.size() : 0;
        const std::uint32_t delay = def && def->animation ? std::max<std::uint32_t>(def->animation->frameDelay, 1) : 1;
        // The turning page's frame from the frames since it began; once past the last, the page shows again.
        const std::size_t f = (ctx.frame - animStart_) / delay;
        if (f >= frames) {
            anim_ = Anim::None; // its callback: the page shows again (bookClickSpell)
        } else {
            drawNamed(anim_ == Anim::Forward ? "BookPageForward" : "BookPageBackward", o.x - 0x18, o.y - 0x4C, f);
        }
    }
    const int n = static_cast<int>(entries_.size());
    const std::uint8_t cls = ctx.playerClass;
    if (anim_ == Anim::None && listMode_) {
        const int lines = linesPerColumn();
        float x = o.x + 0x4E;
        float y = o.y + 0x13;
        // Two columns of titles from this list page, each centred on its x.
        for (int i = 0; i < lines * 2; ++i) {
            const int index = listPage_ * lines * 2 + i;
            if (index >= n) {
                break;
            }
            if (i == lines) {
                x = o.x + 199;
                y = o.y + 0x13;
            }
            const auto e = entries_[static_cast<std::size_t>(index)];
            SDL_Color c = kInk;
            if (guide_) {
                if ((cls == 2 || ctx.multiclass) && !Hud::spellAllowed(*ctx.world, nox::things::guideSummonSpell(static_cast<int>(e)))) {
                    c = kDisabled;
                }
            } else if (entryClass(e, ctx) != 0 && !Hud::spellAllowed(*ctx.world, e)) {
                c = kDisabled;
            }
            const std::string t = title(nox::client::untagged(e), guide_, entryClass(e, ctx));
            say(t, x - measure(t) / 2, y, c);
            y += kLine;
        }
    // One entry's page: its heading, then the spell's details or the creature's picture, and
    // the description on the facing page.
    } else if (anim_ == Anim::None && entry_ < n) {
        const auto tagged = entries_[static_cast<std::size_t>(entry_)];
        const auto e = nox::client::untagged(tagged); // (a multiclass ability's entry: its id)
        const std::uint8_t ecls = guide_ ? cls : entryClass(tagged, ctx);
        std::string desc = description(e, guide_, ecls);
        float titleH = kFontHeight;
        const auto heading = [&](const std::string& t, float gy) {
            const auto [tw, th] = box(t, 0x6C);
            titleH = th;
            const float gx = th > kFontHeight ? o.x + 0x18 : o.x + 0x18 + (0x6C - tw) / 2;
            paragraph(t, gx, gy, 0x6C, kInk);
        };
        if (guide_) {
            // A creature (bookDrawList's guide page): its size for one who can summon it, its title, its picture.
            const int sizeClass = nox::things::guideSize(things_, static_cast<int>(e));
            int dy = 0, iw = 0x26;
            std::string sizeWord;
            // Size 1 small (its picture lower), 2 medium, else large (a picture twice as wide).
            if (sizeClass == 1) {
                sizeWord = text("Small");
                dy = 0x13;
            } else if (sizeClass == 2) {
                sizeWord = text("Medium");
            } else {
                sizeWord = text("Large");
                iw = 0x4C;
            }
            if (canSummon(ctx)) {
                centred(text("Size") + " " + sizeWord, o.y + 0x33);
            }
            const float ty = o.y + 0x35 + kFontHeight;
            heading(title(e, true, cls), ty);
            drawNamed("Spellbook" + std::string(nox::things::kGuideNames[static_cast<std::size_t>(e - 1)]),
                      o.x + 0x18 + static_cast<float>((0x6C - iw) / 2), ty + 2 + titleH + static_cast<float>(dy));
        } else if (ecls == 0) {
            heading(title(e, false, ecls), o.y + 0x35);
        } else {
            // A spell: its cost, how it's cast, our level.
            const auto* def = things_.findSpell(e);
            const std::uint32_t flags = def ? def->flags : 0;
            heading(title(e, false, ecls), o.y + 0x35);
            float y = o.y + 0x37 + titleH;
            const int cost = def ? def->manaCost : 0;
            // No mana cost: "0" for the spells with that flag bit (Drain Mana and the Marks), else "*".
            centred(text("ManaCost") + " " + (cost != 0 ? std::to_string(cost) : (flags & 0x800000) ? "0" : "*"), y + 2);
            y += 4 + kFontHeight;
            for (const auto& [bit, label] : {std::pair{kInstant, "SpellInstant"}, std::pair{kTargeted, "SpellTargeted"},
                                              std::pair{kAtLocation, "SpellAtLocation"}, std::pair{kHostile, "SpellHostile"}}) {
                if (flags & bit) {
                    centred(text(label), y);
                    y += kFontHeight;
                }
            }
            const auto lv = ctx.world->self().spellLevels.find(static_cast<std::uint8_t>(e));
            std::string power = text("PowerLevel");
            const std::string level = std::to_string(lv != ctx.world->self().spellLevels.end() ? lv->second : 0);
            if (const auto at = power.find("%d"); at != std::string::npos) {
                power.replace(at, 2, level);
            } else {
                power += " " + level;
            }
            centred(power, y + kFontHeight);
        }
        // The description on the facing page, centred up and down but never above y 0x34.
        if (!desc.empty()) {
            const auto [dw, dh] = box(desc, 0x5C);
            (void)dw;
            const float dy = std::max(o.y + 0x34, std::floor((0x8D - dh) / 2) + 0x11 + o.y);
            paragraph(desc, o.x + 0x99, dy, 0x5C, kInk);
        }
        // The icon to drag (bookDrawIconFn).
        std::optional<nox::things::ImageRef> ic;
        if (guide_) {
            if (canSummon(ctx)) {
                ic = hud_.icon(nox::things::guideSummonSpell(static_cast<int>(e)), false);
            }
        } else {
            ic = hud_.icon(e, ecls == 0);
        }
        if (ic) {
            drawRef(*ic, o.x + 0x3F, o.y + 0x13);
        }
    }
    // The arrows: back unless on the first list page, on unless on the last entry.
    if (!(listMode_ && listPage_ == 0)) {
        drawNamed("ArrowW", o.x + 0x18, o.y + 0x8A);
    }
    if (listMode_ || entry_ < n - 1) {
        drawNamed("ArrowE", o.x + 0xE9, o.y + 0x8A);
    }
}

} // namespace noxsdl
