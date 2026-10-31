// TradeScreens (trade_screens.hpp): the shop, NPC dialog, player trade and amount windows loaded
// from their .wnd files, kept in step with the client's World each frame.
#include "trade_screens.hpp"

#include "hud.hpp"

#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace noxsdl {

namespace {

namespace oc = nox::things::ObjectClass;

// The windows' control ids, as their .wnd files number them.
// Shop.wnd
constexpr std::int32_t kShopExit = 3801, kShopSell = 3802, kShopRepair = 3803, kShopBuy = 3804, kShopPic = 3805,
                       kShopArea = 3806, kShopSlider = 3807, kShopUp = 3808, kShopDown = 3809, kShopName = 3810;
// Dialog.wnd
constexpr std::int32_t kDlgList = 3901, kDlgDown = 3902, kDlgUp = 3903, kDlgSlider = 3904, kDlgPortrait = 3905,
                       kDlgDone = 3906, kDlgReplay = 3907, kDlgYes = 3908, kDlgNo = 3909, kDlgTitle = 3910;
// Trade.wnd
constexpr std::int32_t kTrdOurName = 3702, kTrdTheirName = 3703, kTrdOurs = 3704, kTrdTheirs = 3705,
                       kTrdAccept = 3708, kTrdCancel = 3710, kTrdOurGold = 3711, kTrdOurOwed = 3712,
                       kTrdTheirGold = 3713;
// MultMove.wnd
constexpr std::int32_t kAmtCount = 3601, kAmtUp = 3602, kAmtDown = 3603, kAmtYes = 3604, kAmtNo = 3605,
                       kAmtLabel = 3606, kAmtTotal = 3607;
// dlg.wnd (the message box)
constexpr std::int32_t kBoxOk = 4001, kBoxCancel = 4002, kBoxText = 4004, kBoxTitle = 4005, kBoxNo = 4006,
                       kBoxYes = 4007;

constexpr int kCell = 50;
constexpr int kStockScrollMax = 300;
/// MultMove.wnd's item, from the window's corner.
constexpr SDL_Point kAmountItem{0x67, 0x26};

const nox::gui::Color kWhite{255, 255, 255, false};
const nox::gui::Color kYellow{255, 255, 0, false};

/// The shopkeepers' pictures by their thing (getShopPic); anything else gets the plain one.
constexpr std::array<std::pair<const char*, const char*>, 8> kKeeperPics{{
    {"Shopkeeper", "ShopKeeperPic"},
    {"ShopkeeperWarriorsRealm", "ShopKeeperWarriorPic"},
    {"ShopkeeperConjurerRealm", "ShopKeeperConjurerPic"},
    {"ShopkeeperWizardRealm", "ShopKeeperWizardPic"},
    {"ShopkeeperLandOfTheDead", "ShopKeeperLandOfTheDeadPic"},
    {"ShopkeeperMagicShop", "ShopKeeperMagicShopPic"},
    {"ShopkeeperPurple", "ShopKeeperPurplePic"},
    {"ShopkeeperYellow", "ShopKeeperBrownPic"},
}};

bool inRect(SDL_FPoint p, SDL_Rect r)
{
    return p.x >= static_cast<float>(r.x) && p.y >= static_cast<float>(r.y) && p.x < static_cast<float>(r.x + r.w) &&
           p.y < static_cast<float>(r.y + r.h);
}

// A window's rectangle in layout units, from its place on the screen (empty for none).
SDL_Rect rectOf(const gui::Window* w)
{
    if (!w) {
        return {};
    }
    const SDL_Point at = w->screenPos();
    return {at.x, at.y, w->rect.w, w->rect.h};
}

} // namespace

