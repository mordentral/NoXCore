// SummonWindow (summon_window.hpp): the creature cage that slides down at the screen's top right,
// its order button and the command list, in game pixels at the HUD's scale.
#include "summon_window.hpp"

#include "hud.hpp"
#include "item_names.hpp"

#include "noxcore/things/field_guide.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace noxsdl {

namespace {

constexpr int kPanelW = 0x57, kPanelH = 0x73;
constexpr int kHiddenY = -0x91;  // (slid up off the top)
constexpr int kSlide = 20;  // (game pixels a game frame)
constexpr float kCell = 0x26;
constexpr float kFontHeight = 10; ///< the default font's height in game pixels
/// The list's lines, by command number; 2 (idle) has none.
constexpr std::array<const char*, 6> kCommands{"ccs:BANISH", "ccs:OBSERVE", "ccs:IDLE", "ccs:GUARD", "ccs:ESCORT",
                                               "ccs:HUNT"};
// The list's lines in order: Banish, Observe, Guard, Escort, Hunt.
constexpr std::array<int, 5> kLines{0, 1, 3, 4, 5};

constexpr SDL_Color kYellow{255, 255, 0, 255};
constexpr SDL_Color kWhite{255, 255, 255, 255};
constexpr SDL_Color kBlue{0, 0xA0, 0xFF, 255};
constexpr SDL_Color kGrey{0x73, 0x73, 0x73, 255};
constexpr SDL_Color kBlack{0, 0, 0, 255};

void fill(SDL_Renderer* r, float x, float y, float w, float h, SDL_Color c, float s)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    const SDL_FRect rect{x * s, y * s, w * s, h * s};
    SDL_RenderFillRect(r, &rect);
}

} // namespace

SummonWindow::SummonWindow(const nox::things::ThingDb& things, const nox::strings::StringTable* strings,
                           SpriteCache& sprites, Font* font)
    : things_(things), strings_(strings), sprites_(sprites), font_(font)
{
    for (const auto& i : things_.images) {
        images_.emplace(i.name, &i);
    }
}

