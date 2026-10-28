// The inventory screen as the game draws and runs it (guiinv.c,
// wndCreateInventoryMB), and the current-weapon panel in the bottom left that opens it.
//
//   panel      563 x 224 at the top left, sliding down 64 a frame when opened and back up 32 a
//              frame (inventoryDrawAllMB): the paper doll on the left (a 200 x 200 black box with our body
//              in our colours and what we wear and wield), then InventoryBase over it,
//              our name and title ("Mo the Warrior Lord"), the gold / identify / map column
//              (InventoryTraySpecial) and the tray: 4 columns of 50 x 50 cells, 21 rows, three in
//              view (InventoryTray1-3 by row), scrolled with the arrows or the mouse wheel.
//   cells      (guiDrawInventoryTray) the item, a stack's count, a weapon's charges, the equipped ring, a
//              red / yellow tint on worn-out items. Picked-up items stack with their type
//              (not NOT_STACKABLE ones, up to 32) or take the first free cell, row by row
//              (Gui_Inv_AddPickedUpItem); gold, the map and identify stay out. One gone
//              closes the grid up: inventory_grid.hpp.
//   mouse      press on a cell or a worn piece to take it (a stack's first, out of the
//              cell); a quick click (within 10 units and a third of a second) uses it, or equips /
//              takes off a weapon or armour; dropped on the doll it's equipped; dropped back in
//              the tray it moves when it was its cell's last (a worn one comes off); dropped
//              outside the panel it goes on the ground at the cursor. Whatever doesn't move goes
//              back at the end of its cell's stack.
//   weapon     CurrentWeapon (lit while the inventory is open) at the bottom left with our
//              weapon in it; its button opens and closes the inventory.
//   pages      the journal button swaps the tray for the journal (guiDrawJournal: the
//              entries oldest first, QUEST: red, COMPLETED: grey, HINT: yellow, scrolled with
//              the tray's arrows); the stats button swaps the doll for the stats (client_makePlayerStatsDlg:
//              level, health / mana / strength / speed bars against the class's most, armour,
//              what's carried against what can be). Closing the panel puts both back.
//   identify   the magnifier in the special column: a click on an item shows what
//              it is over the doll (identify.wnd): durability, weight, armour value or
//              damage, enchantments, description; the magnifier again (or the right button)
//              leaves it. The map's square is lit while the map is up.
//   buffs      under the panel (moving with it, so at the top of the screen while it's shut,
//              inventoryDrawAllMB): an icon for each enchantment on us (its spell's) and each item power
//              (fire / poison / lightning protection, brilliance, speed, regeneration), with
//              their names as tooltips (inventroryOnHovewerSub).
//   a stack    dropped outside the panel asks how many (the amount box, DropLabel).
//   quests     (game flag 0x1000) after the buffs, the ExtraLives ankh and "X <lives>"
//              (Gui_Inv_SetQuestLives), its tooltip thing.db:AnkhGUI; while the keys
//              are shared (MSG_GAUNTLET 0x18) the SharedKeyMode icon after it,
//              its tooltip GeneralPrint:TooltipKeyIcon.
#pragma once

#include "bottle_slots.hpp"
#include "inventory_grid.hpp"
#include "inventory_room.hpp"
#include "sprite_cache.hpp"
#include "text.hpp"
#include "item_names.hpp"
#include "thing_art.hpp"