TradeScreens::TradeScreens(gui::Resources res, std::filesystem::path windowDir, Inventory& inventory,
                           ItemNames& names, ThingArt& art, Actions actions)
    : gui_(std::move(res)), windowDir_(std::move(windowDir)), inventory_(inventory), names_(names), art_(art),
      actions_(std::move(actions))
{
    // The 640 x 480 layout at the screen's bottom right, at the game's scale (the exe places
    // these at the screen's size less theirs).
    gui_.setPlacement([](int w, int h) {
        const float s = Hud::scale(w, h);
        return std::array<float, 3>{s, static_cast<float>(w) - 640 * s, static_cast<float>(h) - 480 * s};
    });

    // ---- the shop
    shop_.window = gui_.load(windowDir_ / "Shop.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message == gui::SliderChanged && from.id == kShopSlider) {
            shop_.scroll = kStockScrollMax - from.sliderValue; // (the shop window's 0x4009)
            return true;
        }
        if (message != gui::Clicked) {
            return false;
        }
        sound(0x2FE); // ButtonPress
        // A button's work is queued in deferred_ and run at the end of update(): it may open, close or
        // destroy windows, so not in the middle of handling the event.
        switch (from.id) {
        case kShopExit:
            deferred_.push_back([this] {
                if (actions_.shopExit) {
                    actions_.shopExit(); // CliNet_SendShopExit
                }
                inventory_.setShopMode(Inventory::ShopMode::None);
            });
            return true;
        // Sell and Repair work by clicking items in the inventory, so it is opened with them.
        case kShopSell:
            deferred_.push_back([this] {
                setShopMode(3);
                inventory_.open();
            });
            return true;
        case kShopRepair:
            deferred_.push_back([this] {
                setShopMode(4);
                inventory_.open();
            });
            return true;
        case kShopBuy:
            deferred_.push_back([this] { setShopMode(2); });
            return true;
        case kShopUp: {
            // One cell up, snapped to a cell's edge.
            const int to = shop_.scroll - kCell;
            scrollStock(to < 0 ? 0 : to - to % kCell);
            return true;
        }
        case kShopDown: {
            const int to = shop_.scroll + kCell;
            scrollStock(to <= kStockScrollMax ? to - to % kCell : kStockScrollMax);
            return true;
        }
        default:
            return false;
        }
    });
    if (shop_.window) {
        // Its drawing: ShopBase and the mode's picture, at the layout's corner.
        shop_.window->draw = [this](gui::Window&, gui::Painter& p) {
            p.image("ShopBase", 0, 0);
            p.image(shop_.mode == 2 ? "ShopTradeMode" : shop_.mode == 3 ? "ShopIdentifyMode" : "ShopRepairMode", 0, 0);
        };
        if (gui::Window* area = shop_.window->find(kShopArea)) {
            area->draw = [this](gui::Window& self, gui::Painter& p) {
                const SDL_Rect r = rectOf(&self);
                // Every mode but Buy (greeting, sell, repair) shows text instead of the stock.
                if (shop_.mode != 2) {
                    // The greeting or the mode's instructions in the text border.
                    p.image("ShopTextBorder", 0, 0);
                    const std::string t = shop_.mode == 3   ? text("SellInstructions", "GUIShop.c")
                                          : shop_.mode == 4 ? text("RepairInstructions", "GUIShop.c")
                                                            : shop_.greeting;
                    p.text(t, r.x + 8, r.y + 8, false, kWhite, r.w - 16);
                    return;
                }
                // The stock, clipped to the area.
                SDL_Renderer* renderer = p.renderer();
                const SDL_FRect clipF = p.toOutput(r.x, r.y, r.w, r.h);
                const SDL_Rect clip{static_cast<int>(clipF.x), static_cast<int>(clipF.y), static_cast<int>(clipF.w),
                                    static_cast<int>(clipF.h)};
                SDL_SetRenderClipRect(renderer, &clip);
                const int fh = p.fontHeight(false);
                // 10 rows of 6 columns (slot [row + column * 10]); rows scrolled out of the area skipped.
                for (int row = 0; row < 10; ++row) {
                    const int y = r.y + row * kCell - shop_.scroll;
                    if (y <= r.y - kCell || y >= r.y + r.h) {
                        continue;
                    }
                    p.image(row % 2 == 0 ? "ShopInventoryBar1" : "ShopInventoryBar2", r.x, y);
                    for (int col = 0; col < 6; ++col) {
                        const StockSlot& s = shop_.slots[static_cast<std::size_t>(row + col * 10)];
                        if (s.items.empty()) {
                            continue;
                        }
                        const int x = r.x + 5 + col * kCell;
                        drawItem(p, s.typeId, s.modifiers, x + 20, y + 25);
                        if (gold_ < s.price) {
                            p.fillAlpha(x - 5, y, kCell, kCell, {0, 0, 0, 128}); // can't afford it
                        }
                        if (s.items.size() > 1) {
                            p.text(std::to_string(s.items.size()), x, y + 5, false, kWhite);
                        }
                        p.text(std::to_string(s.price), x, y - fh + 45, false, kYellow);
                    }
                }
                SDL_SetRenderClipRect(renderer, nullptr);
            };
        }
        if (gui::Window* slider = shop_.window->find(kShopSlider)) {
            // Its thumb: ShopInventorySlider, 16 x 12, 15 up.
            slider->enabledImage = "ShopInventorySlider";
            slider->hiliteImage = "ShopInventorySliderSelected";
            slider->imageOffset = {0, -15};
            slider->status |= nox::gui::status::Image;
        }
        shop_.window->show(false);
    }

    // ---- the NPC dialog
    dialog_.window = gui_.load(windowDir_ / "Dialog.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message == gui::SliderChanged && from.id == kDlgSlider) {
            if (gui::Window* list = dialog_.window->find(kDlgList)) {
                // Lines are 11 high; the slider's top (its max) is the start of the text.
                const int visible = std::max(1, list->rect.h / 11);
                const int most = std::max(0, static_cast<int>(list->lines.size()) - visible);
                const auto* sl = std::get_if<nox::gui::SliderData>(&from.data);
                const int range = sl ? std::max(1, sl->max - sl->min) : 1;
                list->scroll = most * (range - (from.sliderValue - (sl ? sl->min : 0))) / range;
            }
            return true;
        }
        if (message != gui::Clicked) {
            return false;
        }
        sound(0x2FE); // ButtonPress
        switch (from.id) {
        case kDlgDone:
        case kDlgYes:
        case kDlgNo: {
            const std::uint8_t answer = from.id == kDlgDone ? 0 : from.id == kDlgYes ? 1 : 2;  // the dialog answer sent: 0 done, 1 yes, 2 no
            deferred_.push_back([this, answer] {
                if (actions_.dialogAnswer) {
                    actions_.dialogAnswer(answer);
                }
            });
            return true;
        }
        case kDlgReplay:
            deferred_.push_back([this] {
                if (actions_.replay) {
                    actions_.replay();
                }
            });
            return true;
        case kDlgUp:
            if (gui::Window* list = dialog_.window->find(kDlgList)) {
                scrollDialog(list->scroll - 1);
            }
            return true;
        case kDlgDown:
            if (gui::Window* list = dialog_.window->find(kDlgList)) {
                scrollDialog(list->scroll + 1);
            }
            return true;
        default:
            return false;
        }
    });
    if (dialog_.window) {
        // Its background (ShopBase) at the layout's corner.
        dialog_.window->draw = [](gui::Window& self, gui::Painter& p) { p.image(self.backgroundImage, 0, 0); };
        gui::useSliderThumb(dialog_.window->find(kDlgSlider));
        if (gui::Window* done = dialog_.window->find(kDlgDone)) {
            // Once the speech has finished, Done flashes.
            done->draw = [this](gui::Window& self, gui::Painter& p) {
                const std::uint32_t b = frame_;
                // Flashing in bursts: in the first 30 of every 128 frames, 8 on and 8 off.
                if (!voiceBusy_ && (b & 0x7F) < 0x1E && (b & 8)) {
                    const SDL_Rect r = rectOf(&self);
                    const nox::gui::Color orange{0xF0, 0x80, 0x40, false};
                    p.outline(r.x, r.y, r.w, r.h - 2, orange);
                    p.outline(r.x + 1, r.y + 1, r.w - 2, r.h - 4, orange);
                }
                const SDL_Rect r = rectOf(&self);
                const nox::gui::Color c = (self.state & gui::kLit) ? self.hiliteColor : self.textColor;
                const int tw = p.textWidth(self.text, self.largeFont);
                p.text(self.text, r.x + (r.w - tw) / 2, r.y + (r.h - p.fontHeight(self.largeFont)) / 2, self.largeFont,
                       c.transparent ? self.textColor : c);
            };
        }
        dialog_.window->show(false);
    }

    // ---- the player trade
    trade_.window = gui_.load(windowDir_ / "Trade.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        sound(0x2FE); // ButtonPress
        if (from.id == kTrdAccept) {
            deferred_.push_back([this] {
                if (actions_.tradeAccept) {
                    actions_.tradeAccept();
                }
            });
            return true;
        }
        if (from.id == kTrdCancel) {
            deferred_.push_back([this] {
                if (actions_.tradeCancel) {
                    actions_.tradeCancel();
                }
            });
            return true;
        }
        return false;
    });
    if (trade_.window) {
        trade_.window->draw = [this](gui::Window& self, gui::Painter& p) { (void)self; (void)p; };
        trade_.window->show(false);
    }

    // ---- the amount box (gui_itemAmount_init)
    amountWindow_ = gui_.load(windowDir_ / "MultMove.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked || !amount_) {
            return false;
        }
        sound(0x2FE); // ButtonPress
        switch (from.id) {
        // Up/Down step the count between 1 and the most allowed.
        case kAmtUp:
            if (amount_->count + 1 <= amount_->max) {
                ++amount_->count;
            }
            return true;
        case kAmtDown:
            if (amount_->count > 1) {
                --amount_->count;
            }
            return true;
        // Yes (or a click on the label) confirms; the count is clamped to the most once more.
        case kAmtYes:
        case kAmtLabel: {
            auto yes = amount_->yes;
            const int count = std::min(amount_->count, amount_->max);
            deferred_.push_back([this, yes, count] {
                closeAmount();
                if (yes) {
                    yes(count);
                }
            });
            return true;
        }
        case kAmtNo: {
            auto no = amount_->no;
            deferred_.push_back([this, no] {
                closeAmount();
                if (no) {
                    no();
                }
            });
            return true;
        }
        default:
            return false;
        }
    });
    if (amountWindow_) {
        amountWindow_->status |= nox::gui::status::Above;
        amountWindow_->draw = [this](gui::Window& self, gui::Painter& p) {
            // The screen darkened, the base (with the price tag when priced), the item,
            // and the pressed buttons lit.
            const SDL_Rect screen = screenRect();
            p.fillAlpha(screen.x, screen.y, screen.w, screen.h, {0, 0, 0, 128});
            const SDL_Point at = self.screenPos();
            const bool priced = amount_ && amount_->price;
            p.image(priced ? "MultiMoveBase" : "MultiMoveBaseNoTag", at.x, at.y);
            if (amount_) {
                drawItem(p, amount_->typeId, amount_->modifiers, at.x + kAmountItem.x, at.y + kAmountItem.y);
            }
            const auto pressed = [&](std::int32_t id) {
                const gui::Window* b = self.find(id);
                return b && (b->state & gui::kPressed);
            };
            if (pressed(kAmtDown)) {
                p.image("MultiMoveDownLit", at.x, at.y);
            }
            if (pressed(kAmtUp)) {
                p.image("MultiMoveUpLit", at.x, at.y);
            }
            if (pressed(kAmtYes)) {
                p.image(priced ? "MultiMoveYesPressed" : "MultiMoveYesPressedNoTag", at.x, at.y);
            }
            if (pressed(kAmtNo)) {
                p.image(priced ? "MultiMoveNoPressed" : "MultiMoveNoPressedNoTag", at.x, at.y);
            }
        };
        amountWindow_->show(false);
    }
}

