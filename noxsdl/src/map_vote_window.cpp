// The map vote on screen (map_vote_window.hpp): the cards' layout, drawing, and a click's choice.
#include "map_vote_window.hpp"

#include "draw_box.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace noxsdl {

namespace {

constexpr std::uint64_t kResultMs = 5000; // the result stays up this long
// In game pixels (the HUD's 800 x 600, times its scale).
constexpr float kBigCard = 150;
constexpr float kSmallCard = 80;
constexpr float kGap = 10;
constexpr float kPad = 12;

using box::inside;

/// A box's outline `width` pixels thick, growing outward.
void outline(SDL_Renderer* renderer, SDL_FRect r, float width, SDL_Color c)
{
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    for (int i = 0; i < static_cast<int>(std::max(1.0f, std::round(width))); ++i) {
        SDL_RenderRect(renderer, &r);
        r = {r.x - 1, r.y - 1, r.w + 2, r.h + 2};
    }
}

} // namespace

void MapVoteWindow::update(const State& vote, std::uint64_t nowMs)
{
    nowMs_ = nowMs;
    if (vote.open && !wasOpen_) {
        folded_ = false; // (a new vote: unfolded)
    }
    wasOpen_ = vote.open;
    // A new vote state: one that has just closed while shown leaves its result up for kResultMs.
    if (vote.version != version_) {
        const bool closed = version_ != 0 && !vote.open && shown_;
        version_ = vote.version;
        if (closed) {
            resultUntil_ = nowMs + kResultMs;
        }
    }
    shown_ = vote.open || (nowMs < resultUntil_ && !vote.candidates.empty());
    if (!shown_ && resultUntil_ != 0) {
        resultUntil_ = 0;
        thumbnails_.clear();
    }
}

MapVoteWindow::Layout MapVoteWindow::layout(const State& vote, int w, int h, float scale) const
{
    Layout l;
    const int n = static_cast<int>(vote.candidates.size());
    if (n == 0) {
        return l;
    }
    // Kind 0, the vote for the next map: big cards two to a row, the panel centred; a called vote's
    // small cards go in one row at the top.
    const bool big = vote.kind == 0;
    const int cols = big ? std::min(n, 2) : n;
    const int rows = (n + cols - 1) / cols;
    const float s = scale;
    const float card = (big ? kBigCard : kSmallCard) * s;
    l.text = static_cast<float>(font_->lineSize(big ? 11.0f : 9.0f, s));
    const float line = font_->lineHeight(static_cast<int>(l.text));
    const float under = 2 * line + 4 * s;  // name and votes under each picture
    const float header = 2 * line + 6 * s; // title and the time left
    const float pw = cols * card + (cols - 1) * kGap * s + 2 * kPad * s;
    const float ph = folded_ ? header + 2 * kPad * s - 6 * s
                             : header + rows * (card + under) + (rows - 1) * kGap * s + 2 * kPad * s;
    const float px = std::floor((static_cast<float>(w) - pw) / 2);
    // (Folded, at the top whatever its kind: out of the way.)
    const float py = big && !folded_ ? std::floor((static_cast<float>(h) - ph) / 2) : std::floor(40 * s);
    l.panel = {px, py, pw, ph};
    l.toggle = {px + pw - kPad * s - line, py + kPad * s, line, line};
    if (folded_) {
        return l;
    }
    for (int i = 0; i < n; ++i) {
        const int c = i % cols;
        const int r = i / cols;
        l.cards.push_back({px + kPad * s + c * (card + kGap * s), py + kPad * s + header + r * (card + under + kGap * s),
                           card, card});
    }
    return l;
}