#include "noxcore/client/world.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class Inventory {
public:
    /// What the inventory asks the client to do.
    struct Actions {
        std::function<void(std::uint16_t)> use;
        std::function<void(std::uint16_t)> equip;
        std::function<void(std::uint16_t)> unequip;
        /// Drop at the world point.
        std::function<void(std::uint16_t, int, int)> drop;
        /// Make it the secondary weapon (a weapon put down in the tray).
        std::function<void(std::uint16_t)> secondary;
        /// The worn weapon taken into the tray: off and the secondary, or swapped with it.
        std::function<void(std::uint16_t)> putAwayWeapon;
        /// An item type's weapon class bit (2 quiver, 4 bow, 8 crossbow; 0 none).
        std::function<std::uint32_t(std::uint16_t)> weaponBit;
    };

    Inventory(const nox::things::ThingDb& things, ThingArt& art, const nox::things::ModifierDb& modifiers,
              const nox::strings::StringTable* strings, SpriteCache& sprites, Font* font);
    /// noxcpp's multiclass, a mode's (HeadlessClient::multiclass): the stats' mana a
    /// conjurer's, whatever the class.
    void setMulticlass(bool on) { multiclass_ = on; }
    /// The cells that could take `count` more of `typeId`: empty ones, and stacks of it
    /// with room (31; food 3, or 9 in games with flag 0x800 / 0x1000 of `gameFlags`; never a
    /// NOT_STACKABLE thing).
    int room(std::uint16_t typeId, int count, const nox::client::World& world, std::uint32_t gameFlags) const;
    /// The cells room() reads, as data (the game we host reads them from its own thread).
    RoomCells roomCells(std::uint32_t gameFlags) const;
    /// Room for one more (the client asks before picking something up, CliNet_SendPickup).
    bool canTake(std::uint16_t typeId, const nox::client::World& world, std::uint32_t gameFlags) const
    {
        return room(typeId, 1, world, gameFlags) > 0;
    }
    /// The things in the cells in order (plr_rwInventory's solo walk: rows 0-19, each
    /// row's columns 0-3, each cell's stack in order), net codes.
    std::vector<std::uint16_t> cellOrder() const { return grid_.order(); }
    /// The meters' bottle slots (health, mana, cure poison): their types and counts for the HUD.
    const BottleSlots& bottles() const { return bottles_; }
    /// The item a bottle slot's key or click uses now (BottleSlots::item), if any.
    std::optional<std::uint16_t> bottleItem(int slot) const { return bottles_.item(grid_, slot); }
    /// Snd_PlaySound, for the panel's own sounds (taking an item, setting it down).
    void setSound(std::function<void(std::uint32_t)> play) { playSound_ = std::move(play); }
    /// clientSetAltWeapon(0): what taking the secondary weapon's cell's thing out
    /// does (at a drag's start: the secondary none, the server told).
    void setClearSecondary(std::function<void()> clear) { clearSecondary_ = std::move(clear); }
    /// The drag's cancel (Gui_CancelDragAndDrop's): a thing dragged from a cell put back at its
    /// stack's end; the drag over.
    void cancelDrag();

    bool isOpen() const { return state_ == 1 || state_ == 2; }
    /// ToggleInventory.
    void toggle();
    /// Opens it or closes it, with their sounds, if it isn't so already.
    void open()
    {
        if (!isOpen()) {
            toggle();
        }
    }
    void close()
    {
        if (isOpen()) {
            toggle();
        }
    }

    /// A shop's modes that take over a press on a cell: Sell asks what the item fetches
    /// (clientTradeMB), Repair what mending it costs (the mouse handler's mode 6); `pick` gets its net code.
    enum class ShopMode : std::uint8_t { None, Sell, Repair };
    void setShopMode(ShopMode mode) { shopMode_ = mode; }
    ShopMode shopMode() const { return shopMode_; }
    void setShopPick(std::function<void(std::uint16_t)> pick) { shopPick_ = std::move(pick); }
    /// Whether a weapon swap is under way (the weapon panel shows no fist meanwhile).
    void setSwapPending(std::function<bool()> pending) { swapPending_ = std::move(pending); }
    /// gamedata.bin, for the stats page's maximums (the classes' Max* values), read as a game with
    /// gameFlags reads it (the exe's class table: SOLO lines in a solo game, ARENA ones online).
    void setGameData(const nox::things::GameData* data) { gameData_ = data; }
    void setGameFlags(std::uint32_t gameFlags) { gameFlags_ = gameFlags; }
    /// The item names (identify's title).
    void setItemNames(ItemNames* names) { names_ = names; }
    /// The amount box, for dropping part of a stack: label, the item, the most, at output point,
    /// and what to do with the count chosen.
    using AskAmount = std::function<void(const std::string& label, std::uint16_t typeId,
                                         const std::array<std::uint8_t, 4>& mods, int most, SDL_FPoint at,
                                         std::function<void(int)> yes)>;
    void setAskAmount(AskAmount ask) { askAmount_ = std::move(ask); }
    /// Whether the map is up (the special column's map square lights, guiDrawInventoryTray).
    void setMapShown(bool shown) { mapShown_ = shown; }
    bool identifying() const { return identify_; }
    /// Whether output pixel (x, y) is on the secondary weapon's box (the doll page's, open):
    /// the right button there swaps weapons (its handler's case 7).
    bool onSecondaryBox(float x, float y, int w, int h) const;
    /// Leaves identify mode (the right button, or the magnifier again).
    void stopIdentify();
    /// The cursor identify mode wants at output point (x, y): Identify over the tray,
    /// the magnifier or the shop's stock, IdentifyNo elsewhere, nullopt over the doll.
    std::optional<int> identifyCursor(float x, float y, int w, int h) const;
    /// How many items share `netCode`'s cell, 1 if it isn't in one.
    int stackSize(std::uint16_t netCode) const;
    /// An item's picture centred on (gx, gy), in its item colours: output pixels / `s`.
    void drawItem(SDL_Renderer* r, std::uint16_t typeId, const std::array<std::uint8_t, 4>& mods, float gx, float gy,
                  float s) const;

    /// The console's "charges test": the charges meters shown with these numbers whatever is
    /// equipped (nullopt: as the inventory has it).
    void setTestCharges(std::optional<std::pair<int, int>> charges) { testCharges_ = charges; }

    /// Files new items into cells and forgets gone ones; steps the slide (per game frame).
    void update(const nox::client::World& world, std::uint32_t frame);
    void draw(SDL_Renderer* renderer, const nox::client::World& world, std::uint16_t us, int w, int h,
              SDL_FPoint mouse);

    /// Mouse in output pixels. `toWorld` maps a point to world coordinates (for drops). Returns
    /// whether the inventory (or the weapon panel) took the event.
    bool mouseDown(float x, float y, int w, int h, const nox::client::World& world, std::uint32_t nowMs);
    bool mouseUp(float x, float y, int w, int h, const nox::client::World& world, std::uint32_t nowMs,
                 const std::function<SDL_Point(float, float)>& toWorld, const Actions& actions);
    bool wheel(float x, float y, int w, int h, int dir);
    /// Whether (x, y) is on the panel or the weapon panel.
    bool contains(float x, float y, int w, int h) const;
    /// What's at output pixel (x, y), for its tooltip: an item in a cell or worn on the doll
    /// (its net code), the weapon panel's weapon (its window: `weapon` the one in hand,
    /// with `quiver` worn), or its inventory button (`inventoryOpen` says which text).
    struct Hover {
        std::optional<std::uint16_t> item;
        bool weaponPanel = false;
        std::optional<std::uint16_t> weapon;
        std::optional<std::uint16_t> quiver;
        bool inventoryButton = false;
        bool inventoryOpen = false;
        /// A tooltip of its own: the page buttons, the buff icons, the special column.
        std::string text;
    };
    Hover hoverAt(float x, float y, int w, int h, const nox::client::World& world) const;