std::string TradeScreens::text(const std::string& id, const char* file) const
{
    return gui_.text(id, file ? file : "");
}

SDL_Rect TradeScreens::screenRect() const
{
    // Layout (0, 0) is the game screen's (W - 640, H - 480).
    return {640 - gameW_, 480 - gameH_, gameW_, gameH_};
}

std::optional<int> TradeScreens::shopCursor() const
{
    if (!shop_.open) {
        return std::nullopt;
    }
    switch (shop_.mode) {
    case 2:
        return 11; // Buy
    case 3:
        return 12; // Sell
    case 4:
        return inventory_.shopMode() == Inventory::ShopMode::Repair ? std::optional<int>(8) : std::nullopt;
    default:
        return std::nullopt;
    }
}

void TradeScreens::drawItem(gui::Painter& p, std::uint16_t typeId, const std::array<std::uint8_t, 4>& mods, int x,
                            int y) const
{
    const SDL_FRect at = p.toOutput(x, y, 0, 0);
    const float s = p.scale();
    inventory_.drawItem(p.renderer(), typeId, mods, at.x / s, at.y / s, s);
}

void TradeScreens::setShopMode(int mode)
{
    // Sell and Repair hand the inventory's clicks to the shop; Buy (and leaving
    // Repair) give them back.
    shop_.mode = mode;
    inventory_.setShopMode(mode == 3   ? Inventory::ShopMode::Sell
                           : mode == 4 ? Inventory::ShopMode::Repair
                                       : Inventory::ShopMode::None);
}

// The stock scrolled to `to` pixels (0-300), the slider moved to match (it runs the other way).
void TradeScreens::scrollStock(int to)
{
    shop_.scroll = std::clamp(to, 0, kStockScrollMax);
    if (gui::Window* slider = shop_.window ? shop_.window->find(kShopSlider) : nullptr) {
        slider->sliderValue = kStockScrollMax - shop_.scroll; // (0x400A to the slider)
    }
}

// The dialog's text scrolled to line `to` (clamped), the slider moved to match.
void TradeScreens::scrollDialog(int to)
{
    gui::Window* list = dialog_.window ? dialog_.window->find(kDlgList) : nullptr;
    if (!list) {
        return;
    }
    const int visible = std::max(1, list->rect.h / 11);
    const int most = std::max(0, static_cast<int>(list->lines.size()) - visible);
    list->scroll = std::clamp(to, 0, most);
    if (gui::Window* slider = dialog_.window->find(kDlgSlider)) {
        const auto* sl = std::get_if<nox::gui::SliderData>(&slider->data);
        if (sl) {
            const int range = std::max(1, sl->max - sl->min);
            slider->sliderValue = sl->min + range - (most == 0 ? 0 : list->scroll * range / most);
        }
    }
}