std::string SummonWindow::text(const std::string& id) const
{
    if (strings_) {
        if (const auto t = strings_->text(id)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return {};
}

const nox::things::ImageDef* SummonWindow::image(const std::string& name) const
{
    const auto it = images_.find(name);
    return it == images_.end() ? nullptr : it->second;
}

void SummonWindow::drawNamed(SDL_Renderer* r, const std::string& name, float gx, float gy, float s) const
{
    const nox::things::ImageDef* def = image(name);
    const nox::things::ImageRef* ref = def ? (def->image ? &*def->image
                                                         : (def->animation && !def->animation->frames.empty()
                                                                ? &def->animation->frames.front()
                                                                : nullptr))
                                           : nullptr;
    const Sprite* sp = ref ? sprites_.get(*ref) : nullptr;
    if (!sp) {
        return;
    }
    (void)r;
    const SDL_FRect dst{(gx + sp->offsetX) * s, (gy + sp->offsetY) * s, sp->width * s, sp->height * s};
    sp->scale = SDL_SCALEMODE_LINEAR;
    sprites_.render(*sp, nullptr, dst);
}

// The panel's left edge, 0x5F in from the screen's right.
float SummonWindow::panelX(int w, int h) const
{
    return static_cast<float>(w) / Hud::scale(w, h) - 0x5F;
}

SummonWindow::Rect SummonWindow::boxRect(int w, int h) const
{
    return {panelX(w, h) + 5, static_cast<float>(y_) + 0x26, 0x4C, 0x4C};
}

SummonWindow::Rect SummonWindow::buttonRect(int w, int h) const
{
    return {panelX(w, h) + 0x1B, static_cast<float>(y_) + 0xC, 0x22, 0x22};
}

bool SummonWindow::isPlant(const nox::client::World& world, std::uint16_t netCode) const
{
    // A carnivorous plant can't follow or hunt.
    for (const auto& s : world.self().summons) {
        if (s && s->netCode == netCode) {
            return world.thingName(s->typeId) == "CarnivorousPlant";
        }
    }
    return false;
}

std::optional<std::uint16_t> SummonWindow::creatureAt(const nox::client::World& world, float gx, float gy, int w,
                                                      int h) const
{
    // wndSummonGet: the cell, then whoever has it.
    const Rect box = boxRect(w, h);
    if (!box.in(gx, gy)) {
        return std::nullopt;
    }
    const int cx = static_cast<int>((gx - box.x) / kCell);
    const int cy = static_cast<int>((gy - box.y) / kCell);
    if (cx < 0 || cx > 1 || cy < 0 || cy > 1) {
        return std::nullopt;
    }
    const auto& self = world.self();
    const int slot = self.summonCells[static_cast<std::size_t>(cy + cx * 2)];  // (cells by row + column * 2)
    if (slot < 0 || !self.summons[static_cast<std::size_t>(slot)]) {
        return std::nullopt;
    }
    return self.summons[static_cast<std::size_t>(slot)]->netCode;
}

std::optional<int> SummonWindow::itemAt(float gx, float gy) const
{
    if (!menu_ || !menu_->box.in(gx, gy)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kLines.size(); ++i) {
        const Rect line{menu_->box.x, menu_->box.y + static_cast<float>(i) * menu_->step, menu_->box.w, menu_->itemH};
        if (line.in(gx, gy)) {
            return kLines[i];
        }
    }
    return std::nullopt;
}

void SummonWindow::open(float gx, float gy, std::optional<std::uint16_t> creature, int w, int h)
{
    // wndSummonCreateList: as wide as the widest line + 8, centred on the click
    // and kept on the screen (the exe keeps it above the bottom with the screen's width, a slip:
    // the height here).
    const float s = Hud::scale(w, h);
    const int size = font_ ? font_->lineSize(kFontHeight, s) : 0;
    float widest = 0;
    for (const int i : kLines) {
        if (font_) {
            widest = std::max(widest, std::floor(font_->measure(text(kCommands[static_cast<std::size_t>(i)]), size) / s));
        }
    }
    Menu m;
    m.creature = creature;
    m.box.w = widest + 8;
    m.box.h = kFontHeight + 14 + (kFontHeight + 2) * 4;
    m.itemH = kFontHeight + 3;
    m.step = kFontHeight + 4;
    const float W = static_cast<float>(w) / s;
    const float H = static_cast<float>(h) / s;
    m.box.x = std::floor(gx - m.box.w / 2);
    m.box.x = m.box.x < 0 ? 0 : (m.box.x + m.box.w >= W ? W - m.box.w - 1 : m.box.x);
    m.box.y = std::floor(gy - m.box.h / 2);
    m.box.y = m.box.y < 0 ? 0 : (m.box.y + m.box.h >= H ? H - m.box.h - 1 : m.box.y);
    menu_ = m;
    sound(0x317); // InventoryPickup
}

void SummonWindow::close()
{
    // guiHideSummonWindow.
    menu_.reset();
    sound(0x398); // ShellSelect
}

void SummonWindow::update(const nox::client::World& world, std::uint32_t frame, float mx, float my, int w, int h)
{
    const auto& self = world.self();
    // Down with the first creature (Cli_Summon_AddCreature), up with the last (RemoveCreature).
    const bool any = self.summonCount() > 0;
    // state_: 0 hidden, 1 sliding down, 2 down, 3 sliding up.
    if (any && (state_ == 0 || state_ == 3)) {
        state_ = 1;
    } else if (!any && (state_ == 1 || state_ == 2)) {
        state_ = 3;
    }
    // kSlide a game frame, catching up the frames since the last update (a long gap starts afresh).
    if (lastFrame_ == 0 || frame < lastFrame_ || frame - lastFrame_ > 300) {
        lastFrame_ = frame;
    }
    for (; lastFrame_ < frame; ++lastFrame_) {
        if (state_ == 1) {
            y_ += kSlide;
            if (y_ >= 0) {
                y_ = 0;
                state_ = 2;
            }
        } else if (state_ == 3) {
            y_ -= kSlide;
            if (y_ <= kHiddenY) {
                y_ = kHiddenY;
                state_ = 0;
            }
        }
    }
    if (menu_) {
        const float s = Hud::scale(w, h);
        const bool gone = menu_->creature && std::none_of(self.summons.begin(), self.summons.end(), [&](const auto& c) {
                              return c && c->netCode == *menu_->creature;
                          });
        if (gone || !menu_->box.in(mx / s, my / s)) {
            close();
        }
    }
}

void SummonWindow::draw(SDL_Renderer* renderer, const nox::client::World& world, int w, int h, SDL_FPoint mouse)
{
    if (state_ == 0) {
        return;
    }
    const float s = Hud::scale(w, h);
    const float gx = mouse.x / s;
    const float gy = mouse.y / s;
    const float px = panelX(w, h);
    const float py = static_cast<float>(y_);
    const auto& self = world.self();
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // ---- the cage (guiDrawSummonBox)
    drawNamed(renderer, "CreatureCageBottom", px, py, s);
    const Rect box = boxRect(w, h);
    for (const auto& c : self.summons) {
        if (!c) {
            continue;
        }
        // The creature's place in the cage, in cells from the box's corner.
        const float cx = box.x + 2 + static_cast<float>(c->x) * kCell;
        const float cy = box.y + 2 + static_cast<float>(c->y) * kCell;
        // Its cells: size 1 one, size 4 two by two, else one wide and two tall.
        const float cw = (c->size == 4 ? 2 : 1) * kCell - 4;
        const float ch = (c->size == 1 ? 1 : 2) * kCell - 4;
        // Its guide picture (guiDrawSummon), else a grey dot ringed in near-black.
        const std::string name = world.thingName(c->typeId);
        const bool guide = std::find(nox::things::kGuideNames.begin(), nox::things::kGuideNames.end(), name) !=
                           nox::things::kGuideNames.end();
        if (guide && image("CreatureCage" + name)) {
            drawNamed(renderer, "CreatureCage" + name, cx, cy, s);
        } else {
            const float ox = cx + std::floor(cw / 2);
            const float oy = cy + std::floor(ch / 2);
            // A filled circle of radius 9, row by row.
            for (int dy = -9; dy <= 9; ++dy) {
                const float half = std::floor(std::sqrt(81.0f - static_cast<float>(dy * dy)));
                fill(renderer, ox - half, oy + static_cast<float>(dy), half * 2 + 1, 1, kGrey, s);
            }
            SDL_SetRenderDrawColor(renderer, 8, 8, 8, 255);
            for (int a = 0; a < 64; ++a) {
                const float t = static_cast<float>(a) * 6.2831853f / 64;
                const SDL_FRect dot{(ox + 9 * std::cos(t)) * s, (oy + 9 * std::sin(t)) * s, s, s};
                SDL_RenderFillRect(renderer, &dot);
            }
        }
        // Its health down the right edge (the exe's table).
        if (const auto it = self.monitored.find(c->netCode); it != self.monitored.end()) {
            const auto& hp = it->second;
            const float filled = hp.maxHealth == 0 ? 0.0f : std::floor(static_cast<float>(hp.health) * ch / hp.maxHealth);
            const bool poisoned = hp.poison != 0;
            fill(renderer, cx + cw - 2, cy, 2, ch, poisoned ? SDL_Color{0, 100, 0, 255} : SDL_Color{100, 0, 0, 255}, s);
            fill(renderer, cx + cw - 2, cy + ch - filled, 2, filled,
                 poisoned ? SDL_Color{0, 255, 0, 255} : SDL_Color{255, 0, 0, 255}, s);
        }
    }
    drawNamed(renderer, "CreatureCageTop", px, py, s); // (the box's child, at -5, -0x26)

    // ---- the order's button (Gui_Summon_SetCommandIcon), lit under the mouse or while
    // its list is up
    const bool lit = buttonRect(w, h).in(gx, gy) || (menu_ && !menu_->creature);
    const char* order = self.creatureCommand == 3 ? "CreatureCageGuardButton"  // the current order: 3 guard, 5 hunt, else escort
                        : self.creatureCommand == 5 ? "CreatureCageHuntButton"
                                                    : "CreatureCageEscortButton";
    drawNamed(renderer, std::string(order) + (lit ? "Lit" : ""), px, py, s);

    // ---- the command list
    if (!menu_ || !font_) {
        return;
    }
    const Rect& m = menu_->box;
    fill(renderer, m.x, m.y, m.w, m.h, {0, 0, 0, 128}, s); // (client_drawRectFilledAlpha)
    const SDL_Color frame{0, 0, 0x8C, 255};
    fill(renderer, m.x + 1, m.y, m.w - 2, 1, frame, s);
    fill(renderer, m.x + 1, m.y + m.h - 1, m.w - 2, 1, frame, s);
    fill(renderer, m.x, m.y + 1, 1, m.h - 2, frame, s);
    fill(renderer, m.x + m.w - 1, m.y + 1, 1, m.h - 2, frame, s);
    const int size = font_->lineSize(kFontHeight, s);
    // For the whole party (no creature chosen), Escort and Hunt grey out only when every creature is a plant.
    const auto allPlants = std::none_of(self.summons.begin(), self.summons.end(), [&](const auto& c) {
        return c && world.thingName(c->typeId) != "CarnivorousPlant";
    });
    const auto hovered = itemAt(gx, gy);
    if (hovered && *hovered != lastHovered_) {
        lastHovered_ = *hovered;
        sound(0x398);
    }
    for (std::size_t i = 0; i < kLines.size(); ++i) {
        const int item = kLines[i];
        if (!menu_->creature && item == 1) {
            continue; // no Observe for all of them
        }
        const std::string t = text(kCommands[static_cast<std::size_t>(item)]);
        const float tw = std::floor(font_->measure(t, size) / s);
        const float tx = m.x + std::floor((m.w - tw) / 2) + 1;
        const float ty = m.y + static_cast<float>(i) * menu_->step + 3;
        // Escort (4) and Hunt (5) need a creature that moves: a carnivorous plant can't.
        const bool follows = item == 4 || item == 5;
        SDL_Color c = kWhite;
        if (hovered == item) {
            c = kYellow;
        } else if (!menu_->creature) {
            c = follows && allPlants ? kGrey : kBlue;
        } else if (follows && isPlant(world, *menu_->creature)) {
            c = kGrey;
        }
        // Edged in black (the four diagonals, then the line).
        for (const auto [dx, dy] : {std::array<float, 2>{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}) {
            font_->draw(t, (tx + dx) * s, (ty + dy) * s, size, kBlack);
        }
        font_->draw(t, tx * s, ty * s, size, c);
    }
}

bool SummonWindow::contains(float x, float y, int w, int h) const
{
    const float s = Hud::scale(w, h);
    const float gx = x / s;
    const float gy = y / s;
    if (menu_) {
        return true; // (modal)
    }
    if (state_ == 0) {
        return false;
    }
    const Rect panel{panelX(w, h), static_cast<float>(y_), kPanelW, kPanelH};
    return panel.in(gx, gy) || buttonRect(w, h).in(gx, gy);
}

bool SummonWindow::mouseDown(float x, float y, int w, int h)
{
    return contains(x, y, w, h);
}

bool SummonWindow::mouseUp(float x, float y, int w, int h, const nox::client::World& world,
                           const std::function<void(std::uint16_t, std::uint8_t)>& command)
{
    const float s = Hud::scale(w, h);
    const float gx = x / s;
    const float gy = y / s;
    if (menu_) {
        // Gui_Summon_CommandItemProc: Observe needs one creature.
        const auto item = itemAt(gx, gy);
        if (!item || (!menu_->creature && *item == 1)) {
            return true;
        }
        if (command) {
            command(menu_->creature.value_or(0), static_cast<std::uint8_t>(*item));
        }
        close();
        sound(*item == 0 ? 0x309 : 0x382); // BigGong for a banish, else SummonClick
        return true;
    }
    if (state_ == 0) {
        return false;
    }
    if (buttonRect(w, h).in(gx, gy)) {
        open(gx, gy, std::nullopt, w, h); // wndSummonBigButtonProc
        return true;
    }
    if (const auto c = creatureAt(world, gx, gy, w, h)) {
        open(gx, gy, c, w, h); // wndSummonProc
        return true;
    }
    return contains(x, y, w, h);
}

std::set<std::uint16_t> SummonWindow::highlighted(float x, float y, int w, int h, const nox::client::World& world) const
{
    std::set<std::uint16_t> out;
    if (state_ == 0) {
        return out;
    }
    const float s = Hud::scale(w, h);
    const float gx = x / s;
    const float gy = y / s;
    if (buttonRect(w, h).in(gx, gy)) {
        for (const auto& c : world.self().summons) {
            if (c) {
                out.insert(c->netCode);
            }
        }
    } else if (const auto nc = creatureAt(world, gx, gy, w, h)) {
        out.insert(*nc);
    }
    return out;
}

std::string SummonWindow::tooltip(float x, float y, int w, int h, const nox::client::World& world) const
{
    if (state_ == 0 || menu_) {
        return {};
    }
    const float s = Hud::scale(w, h);
    const float gx = x / s;
    const float gy = y / s;
    if (buttonRect(w, h).in(gx, gy)) {
        // The standing order.
        const auto c = world.self().creatureCommand;
        return c == 3 ? text("ccs:GUARD") : c == 4 ? text("ccs:ESCORT") : c == 5 ? text("ccs:HUNT") : std::string();
    }
    if (const auto nc = creatureAt(world, gx, gy, w, h)) {
        // Its thing's pretty name.
        for (const auto& c : world.self().summons) {
            if (c && c->netCode == *nc && c->typeId >= 1 && c->typeId <= things_.things.size()) {
                return prettyName(things_.things[c->typeId - 1u], strings_); // (none for monsters)
            }
        }
    }
    // (The box's ToolTipSummon never shows: a window's tooltip function wins, cursorUpdate.)
    return {};
}

} // namespace noxsdl
