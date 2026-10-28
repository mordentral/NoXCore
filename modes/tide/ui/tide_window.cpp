// TideWindow (tide_window.hpp): the Undead Tide's strip, level-up cards, results and the
// difficulty question, drawn with plain rectangles and the HUD's font over the game view.
#include "tide_window.hpp"

#include "draw_box.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace noxsdl {

namespace {

namespace ts = nox::server;

// In the HUD's game pixels (its 800 x 600, times its scale).
constexpr float kBarH = 7;
constexpr float kWeaponIcon = 28;
constexpr float kPassiveIcon = 20;
constexpr float kCardW = 180;
constexpr float kCardH = 136;
constexpr float kCardGap = 14;
constexpr float kPad = 12;
constexpr float kResultsW = 470;
constexpr float kRowH = 24;

const SDL_Color kWhite{235, 235, 235, 255};
const SDL_Color kGrey{160, 160, 170, 255};
const SDL_Color kGold{255, 205, 70, 255};
const SDL_Color kBlue{110, 175, 255, 255};
const SDL_Color kRed{240, 80, 70, 255};

using box::fill;
using box::inside;
using box::outline;

// Seconds as m:ss.
std::string clock(unsigned seconds)
{
    const unsigned m = seconds / 60;
    const unsigned s = seconds % 60;
    return std::to_string(m) + (s < 10 ? ":0" : ":") + std::to_string(s);
}

/// The point on the edge of the square `box` straight out from its middle at `angle` (radians,
/// clockwise from up).
SDL_FPoint edgeAt(const SDL_FRect& box, float angle)
{
    const float cx = box.x + box.w / 2;
    const float cy = box.y + box.h / 2;
    const float dx = std::sin(angle);
    const float dy = -std::cos(angle);
    const float k = (box.w / 2) / std::max(std::abs(dx), std::abs(dy));
    return {cx + dx * k, cy + dy * k};
}

/// The part of `box` still waiting (`left` of a turn, from up, clockwise) shaded over the icon.
void sweep(SDL_Renderer* renderer, const SDL_FRect& box, float left)
{
    if (left <= 0) {
        return;
    }
    constexpr float kTurn = 6.2831853f;
    const float end = std::min(1.0f, left) * kTurn;
    // A fan from the middle through the corners the shaded part passes (an eighth of a turn past each
    // quarter), a triangle out to the edge between each pair of angles.
    std::vector<float> angles{0.0f};
    for (int c = 0; c < 4; ++c) {
        const float corner = kTurn * (0.125f + 0.25f * static_cast<float>(c));
        if (corner < end) {
            angles.push_back(corner);
        }
    }
    angles.push_back(end);
    const SDL_FColor shade{0, 0, 0, 0.62f};
    std::vector<SDL_Vertex> v;
    const SDL_FPoint mid{box.x + box.w / 2, box.y + box.h / 2};
    for (std::size_t i = 0; i + 1 < angles.size(); ++i) {
        v.push_back({mid, shade, {0, 0}});
        v.push_back({edgeAt(box, angles[i]), shade, {0, 0}});
        v.push_back({edgeAt(box, angles[i + 1]), shade, {0, 0}});
    }
    SDL_RenderGeometry(renderer, nullptr, v.data(), static_cast<int>(v.size()), nullptr, 0);
}

// A pick's name and its line of what it does.
std::string_view pickName(const ts::TidePick& p)
{
    switch (p.kind) {
    case ts::TidePick::Kind::Weapon: return ts::tideWeaponName(static_cast<ts::TideWeapon>(p.which));
    case ts::TidePick::Kind::Passive: return ts::tidePassiveName(static_cast<ts::TidePassive>(p.which));
    case ts::TidePick::Kind::Heal: return "Heal";
    }
    return "";
}

std::string_view pickText(const ts::TidePick& p)
{
    switch (p.kind) {
    case ts::TidePick::Kind::Weapon: return ts::tideWeaponText(static_cast<ts::TideWeapon>(p.which));
    case ts::TidePick::Kind::Passive: return ts::tidePassiveText(static_cast<ts::TidePassive>(p.which));
    case ts::TidePick::Kind::Heal: return "All your health back.";
    }
    return "";
}

} // namespace