void TradeScreens::openShop(const nox::client::World& world)
{
    // Gui_Shop_Open.
    const auto& t = world.trade();
    // Remember the trade's event counters so only offers that arrive after opening show a box.
    shop_.open = true;
    shop_.slots = {};
    shop_.seenShort = t.shortOfGoldCount;
    shop_.seenSell = t.sellOfferCount;
    shop_.seenRepair = t.repairOfferCount;
    shop_.sellBoxOpen = shop_.repairBoxOpen = false;
    setShopMode(1);
    scrollStock(0);
    shop_.greeting = t.greeting.empty() ? std::string()
                                        : (actions_.spoken ? actions_.spoken(t.greeting) : text(t.greeting));
    if (shop_.window) {
        if (gui::Window* name = shop_.window->find(kShopName)) {
            name->text = t.partner;
        }
        // The shopkeeper's picture by its thing name; unknown keepers keep the plain one.
        if (gui::Window* pic = shop_.window->find(kShopPic)) {
            const std::string keeper = world.thingName(t.shopkeeperType);
            pic->backgroundImage = "ShopKeeperPic";
            for (const auto& [thing, image] : kKeeperPics) {
                if (keeper == thing) {
                    pic->backgroundImage = image;
                }
            }
        }
        shop_.window->show(true);
    }
    inventory_.open();
}

void TradeScreens::closeShop()
{
    // Gui_Shop_Close.
    shop_.open = false;
    setShopMode(0);
    shop_.slots = {};
    closeAmount();
    closeMessageBox();
    if (shop_.window) {
        shop_.window->show(false);
    }
    inventory_.close();
}

void TradeScreens::syncStock(const nox::client::World& world)
{
    // Gui_Shop_AddStockItem / RemoveStockItem, from the world's list.
    const auto& stock = world.trade().shopItems;
    // Items match by their net codes: those no longer in the world's stock are dropped first.
    std::set<std::uint16_t> present;
    for (const auto& i : stock) {
        present.insert(i.netCode);
    }
    // Items gone from the stock leave their slots.
    std::set<std::uint16_t> shown;
    for (auto& s : shop_.slots) {
        std::erase_if(s.items, [&](std::uint16_t nc) { return !present.count(nc); });
        if (s.items.empty()) {
            s = StockSlot{};
        }
        shown.insert(s.items.begin(), s.items.end());
    }
    // New items: onto their type's slot when they stack, else the first free one.
    for (const auto& i : stock) {
        if (shown.count(i.netCode)) {
            continue;
        }
        const TypeArt* a = art_.get(i.typeId);
        const bool stacks = !(a && (a->classes & oc::NotStackable));
        StockSlot* slot = nullptr;
        if (stacks) {
            for (auto& s : shop_.slots) {
                if (!s.items.empty() && s.typeId == i.typeId) {
                    slot = &s;
                    break;
                }
            }
        }
        // A free one, row by row.
        for (int row = 0; !slot && row < 10; ++row) {
            for (int col = 0; col < 6; ++col) {
                auto& s = shop_.slots[static_cast<std::size_t>(row + col * 10)];
                if (s.items.empty()) {
                    slot = &s;
                    break;
                }
            }
        }
        if (!slot) {
            continue;
        }
        if (slot->items.empty()) {
            slot->typeId = i.typeId;
            slot->modifiers = i.modifiers;
        }
        if (slot->items.size() < 32) {  // (a slot shows at most 32)
            slot->items.push_back(i.netCode);
        }
        slot->price = i.value;
        shown.insert(i.netCode);
    }
}

std::optional<int> TradeScreens::stockAt(SDL_FPoint l) const
{
    // The column and the scrolled row under the point.
    const SDL_Rect r = rectOf(shop_.window ? shop_.window->find(kShopArea) : nullptr);
    if (!inRect(l, r)) {
        return std::nullopt;
    }
    const int col = std::min(5, static_cast<int>(l.x - static_cast<float>(r.x)) / kCell);
    const int row = std::min(9, (shop_.scroll + static_cast<int>(l.y - static_cast<float>(r.y))) / kCell);
    const int slot = row + col * 10;
    if (shop_.slots[static_cast<std::size_t>(slot)].items.empty()) {
        return std::nullopt;
    }
    return slot;
}

void TradeScreens::openDialog(const nox::client::World& world, float scale)
{
    // Gui_NpcDialog_Open.
    const auto& d = world.dialog();
    dialog_.open = true;
    dialog_.count = d.count;
    if (!dialog_.window) {
        return;
    }
    dialog_.text = actions_.spoken ? actions_.spoken(d.textId) : text(d.textId);
    if (gui::Window* list = dialog_.window->find(kDlgList)) {
        list->scroll = 0;
    }
    wrapDialog(scale);
    scrollDialog(0);
    if (gui::Window* title = dialog_.window->find(kDlgTitle)) {
        title->text = d.title;
    }
    if (gui::Window* portrait = dialog_.window->find(kDlgPortrait)) {
        portrait->backgroundImage = d.portrait;
        portrait->status |= nox::gui::status::Image;
    }
    // The buttons: Yes / No (at 5 and 35) when asked, Replay at 65, Done (or Next) at 95.
    const auto place = [&](std::int32_t id, bool shown, int y) {
        if (gui::Window* b = dialog_.window->find(id)) {
            b->show(shown);
            b->status = shown ? (b->status | nox::gui::status::Enabled) : (b->status & ~nox::gui::status::Enabled);
            b->rect.y = y;
        }
    };
    const bool asks = d.flags & 1;  // DialogState flags: 1 asks Yes / No, 2 the last button reads Next
    place(kDlgYes, asks, 5);
    place(kDlgNo, asks, 0x23);
    place(kDlgReplay, true, 0x41);
    place(kDlgDone, true, 0x5F);
    if (gui::Window* done = dialog_.window->find(kDlgDone)) {
        done->text = text((d.flags & 2) ? "Dialog.wnd:Next" : "Dialog.wnd:Done");
    }
    dialog_.window->show(true);
    // The dialog takes the inventory's place on the screen.
    inventory_.close();
}

