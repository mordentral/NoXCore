// The game's shop, NPC dialog and player trade windows, and the item-amount box they share, as
// GUIShop.c, GUIDlg.c and GUITrade.c run them. One window system (gui.hpp) holds them all, laid
// out at the game's scale with the 640 x 480 layout at the screen's bottom right, as the exe
// places them on a bigger screen.
//
//   shop       (Shop.wnd, opened by MSG_TRADE 0x0D, Gui_Shop_Open) a strip
//              across the bottom on ShopBase, the HUD hidden and the inventory opened: the
//              shopkeeper's picture (by the shopkeeper's type, getShopPic) and the shop's name on
//              the right, Buy / Sell / Repair / Exit beside them, and on the left what
//              the mode shows:
//                greeting  ShopRepairMode, the greeting in the text border;
//                Buy       ShopTradeMode and the stock: 6 columns, 10 rows of 50 scrolled by the
//                          arrows, the slider or the wheel; an item's count, its price
//                          in yellow, darkened when we can't afford one. A click opens the
//                          amount box for it (as many as we can pay for), and buys;
//                Sell      ShopIdentifyMode, SellInstructions; a click on an inventory cell asks
//                          what it fetches (0x1C), the answer opens the amount box (Gui_Shop_ShowSellOffer);
//                Repair    ShopRepairMode, RepairInstructions; a click on a cell asks the cost
//                          (0x1E), then the box (one) or "not enough gold" (Gui_Shop_ShowRepairOffer).
//              Stock arrives an item at a time (0x08); items of one type share a cell unless
//              NOT_STACKABLE, new types going into the first free cell row by row (Gui_Shop_AddStockItem).
//              Exit (or Escape) asks to leave (0x12); MSG_TRADE 0x02 closes it.
//   dialog     (Dialog.wnd, MSG_DIALOG 0x03, Gui_NpcDialog_Open) the same strip:
//              the line (the speech's own variant) word-wrapped in the scrolling text box, the
//              speaker's portrait and name, Done (Next when flagged) and Replay, Yes and No when
//              asked (flag 1). Done flashes once the speech has finished. Done /
//              Yes / No answer 0 / 1 / 2 (Gui_NpcDialog_WndProc); Escape is Done. MSG_DIALOG 0x04 closes it.
//   trade      (Trade.wnd, MSG_TRADE 0x0C) a box at (198, 193): our name and offer on
//              the left, the partner's on the right (2 x 2 cells of 50, counts and values), the
//              gold totals, Accept and Cancel; a click on one of our items takes it back. (1.2b's
//              server only starts these against a shopkeeper object; nothing in its client adds
//              an item, so the window is here for servers that do.)
//   amount     (MultMove.wnd, gui_itemAmountDialog) the screen darkened, the item, how many (the arrows, 1 to
//              the most allowed), the total price when there is one, the action (Buy / Sell /
//              Repair) and yes / no.
#pragma once

#include "gui.hpp"
#include "inventory.hpp"
#include "item_names.hpp"

#include "noxcore/client/world.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class TradeScreens {
public:
    /// What the windows ask the client and the rest of the game to do.
    struct Actions {
        std::function<void()> shopExit;
        std::function<void(std::uint16_t)> buy;                       ///< one item, by net code
        std::function<void(std::uint16_t, std::uint8_t)> buyStack;    ///< type, count
        std::function<void(std::uint16_t)> sell;
        std::function<void(std::uint16_t, std::uint8_t)> sellStack;   ///< type, count
        std::function<void(std::uint16_t)> repair;
        std::function<void(std::uint16_t)> askSellPrice;
        std::function<void(std::uint16_t)> askRepairCost;
        std::function<void()> tradeAccept;
        std::function<void()> tradeCancel;
        std::function<void(std::uint16_t)> tradeWithdraw;
        std::function<void(std::uint8_t)> dialogAnswer; ///< 0 Done, 1 Yes, 2 No
        std::function<void()> replay;                    ///< the speech again
        /// A string's text as it's being spoken (the same variant as the speech).
        std::function<std::string(const std::string& id)> spoken;
        /// Whether the inventory has room for `count` of a type.
        std::function<bool(std::uint16_t typeId, int count)> canTake;
        /// Gui_PrintCenteredMessage.
        std::function<void(const std::string&)> centredMessage;
        /// MSG_INFO_BOOK_DATA for a book's tooltip (HeadlessClient::askBook).
        std::function<void(std::uint16_t, std::uint8_t)> askBook;
    };

    TradeScreens(gui::Resources res, std::filesystem::path windowDir, Inventory& inventory, ItemNames& names,
                 ThingArt& art, Actions actions);

    bool shopOpen() const { return shop_.open; }
    bool dialogOpen() const { return dialog_.open; }
    bool tradeOpen() const { return trade_.open; }
    /// The shop and the dialog hide the HUD while they're up (the client's flag that turns the HUD drawing off).
    bool hidesHud() const { return shop_.open || dialog_.open; }
    /// The cursor the shop wants over the window or the inventory: Buy, Sell or Repair (the
    /// mode's), else nullopt.
    std::optional<int> shopCursor() const;

    /// Opens and closes the windows as the world says; `voiceBusy`: speech is playing.
    void update(const nox::client::World& world, std::uint64_t nowMs, std::uint32_t frame, bool voiceBusy, int w,
                int h);
    void draw(SDL_Renderer* renderer, const nox::client::World& world, int w, int h);
    /// Mouse and keys while a window is up; returns whether one took the event.
    bool handleEvent(const SDL_Event& event, const nox::client::World& world, int w, int h);
    /// Escape: leaves the shop (CliNet_SendShopExit) or answers the dialog Done; whether it did.
    bool escape();
    /// Whether output point (x, y) is on one of the windows.
    bool contains(float x, float y, int w, int h) const;
    std::string tooltip(float x, float y, int w, int h, const nox::client::World& world) const;
    /// The amount box for someone else (the inventory dropping part of a stack): at output point
    /// `at`, 1 to `most`, no price; `yes` gets the count.
    void askAmount(const std::string& label, std::uint16_t typeId, const std::array<std::uint8_t, 4>& mods, int most,
                   SDL_FPoint at, int w, int h, std::function<void(int)> yes);