void TideWindow::update(const std::optional<Hud>& hud, std::uint32_t hudVersion, const Offer& offer, std::uint32_t offerVersion,
                        const std::optional<Results>& results, std::uint32_t resultsVersion, std::uint64_t nowMs)
{
    (void)hud;
    (void)offer;
    if (hudVersion != hudVersion_) {
        hudVersion_ = hudVersion;
        hudAt_ = nowMs;
    }
    if (offerVersion != offerVersion_) {
        offerVersion_ = offerVersion;
        offerAt_ = nowMs;
    }
    if (resultsVersion != resultsVersion_) {
        resultsVersion_ = resultsVersion;
        resultsOpen_ = results.has_value();
    }
}

void TideWindow::drawIcon(SDL_Renderer* renderer, const Sprite* icon, std::string_view name, SDL_FRect box, int text) const
{
    if (icon && icon->texture && icon->width > 0 && icon->height > 0) {
        const float k = std::min(box.w / icon->width, box.h / icon->height);
        const float iw = icon->width * k;
        const float ih = icon->height * k;
        drawSprite(renderer, *icon, nullptr, {box.x + (box.w - iw) / 2, box.y + (box.h - ih) / 2, iw, ih});
        return;
    }
    fill(renderer, box, {40, 40, 70, 230});
    outline(renderer, box, {90, 90, 140, 255});
    if (!name.empty()) {
        const std::string letter(1, name[0]);
        font_->draw(letter, box.x + (box.w - font_->measure(letter, text)) / 2, box.y + (box.h - font_->lineHeight(text)) / 2, text, kWhite);
    }
}

const Sprite* TideWindow::iconOf(const ts::TidePick& p) const
{
    if (p.kind == ts::TidePick::Kind::Weapon) {
        return weaponIcon_ ? weaponIcon_(static_cast<ts::TideWeapon>(p.which)) : nullptr;
    }
    if (p.kind == ts::TidePick::Kind::Passive) {
        return passiveIcon_ ? passiveIcon_(static_cast<ts::TidePassive>(p.which)) : nullptr;
    }
    return passiveIcon_ ? passiveIcon_(ts::TidePassive::Recovery) : nullptr; // (a heal's: Recovery's)
}