void TradeScreens::wrapDialog(float scale)
{
    // GUIDlg's text setter: the line cut into list lines at the box's width less 10 (text_wrap.hpp's
    // dialogLines), as many as the list holds. Measured with the font at the size it's drawn
    // at this scale, so every line fits the box as drawn and none wraps again in the list's own
    // drawing (which wraps a line wider than the box less 7); rewrapped when the scale changes.
    dialog_.wrapScale = scale;
    gui::Window* list = dialog_.window ? dialog_.window->find(kDlgList) : nullptr;
    if (!list) {
        return;
    }
    list->lines.clear();
    const auto& res = gui_.resources();
    if (!res.font) {
        return;
    }
    const auto* ld = std::get_if<nox::gui::ListBoxData>(&list->data);
    const std::size_t most = ld && ld->lines ? ld->lines : 128;
    const gui::Painter p(nullptr, res, scale, 0, 0);
    const float width = static_cast<float>(list->rect.w - 10) * scale;
    for (auto& line : dialogLines(dialog_.text, width, most, res.font->advances(p.pixelSize(list->largeFont)))) {
        list->lines.push_back({std::move(line), list->textColor});
    }
    // (the scroll kept, clamped to the new lines)
    scrollDialog(list->scroll);
}

void TradeScreens::closeDialog()
{
    // Gui_NpcDialog_Close.
    dialog_.open = false;
    if (dialog_.window) {
        if (gui::Window* list = dialog_.window->find(kDlgList)) {
            list->lines.clear();
        }
        dialog_.window->show(false);
    }
}

void TradeScreens::openTrade(const nox::client::World& world)
{
    // Gui_Trade_StartP2P.
    trade_.open = true;
    trade_.ours = {};
    trade_.theirs = {};
    trade_.pressed.reset();
    if (trade_.window) {
        if (gui::Window* n = trade_.window->find(kTrdOurName)) {
            const auto me = world.players().find(world.self().netCode);
            n->text = me != world.players().end() ? me->second.name : std::string();
        }
        if (gui::Window* n = trade_.window->find(kTrdTheirName)) {
            n->text = world.trade().partner;
        }
        trade_.window->show(true);
    }
    inventory_.open();
}

void TradeScreens::closeTrade()
{
    trade_.open = false;
    if (trade_.window) {
        trade_.window->show(false);
    }
    inventory_.close();
}

void TradeScreens::syncOffers(const nox::client::World& world)
{
    // Gui_Trade_AddItem / RemoveItem: a type shares its slot unless it's a
    // weapon, armour or wand; else the first free one, row by row.
    const auto sync = [&](std::array<OfferSlot, 4>& slots, const std::vector<nox::client::TradeItem>& items) {
        std::set<std::uint16_t> present;
        for (const auto& i : items) {
            present.insert(i.netCode);
        }
        // Drop items no longer offered; a slot left empty is cleared.
        for (auto& s : slots) {
            std::erase_if(s.items, [&](std::uint16_t nc) { return !present.count(nc); });
            if (s.items.empty()) {
                s = OfferSlot{};
            }
        }
        // Add each newly offered item to a slot of its type, else to the first free slot.
        for (const auto& i : items) {
            const bool shown = std::any_of(slots.begin(), slots.end(), [&](const OfferSlot& s) {
                return std::find(s.items.begin(), s.items.end(), i.netCode) != s.items.end();
            });
            if (shown) {
                continue;
            }
            const TypeArt* a = art_.get(i.typeId);
            const bool single = a && (a->classes & (oc::Weapon | oc::Armor | oc::Wand));
            OfferSlot* slot = nullptr;
            for (auto& s : slots) {
                if (!single && !s.items.empty() && s.typeId == i.typeId) {
                    slot = &s;
                    break;
                }
            }
            // Slots are numbered column-major: slot = row + column * 2, as in the shop's stock.
            for (int row = 0; !slot && row < 2; ++row) {
                for (int col = 0; col < 2; ++col) {
                    auto& s = slots[static_cast<std::size_t>(row + col * 2)];
                    if (s.items.empty()) {
                        slot = &s;
                        break;
                    }
                }
            }
            if (!slot) {
                continue;
            }
            if (slot->items.empty()) {
                slot->typeId = i.typeId;
                slot->modifiers = i.modifiers;
                slot->value = 0;
            }
            slot->items.push_back(i.netCode);
            slot->value += i.value;
        }
    };
    sync(trade_.ours, world.trade().ourOffer);
    sync(trade_.theirs, world.trade().theirOffer);
}

// Our or the partner's offer slot (2 x 2 cells) under layout point `l`, if it has items.
std::optional<int> TradeScreens::offerAt(SDL_FPoint l, bool ours) const
{
    const SDL_Rect r = rectOf(trade_.window ? trade_.window->find(ours ? kTrdOurs : kTrdTheirs) : nullptr);
    if (!inRect(l, r)) {
        return std::nullopt;
    }
    const int col = std::min(1, static_cast<int>(l.x - static_cast<float>(r.x)) / kCell);
    const int row = std::min(1, static_cast<int>(l.y - static_cast<float>(r.y)) / kCell);
    const int slot = row + col * 2;
    const auto& s = (ours ? trade_.ours : trade_.theirs)[static_cast<std::size_t>(slot)];
    return s.items.empty() ? std::nullopt : std::optional<int>(slot);
}

void TradeScreens::openAmount(const std::string& label, SDL_FPoint at, Amount amount)
{
    // gui_itemAmountDialog: at the mouse less the item's place, kept on the screen.
    amount_ = std::move(amount);
    if (!amountWindow_) {
        return;
    }
    const SDL_Rect screen = screenRect();
    int x = static_cast<int>(at.x) - kAmountItem.x;
    int y = static_cast<int>(at.y) - kAmountItem.y;
    x = std::clamp(x, screen.x, screen.x + screen.w - amountWindow_->rect.w);
    y = std::clamp(y, screen.y, screen.y + screen.h - amountWindow_->rect.h);
    amountWindow_->rect.x = x;
    amountWindow_->rect.y = y;
    if (gui::Window* l = amountWindow_->find(kAmtLabel)) {
        l->text = label;
    }
    amountWindow_->show(true);
    gui_.setModal(amountWindow_);
}

