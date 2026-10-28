// The HUD's layout, drawing, hit tests and tooltips (hud.hpp describes what it shows and where).
#include "hud.hpp"

#include "quickbar_mana.hpp"

#include "noxcore/things/spell_ids.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace noxsdl {

namespace {

constexpr int kSlots = 5;
/// The quickbar slots' spacing and their offsets from the first.
constexpr std::array<int, kSlots> kSlotX{0, 38, 75, 112, 149};
/// The slots' default keys (their controls 0x1C-0x20) and the potions' (0x24 health, 0x25 mana,
/// 0x26 cure poison), from default.cfg.
constexpr std::array<const char*, kSlots> kSlotKeys{"A", "S", "D", "F", "G"};
constexpr std::array<const char*, 3> kPotionKeys{"X", "C", "Z"};

// Meters: the frame at (W - 91, H - 201); tubes and potion slots inside it.
constexpr int kMeterW = 91, kMeterH = 201;
constexpr int kTubeTop = 34, kTubeH = 125, kHealthTubeX = 34, kManaTubeX = 60;
constexpr std::array<int, 3> kPotionSlotX{34, 62, 6}; // health, mana, cure poison
constexpr int kPotionSlotY = 166, kPotionSlotW = 28, kPotionSlotH = 30;

// Quickbar: 320 wide, centred, 74 up.
constexpr int kBarW = 320, kBarUp = 74;

struct Layout {
    float s = 1;       ///< output pixels per game pixel
    float W = 640;     ///< screen size in game pixels
    float H = 480;
    float meterX() const { return W - kMeterW; }
    float meterY() const { return H - kMeterH; }
    float barX() const { return std::floor((W - kBarW) / 2); }
    float barY() const { return H - kBarUp; }
    /// The main row's base window (QuickBarBase) and its first slot.
    float baseX() const { return barX() + 59; }
    float baseY() const { return barY() + 17; }
    float slotX(int i) const { return baseX() + 10 + static_cast<float>(kSlotX[static_cast<std::size_t>(i)]); }
    float slotY() const { return baseY() + 15; }
    /// The trap tray's window (quickBarCreate: its box at bar x + 0x7A, bar y - 0x11, less 10 and 15)
    /// and its three slots (from x 0x15 + 10, y 15, as the bar's).
    float trayX() const { return barX() + 112; }
    float trayY() const { return barY() - 32; }
    float trapSlotX(int i) const { return trayX() + 31 + static_cast<float>(kSlotX[static_cast<std::size_t>(i)]); }
    float trapSlotY() const { return trayY() + 15; }
    /// The right end's window (quickBarCreate's, at bar x + 0x104), dropped by `slide`.
    float rightX() const { return barX() + 0x104; }
};

// The layout for a w x h output: the scale, and the screen's size in game pixels.
Layout layoutFor(int w, int h)
{
    Layout l;
    l.s = Hud::scale(w, h);
    l.W = static_cast<float>(w) / l.s;
    l.H = static_cast<float>(h) / l.s;
    return l;
}

bool inside(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

// The CTF flag icons (GUI_CTF.wnd): an 80 x 312 window whose 32 x 32 children 8811-8826 are the
// icons of teams 1-16, at these places in it, their pictures in these colours.
struct CtfIcon {
    int x = 0;
    int y = 0;
    const char* colour = "";
};
constexpr int kCtfIconSize = 32;
constexpr std::array<CtfIcon, 16> kCtfIcons{{{0, 0, "Red"},     {0, 40, "Blue"},  {0, 80, "Green"},  {0, 120, "Yellow"},
                                             {40, 0, "Red"},    {40, 40, "Red"},  {40, 80, "Red"},   {40, 120, "Red"},
                                             {0, 160, "Red"},   {0, 200, "Red"},  {0, 240, "Red"},   {0, 280, "Red"},
                                             {40, 160, "Red"},  {40, 200, "Red"}, {40, 240, "Red"},  {40, 280, "Red"}}};

// Gui_Ctf_Show's place for that window, left of the meters, by how many icons show: up to four,
// half its width left of the meters and 40 up for each; up to eight, all of it left and its
// middle at the bottom; more, all of it up.
SDL_FPoint ctfWindow(const Layout& l, int count)
{
    const float x = l.meterX() - (count < 5 ? 40.0f : 80.0f);
    const float y = count < 5 ? l.H - static_cast<float>(count * 40) : count < 9 ? l.H - 156 : l.H - 312;
    return {x, y};
}

// The CTF flag icon at game pixel (gx, gy), if they're up.
std::optional<int> ctfIconAt(const nox::client::World& world, const Layout& l, float gx, float gy)
{
    const auto& game = world.game();
    if (!game.ctfIconsShown) {
        return std::nullopt;
    }
    const SDL_FPoint o = ctfWindow(l, game.ctfIconCount);
    for (int i = 0; i < game.ctfIconCount && i < static_cast<int>(kCtfIcons.size()); ++i) {
        const CtfIcon& c = kCtfIcons[static_cast<std::size_t>(i)];
        if (inside(gx, gy, o.x + static_cast<float>(c.x), o.y + static_cast<float>(c.y), kCtfIconSize, kCtfIconSize)) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

Hud::Hud(const nox::things::ThingDb& things, ThingArt& art) : things_(things), art_(art)
{
    for (const auto& i : things_.images) {
        images_.emplace(i.name, &i);
    }
}

float Hud::scale(int w, int h)
{
    // The game ran at 640 x 480 to 1024 x 768; bigger windows get it scaled up.
    return std::max(1.0f, std::min(static_cast<float>(w) / 800.0f, static_cast<float>(h) / 600.0f));
}

const nox::things::ImageDef* Hud::image(const std::string& name) const
{
    const auto it = images_.find(name);
    return it == images_.end() ? nullptr : it->second;
}

std::optional<nox::things::ImageRef> Hud::icon(std::uint32_t id, bool warrior, bool lit) const
{
    if (nox::client::isTaggedAbility(id)) { // (noxcpp's multiclass: an ability among spells)
        id = nox::client::untagged(id);
        warrior = true;
    }
    if (warrior) {
        if (id >= 1 && id <= things_.abilities.size()) {
            return things_.abilities[id - 1].icons[lit ? 1 : 0];
        }
        return std::nullopt;
    }
    if (const auto* s = things_.findSpell(id)) {
        return lit ? s->iconEnabled : s->icon;
    }
    return std::nullopt;
}

void Hud::drawDragged(SDL_Renderer* renderer, SpriteCache& sprites, std::uint32_t id, bool warrior, float x, float y,
                      int w, int h) const
{
    const auto ic = icon(id, warrior);
    const Sprite* sp = ic ? sprites.get(*ic) : nullptr;
    if (!sp) {
        return;
    }
    const float s = scale(w, h);
    const float gx = x / s - 15;
    const float gy = y / s - 15;
    const SDL_FRect dst{(gx + sp->offsetX) * s, (gy + sp->offsetY) * s, sp->width * s, sp->height * s};
    sp->scale = SDL_SCALEMODE_LINEAR;
    drawSprite(renderer, *sp, nullptr, dst);
}

bool Hud::spellAllowed(const nox::client::World& world, std::uint32_t spell)
{
    // The game's spell settings (GAME_SETTINGS_2); none sent: all of them.
    const auto& allowed = world.game().allowedSpells;
    if (std::all_of(allowed.begin(), allowed.end(), [](std::uint8_t b) { return b == 0; })) {
        return true;
    }
    // A bit per spell id, the low bit of each byte first.
    return spell / 8 < allowed.size() && ((allowed[spell / 8] >> (spell % 8)) & 1);
}

SDL_FPoint Hud::slotPosition(int w, int h, int set, int slot, bool expanded, int currentSet)
{
    const Layout l = layoutFor(w, h);
    // The four other boxes stand 60 apart above the bar, the first at the top (quickBarCreate).
    const int box = expanded && set != currentSet ? set : 4;
    return {l.slotX(slot), l.slotY() - static_cast<float>(60 * (4 - box))};
}

std::optional<std::pair<int, int>> Hud::slotAt(const QuickbarState& bar, float x, float y, int w, int h) const
{
    if (!bar.bars) {
        return std::nullopt;
    }
    const float s = scale(w, h);
    const int current = bar.bars->row;
    for (int set = 0; set < 5; ++set) {
        if (!bar.expanded && set != current) {
            continue;
        }
        for (int i = 0; i < kSlots; ++i) {
            const SDL_FPoint p = slotPosition(w, h, set, i, bar.expanded, current);
            if (inside(x / s, y / s, p.x, p.y, 34, 34)) {
                return std::pair{set, i};
            }
        }
    }
    return std::nullopt;
}

std::optional<int> Hud::trapSlotAt(const QuickbarState& bar, float x, float y, int w, int h) const
{
    if (!bar.trapOpen) {
        return std::nullopt;
    }
    const Layout l = layoutFor(w, h);
    for (int i = 0; i < 3; ++i) {
        if (inside(x / l.s, y / l.s, l.trapSlotX(i), l.trapSlotY(), 34, 34)) {
            return i;
        }
    }
    return std::nullopt;
}

int Hud::trapCost(std::uint32_t spell) const
{
    return trapManaCost(things_, gameData_, spell, gameFlags_);
}

// A string by id: first qualified with the source file, as the game's lookups are, then bare;
// empty if neither is there.
std::string Hud::text(const std::string& id, const char* file) const
{
    if (strings_) {
        if (const auto t = strings_->text(std::string(file) + ":" + id)) {
            return nox::strings::toUtf8(*t);
        }
        if (const auto t = strings_->text(id)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return {};
}

std::string Hud::tooltip(const nox::client::World& world, std::uint16_t us, const QuickbarState& bar, float x, float y,
                         int w, int h) const
{
    const auto hit = this->hit(world, us, bar, x, y, w, h);
    if (!hit) {
        return {};
    }
    const auto me = world.players().find(us);
    const std::uint8_t cls = me != world.players().end() ? me->second.playerClass : 0;
    const auto title = [&](std::uint32_t id) -> std::string {
        if (id == 0) {
            return {};
        }
        if (cls == 0) {
            return id >= 1 && id <= things_.abilities.size() ? text(things_.abilities[id - 1].title, "") : "";
        }
        const auto* s = things_.findSpell(id);
        return s ? text(s->title, "") : "";
    };
    const char* spell = "guispell.c";
    switch (hit->kind) {
    case HudHit::Kind::Slot:
        // The slot's window tooltip, its spell's or ability's title (spellKeyPackSetSpell, quickBarWarriorDraw).
        return bar.bars ? title(bar.bars->rows[static_cast<std::size_t>(hit->set)][static_cast<std::size_t>(hit->index)].id)
                        : "";
    case HudHit::Kind::TrapSlot:
        return bar.bars ? title(bar.bars->currentTrap()[static_cast<std::size_t>(hit->index)].id) : "";
    case HudHit::Kind::Target: {
        // On us while the nugget's lit.
        const bool on = bar.bars &&
                        bar.bars->rows[static_cast<std::size_t>(hit->set)][static_cast<std::size_t>(hit->index)].target;
        return text(on ? "ToolTipCastOnMe" : "ToolTipCastAtOther", spell);
    }
    case HudHit::Kind::Spellbook:
        return text(bar.bookOpen ? "CloseSpellbookTT" : "OpenSpellbookTT", spell); // quickbarButtonBook
    case HudHit::Kind::SetUp:
        return text("ToolTipPrevSpellSet", spell);
    case HudHit::Kind::SetDown:
        return text("ToolTipNextSpellSet", spell);
    case HudHit::Kind::SetMax:
        return text("ToolTipAllSpellSets", spell);
    case HudHit::Kind::TrapBuild:
        return text(cls == 2 ? "ToolTipSummonBomber" : "ToolTipLayTrap", spell);
    case HudHit::Kind::TrapToggle:
        return text("ToolTipTrapConstruct", spell);
    case HudHit::Kind::TrapUp:
        return text("ToolTipPrevTrap", spell);
    case HudHit::Kind::TrapDown:
        return text("ToolTipNextTrap", spell);
    case HudHit::Kind::Tube: {
        // Which tube: the health one, or the mana one beside it (guimeter.c guiHealthManaInit).
        const Layout l = layoutFor(w, h);
        const bool mana = cls != 0 && x / l.s >= l.meterX() + kManaTubeX;
        return text(mana ? "ToolTipMana" : "ToolTipHealth", "guimeter.c");
    }
    case HudHit::Kind::Potion:
        return text(hit->index == 0 ? "HealthSlotTT" : hit->index == 1 ? "ManaSlotTT" : "CurePoisonSlotTT", "guimeter.c");
    case HudHit::Kind::Flag: {
        // Gui_Ctf_SetFlagStatus: by the state, and whether it's our team's (the framed one).
        const std::uint8_t state = world.game().ctfIconStates[static_cast<std::size_t>(hit->index)];
        const bool ours = me != world.players().end() && me->second.flagIconTeam == static_cast<std::uint32_t>(hit->index + 1);
        const char* id = state == 1 ? (ours ? "YourFlagCarriedTT" : "TheirFlagCarriedTT")
                       : state == 2 ? "FlagAwayTT"
                                    : "FlagHomeTT";
        return text(id, "gui_ctf.c");
    }
    case HudHit::Kind::Panel:
        break;
    }
    return {};
}

bool Hud::inBarArea(float x, float y, int w, int h)
{
    const Layout l = layoutFor(w, h);
    // From the bar's left to its right, 17 above it to the screen's bottom.
    return inside(x / l.s, y / l.s, l.barX(), l.barY() - 17, kBarW, kBarUp + 17);
}
int Hud::manaCost(std::uint32_t spell) const
{
    // spellManaCost's ordinary mode: thing.bin's cost; the summonings their creature's price (VD-34).
    return slotManaCost(things_, spell);
}

void Hud::draw(SDL_Renderer* renderer, Font* font, SpriteCache& sprites, const nox::client::World& world,
               std::uint16_t us, const QuickbarState& qb, int w, int h, std::uint32_t frame)
{
    const Layout l = layoutFor(w, h);
    const auto& self = world.self();
    const auto me = world.players().find(us);
    const std::uint8_t cls = me != world.players().end() ? me->second.playerClass : 0;
    const bool warrior = cls == 0 && !qb.multiclass; // (multiclass: a caster's meters and bars)
    const bool poisoned = me != world.players().end() && (me->second.status & 0x400);  // status 0x400: poisoned
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Drawing in game pixels.
    const auto drawRef = [&](const nox::things::ImageRef& ref, float gx, float gy, Uint8 shade = 255) {
        const Sprite* sp = sprites.get(ref);
        if (!sp) {
            return;
        }
        const SDL_FRect dst{(gx + sp->offsetX) * l.s, (gy + sp->offsetY) * l.s, sp->width * l.s, sp->height * l.s};
        sp->scale = SDL_SCALEMODE_LINEAR;
        SDL_SetTextureColorMod(sp->texture, shade, shade, shade);
        drawSprite(renderer, *sp, nullptr, dst);
        SDL_SetTextureColorMod(sp->texture, 255, 255, 255);
    };
    const auto drawNamed = [&](const std::string& name, float gx, float gy, std::size_t frameIndex = 0) {
        const nox::things::ImageDef* def = image(name);
        if (!def) {
            return;
        }
        if (def->image) {
            drawRef(*def->image, gx, gy);
        } else if (def->animation && !def->animation->frames.empty()) {
            drawRef(def->animation->frames[frameIndex % def->animation->frames.size()], gx, gy);
        }
    };
    const auto fill = [&](float gx, float gy, float gw, float gh, SDL_Color c) {
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        const SDL_FRect r{gx * l.s, gy * l.s, gw * l.s, gh * l.s};
        SDL_RenderFillRect(renderer, &r);
    };
    const auto shade = [&](float gx, float gy, float gw, float gh) { fill(gx, gy, gw, gh, {0, 0, 0, 128}); };
    // Text as the game's drawString puts it, in its 9-high fonts (the default and the small font,
    // the same file): (gx, gy) the cell's top left, the capitals 7 high from there.
    const auto text = [&](const std::string& t, float gx, float gy, SDL_Color c) {
        if (font && !t.empty()) {
            font->drawGameText(t, gx, gy, l.s, c);
        }
    };
    const auto textWidth = [&](const std::string& t) { return font ? font->measureGameText(t, l.s) : 0.0f; };

    // ---- the quickbar (quickBarCreate)
    const float bx = l.barX();
    const float by = l.barY();
    const nox::client::SpellBars empty;
    const nox::client::SpellBars& bars = qb.bars ? *qb.bars : empty;
    // Every slot dark (quickBarWarriorDraw): no drawable of ours, it dying, dead or electrocuted (states 1, 2,
    // 0x33: playerAnimCheck), or under ANTI_MAGIC (enchantment 0x1D: War Cry's
    // Nullify, Cli_DrawableHasEnchant).
    // (Dead or observing in the player list too: an observer's old drawable may linger here.)
    const auto mine = world.drawables().find(us);
    const bool unable = mine == world.drawables().end() || mine->second.animState == 1 || mine->second.animState == 2 ||
                        mine->second.animState == 0x33 || (mine->second.buffs & (1u << 0x1D)) != 0 ||
                        (me != world.players().end() && (me->second.dead || (me->second.status & 1)));  // status 1: observing
    // A box's slots' nuggets (lit: the spell goes on its caster): their own windows,
    // over the bar and its "Spell Set" title, so drawn after them.
    const auto nuggets = [&](int set, bool main) {
        // The other sets' boxes stack upward when expanded: set n sits 60 * (4 - n) above the bar.
        const float dy = main ? 0.0f : -static_cast<float>(60 * (4 - set));
        const auto& row = bars.rows[static_cast<std::size_t>(set)];
        for (int i = 0; i < kSlots; ++i) {
            const auto& slot = row[static_cast<std::size_t>(i)];
            const std::string n = std::to_string(i + 1);
            drawNamed(warrior ? "QuickBarWarriorNugget" + n
                              : "QuickBarNugget" + n + (slot.target && slot.id != 0 ? "Lit" : ""),
                      l.baseX() - 60, l.baseY() - 18 + dy);
        }
    };
    // One box: its base and the icons.
    const auto box = [&](int set, bool main) {
        const float dy = main ? 0.0f : -static_cast<float>(60 * (4 - set));
        drawNamed("QuickBarBase", l.baseX() - 61, l.baseY() - 18 + dy);
        const auto& row = bars.rows[static_cast<std::size_t>(set)];
        for (int i = 0; i < kSlots; ++i) {
            const float sx = l.slotX(i);
            const float sy = l.slotY() + dy;
            const auto& slot = row[static_cast<std::size_t>(i)];
            if (slot.id == 0) {
                continue;
            }
            // Lit for 10 frames after it's used (quickBarWarriorDraw, quickBarDrawFn).
            const bool lit = main && i == qb.lastSlot && frame >= 11 && frame - qb.lastFrame <= 9;
            if (const auto ic = icon(slot.id, warrior, lit)) {
                drawRef(*ic, sx, sy);
            }
            // (An ability's slot: a warrior's, or a multiclass player's tagged one.)
            const bool abilitySlot = warrior || nox::client::isTaggedAbility(slot.id);
            const std::uint32_t a = nox::client::untagged(slot.id);
            if (abilitySlot) {
                // quickBarDrawFn: recharging (or still active), the top darkened, 34 less one pixel for
                // every Delay / 30 frames since it went unready, so the clear part rises.
                const bool ready = a < self.abilityReady.size() && self.abilityReady[a];
                const bool active = a < 8 && (self.activeAbilities & (1u << a));
                const int delay = abilityDelayFrames(gameData_, a, gameFlags_); // (the game's own lines: SOLO in a solo game)
                if ((!ready || active) && !lit && delay / 30 != 0 && a < self.abilityCooldownStart.size()) {
                    // (While ready the start is 0: the exe's height comes out below 0, so nothing.)
                    const auto start = self.abilityCooldownStart[a];
                    const int elapsed = static_cast<int>(frame) - static_cast<int>(start);
                    const int height = std::min(34, 34 - std::max(elapsed, 0) / (delay / 30));
                    if (start != 0 && height > 0) {
                        shade(sx, sy, 34, static_cast<float>(height));
                    }
                }
            } else if (unable || !spellAllowed(world, slot.id)) {
                shade(sx, sy, 30, 30);
            } else if (limited_.contains(slot.id)) {
                // Its last cast is still out, so the server would refuse it: darkened, with red.
                fill(sx, sy, 30, 30, {70, 0, 0, 150});
            } else if (const int height = manaShadeHeight(manaCost(slot.id), self.mana); height > 0) {
                // Short of mana: the top (cost - mana) / cost of it (quickBarWarriorDraw). A summon
                // reads its creature's price, so it darkens too (VD-34; the exe's never does, OB-223).
                shade(sx, sy, 30, static_cast<float>(height));
            }
        }
    };
    if (qb.expanded) {
        for (int set = 0; set < 4; ++set) {
            box(set, false);
        }
    }
    box(bars.row, true);
    if (warrior) {
        drawNamed("QuickBarWarriorLeft", bx, by);
        drawNamed("QuickBarWarriorRight", bx - 3, by);
    } else {
        // The right end (quickBarCreate's window): the trap or bomber with the tray's button above,
        // or the warrior's end while there's no trap to lay; dropped while it slides.
        const float ry = by + qb.trapSlide;
        if (qb.trapAvailable) {
            drawNamed(std::string(cls == 1 ? "QuickBarTrap" : "QuickBarBomber") + (qb.trapHit ? "Hit" : ""), bx - 3, ry);
            drawNamed(qb.trapOpen ? "QuickBarTrapButtonLit" : "QuickBarTrapButton", bx - 5, ry - 4);
        } else {
            drawNamed("QuickBarWarriorRight", bx - 3, ry);
        }
        if (qb.trapOpen) {
            // The tray: the trap set's three slots, its arrows and title.
            const float tx = l.trayX();
            const float ty = l.trayY();
            drawNamed("QuickBarTrapTray", tx - 40, ty - 20);
            if (qb.flash == 3) {
                drawNamed("QuickBarTrapTrayUpLit", tx - 40, ty - 20);
            } else if (qb.flash == 4) {
                drawNamed("QuickBarTrapTrayDownLit", tx - 40, ty - 20);
            }
            // Mana left for each slot once the slots before it are paid at their trap costs (quickBarWarriorDraw).
            int mana = self.mana;
            for (int i = 0; i < 3; ++i) {
                const auto& slot = bars.currentTrap()[static_cast<std::size_t>(i)];
                const float sx = l.trapSlotX(i);
                const float sy = l.trapSlotY();
                if (slot.id == 0) {
                    continue;
                }
                if (const auto ic = icon(slot.id, false)) {
                    drawRef(*ic, sx, sy);
                }
                if (unable || !spellAllowed(world, slot.id)) {
                    shade(sx, sy, 30, 30);
                } else if (const int height = manaShadeHeight(manaCost(slot.id), mana); height > 0) {
                    // (Once `mana` is below 0 the exe's height passes 30 and its shade runs off the
                    // slot; ours stays on it.)
                    shade(sx, sy, 30, static_cast<float>(std::min(30, height)));
                }
                mana -= trapCost(slot.id);
            }
            if (font) {
                // "Trap Set n" (quickbarDraw) on the tray's 0x6E-wide title at (0x14, -7).
                std::string title = "Trap Set " + std::to_string(bars.trapRow + 1);
                const float tw = textWidth(title);
                text(title, tx + 0x14 + (0x6E - tw) / 2 - 8, ty - 7, {255, 255, 255, 255});
            }
        }
        drawNamed("QuickBarSpellSetBase", bx, by);
        // The arrows' and the all-sets button's lit images, a moment after a click (quickbarTrapUpDownDraw).
        if (qb.flash == 0) {
            drawNamed("QuickBarSpellSetUpLit", bx, by);
        } else if (qb.flash == 1) {
            drawNamed("QuickBarSpellSetDownLit", bx, by);
        } else if (qb.flash == 2) {
            drawNamed("QuickBarSpellSetMaxLit", bx, by);
        }
        if (!qb.expanded && !qb.trapOpen) {
            drawNamed("QuickBarTitle", bx, by);
            // "Spell Set n" over the bar: centred on QuickBarTitle's 101 x 14 child at (115, 6),
            // a pixel down (the small font's height + 1 less the default's).
            if (font) {
                std::string title = "Spell Set " + std::to_string(bars.row + 1);
                const float tw = textWidth(title);
                text(title, bx + 115 + (101 - tw) / 2, by + 6 + 1, {255, 255, 255, 255});
            }
        }
    }
    if (qb.expanded) {
        for (int set = 0; set < 4; ++set) {
            nuggets(set, false);
        }
    }
    nuggets(bars.row, true);
    drawNamed(qb.bookOpen ? "SpellbookButtonLit" : "SpellbookButton", bx - 1, by + 35);
    for (int i = 0; i < kSlots; ++i) {
        // The slot's key, on its plate's child at (13, 39).
        text(kSlotKeys[static_cast<std::size_t>(i)], l.slotX(i) + 13, l.slotY() + 29, {255, 255, 255, 255});
    }
    // ---- the meters (guiHealthManaInit)
    const float mx = l.meterX();
    const float my = l.meterY();
    if (self.maxHealth != lastHealth_) {
        flashTimer_ = 32; // a new maximum (Gui_Meters_SetHealthAndMax)
        lastHealth_ = self.maxHealth;
    }
    // The frame's flash picture: HealthMana1-10, flashAcc_ / 8 choosing which.
    int flashFrame = flashAcc_ >> 3;
    drawNamed("HealthMana" + std::to_string(flashFrame + 1), mx, my);
    drawNamed(warrior ? "WarriorHealthTube" : "HealthManaTubes", mx, my);

    // Bubbles and the flash move on at 30 a second.
    const bool step = frame != bubbleFrame_;
    bubbleFrame_ = frame;
    // The HUD's own small generator for the bubbles.
    const auto rand = [&](int lo, int hi) {
        seed_ = seed_ * 1103515245u + 12345u;
        return lo + static_cast<int>((seed_ >> 16) % static_cast<std::uint32_t>(hi - lo + 1));
    };
    // A new maximum runs the flash: flashTimer_ counts down from 32 and is added each frame, so the
    // pictures go by fast at first and slow down; back to the first after the tenth.
    if (step && flashTimer_ > 0) {
        flashAcc_ += flashTimer_;
        --flashTimer_;
        if ((flashAcc_ >> 3) > 9) {
            flashAcc_ = 0;
        }
    }
    // The meters' fill colours (guiHealthManaColorInit): health red, green while poisoned
    // (Gui_Meters_SetPoisoned); mana blue.
    const auto meterColor = [&](int which) {
        return which == 0 ? (poisoned ? SDL_Color{0, 255, 0, 255} : SDL_Color{255, 0, 0, 255}) : SDL_Color{0, 0, 255, 255};
    };
    const auto tube = [&](int which, float tx, std::uint16_t value, std::uint16_t max) {
        // guiHealthManaTubeDraw: the column 5 in, 15 wide, 125 high.
        const float x = tx + 5;
        const float y = my + kTubeTop;
        if (max == 0) {
            shade(x, y, 15, kTubeH);
            return;
        }
        const int filled = std::min(kTubeH, static_cast<int>(value) * kTubeH / max);
        const SDL_Color color = meterColor(which);
        shade(x, y, 15, static_cast<float>(kTubeH - filled));
        fill(x, y + static_cast<float>(kTubeH - filled), 15, static_cast<float>(filled), {color.r, color.g, color.b, 128});
        fill(x, y + static_cast<float>(kTubeH - filled), 15, 1, color);
        // The bubbles: rise from the bottom, pop at the surface.
        for (auto& b : bubbles_[static_cast<std::size_t>(which)]) {
            if (!b.live) {
                continue;
            }
            const int by16 = b.y16 >> 4;
            if (by16 < kTubeH - filled) {
                b.live = false;
                continue;
            }
            // Poisoned, every tube's bubbles are its fill colour (guiHealthManaTubeDraw).
            const SDL_Color c = poisoned ? color : b.color;
            if (b.size < 3) {
                fill(x + static_cast<float>(b.x), y + static_cast<float>(by16), static_cast<float>(b.size),
                     static_cast<float>(b.size), c);
            } else {
                fill(x + static_cast<float>(b.x) + 1, y + static_cast<float>(by16), 1, 3, c);
                fill(x + static_cast<float>(b.x), y + static_cast<float>(by16) + 1, 3, 1, c);
            }
            if (step) {
                b.y16 -= b.speed;
            }
        }
        if (step && filled > 1) {
            for (auto& b : bubbles_[static_cast<std::size_t>(which)]) {
                if (b.live) {
                    continue;
                }
                // Refill dead bubbles at the bottom: mostly 1 pixel, sometimes 2, rarely a 3-pixel cross;
                // speed in 16ths of a pixel per frame.
                const int r = rand(1, 100);
                b.size = r < 80 ? 1 : r > 94 ? 3 : 2;
                b.x = std::min(rand(0, 14), 15 - b.size);
                b.y16 = (kTubeH - b.size) * 16;
                b.speed = rand(4, 48);
                b.live = true;
                const auto v = static_cast<Uint8>(rand(0, 64));
                b.color = which == 0 ? SDL_Color{255, v, v, 255} : SDL_Color{v, v, 255, 255};
            }
        }
    };
    // Poisoned, the green frame goes over the tubes' picture, drawn by the health tube itself
    // before its column (guiHealthManaTubeDraw: PoisonTube / WarriorPoisonTube at the meters' window).
    if (poisoned) {
        drawNamed(warrior ? "WarriorPoisonTube" : "PoisonTube", mx, my);
    }
    tube(0, mx + kHealthTubeX, self.health, self.maxHealth);
    if (!warrior) {
        tube(1, mx + kManaTubeX, self.mana, self.maxMana);
    }

    // The potion slots (guiBottleSlotDrawFn): what the slot holds (the health slot's food when
    // there's no red potion), the first stack's count, and the key.
    for (int kind = 0; kind < 3; ++kind) {
        if (kind == 1 && warrior) {
            continue;
        }
        const float px = mx + static_cast<float>(kPotionSlotX[static_cast<std::size_t>(kind)]);
        const float py = my + kPotionSlotY;
        const int count = qb.bottles ? qb.bottles->count(kind) : 0;
        if (count > 0) {
            if (const TypeArt* a = art_.get(qb.bottles->type(kind)); a && a->still) {
                // The slot's drawable at (x + 14, y + 15) (ADD 0xE, ADD 0xF).
                drawRef(*a->still, px + 14 - a->halfWidth, py + 15 - a->halfHeight - a->z);
            }
            // The count's cell 10 less the small font's height (9) down, the key's 0x21 less it.
            text(std::to_string(count), px - 2, py + 10 - 9, {255, 255, 255, 255});
        }
        text(kPotionKeys[static_cast<std::size_t>(kind)], px - 2, py + 0x21 - 9, {255, 255, 255, 255});
    }

    // The small bars beside us, when a tube's been clicked (drawHealthManaBar).
    if (barsShown_ && me != world.players().end() && !(me->second.status & 1)) {  // status 1: observing
        const auto smallBar = [&](float x, std::uint16_t v, std::uint16_t max, SDL_Color c, SDL_Color border) {
            const float top = l.H / 2 - 48;
            const float filled = max ? std::floor(static_cast<float>(v) * 48 / static_cast<float>(max)) : 0;
            fill(x, top, 2, 48, {0, 0, 0, 255});
            fill(x, top + 48 - filled, 2, filled, c);
            SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, 255);
            const SDL_FRect r{(x - 1) * l.s, (l.H / 2 - 49) * l.s, 4 * l.s, 50 * l.s};
            SDL_RenderRect(renderer, &r);
        };
        // The meter's colour, in a frame of dark red (dark green while poisoned) or dark blue.
        smallBar(std::floor(l.W / 2) + 15, self.health, self.maxHealth, meterColor(0),
                 poisoned ? SDL_Color{0, 100, 0, 255} : SDL_Color{100, 0, 0, 255});
        if (!warrior) {
            smallBar(std::floor(l.W / 2) + 21, self.mana, self.maxMana, meterColor(1), {0, 0, 140, 255});
        }
    }

    // ---- the CTF flag icons (GUI_CTF.wnd, a window above the others): each icon's picture by
    // its flag's state (its draw: carried the window's enabled image, dropped its disabled one,
    // else its background one), and round our team's FlagTeamBorder at (-4, -4) (window flag
    // 0x20, set for the team we're put in).
    const auto& game = world.game();
    if (game.ctfIconsShown) {
        const SDL_FPoint o = ctfWindow(l, game.ctfIconCount);
        const std::uint32_t ours = me != world.players().end() ? me->second.flagIconTeam : 0;
        for (int i = 0; i < game.ctfIconCount && i < static_cast<int>(kCtfIcons.size()); ++i) {
            const CtfIcon& c = kCtfIcons[static_cast<std::size_t>(i)];
            const float x = o.x + static_cast<float>(c.x);
            const float y = o.y + static_cast<float>(c.y);
            const std::uint8_t state = game.ctfIconStates[static_cast<std::size_t>(i)];
            const char* picture = state == 1 ? "FlagCarried" : state == 2 ? "FlagDroppedInField" : "FlagAtHome";
            drawNamed(std::string(picture) + c.colour, x, y);
            if (ours == static_cast<std::uint32_t>(i + 1)) {
                drawNamed("FlagTeamBorder", x - 4, y - 4);
            }
        }
    }
}

std::optional<HudHit> Hud::hit(const nox::client::World& world, std::uint16_t us, const QuickbarState& bar, float x,
                               float y, int w, int h) const
{
    const Layout l = layoutFor(w, h);
    const float gx = x / l.s;
    const float gy = y / l.s;
    const auto me = world.players().find(us);
    const bool warrior = (me == world.players().end() || me->second.playerClass == 0) && !bar.multiclass;
    if (const auto i = ctfIconAt(world, l, gx, gy)) {
        return HudHit{HudHit::Kind::Flag, *i};
    }
    if (!warrior && bar.trapAvailable) {
        // The right end's buttons (quickBarCreate: one at (0, 0x13), one at (9, 0x21)),
        // over the open tray's box, which reaches under the toggle.
        if (inside(gx, gy, l.rightX(), l.barY() + 0x13 + bar.trapSlide, 12, 12)) {
            return HudHit{HudHit::Kind::TrapToggle};
        }
        if (inside(gx, gy, l.rightX() + 9, l.barY() + 0x21 + bar.trapSlide, 32, 32)) {
            return HudHit{HudHit::Kind::TrapBuild};
        }
    }
    if (!warrior && bar.trapOpen) {
        // The tray over the bar's middle: its slots, arrows, and the rest of its box.
        if (const auto i = trapSlotAt(bar, x, y, w, h)) {
            return HudHit{HudHit::Kind::TrapSlot, *i};
        }
        if (inside(gx, gy, l.trayX() + 15, l.trayY() + 12, 10, 14)) {
            return HudHit{HudHit::Kind::TrapUp};
        }
        if (inside(gx, gy, l.trayX() + 15, l.trayY() + 32, 10, 14)) {
            return HudHit{HudHit::Kind::TrapDown};
        }
        if (inside(gx, gy, l.trayX(), l.trayY(), 199, 59)) {
            return HudHit{HudHit::Kind::Panel};
        }
    }
    if (bar.bars) {
        const int current = bar.bars->row;
        // Expanded, every set's slots can be hit; otherwise only the current set's.
        for (int set = 0; set < 5; ++set) {
            if (!bar.expanded && set != current) {
                continue;
            }
            for (int i = 0; i < kSlots; ++i) {
                const SDL_FPoint p = slotPosition(w, h, set, i, bar.expanded, current);
                if (inside(gx, gy, p.x, p.y, 34, 34)) {
                    return HudHit{HudHit::Kind::Slot, i, set};
                }
                // The nugget's corner above the slot (its own 10 x 10 window).
                if (!warrior && inside(gx, gy, p.x + 12, p.y - 10, 10, 10)) {
                    return HudHit{HudHit::Kind::Target, i, set};
                }
            }
            // Another set's box, off its slots, is still the HUD's (so the click is no attack).
            if (set != current && inside(gx, gy, l.baseX(), l.baseY() - static_cast<float>(60 * (4 - set)), 199, 59)) {
                return HudHit{HudHit::Kind::Panel};
            }
        }
    }
    const float bx = l.barX();
    const float by = l.barY();
    if (inside(gx, gy, bx - 1, by + 35, 29, 30)) {
        return HudHit{HudHit::Kind::Spellbook};
    }
    if (!warrior) {
        // The set arrows and the all-sets button beside the spellbook button (casters only).
        if (inside(gx, gy, bx + 29, by + 26, 15, 19)) {
            return HudHit{HudHit::Kind::SetUp};
        }
        if (inside(gx, gy, bx + 29, by + 55, 15, 19)) {
            return HudHit{HudHit::Kind::SetDown};
        }
        if (inside(gx, gy, bx + 47, by + 42, 13, 17)) {
            return HudHit{HudHit::Kind::SetMax};
        }
    }
    const float mx = l.meterX();
    const float my = l.meterY();
    if (inside(gx, gy, mx + kHealthTubeX, my + kTubeTop, 25, kTubeH) ||
        (!warrior && inside(gx, gy, mx + kManaTubeX, my + kTubeTop, 25, kTubeH))) {
        return HudHit{HudHit::Kind::Tube};
    }
    for (int kind = 0; kind < 3; ++kind) {
        if (kind == 1 && warrior) {
            continue;
        }
        if (inside(gx, gy, mx + static_cast<float>(kPotionSlotX[static_cast<std::size_t>(kind)]), my + kPotionSlotY,
                   kPotionSlotW, kPotionSlotH)) {
            return HudHit{HudHit::Kind::Potion, kind};
        }
    }
    // Anywhere else on the bar or the meters: taken, so it isn't an attack.
    if (inside(gx, gy, bx, by, kBarW, kBarUp) || inside(gx, gy, mx, my, kMeterW, kMeterH)) {
        return HudHit{HudHit::Kind::Panel};
    }
    return std::nullopt;
}

} // namespace noxsdl