void TideWindow::drawStrip(SDL_Renderer* renderer, const Hud& hud, int w, float s, std::uint64_t nowMs) const
{
    // Pixel sizes giving 11- and 17-game-pixel lines at this scale.
    const int text = font_->lineSize(11, s, 8);
    const int big = font_->lineSize(17, s, 10);
    const float fw = static_cast<float>(w);
    // The XP bar along the very top.
    const SDL_FRect bar{0, 0, fw, std::round(kBarH * s)};
    fill(renderer, bar, {8, 8, 22, 210});
    // The filled part: the XP toward the next level.
    const float part = std::clamp(static_cast<float>(hud.xp) / static_cast<float>(std::max<std::uint16_t>(1, hud.need)), 0.0f, 1.0f);
    fill(renderer, {0, 0, std::round(fw * part), bar.h}, {60, 135, 245, 240});
    fill(renderer, {0, bar.h - 1, fw, 1}, {20, 30, 70, 255});
    // The clock in the middle, the level to its left, the slain to its right.
    float y = bar.h + 3 * s;
    const std::string time = clock(hud.seconds);
    const float tw = font_->measure(time, big);
    font_->draw(time, (fw - tw) / 2, y, big, kWhite);
    const float mid = y + (font_->lineHeight(big) - font_->lineHeight(text)) / 2;
    const std::string level = "Level " + std::to_string(hud.level);
    font_->draw(level, (fw - tw) / 2 - 18 * s - font_->measure(level, text), mid, text, kBlue);
    font_->draw(std::to_string(hud.kills) + " slain", (fw + tw) / 2 + 18 * s, mid, text, kGrey);
    if (hud.stage > 1) { // (the land the party's carried on to, by its guardians slain)
        const std::string stage = "Stage " + std::to_string(hud.stage);
        font_->draw(stage, (fw - tw) / 2 - 18 * s - font_->measure(level, text) - 18 * s - font_->measure(stage, text), mid, text, kGold);
    }
    y += font_->lineHeight(big);
    std::string under;
    SDL_Color underColour = kGrey;
    // Under it, the guardian and dawn (boss 0: not come yet, 1: walking, 2: slain).
    if (hud.boss == 0) {
        under = hud.bossAt >= hud.dawn ? "The night's guardian comes at dawn, " + clock(hud.bossAt)
                                       : "Guardian at " + clock(hud.bossAt) + "   Dawn at " + clock(hud.dawn);
    } else if (hud.boss == 1) {
        under = hud.bossAt >= hud.dawn ? std::string("The night's guardian walks! Slay it to go on.") : "The night's guardian walks!   Dawn at " + clock(hud.dawn);
        underColour = kRed;
    } else {
        under = "The guardian is slain: on to a new land";
        underColour = kGold;
    }
    font_->draw(under, (fw - font_->measure(under, text)) / 2, y, text, underColour);
    y += font_->lineHeight(text) + 4 * s;
    // The weapons, then the passives taken, centred under it.
    std::vector<std::size_t> passives;
    for (std::size_t k = 0; k < hud.passives.size() && k < ts::kTidePassives; ++k) {  // passives[k] is that passive's level, 0 untaken
        if (hud.passives[k] != 0) {
            passives.push_back(k);
        }
    }
    const float wi = std::round(kWeaponIcon * s);
    const float pi = std::round(kPassiveIcon * s);
    const float gap = std::round(4 * s);
    // The row's width: the weapons, a 10-pixel gap, then the passives.
    const float rowW = static_cast<float>(hud.weapons.size()) * (wi + gap) + (passives.empty() ? 0 : 10 * s) +
                       static_cast<float>(passives.size()) * (pi + gap);
    float x = std::round((fw - rowW) / 2);
    const int small = font_->lineSize(9, s, 7);
    const float ran = static_cast<float>(nowMs - hudAt_) * 30.0f / 1000.0f; // (frames since the server's word)
    // A box's level, gold on a dark tag in its bottom right corner.
    const auto levelTag = [&](const SDL_FRect& box, int n) {
        const std::string t = std::to_string(n);
        const float lw = font_->measure(t, small);
        const float lh = font_->lineHeight(small);
        const SDL_FRect tag{box.x + box.w - lw - 3 * s, box.y + box.h - lh, lw + 3 * s, lh};
        fill(renderer, tag, {0, 0, 0, 190});
        font_->draw(t, tag.x + 1.5f * s, tag.y, small, kGold);
    };
    for (const Hud::Weapon& wp : hud.weapons) {
        const SDL_FRect box{x, y, wi, wi};
        fill(renderer, box, {0, 0, 0, 140});
        const auto kind = static_cast<ts::TideWeapon>(wp.kind);
        drawIcon(renderer, weaponIcon_ ? weaponIcon_(kind) : nullptr, ts::tideWeaponName(kind), box, text);
        if (wp.cooldown != 0) {
            // The wait left (in frames, less those run since the server's word) of the whole cooldown.
            sweep(renderer, box, (static_cast<float>(wp.readyIn) - ran) / static_cast<float>(wp.cooldown));
        }
        outline(renderer, box, {120, 110, 80, 255});
        levelTag(box, wp.level);
        x += wi + gap;
    }
    x += passives.empty() ? 0 : 10 * s;
    for (const std::size_t k : passives) {
        const SDL_FRect box{x, y + (wi - pi) / 2, pi, pi};
        fill(renderer, box, {0, 0, 0, 140});
        const auto kind = static_cast<ts::TidePassive>(k);
        drawIcon(renderer, passiveIcon_ ? passiveIcon_(kind) : nullptr, ts::tidePassiveName(kind), box, small);
        outline(renderer, box, {70, 90, 130, 255});
        levelTag(box, hud.passives[k]);
        x += pi + gap;
    }
}

// The cards in a row centred a little above the middle, under a title and a hint.
TideWindow::CardsLayout TideWindow::cardsLayout(const Offer& offer, int w, int h, float s) const
{
    CardsLayout l;
    l.text = font_->lineSize(11, s, 8);
    l.big = font_->lineSize(15, s, 10);
    l.line = font_->lineHeight(l.text);
    const float n = static_cast<float>(offer.picks.size());
    const float cw = std::round(kCardW * s);
    const float ch = std::round(kCardH * s);
    const float gap = std::round(kCardGap * s);
    const float header = font_->lineHeight(l.big) + l.line + 8 * s;
    const float aw = n * cw + (n - 1) * gap;
    l.area = {std::floor((static_cast<float>(w) - aw) / 2) - kPad * s, std::floor(static_cast<float>(h) * 0.42f - (ch + header) / 2) - kPad * s,
              aw + 2 * kPad * s, ch + header + 2 * kPad * s};
    for (std::size_t i = 0; i < offer.picks.size(); ++i) {
        l.cards.push_back({l.area.x + kPad * s + static_cast<float>(i) * (cw + gap), l.area.y + kPad * s + header, cw, ch});
    }
    return l;
}