void TradeScreens::askAmount(const std::string& label, std::uint16_t typeId, const std::array<std::uint8_t, 4>& mods,
                             int most, SDL_FPoint at, int w, int h, std::function<void(int)> yes)
{
    Amount a;
    a.typeId = typeId;
    a.modifiers = mods;
    a.max = std::max(1, most);
    a.yes = std::move(yes);
    openAmount(label, gui_.layoutPoint(at.x, at.y, w, h), std::move(a));
}

void TradeScreens::closeAmount()
{
    amount_.reset();
    if (amountWindow_) {
        amountWindow_->show(false);
    }
    if (gui_.modal() == amountWindow_) {
        gui_.setModal(messageBox_);
    }
}

void TradeScreens::closeMessageBox()
{
    if (messageBox_) {
        gui_.destroy(messageBox_);
        messageBox_ = nullptr;
    }
}

void TradeScreens::message(const std::string& title, const std::string& body)
{
    // Gui_Dialog_Create over the shop, with OK.
    closeMessageBox();
    messageBox_ = gui_.load(windowDir_ / "dlg.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        if (from.id != kBoxOk && from.id != kBoxYes && from.id != kBoxNo && from.id != kBoxCancel) {
            return false;
        }
        sound(0x2FE);
        deferred_.push_back([this] {
            closeMessageBox();
            // Hand the modal back to the amount box when one is still open beneath.
            gui_.setModal(amount_ ? amountWindow_ : nullptr);
        });
        return true;
    });
    if (!messageBox_) {
        return;
    }
    const SDL_Rect screen = screenRect();
    messageBox_->rect.x = screen.x + (screen.w - messageBox_->rect.w) / 2;
    messageBox_->rect.y = screen.y + (screen.h - messageBox_->rect.h) / 2;
    messageBox_->status |= nox::gui::status::Above;
    if (gui::Window* t = messageBox_->find(kBoxTitle)) {
        t->text = title;
    }
    if (gui::Window* t = messageBox_->find(kBoxText)) {
        t->text = body;
    }
    // Only OK is shown: this box is a notice, not a question.
    for (const auto id : {kBoxYes, kBoxNo, kBoxCancel}) {
        if (gui::Window* b = messageBox_->find(id)) {
            b->show(false);
        }
    }
    if (gui::Window* b = messageBox_->find(kBoxOk)) {
        b->show(true);
    }
    gui_.setModal(messageBox_);
}

void TradeScreens::notEnoughGold(std::uint32_t shortBy)
{
    // Gui_Shop_ShowNotEnoughGold: NotEnoughGold with the shortfall, NoCanDo.
    std::string body = text("NotEnoughGold", "GUIShop.c");
    // The string's number format replaced by the shortfall.
    for (const char* f : {"%d", "%u", "%i"}) {
        if (const auto at = body.find(f); at != std::string::npos) {
            body.replace(at, 2, std::to_string(shortBy));
            break;
        }
    }
    message(text("ShopInformationTitle", "GUIShop.c"), body);
    sound(0x39D);
}

void TradeScreens::update(const nox::client::World& world, std::uint64_t nowMs, std::uint32_t frame, bool voiceBusy,
                          int w, int h)
{
    // The game screen in game pixels (screenRect's).
    const float s = Hud::scale(w, h);
    gameW_ = static_cast<int>(std::lround(static_cast<float>(w) / s));
    gameH_ = static_cast<int>(std::lround(static_cast<float>(h) / s));
    frame_ = frame;
    voiceBusy_ = voiceBusy;
    gold_ = world.self().gold;
    const auto& t = world.trade();

    // ---- the shop
    const bool shopWanted = t.kind == nox::client::TradeState::Kind::Shop;
    if (shopWanted && !shop_.open) {
        openShop(world);
    } else if (!shopWanted && shop_.open) {
        closeShop();
    }
    if (shop_.open) {
        syncStock(world);
        // The world counts each shop event, so a repeated one (same item twice) still shows.
        if (t.shortOfGoldCount != shop_.seenShort) {
            shop_.seenShort = t.shortOfGoldCount;
            notEnoughGold(t.shortOfGold); // (0x1B; World played NoCanDo)
        }
        if (t.sellOfferCount != shop_.seenSell) {
            shop_.seenSell = t.sellOfferCount;
            // Gui_Shop_ShowSellOffer: the box for the item, as many as are in its cell.
            const auto inv = world.self().inventory.find(t.sellOffer ? t.sellOffer->first : 0);
            if (!shop_.sellBoxOpen && t.sellOffer && inv != world.self().inventory.end()) {
                Amount a;
                a.netCode = t.sellOffer->first;
                a.typeId = inv->second.typeId;
                a.modifiers = inv->second.modifiers;
                a.max = inventory_.stackSize(a.netCode);
                a.price = t.sellOffer->second;
                a.yes = [this, nc = a.netCode, type = a.typeId](int n) {
                    shop_.sellBoxOpen = false;
                    if (n == 1 && actions_.sell) {
                        actions_.sell(nc);
                    } else if (n > 1 && actions_.sellStack) {
                        actions_.sellStack(type, static_cast<std::uint8_t>(n));
                    }
                };
                a.no = [this] { shop_.sellBoxOpen = false; };
                shop_.sellBoxOpen = true;
                SDL_FPoint m{};
                SDL_GetMouseState(&m.x, &m.y);
                openAmount(text("SellLabel", "GUIShop.c"), gui_.layoutPoint(m.x, m.y, w, h), std::move(a));
            }
        }
        if (t.repairOfferCount != shop_.seenRepair) {
            shop_.seenRepair = t.repairOfferCount;
            // Gui_Shop_ShowRepairOffer: one, if we can pay; else not enough gold, and
            // the inventory's repair mode ends.
            const auto inv = world.self().inventory.find(t.repairOffer ? t.repairOffer->first : 0);
            if (!shop_.repairBoxOpen && t.repairOffer && inv != world.self().inventory.end()) {
                if (t.repairOffer->second <= gold_) {
                    Amount a;
                    a.netCode = t.repairOffer->first;
                    a.typeId = inv->second.typeId;
                    a.modifiers = inv->second.modifiers;
                    a.max = 1;
                    a.price = t.repairOffer->second;
                    a.yes = [this, nc = a.netCode](int) {
                        shop_.repairBoxOpen = false;
                        if (actions_.repair) {
                            actions_.repair(nc);
                        }
                    };
                    a.no = [this] { shop_.repairBoxOpen = false; };
                    shop_.repairBoxOpen = true;
                    SDL_FPoint m{};
                    SDL_GetMouseState(&m.x, &m.y);
                    openAmount(text("RepairLabel", "GUIShop.c"), gui_.layoutPoint(m.x, m.y, w, h), std::move(a));
                } else {
                    notEnoughGold(t.repairOffer->second - gold_);
                    inventory_.setShopMode(Inventory::ShopMode::None);
                }
            }
        }
    }

    // ---- the dialog
    const auto& d = world.dialog();
    // A new dialog page (its count changed) rebuilds the window even while one is open.
    // (at the windows' scale, s)
    if (d.open && (!dialog_.open || d.count != dialog_.count)) {
        openDialog(world, s);
    } else if (!d.open && dialog_.open) {
        closeDialog();
    } else if (dialog_.open && s != dialog_.wrapScale) {
        wrapDialog(s); // the window resized: the text to the new widths
    }

    // ---- the player trade
    const bool tradeWanted = t.kind == nox::client::TradeState::Kind::Player;
    if (tradeWanted && !trade_.open) {
        openTrade(world);
    } else if (!tradeWanted && trade_.open) {
        closeTrade();
    }
    if (trade_.open && trade_.window) {
        syncOffers(world);
        // At (198, 193) on the game screen (Gui_Trade_StartP2P).
        const SDL_Rect screen = screenRect();
        trade_.window->rect.x = screen.x + 0xC6;
        trade_.window->rect.y = screen.y + 0xC1;
        // Gui_Trade_SetGoldValues: ours, what we owe "(n)" (none at 0), theirs.
        const auto set = [&](std::int32_t id, const std::string& value) {
            if (gui::Window* l = trade_.window->find(id)) {
                l->text = value;
            }
        };
        set(kTrdOurGold, std::to_string(t.gold[0]));
        set(kTrdOurOwed, t.gold[1] == 0 ? std::string() : "(" + std::to_string(t.gold[1]) + ")");
        set(kTrdTheirGold, std::to_string(t.gold[2]));
    }

    // ---- the amount box's numbers
    if (amount_ && amountWindow_) {
        if (gui::Window* c = amountWindow_->find(kAmtCount)) {
            c->text = std::to_string(amount_->count);
        }
        if (gui::Window* total = amountWindow_->find(kAmtTotal)) {
            total->text = amount_->price ? std::to_string(static_cast<std::uint64_t>(*amount_->price) *
                                                          static_cast<std::uint64_t>(amount_->count))
                                         : std::string();
        }
    }

    gui_.update(nowMs);
    // The buttons' queued work, now no event is being handled.
    gui::runQueued(deferred_);
}