private:
    struct StockSlot {
        std::uint16_t typeId = 0;
        std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF};
        std::vector<std::uint16_t> items; ///< net codes; the last is the one bought first
        std::uint32_t price = 0;
    };
    struct OfferSlot {
        std::uint16_t typeId = 0;
        std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF};
        std::vector<std::uint16_t> items;
        std::uint32_t value = 0;
    };
    struct Amount {
        std::uint16_t netCode = 0;
        std::uint16_t typeId = 0;
        std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF};
        int count = 1;
        int max = 1;
        std::optional<std::uint32_t> price; ///< each; shows the total
        std::function<void(int)> yes;
        std::function<void()> no;
    };

    void openShop(const nox::client::World& world);
    void closeShop();
    void setShopMode(int mode);
    void syncStock(const nox::client::World& world);
    void openDialog(const nox::client::World& world, float scale);
    /// The dialog's text onto its list's lines, at the widths it's drawn with at `scale`.
    void wrapDialog(float scale);
    void closeDialog();
    void openTrade(const nox::client::World& world);
    void closeTrade();
    void syncOffers(const nox::client::World& world);
    void openAmount(const std::string& label, SDL_FPoint at, Amount amount);
    void closeAmount();
    void message(const std::string& title, const std::string& text);
    /// The message box gone, if one is up.
    void closeMessageBox();
    /// Any of the screens, the amount box or the message box is up.
    bool anyOpen() const { return shop_.open || dialog_.open || trade_.open || amount_ || messageBox_; }
    void notEnoughGold(std::uint32_t shortBy);
    std::optional<int> stockAt(SDL_FPoint layout) const;
    std::optional<int> offerAt(SDL_FPoint layout, bool ours) const;
    void scrollStock(int to);
    void scrollDialog(int to);
    void drawItem(gui::Painter& p, std::uint16_t typeId, const std::array<std::uint8_t, 4>& mods, int x, int y) const;
    std::string text(const std::string& id, const char* file = nullptr) const;
    void sound(std::uint32_t id) const { gui_.resources().sound(id); }
    SDL_Rect screenRect() const; ///< the game screen in the layout's units

    gui::Gui gui_;
    std::filesystem::path windowDir_;
    Inventory& inventory_;
    ItemNames& names_;
    ThingArt& art_;
    Actions actions_;
    int gameW_ = 640, gameH_ = 480; ///< the game screen in game pixels (Hud::scale)
    std::uint32_t frame_ = 0;
    bool voiceBusy_ = false;
    std::uint32_t gold_ = 0;

    struct {
        bool open = false;
        int mode = 1; ///< 1 greeting, 2 buy, 3 sell, 4 repair (g_guiShopMode)
        gui::Window* window = nullptr;
        std::string greeting;
        std::array<StockSlot, 60> slots{}; ///< [row + column * 10]
        int scroll = 0;                     ///< 0-300, in pixels
        std::uint32_t seenShort = 0, seenSell = 0, seenRepair = 0;
        bool sellBoxOpen = false, repairBoxOpen = false;
    } shop_;
    struct {
        bool open = false;
        gui::Window* window = nullptr;
        std::uint32_t count = 0;
        std::string voiceLine;
        std::string text;     ///< the line as spoken, before wrapping
        float wrapScale = 0;  ///< the scale its list lines were wrapped at (0: none yet)
    } dialog_;
    struct {
        bool open = false;
        gui::Window* window = nullptr;
        std::array<OfferSlot, 4> ours{}, theirs{}; ///< [row + column * 2]
        std::optional<std::pair<int, std::uint64_t>> pressed; ///< our slot, when
        SDL_FPoint pressedAt{};
    } trade_;
    gui::Window* amountWindow_ = nullptr;
    std::optional<Amount> amount_;
    gui::Window* messageBox_ = nullptr;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