void MapVoteWindow::draw(SDL_Renderer* renderer, const State& vote, int w, int h, float scale, SDL_FPoint mouse)
{
    if (!shown_ || !font_) {
        return;
    }
    const Layout l = layout(vote, w, h, scale);
    if (l.panel.w <= 0) {
        return;
    }
    const int size = static_cast<int>(l.text);
    const float line = font_->lineHeight(size);
    const SDL_Color white{235, 235, 235, 255};
    const SDL_Color grey{160, 160, 160, 255};
    const SDL_Color gold{255, 200, 60, 255};

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 190);
    SDL_RenderFillRect(renderer, &l.panel);
    outline(renderer, l.panel, 1, {90, 90, 90, 255});

    // The title and the time left, or the result.
    const float tx = l.panel.x + kPad * scale;
    float ty = l.panel.y + kPad * scale;
    std::string title;
    std::string sub;
    if (vote.open) {
        title = vote.kind == 0 ? "Vote for the next map" : "Map vote: a majority changes the map now";
        const auto now = static_cast<std::uint32_t>(nowMs_);
        // Whole seconds left, rounded up; the 32-bit difference read as signed so a passed end shows 0.
        const int left = static_cast<int>(vote.endsAtMs - now) > 0 ? static_cast<int>((vote.endsAtMs - now + 999) / 1000) : 0;
        sub = std::format("{} s left - click a map, or type its number in chat", left);
    } else if (vote.result && *vote.result >= 0 && *vote.result < static_cast<int>(vote.candidates.size())) {
        title = "Next map: " + vote.candidates[static_cast<std::size_t>(*vote.result)];
    } else {
        title = vote.kind == 0 ? "No votes: the map cycle goes on" : "The vote failed";
    }
    font_->draw(title, tx, ty, size, vote.open ? white : gold);
    ty += line;
    if (!sub.empty()) {
        font_->draw(sub, tx, ty, size, grey);
    }
    {
        // The fold arrow: up folds the cards away, down brings them back.
        const SDL_FRect& t = l.toggle;
        const bool hover = inside(t, mouse.x, mouse.y);
        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 220);
        SDL_RenderFillRect(renderer, &t);
        outline(renderer, t, 1, hover ? white : SDL_Color{110, 110, 110, 255});
        const SDL_FColor c = hover ? SDL_FColor{1, 1, 1, 1} : SDL_FColor{0.85f, 0.75f, 0.45f, 1};
        const float cx = t.x + t.w / 2;
        const float m = t.w * 0.28f;
        const float top = t.y + t.h * 0.3f;
        const float bottom = t.y + t.h * 0.7f;
        SDL_Vertex tri[3];
        if (folded_) { // ▼
            tri[0] = {{cx - m, top}, c, {}};
            tri[1] = {{cx + m, top}, c, {}};
            tri[2] = {{cx, bottom}, c, {}};
        } else { // ▲
            tri[0] = {{cx - m, bottom}, c, {}};
            tri[1] = {{cx + m, bottom}, c, {}};
            tri[2] = {{cx, top}, c, {}};
        }
        SDL_RenderGeometry(renderer, nullptr, tri, 3, nullptr, 0);
    }

    // Each card: the map's rendered top-down picture, or a note while it is being drawn or missing.
    for (std::size_t i = 0; i < l.cards.size(); ++i) {
        const SDL_FRect& r = l.cards[i];
        const auto& picture = thumbnails_.get(renderer, vote.candidates[i]);
        SDL_SetRenderDrawColor(renderer, 16, 16, 16, 255);
        SDL_RenderFillRect(renderer, &r);
        if (picture.texture) {
            SDL_RenderTexture(renderer, picture.texture, nullptr, &r);
        } else {
            const char* what = picture.done ? "not on this computer" : "drawing...";
            const float tw = font_->measure(what, size);
            font_->draw(what, r.x + (r.w - tw) / 2, r.y + (r.h - line) / 2, size, grey);
        }
        // The frame: gold for our own choice or the winner, white under the mouse while voting.
        const bool mine = vote.mine && *vote.mine == static_cast<int>(i);
        const bool won = !vote.open && vote.result && *vote.result == static_cast<int>(i);
        const bool hover = vote.open && inside(r, mouse.x, mouse.y);
        if (mine || won) {
            outline(renderer, r, 3 * scale / 2, gold);
        } else if (hover) {
            outline(renderer, r, scale, white);
        } else {
            outline(renderer, r, 1, {70, 70, 70, 255});
        }
        // Its number, in the corner, for the chat vote.
        const std::string number = std::to_string(i + 1);
        const float nw = font_->measure(number, size) + 6 * scale;
        const SDL_FRect badge{r.x + 2, r.y + 2, nw, line};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
        SDL_RenderFillRect(renderer, &badge);
        font_->draw(number, badge.x + 3 * scale, badge.y, size, mine ? gold : white);
        // The name (cut to the card) and the votes.
        std::string name = vote.candidates[i];
        while (name.size() > 1 && font_->measure(name, size) > r.w) {
            name.pop_back();
        }
        font_->draw(name, r.x, r.y + r.h + 2 * scale, size, mine || won ? gold : white);
        const int votes = i < vote.tally.size() ? vote.tally[i] : 0;
        font_->draw(votes == 1 ? "1 vote" : std::format("{} votes", votes), r.x, r.y + r.h + 2 * scale + line, size, grey);
    }
}

bool MapVoteWindow::handleEvent(const SDL_Event& event, const State& vote, int w, int h, float scale,
                                std::optional<int>& choice)
{
    if (!shown_) {
        return false;
    }
    float x = 0;
    float y = 0;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        x = event.button.x;
        y = event.button.y;
    } else {
        // (Motion, releases, the wheel and the other buttons go on: walking with the right
        // button under a called vote's row at the top still works.)
        return false;
    }
    const Layout l = layout(vote, w, h, scale);
    if (!inside(l.panel, x, y)) {
        return false;
    }
    if (inside(l.toggle, x, y)) {
        folded_ = !folded_;
        return true;
    }
    if (vote.open) {
        for (std::size_t i = 0; i < l.cards.size(); ++i) {
            if (inside(l.cards[i], x, y)) {
                choice = static_cast<int>(i);
            }
        }
    }
    return true;
}

} // namespace noxsdl