void TradeScreens::draw(SDL_Renderer* renderer, const nox::client::World& world, int w, int h)
{
    if (!anyOpen()) {
        return;
    }
    if (trade_.open && trade_.window) {
        // TradeBase and its gold, the accept lights, then the offers.
        trade_.window->draw = [this, &world](gui::Window& self, gui::Painter& p) {
            const SDL_Point at = self.screenPos();
            p.image("TradeBase", at.x, at.y);
            p.image("TradeGold", at.x - 5, at.y);
            p.image("TradeGold", at.x + 243, at.y);
            const gui::Window* accept = self.find(kTrdAccept);
            const auto& t = world.trade();
            if (t.accepted & 1) {  // bit 0: we accepted; bit 1 (below): the partner did
                p.image("TradeLeftAcceptLit", at.x, at.y);
            } else if (accept && (accept->state & gui::kPressed)) {
                p.image("TradeLeftAcceptPushed", at.x, at.y);
            }
            if (t.accepted & 2) {
                p.image("TradeRightAcceptLit", at.x, at.y);
            }
            const gui::Window* cancel = self.find(kTrdCancel);
            if (cancel && (cancel->state & gui::kPressed)) {
                p.image("TradeCancelLit", at.x, at.y);
            }
            const int fh = p.fontHeight(false);
            // Both offer panes: 2 x 2 cells of 50, each with its count and total value.
            for (const bool ours : {true, false}) {
                const SDL_Rect r = rectOf(self.find(ours ? kTrdOurs : kTrdTheirs));
                const auto& slots = ours ? trade_.ours : trade_.theirs;
                for (int row = 0; row < 2; ++row) {
                    for (int col = 0; col < 2; ++col) {
                        const OfferSlot& s = slots[static_cast<std::size_t>(row + col * 2)];
                        if (s.items.empty()) {
                            continue;
                        }
                        const int x = r.x + col * kCell;
                        const int y = r.y + row * kCell;
                        drawItem(p, s.typeId, s.modifiers, x + 25, y + 25);
                        p.text(std::to_string(s.items.size()), x + 5, y + 5, false, kWhite);
                        p.text(std::to_string(s.value), x + 5, y - fh + 45, false, kYellow);
                    }
                }
            }
        };
    }
    gui_.draw(renderer, w, h);
}