void TideWindow::drawCards(SDL_Renderer* renderer, const Offer& offer, int w, int h, float s, SDL_FPoint mouse, std::uint64_t nowMs) const
{
    const CardsLayout l = cardsLayout(offer, w, h, s);
    fill(renderer, l.area, {6, 8, 20, 215});
    outline(renderer, l.area, {70, 90, 150, 255});
    const std::string title = "Level " + std::to_string(offer.level) + "!  Choose one";
    font_->draw(title, l.area.x + (l.area.w - font_->measure(title, l.big)) / 2, l.area.y + kPad * s, l.big, kGold);
    const auto gone = static_cast<int>((nowMs - offerAt_) / 1000);
    const int left = std::max(0, static_cast<int>(offer.seconds) - gone);
    const std::string hint = "Click one, or press 1-" + std::to_string(offer.picks.size()) + "   (" + std::to_string(left) + " s, then the first)";
    font_->draw(hint, l.area.x + (l.area.w - font_->measure(hint, l.text)) / 2, l.area.y + kPad * s + font_->lineHeight(l.big), l.text, kGrey);
    for (std::size_t i = 0; i < offer.picks.size() && i < l.cards.size(); ++i) {
        const ts::TidePick& p = offer.picks[i];
        const SDL_FRect& c = l.cards[i];
        const bool hover = inside(c, mouse.x, mouse.y);
        fill(renderer, c, hover ? SDL_Color{34, 36, 70, 240} : SDL_Color{18, 20, 42, 240});
        outline(renderer, c, hover ? kGold : SDL_Color{80, 90, 140, 255});
        // Its key in the corner.
        const std::string key = std::to_string(i + 1);
        const SDL_FRect keyBox{c.x + 5 * s, c.y + 5 * s, l.line + 2 * s, l.line + 2 * s};
        fill(renderer, keyBox, {0, 0, 0, 200});
        outline(renderer, keyBox, {120, 120, 150, 255});
        font_->draw(key, keyBox.x + (keyBox.w - font_->measure(key, l.text)) / 2, keyBox.y + s, l.text, kWhite);
        // The icon, the name, new or its levels, what it does.
        const float is = std::round(44 * s);
        const SDL_FRect iconBox{c.x + (c.w - is) / 2, c.y + 10 * s, is, is};
        drawIcon(renderer, iconOf(p), pickName(p), iconBox, l.big);
        float y = iconBox.y + is + 6 * s;
        const std::string name(pickName(p));
        font_->draw(name, c.x + (c.w - font_->measure(name, l.big)) / 2, y, l.big, hover ? kGold : kWhite);
        y += font_->lineHeight(l.big);
        std::string what;
        SDL_Color whatColour = kBlue;
        if (p.kind == ts::TidePick::Kind::Heal) {
            what = "Nothing left to learn";
            whatColour = kGrey;
        } else if (p.level <= 1) {
            what = p.kind == ts::TidePick::Kind::Weapon ? "New weapon!" : "New!";
            whatColour = kGold;
        } else {
            what = "Level " + std::to_string(p.level - 1) + " > " + std::to_string(p.level);
        }
        font_->draw(what, c.x + (c.w - font_->measure(what, l.text)) / 2, y, l.text, whatColour);
        y += l.line + 3 * s;
        for (const std::string& line : font_->wrap(pickText(p), l.text, c.w - 14 * s)) {
            if (y + l.line > c.y + c.h - 3 * s) {
                break;
            }
            font_->draw(line, c.x + (c.w - font_->measure(line, l.text)) / 2, y, l.text, kGrey);
            y += l.line;
        }
    }
}