private:
    std::optional<std::pair<int, int>> testCharges_;
    static constexpr int kCols = InventoryGrid::kCols, kRows = InventoryGrid::kRows;
    using Cell = InventoryGrid::Cell;
    struct Drag {
        std::uint16_t netCode = 0;
        std::uint16_t typeId = 0;
        std::optional<int> fromCell; ///< else from the doll; out of the cell while dragged
        SDL_FPoint pressed{};         ///< in game pixels, relative to the panel
        std::uint32_t atMs = 0;
    };
    /// The dragged thing back at the end of its cell's stack (at the click cell);
    /// the cell holding another type by then, filed as a new pickup with `fileAnew`
    /// (the drag cancel's), else left out of the grid (the mouse handler's).
    void putBack(const Drag& d, bool fileAnew);

    const nox::things::ImageDef* image(const std::string& name) const;
    void drawNamed(SDL_Renderer* r, const std::string& name, float gx, float gy, float s) const;
    void drawRef(SDL_Renderer* r, const nox::things::ImageRef& ref, float gx, float gy, float s,
                 const SpriteCache::Materials* materials = nullptr) const;
    void drawDoll(SDL_Renderer* r, const nox::client::World& world, std::uint16_t us, float x, float y, float s) const;
    std::optional<int> cellAt(float px, float py) const;
    /// The worn item at a doll point.
    std::optional<std::uint16_t> wornAt(const nox::client::World& world, float px, float py) const;
    std::optional<int> findCell(std::uint16_t netCode) const { return grid_.find(netCode); }
    bool excluded(std::uint16_t typeId, const nox::client::World& world) const;
    std::string title(const nox::client::World& world, std::uint16_t us) const;

    const nox::things::ThingDb& things_;
    ThingArt& art_;
    const nox::things::ModifierDb& modifiers_;
    const nox::strings::StringTable* strings_;
    SpriteCache& sprites_;
    Font* font_;
    std::map<std::string, const nox::things::ImageDef*, std::less<>> images_;
    InventoryGrid grid_;
    /// The meters' bottle slots, re-picked as food goes into the grid or out of it.
    BottleSlots bottles_;
    /// Whether a type is food (class FOOD, the potions too): its filing re-picks the bottle slots.
    bool food(std::uint16_t typeId) const;
    /// What the grid holds, by net code: the pickup report it was filed by
    /// (InventoryItem::pickupOrder; a later one is a new pickup of it).
    std::map<std::uint16_t, std::uint32_t> filed_;
    /// clientSetAltWeapon(0) (the client's secondary weapon none, the server told).
    std::function<void()> clearSecondary_;
    int state_ = 0;       ///< 0 closed, 1 opening, 2 open, 3 closing
    int slide_ = -225;    ///< the panel's top
    int scroll_ = 0;      ///< tray scroll in pixels, 0-850
    std::uint32_t lastFrame_ = 0;
    std::function<void(std::uint32_t)> playSound_;
    void sound(std::uint32_t id) const
    {
        if (playSound_) {
            playSound_(id);
        }
    }
    std::optional<Drag> drag_;
    ShopMode shopMode_ = ShopMode::None;
    std::function<void(std::uint16_t)> shopPick_;
    std::function<bool()> swapPending_;

    std::string text(const std::string& id) const;
    void drawBuffs(SDL_Renderer* r, const nox::client::World& world, float s);
    void drawJournal(SDL_Renderer* r, const nox::client::World& world, float x, float y, float s);
    void drawStats(SDL_Renderer* r, const nox::client::World& world, std::uint16_t us, float x, float y, float s);
    void drawIdentify(SDL_Renderer* r, const nox::client::World& world, float x, float y, float s);
    /// The class's most health, mana, speed and strength (the exe's class table, times the
    /// server's stat multipliers); `cls` 0 warrior, 1 wizard, 2 conjurer.
    std::array<float, 4> classMax(const nox::client::World& world, int cls) const;
    bool multiclass_ = false;
    /// The buff icon slot under output x (inventroryOnHovewerSub), as a (spell, item power bit) pair.
    std::optional<std::pair<std::uint32_t, std::uint8_t>> buffAt(const nox::client::World& world, float gx) const;
    int weightOf(std::uint16_t typeId) const;
    /// The scroll's range: the tray's 850, or the journal's height less the 150 shown.
    int scrollMax() const { return journal_ ? std::max(0, journalHeight_ - 150) : kScrollMax; }
    static constexpr int kScrollMax = 850;
    /// The up and down arrows' scroll (the wheel's too): up 25 and back to a row, down a row on.
    void scrollUp();
    void scrollDown();

    const nox::things::GameData* gameData_ = nullptr;
    std::uint32_t gameFlags_ = 0; ///< setGameFlags
    ItemNames* names_ = nullptr;
    AskAmount askAmount_;
    bool mapShown_ = false;
    bool journal_ = false;     ///< the tray shows the journal
    bool stats_ = false;       ///< the doll shows the stats
    bool identify_ = false;    ///< identify mode (mode 5)
    std::optional<std::uint16_t> identified_; ///< the item identify shows
    int trayScroll_ = 0;       ///< the tray's scroll while the journal has the arrows
    int journalScroll_ = 0;
    int journalHeight_ = 0;    ///< the journal's text height (Gui_Journal_UpdateTextHeight), for its scroll range
};

} // namespace noxsdl