bool TradeScreens::handleEvent(const SDL_Event& event, const nox::client::World& world, int w, int h)
{
    if (!anyOpen()) {
        return false;
    }
    (void)world;
    const bool modal = amount_ || messageBox_;
    // With no box up: our own clicks on the stock and the offers, and the wheel.
    if (!modal) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            const SDL_FPoint l = gui_.layoutPoint(event.button.x, event.button.y, w, h);
            if (shop_.open && shop_.mode == 2) {
                if (const auto slot = stockAt(l)) {
                    // As many as we can pay for, else not enough gold.
                    const StockSlot& s = shop_.slots[static_cast<std::size_t>(*slot)];
                    int count = static_cast<int>(s.items.size());
                    // Fewer than the whole stack when the gold doesn't cover it.
                    if (s.price != 0 && gold_ < s.price * static_cast<std::uint32_t>(count)) {
                        count = static_cast<int>(gold_ / s.price);
                    }
                    if (count == 0) {
                        notEnoughGold(s.price - gold_);
                        return true;
                    }
                    Amount a;
                    a.netCode = s.items.back();
                    a.typeId = s.typeId;
                    a.modifiers = s.modifiers;
                    a.max = count;
                    a.price = s.price;
                    a.yes = [this, nc = a.netCode, type = a.typeId](int n) {
                        // CliNet_SendShopBuyItem / BuyStack: if there's room.
                        if (actions_.canTake && !actions_.canTake(type, n)) {
                            sound(0x39D); // NoCanDo
                            if (actions_.centredMessage) {
                                actions_.centredMessage(text("pickup.c:CarryingTooMuch"));
                            }
                            return;
                        }
                        if (n == 1 && actions_.buy) {
                            actions_.buy(nc);
                        } else if (n > 1 && actions_.buyStack) {
                            actions_.buyStack(type, static_cast<std::uint8_t>(n));
                        }
                    };
                    openAmount(text("BuyLabel", "GUIShop.c"), l, std::move(a));
                    return true;
                }
            }
            // Pressing one of our offered items: what happens is decided on release.
            if (trade_.open) {
                if (const auto slot = offerAt(l, true)) {
                    trade_.pressed = std::pair{*slot, SDL_GetTicks()};
                    trade_.pressedAt = l;
                    sound(0x317); // InventoryPickup
                    return true;
                }
            }
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && trade_.pressed) {
            // A quick click on our item, or letting it go off our side, takes it back.
            const SDL_FPoint l = gui_.layoutPoint(event.button.x, event.button.y, w, h);
            const auto [slot, when] = *trade_.pressed;
            trade_.pressed.reset();
            const SDL_Rect ours = rectOf(trade_.window ? trade_.window->find(kTrdOurs) : nullptr);
            const float dx = l.x - trade_.pressedAt.x;
            const float dy = l.y - trade_.pressedAt.y;
            // A click: let go within a third of a second, moved under 10 units.
            const bool click = SDL_GetTicks() - when < 333 && dx * dx + dy * dy < 100;
            const auto& s = trade_.ours[static_cast<std::size_t>(slot)];
            if ((!inRect(l, ours) || click) && !s.items.empty() && actions_.tradeWithdraw) {
                actions_.tradeWithdraw(s.items.back());
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            SDL_FPoint m{};
            SDL_GetMouseState(&m.x, &m.y);
            const SDL_FPoint l = gui_.layoutPoint(m.x, m.y, w, h);
            const int dir = event.wheel.y > 0 ? -1 : event.wheel.y < 0 ? 1 : 0;  // wheel up scrolls up (negative step)
            if (shop_.open && shop_.mode == 2 && inRect(l, rectOf(shop_.window))) {
                // The shop area's 0x13 / 0x14: the arrows' steps.
                const int to = shop_.scroll + dir * kCell;
                scrollStock(dir < 0 ? (to < 0 ? 0 : to - to % kCell) : std::min(kStockScrollMax, to - to % kCell));
                return true;
            }
            if (dialog_.open && inRect(l, rectOf(dialog_.window))) {
                if (gui::Window* list = dialog_.window->find(kDlgList)) {
                    scrollDialog(list->scroll + dir);
                }
                return true;
            }
        }
    }
    const bool took = gui_.handleEvent(event, w, h);
    if (modal && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                  event.type == SDL_EVENT_MOUSE_WHEEL)) {
        return true; // the box takes every click
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        return false; // (the cursor and the rest still follow the mouse)
    }
    // Clicks on the windows themselves are kept from the game even when no control took them.
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        return took || contains(event.button.x, event.button.y, w, h);
    }
    return took;
}

bool TradeScreens::escape()
{
    if (amount_) {
        auto no = amount_->no;
        closeAmount();
        if (no) {
            no();
        }
        return true;
    }
    if (messageBox_) {
        gui_.destroy(messageBox_);
        messageBox_ = nullptr;
        gui_.setModal(nullptr);
        return true;
    }
    if (shop_.open) {
        if (actions_.shopExit) {
            actions_.shopExit(); // CliNet_SendShopExit
        }
        inventory_.setShopMode(Inventory::ShopMode::None);
        return true;
    }
    if (dialog_.open) {
        if (actions_.dialogAnswer) {
            actions_.dialogAnswer(0); // CliNet_SendDialogDone
        }
        return true;
    }
    if (trade_.open) {
        if (actions_.tradeCancel) {
            actions_.tradeCancel();
        }
        return true;
    }
    return false;
}

bool TradeScreens::contains(float x, float y, int w, int h) const
{
    if (amount_ || messageBox_) {
        return true;
    }
    const SDL_FPoint l = gui_.layoutPoint(x, y, w, h);
    return (shop_.open && inRect(l, rectOf(shop_.window))) || (dialog_.open && inRect(l, rectOf(dialog_.window))) ||
           (trade_.open && inRect(l, rectOf(trade_.window)));
}

std::string TradeScreens::tooltip(float x, float y, int w, int h, const nox::client::World& world) const
{
    if (amount_ || messageBox_) {
        return {};
    }
    const SDL_FPoint l = gui_.layoutPoint(x, y, w, h);
    const auto ask = [this](std::uint16_t nc, std::uint8_t kind) {
        if (actions_.askBook) {
            actions_.askBook(nc, kind);
        }
    };
    if (shop_.open && shop_.mode == 2) {
        // The stock item's name.
        if (const auto slot = stockAt(l)) {
            const StockSlot& s = shop_.slots[static_cast<std::size_t>(*slot)];
            return names_.name(s.items.back(), s.typeId, s.modifiers, world, ask);
        }
    }
    if (trade_.open) {
        // An offered item's name.
        for (const bool ours : {true, false}) {
            if (const auto slot = offerAt(l, ours)) {
                const OfferSlot& s = (ours ? trade_.ours : trade_.theirs)[static_cast<std::size_t>(*slot)];
                return names_.name(s.items.back(), s.typeId, s.modifiers, world, ask);
            }
        }
    }
    return {};
}

} // namespace noxsdl