// The results panel: a header, the column names and a row a player, OK at the bottom.
TideWindow::ResultsLayout TideWindow::resultsLayout(const Results& results, int w, int h, float s) const
{
    ResultsLayout l;
    l.text = font_->lineSize(11, s, 8);
    l.big = font_->lineSize(18, s, 10);
    l.line = font_->lineHeight(l.text);
    const float pw = std::round(kResultsW * s);
    const float rows = static_cast<float>(results.players.size() + 1);
    const float ph = kPad * s * 2 + font_->lineHeight(l.big) + l.line * 2 + 10 * s + rows * kRowH * s + 10 * s + l.line + 10 * s;
    l.panel = {std::floor((static_cast<float>(w) - pw) / 2), std::floor((static_cast<float>(h) - ph) / 2 - 30 * s), pw, ph};
    const float bw = std::round(80 * s);
    const float bh = l.line + 8 * s;
    l.ok = {l.panel.x + (pw - bw) / 2, l.panel.y + ph - kPad * s - bh, bw, bh};
    return l;
}

void TideWindow::drawResults(SDL_Renderer* renderer, const Results& r, int w, int h, float s, SDL_FPoint mouse) const
{
    const ResultsLayout l = resultsLayout(r, w, h, s);
    fill(renderer, l.panel, {8, 6, 16, 235});
    outline(renderer, l.panel, r.won ? SDL_Color{170, 140, 60, 255} : SDL_Color{150, 50, 45, 255});
    const std::string title = r.won && r.boss == 2 ? "The guardian falls!" : r.won ? "Dawn breaks!" : "The tide takes everyone";
    float y = l.panel.y + kPad * s;
    font_->draw(title, l.panel.x + (l.panel.w - font_->measure(title, l.big)) / 2, y, l.big, r.won ? kGold : kRed);
    y += font_->lineHeight(l.big);
    const std::string time = (r.stage > 1 ? "Stage " + std::to_string(r.stage) + "   " : std::string()) + (r.won ? "Survived " : "Fell at ") +
                             clock(r.seconds) + "   " + std::to_string(r.elites) +
                             (r.elites == 1 ? " elite slain" : " elites slain");
    font_->draw(time, l.panel.x + (l.panel.w - font_->measure(time, l.text)) / 2, y, l.text, kWhite);
    y += l.line;
    const std::string guardian = r.boss == 0   ? std::string("The night's guardian never came")
                                 : r.boss == 2 ? "The night's guardian slain" + (r.bossName.empty() ? std::string() : " (" + r.bossName + ")")
                                               : "The night's guardian still stands" + (r.bossName.empty() ? std::string() : " (" + r.bossName + ")");
    font_->draw(guardian, l.panel.x + (l.panel.w - font_->measure(guardian, l.text)) / 2, y, l.text, kGrey);
    y += l.line + 10 * s;
    // The players: name, level, slain, weapons.
    const float x0 = l.panel.x + kPad * s;
    const float xLevel = x0 + 150 * s;
    const float xSlain = x0 + 200 * s;
    const float xKit = x0 + 250 * s;
    const float rh = kRowH * s;
    const float ty = (rh - l.line) / 2;
    font_->draw("Player", x0, y + ty, l.text, kGrey);
    font_->draw("Level", xLevel, y + ty, l.text, kGrey);
    font_->draw("Slain", xSlain, y + ty, l.text, kGrey);
    font_->draw("Weapons", xKit, y + ty, l.text, kGrey);
    y += rh;
    for (const Results::Player& p : r.players) {
        fill(renderer, {x0 - 4 * s, y, l.panel.w - 2 * kPad * s + 8 * s, rh - 2 * s}, {22, 20, 36, 200});
        std::string name = p.name;
        // A name cut to fit its column.
        while (name.size() > 1 && font_->measure(name, l.text) > xLevel - x0 - 8 * s) {
            name.pop_back();
        }
        font_->draw(name, x0, y + ty, l.text, kWhite);
        font_->draw(std::to_string(p.level), xLevel, y + ty, l.text, kBlue);
        font_->draw(std::to_string(p.kills), xSlain, y + ty, l.text, kWhite);
        float x = xKit;
        const float is = rh - 6 * s;
        for (const auto& [kind, level] : p.weapons) {
            if (x + is > l.panel.x + l.panel.w - kPad * s) {
                break;
            }
            const auto k = static_cast<ts::TideWeapon>(kind);
            const SDL_FRect box{x, y + 2 * s, is, is};
            drawIcon(renderer, weaponIcon_ ? weaponIcon_(k) : nullptr, ts::tideWeaponName(k), box, l.text);
            const std::string t = std::to_string(level);
            font_->draw(t, box.x + box.w - font_->measure(t, l.text) * 0.6f, box.y + box.h - l.line * 0.7f, l.text, kGold);
            x += is + 6 * s;
        }
        y += rh;
    }
    const bool hover = inside(l.ok, mouse.x, mouse.y);
    fill(renderer, l.ok, hover ? SDL_Color{60, 55, 30, 255} : SDL_Color{30, 28, 20, 255});
    outline(renderer, l.ok, hover ? kGold : SDL_Color{120, 110, 70, 255});
    font_->draw("OK", l.ok.x + (l.ok.w - font_->measure("OK", l.text)) / 2, l.ok.y + (l.ok.h - l.line) / 2, l.text, hover ? kGold : kWhite);
}

void TideWindow::draw(SDL_Renderer* renderer, const std::optional<Hud>& hud, const Offer& offer, const std::optional<Results>& results, int w,
                      int h, float scale, SDL_FPoint mouse, std::uint64_t nowMs) const
{
    if (!font_) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (hud) {
        drawStrip(renderer, *hud, w, scale, nowMs);
    }
    if (!offer.picks.empty()) {
        drawCards(renderer, offer, w, h, scale, mouse, nowMs);
    }
    if (resultsOpen_ && results) {
        drawResults(renderer, *results, w, h, scale, mouse);
    }
}

// The question's panel: a header, a row a difficulty, Cancel.
TideWindow::AskLayout TideWindow::askLayout(int w, int h, float s) const
{
    AskLayout l;
    l.text = font_->lineSize(11, s, 8);
    l.big = font_->lineSize(16, s, 10);
    l.line = font_->lineHeight(l.text);
    const float pw = std::round(420 * s);
    const float rowH = std::round(l.line * 2 + 10 * s);
    const float header = font_->lineHeight(l.big) + l.line + 10 * s;
    const float bh = l.line + 8 * s;
    const float ph = kPad * s * 2 + header + static_cast<float>(ts::kTideDifficulties) * (rowH + 4 * s) + 8 * s + bh;
    l.panel = {std::floor((static_cast<float>(w) - pw) / 2), std::floor((static_cast<float>(h) - ph) / 2 - 30 * s), pw, ph};
    for (std::size_t i = 0; i < ts::kTideDifficulties; ++i) {
        l.rows.push_back({l.panel.x + kPad * s, l.panel.y + kPad * s + header + static_cast<float>(i) * (rowH + 4 * s), pw - 2 * kPad * s, rowH});
    }
    const float bw = std::round(90 * s);
    l.cancel = {l.panel.x + (pw - bw) / 2, l.panel.y + ph - kPad * s - bh, bw, bh};
    return l;
}

void TideWindow::drawAsk(SDL_Renderer* renderer, const std::optional<std::uint8_t>& ask, int w, int h, float s, SDL_FPoint mouse) const
{
    if (!ask || !font_) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const AskLayout l = askLayout(w, h, s);
    fill(renderer, l.panel, {8, 6, 16, 235});
    outline(renderer, l.panel, {120, 100, 160, 255});
    float y = l.panel.y + kPad * s;
    const std::string title = "Begin the run at which difficulty?";
    font_->draw(title, l.panel.x + (l.panel.w - font_->measure(title, l.big)) / 2, y, l.big, kGold);
    y += font_->lineHeight(l.big);
    const std::string hint = "Click one, or press 1-4 (Enter: the one marked). Everyone here comes along.";
    font_->draw(hint, l.panel.x + (l.panel.w - font_->measure(hint, l.text)) / 2, y, l.text, kGrey);
    static constexpr SDL_Color kTint[] = {{120, 200, 120, 255}, {230, 200, 90, 255}, {240, 130, 60, 255}, {230, 70, 70, 255}};
    for (std::size_t i = 0; i < l.rows.size(); ++i) {
        const SDL_FRect& r = l.rows[i];
        const auto d = static_cast<ts::TideDifficulty>(i);
        const bool hover = inside(r, mouse.x, mouse.y);
        const bool now = *ask == i;
        fill(renderer, r, hover ? SDL_Color{40, 34, 60, 240} : SDL_Color{20, 18, 34, 240});
        outline(renderer, r, hover || now ? kGold : SDL_Color{70, 64, 100, 255});
        const std::string name = std::to_string(i + 1) + ".  " + std::string(ts::tideDifficultyName(d)) + (now ? "   (set now)" : "");
        font_->draw(name, r.x + 8 * s, r.y + 4 * s, l.text, kTint[i]);
        std::string about(ts::tideDifficultyAbout(d));
        while (about.size() > 1 && font_->measure(about, l.text) > r.w - 24 * s) {
            about.pop_back();
        }
        font_->draw(about, r.x + 20 * s, r.y + 4 * s + l.line, l.text, kGrey);
    }
    const bool hover = inside(l.cancel, mouse.x, mouse.y);
    fill(renderer, l.cancel, hover ? SDL_Color{60, 55, 30, 255} : SDL_Color{30, 28, 20, 255});
    outline(renderer, l.cancel, hover ? kGold : SDL_Color{120, 110, 70, 255});
    font_->draw("Cancel", l.cancel.x + (l.cancel.w - font_->measure("Cancel", l.text)) / 2, l.cancel.y + (l.cancel.h - l.line) / 2, l.text,
                hover ? kGold : kWhite);
}

bool TideWindow::handleAskEvent(const SDL_Event& event, const std::optional<std::uint8_t>& ask, int w, int h, float scale,
                                std::optional<std::uint8_t>& chosen, bool& dismissed) const
{
    if (!ask || !font_) {
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        const SDL_Keycode k = event.key.key;
        if (k >= SDLK_1 && k < SDLK_1 + static_cast<SDL_Keycode>(ts::kTideDifficulties)) {
            chosen = static_cast<std::uint8_t>(k - SDLK_1);
            return true;
        }
        if (k >= SDLK_KP_1 && k < SDLK_KP_1 + static_cast<SDL_Keycode>(ts::kTideDifficulties)) {
            chosen = static_cast<std::uint8_t>(k - SDLK_KP_1);
            return true;
        }
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            chosen = *ask;
            return true;
        }
        if (k == SDLK_ESCAPE) {
            dismissed = true;
            return true;
        }
        return false;
    }
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    const AskLayout l = askLayout(w, h, scale);
    if (!inside(l.panel, event.button.x, event.button.y)) {
        return false; // (walking off the gate goes on: the server puts the question away)
    }
    for (std::size_t i = 0; i < l.rows.size(); ++i) {
        if (inside(l.rows[i], event.button.x, event.button.y)) {
            chosen = static_cast<std::uint8_t>(i);
        }
    }
    if (inside(l.cancel, event.button.x, event.button.y)) {
        dismissed = true;
    }
    return true;
}

bool TideWindow::handleEvent(const SDL_Event& event, const Offer& offer, const std::optional<Results>& results, int w, int h, float scale,
                             std::optional<std::size_t>& pick)
{
    if (!font_) {
        return false;
    }
    // The results take Enter, Escape, OK and any click on them first.
    if (resultsOpen_ && results) {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE)) {
            resultsOpen_ = false;
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            const ResultsLayout l = resultsLayout(*results, w, h, scale);
            if (inside(l.ok, event.button.x, event.button.y)) {
                resultsOpen_ = false;
                return true;
            }
            if (inside(l.panel, event.button.x, event.button.y)) {
                return true;
            }
        }
    }
    if (offer.picks.empty()) {
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        int n = -1;
        if (event.key.key >= SDLK_1 && event.key.key <= SDLK_9) {
            n = static_cast<int>(event.key.key - SDLK_1);
        } else if (event.key.key >= SDLK_KP_1 && event.key.key <= SDLK_KP_9) {
            n = static_cast<int>(event.key.key - SDLK_KP_1);
        }
        if (n >= 0 && static_cast<std::size_t>(n) < offer.picks.size()) {
            pick = static_cast<std::size_t>(n);
            return true;
        }
        return false;
    }
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    const CardsLayout l = cardsLayout(offer, w, h, scale);
    if (!inside(l.area, event.button.x, event.button.y)) {
        return false; // (walking on goes on)
    }
    for (std::size_t i = 0; i < offer.picks.size() && i < l.cards.size(); ++i) {
        if (inside(l.cards[i], event.button.x, event.button.y)) {
            pick = i;
        }
    }
    return true;
}

} // namespace noxsdl
