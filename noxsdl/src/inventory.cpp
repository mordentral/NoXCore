// The inventory panel and the current-weapon panel (inventory.hpp describes what they show and how
// the mouse works on them). Which item sits in which cell is InventoryGrid's job; this file draws,
// hit-tests and turns mouse gestures into the client's use / equip / drop requests.
#include "inventory.hpp"

#include "hud.hpp"
#include "player_art.hpp"

#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl {

namespace oc = nox::things::ObjectClass;

namespace {

constexpr int kPanelW = 563, kPanelH = 224;
constexpr int kCell = 50;
/// Rectangles in the panel (x0, y0, x1, y1): the doll, the tray, and the two strips at the
/// bottom right that aren't the panel.
constexpr SDL_Rect kDoll{11, 15, 210, 214};
constexpr SDL_Rect kTray{314, 13, 513, 162};
/// The special column and its squares: gold, identify, the map.
constexpr SDL_Rect kSpecial{254, 13, 313, 162};
constexpr SDL_Rect kOutside1{277, 176, 563, 264};
constexpr SDL_Rect kOutside2{211, 204, 563, 264};
/// The secondary weapon's box on the doll page (window 0x1062468 at (0xAD, 0xAE), 50 x 50);
/// hidden on the stats page.
constexpr SDL_Rect kSecondaryBox{0xAD, 0xAE, 0xAD + 49, 0xAE + 49};
/// The doll's slots, from its corner.
constexpr std::array<SDL_Rect, 9> kDollSlots{{{67, 30, 87, 47},
                                              {75, 0, 130, 40},
                                              {89, 41, 124, 92},
                                              {82, 93, 127, 148},
                                              {75, 149, 135, 200},
                                              {63, 41, 88, 102},
                                              {125, 41, 150, 83},
                                              {0, 0, 81, 200},
                                              {125, 0, 200, 200}}};


/// The doll's armour pictures by armour class bit, then the weapons'.
constexpr std::array<const char*, 26> kMaleArmor{
    "MaleStreetSneakers", "MaleMedievalCloak", "MaleStreetPants",   "MaleMedievalPants",  "MaleLeatherLeggings",
    "MaleChainLeggings",  "MaleLeatherBoots",  "MaleLeatherArmoredBoots", "MalePlateBoots", "MalePlateLeggings",
    "MaleStreetShirt",    "MaleMedievalShirt", "MaleLeatherArmbands", "MalePlateArms",     "MaleWizardRobe",
    "MaleLeatherTunic",   "MaleChainTunic",    "MalePlateBreast",   "MaleChainCoif",      "MaleWizardHelm",
    "MaleConjurerHelm",   "MaleLeatherHelm",   "MalePlateHelm",     "MaleOrnateHelm",     "MaleRoundShield",
    "MaleKiteShield"};
/// The female table isn't in class-bit order; the doll indexes it by bit all the same.
constexpr std::array<const char*, 26> kFemaleArmor{
    "FemaleStreetPants",  "FemaleMedievalPants", "FemaleStreetSneakers", "FemaleLeatherBoots",
    "FemaleLeatherArmoredBoots", "FemalePlateBoots", "FemaleLeatherLeggings", "FemaleChainLeggings",
    "FemalePlateLeggings", "FemaleStreetShirt",  "FemaleMedievalShirt",  "FemaleLeatherArmbands",
    "FemalePlateArms",    "FemaleWizardRobe",    "FemaleLeatherTunic",   "FemaleChainTunic",
    "FemalePlateBreast",  "FemaleMedievalCloak", "FemaleRoundShield",    "FemaleKiteShield",
    "FemaleChainCoif",    "FemaleWizardHelm",    "FemaleConjurerHelm",   "FemaleLeatherHelm",
    "FemalePlateHelm",    "FemaleOrnateHelm"};
constexpr std::array<const char*, 27> kMaleWeapons{
    "MaleFlag",  "MaleQuiver", "MaleBow",       "MaleCrossbow", "MaleArrow",  "MaleBolt",     "MaleChakram",
    "MaleShuriken", "MaleSword", "MaleLongSword", "MaleGreatSword", "MaleMace", "MaleAxe",   "MaleOgreAxe",
    "MaleHammer", "MaleStaff", "MaleStaffSulphorousFlare", "MaleStaffSulphorousShower", "MaleStaffLightning",
    "MaleStaffFireball", "MaleStaffTripleFireball", "MaleStaffForceOfNature", "MaleStaffDeathRay",
    "MaleStaffOblivionHalberd", "MaleStaffOblivionHeart", "MaleStaffOblivionWierdling", "MaleStaffOblivionOrb"};
/// No ogre axe in the female table, so it's one short from there on (the doll indexes it as is).
constexpr std::array<const char*, 27> kFemaleWeapons{
    "FemaleFlag", "FemaleQuiver", "FemaleBow", "FemaleCrossbow", "FemaleArrow", "FemaleBolt", "FemaleChakram",
    "FemaleShuriken", "FemaleSword", "FemaleLongSword", "FemaleGreatSword", "FemaleMace", "FemaleAxe",
    "FemaleHammer", "FemaleStaff", "FemaleStaffSulphorousFlare", "FemaleStaffSulphorousShower",
    "FemaleStaffLightning", "FemaleStaffFireball", "FemaleStaffTripleFireball", "FemaleStaffForceOfNature",
    "FemaleStaffDeathRay", "FemaleStaffOblivionHalberd", "FemaleStaffOblivionHeart",
    "FemaleStaffOblivionWierdling", "FemaleStaffOblivionOrb", ""};

/// Class names for the title (and gamedata's Max* keys).
constexpr std::array<const char*, 3> kClassNames{"Warrior", "Wizard", "Conjurer"};

/// Whether layout point (gx, gy) is on the weapon panel's button, (5, 11) 28 x 29 in the panel
/// at (-1, H - 127) (wndCreateInventoryMB); H is the layout's height.
bool onWeaponButton(float gx, float gy, float H)
{
    return gx >= 4 && gy >= H - 116 && gx < 32 && gy < H - 87;
}

// Whether (x, y) is inside r, edges included; the rects here keep x0, y0, x1, y1 in x, y, w, h.
bool in(const SDL_Rect& r, float x, float y)
{
    return x >= static_cast<float>(r.x) && y >= static_cast<float>(r.y) && x <= static_cast<float>(r.w) &&
           y <= static_cast<float>(r.h);
}

/// Which doll slot an item goes in: 0 quiver, 1 helmet, 2 body, 3 legs, 4 boots,
/// 5 arms, 7 weapons, 8 shield or bow, 9 none.
int dollSlot(const TypeArt& a)
{
    const std::uint32_t c = a.classes;
    const std::uint32_t s = a.subclasses;
    if ((c & oc::Weapon) && (s & 0x2)) {
        return 0;
    }
    if (c & oc::Armor) {
        if (s & 0x1) {
            return 1;
        }
        if (s & 0x144) {
            return 2;
        }
        if (s & 0x90) {
            return 3;
        }
        if (s & 0x20) {
            return 4;
        }
        if (s & 0x2) {
            return 8;
        }
        if (s & 0x8) {
            return 5;
        }
    }
    if (c & oc::Weapon) {
        return (s & 0x4) ? 8 : 7;
    }
    if (c & oc::Wand) {
        return 7;
    }
    return 9;
}

/// The charges bar beside the weapon panel (meter 5): 61 one-pixel lines (x, y, width
/// from the window's corner, the exe's table) making the curved tube, cut into one segment per
/// charge from the bottom, a line apart under 31 charges; a charge left magenta, a spent one dark
/// purple. The leftover lines go one each to the first segments, and a line belongs to the first
/// segment that reaches it. No maximum (infinite): all lit.
void drawChargeBar(SDL_Renderer* r, float gx, float gy, int value, int max, float s)
{
    struct Line {
        std::uint8_t x, y, w;
    };
    static constexpr std::array<Line, 61> kLines{{
        {4, 7, 6},    {5, 8, 8},    {6, 9, 8},    {7, 10, 8},   {8, 11, 7},   {8, 12, 8},   {9, 13, 8},
        {10, 14, 7},  {11, 15, 7},  {11, 16, 7},  {12, 17, 7},  {12, 18, 7},  {13, 19, 7},  {13, 20, 7},
        {14, 21, 6},  {14, 22, 7},  {14, 23, 7},  {15, 24, 6},  {15, 25, 7},  {15, 26, 7},  {15, 27, 7},
        {16, 28, 6},  {16, 29, 7},  {16, 30, 7},  {16, 31, 7},  {16, 32, 7},  {16, 33, 7},  {17, 34, 6},
        {17, 35, 6},  {17, 36, 6},  {17, 37, 6},  {17, 38, 6},  {17, 39, 6},  {17, 40, 6},  {17, 41, 6},
        {16, 42, 7},  {16, 43, 7},  {16, 44, 7},  {16, 45, 7},  {16, 46, 6},  {15, 47, 7},  {15, 48, 7},
        {15, 49, 7},  {15, 50, 6},  {14, 51, 7},  {14, 52, 7},  {14, 53, 6},  {13, 54, 7},  {13, 55, 7},
        {12, 56, 7},  {12, 57, 7},  {11, 58, 7},  {10, 59, 8},  {10, 60, 7},  {9, 61, 8},   {8, 62, 8},
        {7, 63, 8},   {7, 64, 7},   {6, 65, 8},   {5, 66, 7},   {4, 67, 5},
    }};
    constexpr int kCount = static_cast<int>(kLines.size());
    const SDL_Color lit{255, 0, 255, 128};
    const SDL_Color spent{50, 0, 50, 128};
    std::array<bool, kLines.size()> drawn{};
    const auto line = [&](int i, SDL_Color c) {
        if (i < 0 || i >= kCount || drawn[static_cast<std::size_t>(i)]) {
            return;
        }
        drawn[static_cast<std::size_t>(i)] = true;
        const Line& l = kLines[static_cast<std::size_t>(i)];
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        const SDL_FRect f{(gx + l.x) * s, (gy + l.y) * s, l.w * s, s};
        SDL_RenderFillRect(r, &f);
    };
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (max < 1) {
        for (int i = 0; i < kCount; ++i) {
            line(i, lit);
        }
        return;
    }
    // A line apart under 31 charges; each segment gets an even share of the lines the gaps leave.
    const int gap = max < 31 ? 1 : 0;
    const int height = (gap + kCount) / max - gap;
    // The fraction of a line left over per segment, carried until it makes a whole extra line.
    const float extra = static_cast<float>(gap + kCount - (height + gap) * max) / static_cast<float>(max);
    float carried = 0.001f;
    // Segments are laid from the bottom of the tube (the last line) upwards.
    int top = kCount - height;
    for (int i = 0; i < max; ++i) {
        const SDL_Color c = i < value ? lit : spent;
        int next = top - (height + gap);
        int n = std::max(height, 1);
        carried += extra;
        if (carried >= 1.0f) {
            carried -= 1.0f;
            --next;
            --top;
            ++n;
        }
        // The top segment may reach above line 0: cut to the lines there are.
        int from = top;
        if (from < 0) {
            n += from;
            from = 0;
        }
        for (int k = 0; k < n; ++k) {
            line(from + k, c);
        }
        top = next;
    }
}

/// The weapon in hand: a weapon that isn't a quiver (a bow too, which the doll
/// draws on the shield side), or a wand.
bool inHand(const TypeArt& a)
{
    return ((a.classes & oc::Weapon) && !(a.subclasses & 0x2)) || (a.classes & oc::Wand);
}


/// The spell each enchantment shows as, by ENCHANT_* number; 0 none.
constexpr std::array<std::uint32_t, 32> kEnchantSpells{
    0x27, 0x36, 0x03, 0x0C, 0x48, 0x4A, 0, 0, 0x46, 0x24, 0x83, 0x1A, 0, 0x82, 0x01, 0x2A,
    0x0F, 0x3E, 0x40, 0x3F, 0x3D, 0x25, 0x47, 0x28, 0x7F, 0x20, 0x33, 0x43, 0x09, 0x37, 0, 0};

/// The item powers' icons and names, by bit.
struct ItemPower {
    std::uint8_t bit;
    const char* icon;
    const char* name;
};
constexpr std::array<ItemPower, 6> kItemPowers{{
    {0x01, "FireProtectItemIcon", "modifier.db:FireProtectItemEnchantDesc"},
    {0x02, "PoisonProtectItemIcon", "modifier.db:PoisonProtectItemEnchantDesc"},
    {0x04, "LightningProtectItemIcon", "modifier.db:LightningProtItemEnchantDesc"},
    {0x08, "BrillianceItemIcon", "modifier.db:BrillianceItemEnchantDesc"},
    {0x10, "SpeedItemIcon", "modifier.db:SpeedItemEnchantDesc"},
    {0x20, "RegenerationItemIcon", "thing.db:LesserHeal"},
}};


/// Formats "%d" / "%s" / "%.2f" in order from `args` (the string table's own formats).
std::string format(std::string f, std::initializer_list<std::string> args)
{
    auto it = args.begin();
    for (std::size_t at = f.find('%'); at != std::string::npos && it != args.end(); at = f.find('%', at)) {
        std::size_t end = at + 1;
        while (end < f.size() && !std::isalpha(static_cast<unsigned char>(f[end]))) {
            ++end;
        }
        if (end >= f.size()) {
            break;
        }
        f.replace(at, end - at + 1, *it);
        at += it->size();
        ++it;
    }
    return f;
}

std::string fixed2(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v));
    return buf;
}

} // namespace

Inventory::Inventory(const nox::things::ThingDb& things, ThingArt& art, const nox::things::ModifierDb& modifiers,
                     const nox::strings::StringTable* strings, SpriteCache& sprites, Font* font)
    : things_(things), art_(art), modifiers_(modifiers), strings_(strings), sprites_(sprites), font_(font),
      // (A stack: not NOT_STACKABLE, the client's thing's class & 0x4000000.)
      grid_([this](std::uint16_t typeId) {
          return typeId != 0 && typeId <= things_.things.size() &&
                 !(nox::things::thingClasses(things_.things[typeId - 1u]) & oc::NotStackable);
      }),
      bottles_(BottleSlots::typesIn(things))
{
    // Index thing.db's images by name for drawNamed.
    for (const auto& i : things_.images) {
        images_.emplace(i.name, &i);
    }
    bottles_.pick(grid_); // (the meters made: the empty grid's)
}

bool Inventory::food(std::uint16_t typeId) const
{
    return typeId != 0 && typeId <= things_.things.size() &&
           (nox::things::thingClasses(things_.things[typeId - 1u]) & oc::Food);
}

// A thing.db image by name; nullptr if there is none.
const nox::things::ImageDef* Inventory::image(const std::string& name) const
{
    const auto it = images_.find(name);
    return it == images_.end() ? nullptr : it->second;
}

// A sprite at game pixel (gx, gy) plus its own offset, scaled by s; tinted when `materials` is given.
void Inventory::drawRef(SDL_Renderer* /*r*/, const nox::things::ImageRef& ref, float gx, float gy, float s,
                        const SpriteCache::Materials* materials) const
{
    const Sprite* sp = materials ? sprites_.tinted(ref, *materials) : sprites_.get(ref);
    if (!sp) {
        return;
    }
    const SDL_FRect dst{(gx + sp->offsetX) * s, (gy + sp->offsetY) * s, sp->width * s, sp->height * s};
    sp->scale = SDL_SCALEMODE_LINEAR;
    sprites_.render(*sp, nullptr, dst);
}

// A named image, or the first frame of a named animation.
void Inventory::drawNamed(SDL_Renderer* r, const std::string& name, float gx, float gy, float s) const
{
    const nox::things::ImageDef* def = image(name);
    if (!def) {
        return;
    }
    if (def->image) {
        drawRef(r, *def->image, gx, gy, s);
    } else if (def->animation && !def->animation->frames.empty()) {
        drawRef(r, def->animation->frames.front(), gx, gy, s);
    }
}

void Inventory::drawItem(SDL_Renderer* r, std::uint16_t typeId, const std::array<std::uint8_t, 4>& mods, float gx,
                         float gy, float s) const
{
    const TypeArt* a = art_.get(typeId);
    if (!a || !a->still) {
        return;
    }
    // As on the ground: the drawable's picture about its point.
    const auto colours = itemMaterials(*a, &mods, modifiers_);
    drawRef(r, *a->still, gx - a->halfWidth, gy - a->halfHeight - a->z, s, colours ? &*colours : nullptr);
}

// Things never filed into the tray: gold, the map and identify have squares of their own in the special column.
bool Inventory::excluded(std::uint16_t typeId, const nox::client::World& world) const
{
    const std::string name = world.thingName(typeId);
    return name == "Gold" || name == "QuestGoldPile" || name == "QuestGoldChest" || name == "AutoMap" ||
           name == "Identify";
}

int Inventory::stackSize(std::uint16_t netCode) const
{
    const auto cell = findCell(netCode);
    return cell ? std::max<int>(1, static_cast<int>(grid_.cell(*cell).items.size())) : 1;
}

int Inventory::room(std::uint16_t typeId, int count, const nox::client::World& /*world*/, std::uint32_t gameFlags) const
{
    return roomIn(roomCells(gameFlags), typeId, count);
}

RoomCells Inventory::roomCells(std::uint32_t gameFlags) const
{
    // (The rows below 0x14 only: the cell lookup answers 0 for the 21st, and the loop ends there.)
    RoomCells out{};
    for (int row = 0; row < 0x14; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const Cell& c = grid_.at(col, row);
            RoomCell& r = out[static_cast<std::size_t>(row * kCols + col)];
            if (c.items.empty()) {
                continue;
            }
            // (The cell's drawable's type: a stack of 31 at most, food 3, or 9 in a solo or quest
            // game; never a NOT_STACKABLE thing's.)
            const TypeArt* a = art_.get(c.typeId);
            r.typeId = c.typeId;
            r.count = static_cast<std::uint16_t>(std::min<std::size_t>(c.items.size(), 0xFFFF));
            r.limit = (a && (a->classes & oc::Food)) ? ((gameFlags & 0x1800) ? 9 : 3) : 31;
            r.stackable = a && !(a->classes & oc::NotStackable);
        }
    }
    return out;
}

void Inventory::cancelDrag()
{
    if (drag_) {
        const Drag d = *drag_;
        drag_.reset();
        putBack(d, true);
    }
}

void Inventory::putBack(const Drag& d, bool fileAnew)
{
    // Put at the click cell: back at the end of its stack (nothing for one taken from
    // the doll). Refused there (the cell another type's by now): it's filed anew
    // (Gui_Inv_AddPickedUpItem); the inventory window's own put-backs leave it out of the grid.
    if (!d.fromCell) {
        return;
    }
    if (fileAnew) {
        if (!grid_.put(*d.fromCell, d.netCode, d.typeId)) {
            if (grid_.add(d.netCode, d.typeId) && food(d.typeId)) {
                bottles_.pick(grid_); // (filed as a pickup: food re-picks the bottle slots)
            }
        }
    } else {
        grid_.drop(*d.fromCell, std::nullopt, d.netCode, d.typeId);
    }
}

void Inventory::toggle()
{
    // client_toggleInventory (InventoryOpen), or the close (InventoryClose: a drag put
    // back).
    if (isOpen()) {
        state_ = 3;
        cancelDrag();
        stopIdentify();
        sound(0x316);
    } else {
        state_ = 1;
        sound(0x315);
    }
}

void Inventory::update(const nox::client::World& world, std::uint32_t frame)
{
    const auto& inv = world.self().inventory;
    // Gone ones (MSG_REPORT_DROP, Gui_Inv_OnItemDropped): out of their cells, the grid closed up
    // after each; one on the cursor, the drag over. One reported anew (a later
    // pickup report) goes too, to be filed again.
    for (const auto& [nc, order] : std::map<std::uint16_t, std::uint32_t>(filed_)) {  // a copy: entries are erased as it goes
        const auto it = inv.find(nc);
        if (it == inv.end() || it->second.pickupOrder != order) {
            filed_.erase(nc);
            // Food going (the potions too) re-picks the bottle slots; one not found doesn't.
            if (drag_ && drag_->netCode == nc) {
                const bool wasFood = food(drag_->typeId);
                drag_.reset();
                if (wasFood) {
                    bottles_.pick(grid_);
                }
            } else if (const auto cell = grid_.find(nc)) {
                const bool wasFood = food(grid_.cell(*cell).typeId);
                grid_.remove(nc);
                if (wasFood) {
                    bottles_.pick(grid_);
                }
            }
        }
    }
    // New ones in the order their pickup reports came (Gui_Inv_AddPickedUpItem: onto
    // the first stack of their type with room, else the first free cell, row by row); gold, the
    // map and identify not. No room: left out of the grid (InventoryFull), not tried again.
    std::vector<const nox::client::InventoryItem*> fresh;
    for (const auto& [nc, item] : inv) {
        if (!filed_.contains(nc) && !excluded(item.typeId, world)) {
            fresh.push_back(&item);
        }
    }
    std::ranges::sort(fresh, {}, &nox::client::InventoryItem::pickupOrder);
    for (const auto* item : fresh) {
        if (drag_ && drag_->netCode == item->netCode) {
            continue;
        }
        // Food filed (the potions too) re-picks the bottle slots; no room, no re-pick.
        if (grid_.add(item->netCode, item->typeId) && food(item->typeId)) {
            bottles_.pick(grid_);
        }
        filed_[item->netCode] = item->pickupOrder;
    }
    // The slide, a step a game frame (inventoryDrawAllMB).
    // Resync after a jump (the first frame, frames going back, or over 300 behind) instead of replaying it.
    if (lastFrame_ == 0 || frame < lastFrame_ || frame - lastFrame_ > 300) {
        lastFrame_ = frame;
    }
    for (; lastFrame_ < frame; ++lastFrame_) {
        if (state_ == 1) {
            slide_ += 64;
            if (slide_ > 0) {
                slide_ = 0;
                state_ = 2;
            }
        } else if (state_ == 3) {
            slide_ -= 32;
            if (slide_ < -224) {
                slide_ = -225;
                state_ = 0;
                // Shut, it opens on the tray and the doll again (inventoryDrawAllMB).
                if (journal_) {
                    journalScroll_ = scroll_;
                    scroll_ = trayScroll_;
                }
                journal_ = false;
                stats_ = false;
            }
        }
    }
}

// Our name and rank for the panel's top line.
std::string Inventory::title(const nox::client::World& world, std::uint16_t us) const
{
    // inventoryNameSignInit: "<name> the <experience:ClassLevel>".
    const auto me = world.players().find(us);
    if (me == world.players().end()) {
        return {};
    }
    const std::uint8_t cls = std::min<std::uint8_t>(me->second.playerClass, 2);
    const std::string key = std::string("experience:") + kClassNames[cls] + std::to_string(world.self().level);
    std::string rank = kClassNames[cls];
    std::string format = "%s the %s";
    if (strings_) {
        if (const auto t = strings_->text(key)) {
            rank = nox::strings::toUtf8(*t);
        }
        if (const auto t = strings_->text("GuiInv.c:ElaborateNameFormat")) {
            format = nox::strings::toUtf8(*t);
        }
    }
    // Fill the format's %s / %ls in order: our name first, then the rank.
    std::string out;
    int arg = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] == '%' && i + 1 < format.size()) {
            std::size_t j = i + 1;
            if (format[j] == 'l' && j + 1 < format.size()) {
                ++j;
            }
            if (format[j] == 's' || format[j] == 'S') {
                out += arg++ == 0 ? me->second.name : rank;
                i = j;
                continue;
            }
        }
        out += format[i];
    }
    return out;
}

void Inventory::drawDoll(SDL_Renderer* r, const nox::client::World& world, std::uint16_t us, float x, float y,
                         float s) const
{
    // The doll: a black box, our body in our colours, then what we wear and wield.
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    const SDL_FRect box{x * s, y * s, 200 * s, 200 * s};
    SDL_RenderFillRect(r, &box);
    const auto me = world.players().find(us);
    if (me == world.players().end()) {
        return;
    }
    const auto& p = me->second;
    const bool female = p.female;
    const auto body = playerMaterials(p.colors);
    const bool bald = std::equal(p.colors.begin(), p.colors.begin() + 3, p.colors.begin() + 3); // hair = skin
    const char* base = female ? (bald ? "FemaleBaldBase" : "FemaleBase") : (bald ? "MaleBaldBase" : "MaleBase");
    if (const auto* def = image(base); def && def->image) {
        drawRef(r, *def->image, x, y, s, &body);
    }
    // One worn picture over the doll, in the colours its equip definition and enchantments give it.
    const auto piece = [&](const char* name, std::uint32_t bit, bool armor) {
        const nox::things::ImageDef* def = name && *name ? image(name) : nullptr;
        if (!def || !def->image) {
            return;
        }
        SpriteCache::Materials m{};
        m.fill({255, 255, 255});
        const auto& mods = armor ? p.armorModifiers : p.weaponModifiers;
        const auto it = mods.find(bit);
        const auto* equipDef = armor ? modifiers_.findArmor(nox::things::armorItemName(bit))
                                     : modifiers_.findWeapon(nox::things::weaponItemName(bit));
        applyItemColours(m, equipDef, it != mods.end() ? &it->second : nullptr, modifiers_);
        drawRef(r, *def->image, x, y, s, &m);
    };
    const auto& armorNames = female ? kFemaleArmor : kMaleArmor;
    const auto& weaponNames = female ? kFemaleWeapons : kMaleWeapons;
    // The round and kite shield bits (24, 25): drawn after the other armour and the cloak's top.
    constexpr std::uint32_t kShields = 0x3000000;
    for (std::uint32_t i = 0; i < 26; ++i) {
        const std::uint32_t bit = 1u << i;
        if ((p.armorEquip & bit) && !(bit & kShields)) {
            piece(armorNames[i], bit, true);
        }
    }
    if (p.armorEquip & 0x2) {
        piece("MaleMedievalCloakTop", 0x2, true); // the cloak's top over the rest
    }
    for (std::uint32_t i = 0; i < 26; ++i) {
        const std::uint32_t bit = 1u << i;
        if ((p.armorEquip & bit) && (bit & kShields)) {
            piece(armorNames[i], bit, true);
        }
    }
    // Then the weapons, by weapon class bit.
    for (std::uint32_t i = 0; i < 27; ++i) {
        const std::uint32_t bit = 1u << i;
        if (p.weaponEquip & bit) {
            piece(weaponNames[i], bit, false);
        }
    }
}

void Inventory::draw(SDL_Renderer* r, const nox::client::World& world, std::uint16_t us, int w, int h,
                     SDL_FPoint mouse)
{
    const float s = Hud::scale(w, h);
    const float H = static_cast<float>(h) / s;
    const auto& inv = world.self().inventory;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    // The font size that is `gamePixels` game pixels tall at this scale.
    const auto fontSize = [&](float gamePixels) {
        return font_ ? font_->lineSize(gamePixels, s)
                     : 0;
    };
    const int smallSize = fontSize(8);
    const int textSize = fontSize(10);
    const auto text = [&](const std::string& t, float gx, float gy, SDL_Color c, bool small = true) {
        if (font_ && !t.empty()) {
            font_->draw(t, gx * s, gy * s, small ? smallSize : textSize, c);
        }
    };
    const auto textWidth = [&](const std::string& t, bool small = true) {
        return font_ ? font_->measure(t, small ? smallSize : textSize) / s : 0.0f;
    };
    // Ammo and charges (ours, not the game's blue): white and bold, the font drawn twice a pixel
    // apart over a dark shadow, right-aligned to `right`.
    const auto ammoText = [&](const std::string& t, float right, float gy) {
        if (!font_ || t.empty()) {
            return;
        }
        const float x = (right - textWidth(t)) * s - 1;
        const float y = gy * s;
        font_->draw(t, x + 1, y + 1, smallSize, {0, 0, 0, 200});
        font_->draw(t, x + 2, y + 1, smallSize, {0, 0, 0, 200});
        font_->draw(t, x, y, smallSize, {255, 255, 255, 255});
        font_->draw(t, x + 1, y, smallSize, {255, 255, 255, 255});
    };

    // ---- the current-weapon panel, bottom left (inventoryDrawProc): our weapon, and V to swap
    {
        const float wx = -1;
        const float wy = H - 127;
        // The window at (-1, H - 127), its picture at its own top left (the offset the window is
        // given isn't used by its draw).
        drawNamed(r, isOpen() ? "CurrentWeaponLit" : "CurrentWeapon", wx, wy, s);
        const nox::client::InventoryItem* held = nullptr;
        const nox::client::InventoryItem* quiver = nullptr;
        // The equipped hand weapon, and the equipped quiver (weapon subclass 0x2).
        for (const auto& [nc, item] : inv) {
            const TypeArt* a = art_.get(item.typeId);
            if (item.equipped && a && inHand(*a)) {
                held = &item;
            } else if (item.equipped && a && (a->classes & oc::Weapon) && (a->subclasses & 0x2)) {
                quiver = &item;
            }
        }
        if (held) {
            drawItem(r, held->typeId, held->modifiers, wx + 24 + 26, wy + 51 + 26, s);
        } else if (!(swapPending_ && swapPending_())) {
            // Bare-handed: the fist (GUIFist at the panel's (0x15, 0x32)); none while
            // a swap is under way (g_guiInvPendingAltWeaponNetCode, g_guiInvEquippedWeaponDrawable).
            drawNamed(r, "GUIFist", wx + 0x15, wy + 0x32, s);
        }
        // The charges meters (windows 5 and 6, shown by an equipped item's
        // MSG_REPORT_CHARGES): what's left in the staff in hand, else in the quiver.
        const nox::client::InventoryItem* ammo =
            held && held->charges && held->maxCharges.value_or(0) > 0 ? held : quiver && quiver->charges ? quiver : nullptr;
        std::optional<std::pair<int, int>> charges = testCharges_;
        if (!charges && ammo) {
            charges = {static_cast<int>(*ammo->charges), static_cast<int>(ammo->maxCharges.value_or(0))};
        }
        if (charges) {
            drawChargeBar(r, wx + 0x4F, wy + 0x28, charges->first, charges->second, s);
            // The count in the green bead above (window 6, 15 x 15 at (0x3E, 0x19): its draw puts
            // it white in the small font, centred). Larger and bold here, to read at a glance.
            if (font_) {
                const std::string t = std::to_string(charges->first);
                const int size = textSize;
                // Where the original's digits are (and the bead's middle): its draw centres them
                // at the window's x + 8 (the window at the panel's (0x3E, 0x19)) and draws the
                // small font from its top + 1, its digits' middle about 5.5 down. Ours centred on
                // the digits themselves: their cap height (about 0.7 of the em) above the
                // baseline, not the line's, which holds room for descenders; the bold copy a
                // pixel right counted in the width.
                const float cx = (wx + 0x3E + 8) * s;
                const float cy = (wy + 0x19 + 5.5f) * s;
                const float x = cx - (font_->measure(t, size) + 1) / 2;
                const float y = cy - font_->ascent(size) + 0.7f * static_cast<float>(size) / 2;
                font_->draw(t, x + 1, y + 1, size, {0, 0, 0, 200});
                font_->draw(t, x, y, size, {255, 255, 255, 255});
                font_->draw(t, x + 1, y, size, {255, 255, 255, 255});
            }
        }
        // The swap key (control 0x23, SwapWeapons) in white in the small font, its cell's top
        // left at the window's (0x13, 0x66): in the ring's bottom-left tab.
        if (font_) {
            font_->drawGameText("V", wx + 0x13, wy + 0x66, s, {255, 255, 255, 255});
        }
    }

    // ---- the buffs' icons under the panel (at the top of the screen while it's shut)
    drawBuffs(r, world, s);

    if (state_ == 0) {  // closed: only the weapon panel and the buffs show
        return;
    }
    const float px = 0;
    // The panel's top as it slides (negative while it is above the screen).
    const float py = static_cast<float>(slide_);

    // ---- the tray (guiDrawInventoryTray), clipped to its window
    {
        const float tx = px + 254;
        const float ty = py + 13;
        const float top = std::max(0.0f, ty);
        const SDL_Rect clip{static_cast<int>(tx * s), static_cast<int>(top * s), static_cast<int>(260 * s),
                            static_cast<int>((ty + 150 - top) * s)};
        if (clip.h > 0 && journal_) {
            SDL_SetRenderClipRect(r, &clip);
            drawJournal(r, world, tx, ty, s);
            SDL_SetRenderClipRect(r, nullptr);
        } else if (clip.h > 0) {
            SDL_SetRenderClipRect(r, &clip);
            drawNamed(r, "InventoryTraySpecial", tx, ty, s);
            if (identify_) {
                drawNamed(r, "InventoryTrayIdentifyLit", tx, ty + 0x32, s);
            }
            if (mapShown_) {
                drawNamed(r, "InventoryTrayMapLit", tx, ty + 100, s);
            }
            const std::string gold = std::to_string(world.self().gold);
            text(gold, tx + 43 - textWidth(gold), ty + 36, {255, 255, 0, 255});
            for (int row = 0; row < kRows; ++row) {
                // Only rows at least partly inside the 150-pixel window.
                const float y = ty - static_cast<float>(scroll_) + static_cast<float>(row * kCell);
                if (y <= ty - kCell || y > ty + 150) {
                    continue;
                }
                // The tray's background art cycles through three pictures by row.
                drawNamed(r, "InventoryTray" + std::to_string(row % 3 + 1), tx + 60, y, s);
                for (int col = 0; col < kCols; ++col) {
                    const float x = tx + 60 + static_cast<float>(col * kCell);
                    const auto& c = grid_.at(col, row); // (what's on the cursor is out of it)
                    if (c.items.empty() || !inv.count(c.items.front())) {
                        continue;
                    }
                    const auto& item = inv.at(c.items.front());
                    // Worn out: red under a quarter, yellow under a half (alpha 0x40).
                    const int health = item.health.value_or(0);
                    const int maxHealth = item.maxHealth.value_or(0);
                    if (maxHealth > 0 && item.health && health * 2 < maxHealth) {
                        const bool red = health * 4 < maxHealth;
                        SDL_SetRenderDrawColor(r, 255, red ? 0 : 255, 0, 0x40);
                        const SDL_FRect f{x * s, y * s, kCell * s, kCell * s};
                        SDL_RenderFillRect(r, &f);
                    }
                    const bool worn = std::any_of(c.items.begin(), c.items.end(), [&](std::uint16_t nc) {
                        return inv.count(nc) && inv.at(nc).equipped;
                    });
                    // Worn: the equip ring; the secondary weapon: the quick-item ring (guiDrawInventoryTray).
                    const std::uint16_t secondary = world.self().secondaryWeapon;
                    const bool second = secondary != 0 && std::ranges::find(c.items, secondary) != c.items.end();
                    if (worn || second) {
                        if (const auto* def = image(worn ? "InventoryEquipRing" : "InventoryQuickItemRing"); def && def->image) {
                            if (const Sprite* sp = sprites_.get(*def->image)) {
                                SDL_SetTextureAlphaMod(sp->texture, 0x80);
                                drawRef(r, *def->image, x, y, s);
                                SDL_SetTextureAlphaMod(sp->texture, 255);
                            }
                        }
                    }
                    drawItem(r, item.typeId, item.modifiers, x + 25, y + 25, s);
                    if (c.items.size() > 1) {
                        text(std::to_string(c.items.size()), x + 6, y + 6, {255, 255, 255, 255});
                    }
                    if (item.maxCharges.value_or(0) > 0) {
                        // (The game's is blue: guiDrawInventoryTray. White and bold reads far better.)
                        ammoText(std::to_string(item.charges.value_or(0)), x + 44, y + 6);
                    }
                }
            }
            SDL_SetRenderClipRect(r, nullptr);
        }
    }

    // ---- the doll (or the stats, or identify), the frame over it, our name (inventoryDrawAllMB)
    if (identify_) {
        drawIdentify(r, world, px, py, s);
        drawNamed(r, "InventoryIdentifyBase", px, py, s);
    } else if (stats_) {
        drawStats(r, world, us, px, py, s);
        drawNamed(r, "InventoryIdentifyBase", px, py, s);
    } else {
        drawDoll(r, world, us, px + 11, py + 15, s);
        drawNamed(r, "InventoryBase", px, py, s);
    }
    // ---- the secondary weapon's box: the secondary in its middle, the swap key under
    if (!identify_ && !stats_) {
        const float bx = px + kSecondaryBox.x;
        const float by = py + kSecondaryBox.y;
        const std::uint16_t secondary = world.self().secondaryWeapon;
        if (secondary != 0 && inv.count(secondary)) {
            const auto& item = inv.at(secondary);
            drawItem(r, item.typeId, item.modifiers, bx + 25, by + 25, s);
        }
        text("V", bx + 0x16, by + 0x29 - 8, {255, 255, 255, 255});
    }
    if (font_ && !identify_) {
        const auto lines = font_->wrap(title(world, us), textSize, 196 * s);
        float ly = py + 17;
        for (const auto& line : lines) {
            text(line, px + 13, ly, {255, 255, 255, 255}, false);
            ly += 10;
        }
    }

    // ---- the buttons; the stats, journal and close pictures cover the
    // whole panel and are drawn at its corner.
    const float mx = mouse.x / s;
    const float my = mouse.y / s - py;
    const auto lit = [&](float x, float y, float bw, float bh) {
        return mx >= x && my >= y && mx < x + bw && my < y + bh;
    };
    drawNamed(r, lit(522, 2, 20, 25) ? "InventoryUpButtonLit" : "InventoryUpButton", px + 522, py + 2, s);
    drawNamed(r, lit(522, 148, 20, 25) ? "InventoryDownButtonLit" : "InventoryDownButton", px + 522, py + 148, s);
    // The slider's knob travels 75 pixels (its 91-pixel track less the knob's 16) over the scroll range.
    const int most = std::max(1, scrollMax());
    drawNamed(r, "InventorySliderButton", px + 524,
              py + 42 + static_cast<float>(std::min(scroll_, most)) * (91 - 16) / static_cast<float>(most), s);
    // The page buttons show the page they go to.
    if (stats_) {
        if (lit(5, 186, 34, 34)) {
            drawNamed(r, "InventoryDollButtonLit", px, py, s);
        }
    } else {
        drawNamed(r, lit(5, 186, 34, 34) ? "InventoryStatsButtonLit" : "InventoryStatsButton", px, py, s);
    }
    if (journal_) {
        drawNamed(r, lit(243, 170, 34, 34) ? "InventoryInventoryButtonLit" : "InventoryInventoryButton", px, py, s);
    } else if (lit(243, 170, 34, 34)) {
        drawNamed(r, "InventoryJournalButtonLit", px, py, s);
    }
    if (lit(547, 2, 16, 16)) {
        drawNamed(r, "InventoryCloseButtonLit", px, py, s);
    }

    // ---- what's being dragged, on the cursor
    if (drag_ && inv.count(drag_->netCode)) {
        const auto& item = inv.at(drag_->netCode);
        drawItem(r, item.typeId, item.modifiers, mouse.x / s, mouse.y / s, s);
    }
}

Inventory::Hover Inventory::hoverAt(float x, float y, int w, int h, const nox::client::World& world) const
{
    Hover out;
    const float s = Hud::scale(w, h);
    const float gx = x / s;
    const float gy = y / s;
    const float H = static_cast<float>(h) / s;
    // The weapon panel (wndCreateInventoryMB: at (-1, H - 127)): its button (5, 11) 28 x 29, and the weapon's
    // window (0x18, 0x33) 0x35 x 0x35.
    if (onWeaponButton(gx, gy, H)) {
        out.inventoryButton = true;
        out.inventoryOpen = state_ == 2;
        return out;
    }
    // The buffs' row under the panel (inventroryOnHovewerSub): the enchantment's spell title, or the item
    // power's name.
    if (const float rowY = static_cast<float>(slide_) + 0xE0 + buffsDown(s); gy >= rowY && gy < rowY + 0x28) {
        if (const auto b = buffAt(world, gx)) {
            if (b->first != 0) {
                if (const auto* spell = things_.findSpell(b->first)) {
                    out.text = text(spell->title);
                }
            } else {
                for (const auto& p : kItemPowers) {
                    if (p.bit == b->second) {
                        out.text = text(p.name);
                    }
                }
            }
            if (!out.text.empty()) {
                return out;
            }
        }
    }
    // A quest's ankh (inventroryOnHovewerSub's rect x - 0x18, top + 0xDB to x + 0x24, top + 0x103,
    // x where the ankh's slot starts): "thing.db:AnkhGUI".
    if (world.game().gameFlags & 0x1000u) {
        const auto* self = world.drawable(world.self().netCode);
        const std::uint32_t buffs = self ? self->buffs : 0;
        // The ankh's slot comes after the buffs: one 35-pixel step per enchantment and item power shown.
        float ax = 10;
        for (std::uint32_t i = 0; i < 30; ++i) {
            if (buffs & (1u << i)) {
                ax += 0x23;
            }
        }
        for (const auto& p : kItemPowers) {
            if (world.self().itemBuffs & p.bit) {
                ax += 0x23;
            }
        }
        const float top = static_cast<float>(slide_);
        if (gx >= ax - 0x18 && gx <= ax + 0x24 && gy >= top + 0xDB && gy <= top + 0x103) { // (pointInRect: edges in)
            out.text = text("thing.db:AnkhGUI");
            return out;
        }
        // The shared keys' icon (its rect 0x1E either side of x + 0x48 and 0x14 of
        // top + 0xF4 after the ankh; x + 0x42, top + 0xEF without it), while they're shared:
        // "GeneralPrint:TooltipKeyIcon".
        const nox::things::ImageDef* ankh = image("ExtraLives");
        const bool ankhDrawn = ankh && ankh->animation && !ankh->animation->frames.empty();
        const float kx = ax + (ankhDrawn ? 0x48 : 0x42);
        const float ky = top + (ankhDrawn ? 0xF4 : 0xEF);
        if (world.game().sharedKeys && gx >= kx - 0x1E && gx <= kx + 0x1E && gy >= ky - 0x14 && gy <= ky + 0x14) { // (edges in)
            out.text = text("GeneralPrint:TooltipKeyIcon");
            return out;
        }
    }
    // The weapon panel's weapon window (0x35 square).
    if (gx >= 23 && gy >= H - 76 && gx < 23 + 0x35 && gy < H - 76 + 0x35) {
        out.weaponPanel = true;
        for (const auto& [nc, item] : world.self().inventory) {
            const TypeArt* a = art_.get(item.typeId);
            if (item.equipped && a && inHand(*a)) {
                out.weapon = nc;
            } else if (item.equipped && nox::things::weaponBitOf(world.thingName(item.typeId)) == 2u) { // (a quiver, a pack's too)
                out.quiver = nc;
            }
        }
        return out;
    }
    if (state_ != 2 || !contains(x, y, w, h)) {  // the rest needs the panel fully open (state 2)
        return out;
    }
    const float px = gx;
    const float py = gy - static_cast<float>(slide_);
    const auto button = [&](float bx, float by, float bw, float bh) {
        return px >= bx && py >= by && px < bx + bw && py < by + bh;
    };
    // The page and close buttons.
    if (button(243, 170, 34, 34)) {
        out.text = text(journal_ ? "guiinv.c:InventoryModeTT" : "guiinv.c:JournalModeTT");
        return out;
    }
    if (button(5, 186, 34, 34)) {
        out.text = text(stats_ ? "guiinv.c:PaperDollModeTT" : "guiinv.c:StatsModeTT");
        return out;
    }
    if (button(547, 2, 16, 16)) {
        out.text = text("guiinv.c:CloseInventoryTT");
        return out;
    }
    // The special column's squares: gold, the magnifier, the map.
    if (!journal_ && in(kSpecial, px, py)) {
        static constexpr std::array<const char*, 3> kSpecials{"Gold", "Identify", "AutoMap"};
        const int row = (static_cast<int>(py) - 13) / kCell;
        if (row >= 0 && row < 3) {
            if (const auto* t = things_.findThing(kSpecials[static_cast<std::size_t>(row)])) {
                out.text = prettyName(*t, strings_);
            }
        }
        return out;
    }
    if (journal_ && in(kTray, px, py)) {
        return out;
    }
    if (in(kDoll, px, py)) {
        if (!stats_ && !identify_) {
            out.item = wornAt(world, px, py);
        }
    } else if (const auto cell = cellAt(px, py)) {
        const auto& c = grid_.cell(*cell);
        if (!c.items.empty()) {
            out.item = c.items.front();
        }
    }
    return out;
}

// The tray cell under panel point (px, py), scrolled; cells count column by column (InventoryGrid::index).
std::optional<int> Inventory::cellAt(float px, float py) const
{
    if (!in(kTray, px, py)) {
        return std::nullopt;
    }
    const int col = (static_cast<int>(px) - 314) / kCell;
    const int row = (static_cast<int>(py) - 13 + scroll_) / kCell;
    if (col < 0 || col >= kCols || row < 0 || row >= kRows) {
        return std::nullopt;
    }
    return col * kRows + row;
}

std::optional<std::uint16_t> Inventory::wornAt(const nox::client::World& world, float px, float py) const
{
    // The doll slot under the point; the right arm is the shield's if one's worn.
    const float dx = px - 11;
    const float dy = py - 15;
    const auto& inv = world.self().inventory;
    const auto wornIn = [&](int slot) -> std::optional<std::uint16_t> {
        for (const auto& [nc, item] : inv) {
            const TypeArt* a = art_.get(item.typeId);
            if (item.equipped && a && dollSlot(*a) == slot) {
                return nc;
            }
        }
        return std::nullopt;
    };
    // Earlier slots win where they overlap. Slot 6 is the right arm: the shield if one is worn, else the
    // arm piece. A miss on the small quiver slot 0 goes on to the slots under it; a miss anywhere else ends it.
    for (int i = 0; i < static_cast<int>(kDollSlots.size()); ++i) {
        if (!in(kDollSlots[static_cast<std::size_t>(i)], dx, dy)) {
            continue;
        }
        if (i == 6) {
            if (const auto shield = wornIn(8)) {
                return shield;
            }
            return wornIn(5);
        }
        if (const auto nc = wornIn(i)) {
            return nc;
        }
        if (i != 0) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool Inventory::contains(float x, float y, int w, int h) const
{
    const float s = Hud::scale(w, h);
    const float gx = x / s;
    const float gy = y / s;
    const float H = static_cast<float>(h) / s;
    // The weapon panel's button.
    if (onWeaponButton(gx, gy, H)) {
        return true;
    }
    if (state_ == 0) {
        return false;
    }
    const float px = gx;
    const float py = gy - static_cast<float>(slide_);
    if (px < 0 || py < 0 || px >= kPanelW || py >= kPanelH) {
        return false;
    }
    return !in(kOutside1, px, py) && !in(kOutside2, px, py);
}

bool Inventory::mouseDown(float x, float y, int w, int h, const nox::client::World& world, std::uint32_t nowMs)
{
    const float s = Hud::scale(w, h);
    const float gx = x / s;
    const float gy = y / s;
    const float H = static_cast<float>(h) / s;
    if (onWeaponButton(gx, gy, H)) {
        toggle(); // the weapon panel's button, (5, 11) 28 x 29
        return true;
    }
    if (state_ != 2 || !contains(x, y, w, h)) {
        return false;
    }
    const float px = gx;
    const float py = gy - static_cast<float>(slide_);
    const auto button = [&](float bx, float by, float bw, float bh) {
        return px >= bx && py >= by && px < bx + bw && py < by + bh;
    };
    // The buttons: up snaps back to a row, down a row on.
    if (button(522, 2, 20, 25)) {
        scrollUp();
        sound(0x2FE); // ButtonPress
        return true;
    }
    if (button(522, 148, 20, 25)) {
        scrollDown();
        sound(0x2FE);
        return true;
    }
    if (button(547, 2, 16, 16)) {
        toggle();
        return true;
    }
    // The slider's track: jump to the point pressed.
    if (button(524, 42, 16, 91)) {
        scroll_ = std::clamp(static_cast<int>((py - 42) * static_cast<float>(scrollMax()) / 91), 0, scrollMax());
        return true;
    }
    // The page buttons (not while identifying): the journal for the tray, keeping
    // each one's scroll, and the stats for the doll.
    if (button(243, 170, 34, 34) && !identify_) {
        if (journal_) {
            journalScroll_ = scroll_;
            scroll_ = trayScroll_;
        } else {
            trayScroll_ = scroll_;
            scroll_ = std::min(journalScroll_, std::max(0, journalHeight_ - 150));
        }
        journal_ = !journal_;
        return true;
    }
    if (button(5, 186, 34, 34) && !identify_) {
        stats_ = !stats_;
        return true;
    }
    if (identify_) {
        // The inventory window in mode 5: a press on an item shows it.
        if (const auto cell = cellAt(px, py); cell && !journal_) {
            const auto& c = grid_.cell(*cell);
            if (!c.items.empty()) {
                identified_ = c.items.front();
            }
        }
        return true;
    }
    if (journal_ && in(kTray, px, py)) {
        return true; // (the journal's page has no cells)
    }
    if (in(kSpecial, px, py)) {
        return true; // (the magnifier works on release)
    }
    // In a shop's sell or repair mode a press on a cell asks the shop about it instead.
    if (shopMode_ != ShopMode::None) {
        if (const auto cell = cellAt(px, py)) {
            const auto& c = grid_.cell(*cell);
            if (!c.items.empty() && shopPick_) {
                shopPick_(c.items.front());
            }
        }
        return true;
    }
    // Take an item: from the doll, or from a cell.
    const auto& inv = world.self().inventory;
    if (in(kDoll, px, py)) {
        if (const auto nc = wornAt(world, px, py)) {
            drag_ = Drag{*nc, inv.at(*nc).typeId, std::nullopt, {px, py}, nowMs};
            sound(0x317); // InventoryPickup
        }
        return true;
    }
    if (const auto cell = cellAt(px, py)) {
        const auto& c = grid_.cell(*cell);
        if (!c.items.empty() && inv.count(c.items.front())) {
            // cliInventorySpriteUpd: its first taken out; the
            // secondary weapon's cell so emptied, the secondary is none, the server told
            // (clientSetAltWeapon(0): an original bug).
            const std::uint16_t typeId = c.typeId;
            const auto nc = grid_.take(*cell);
            drag_ = Drag{*nc, typeId, *cell, {px, py}, nowMs};
            if (*nc == world.self().secondaryWeapon && clearSecondary_) {
                clearSecondary_();
            }
            sound(0x317); // InventoryPickup
        }
    }
    return true;
}

bool Inventory::mouseUp(float x, float y, int w, int h, const nox::client::World& world, std::uint32_t nowMs,
                        const std::function<SDL_Point(float, float)>& toWorld, const Actions& actions)
{
    if (!drag_) {
        // The magnifier (the special column's second square, the inventory window's case 7): identify mode
        // on, or off again.
        const float s0 = Hud::scale(w, h);
        const float px0 = x / s0;
        const float py0 = y / s0 - static_cast<float>(slide_);
        if (state_ == 2 && !journal_ && in(kSpecial, px0, py0) && (static_cast<int>(py0) - 13) / kCell == 1) {
            if (identify_) {
                stopIdentify();
            } else {
                identify_ = true;
                identified_.reset();
            }
            return true;
        }
        return state_ == 2 && contains(x, y, w, h);
    }
    const Drag d = *drag_;
    drag_.reset();
    const auto& inv = world.self().inventory;
    if (!inv.count(d.netCode)) {
        return true;
    }
    const auto& item = inv.at(d.netCode);
    const TypeArt* a = art_.get(item.typeId);
    const bool wearable = a && (a->classes & (oc::Weapon | oc::Armor | oc::Wand));
    const float s = Hud::scale(w, h);
    const float px = x / s;
    const float py = y / s - static_cast<float>(slide_);

    // Outside the panel: onto the ground at the cursor (CliNet_SendDropDraggedItem); a stack asks
    // how many first (the amount box, DropLabel, its most the cell's count with the dragged one).
    // Either way it's back in its cell till the server says it's dropped (the inventory window's put-back);
    // the box's answer drops that many of the cell holding it then, from its first.
    if (!contains(x, y, w, h)) {
        const SDL_Point at = toWorld(x, y);
        const std::size_t rest = d.fromCell ? grid_.cell(*d.fromCell).items.size() : 0;
        putBack(d, false);
        if (rest != 0 && askAmount_) {
            const auto drop = actions.drop;
            const std::uint16_t netCode = d.netCode;
            askAmount_(text("guiinv.c:DropLabel"), item.typeId, item.modifiers, static_cast<int>(rest + 1),
                       SDL_FPoint{x, y}, [this, drop, netCode, at](int n) {
                           const auto cell = grid_.find(netCode);
                           if (!cell) {
                               return;
                           }
                           const std::vector<std::uint16_t> stack = grid_.cell(*cell).items;
                           for (int i = 0; i < n && i < static_cast<int>(stack.size()); ++i) {
                               if (drop) {
                                   drop(stack[static_cast<std::size_t>(i)], at.x, at.y);
                               }
                           }
                       });
            return true;
        }
        if (actions.drop) {
            actions.drop(d.netCode, at.x, at.y);
        }
        return true;
    }
    // A quick click where it was taken: use it, or put it on / take it off (the inventory window's case 7).
    const float dx = d.pressed.x - px;
    const float dy = d.pressed.y - py;
    if (nowMs - d.atMs < 333 && dx * dx + dy * dy < 100) {  // a third of a second, within 10 pixels
        putBack(d, false); // (back at its stack's end)
        if (!wearable) {
            if (actions.use) {
                actions.use(d.netCode);
            }
        } else if (item.equipped) {
            if (actions.unequip) {
                actions.unequip(d.netCode);
            }
        } else if (actions.equip) {
            actions.equip(d.netCode);
        }
        return true;
    }
    // Onto the secondary weapon's box (its window's case 6): a weapon or wand from the tray (not a
    // worn one) becomes the secondary, a quiver only with a bow or crossbow worn; the worn weapon
    // put away there comes off and becomes the secondary, or swaps with it.
    const bool weaponLike = a && (a->classes & (oc::Weapon | oc::Wand));
    if (!identify_ && !stats_ && in(kSecondaryBox, px, py)) {
        if (d.fromCell) {
            putBack(d, false); // (back in its cell first, whatever it is)
            if (weaponLike && !item.equipped && actions.secondary) {
                bool make = true;
                // A quiver (weapon bit 2) only while a bow or crossbow (bits 4 / 8) is worn.
                if (actions.weaponBit && actions.weaponBit(item.typeId) == 2) {
                    make = std::ranges::any_of(inv, [&](const auto& e) {
                        return e.second.equipped && (actions.weaponBit(e.second.typeId) & 0xC);
                    });
                }
                if (make) {
                    actions.secondary(d.netCode);
                }
            }
        } else if (weaponLike && actions.putAwayWeapon) {
            actions.putAwayWeapon(d.netCode);
        }
        return true;
    }
    // Onto the doll: wear it (and back in its cell).
    if (in(kDoll, px, py)) {
        putBack(d, false);
        if (d.fromCell && wearable && !item.equipped && actions.equip) {
            actions.equip(d.netCode);
        }
        return true;
    }
    // Into the tray: a worn piece taken from the doll comes off, unless the cell holds one to
    // wear in its place (a weapon or wand for a weapon or wand, armour of its kind): that goes on.
    // Otherwise, when it was its cell's last, it moves there if the cell is empty or
    // holds a stack of its type with room, and the grid closes up;
    // else it's back at the end of its own cell's stack.
    if (const auto cell = cellAt(px, py)) {
        if (!d.fromCell) {
            const auto& there = grid_.cell(*cell).items;
            const TypeArt* ta = !there.empty() && inv.count(there.front()) ? art_.get(inv.at(there.front()).typeId) : nullptr;
            const bool swaps = ta && a &&
                               (((ta->classes & oc::Armor) && (a->classes & oc::Armor) && ta->subclasses == a->subclasses) ||
                                ((ta->classes & (oc::Weapon | oc::Wand)) && (a->classes & (oc::Weapon | oc::Wand))));
            if (swaps && actions.equip) {
                actions.equip(there.front());
            } else if (actions.unequip) {
                actions.unequip(d.netCode);
            }
            return true;
        }
        if (grid_.drop(*d.fromCell, *cell, d.netCode, d.typeId)) {
            sound(0x318); // InventoryDrop
        }
        return true;
    }
    putBack(d, false);
    return true;
}

bool Inventory::onSecondaryBox(float x, float y, int w, int h) const
{
    if (state_ != 2 || identify_ || stats_ || !contains(x, y, w, h)) {
        return false;
    }
    const float s = Hud::scale(w, h);
    return in(kSecondaryBox, x / s, y / s - static_cast<float>(slide_));
}

bool Inventory::wheel(float x, float y, int w, int h, int dir)
{
    if (state_ != 2 || !contains(x, y, w, h)) {
        return false;
    }
    // Wheel up / down press the arrows (the inventory window's cases 0x13 / 0x14).
    if (dir > 0) {
        scrollUp();
    } else {
        scrollDown();
    }
    return true;
}

void Inventory::scrollUp()
{
    const int to = scroll_ - 25;
    scroll_ = to < 0 ? 0 : to - to % kCell;
}

void Inventory::scrollDown()
{
    const int to = scroll_ + kCell;
    scroll_ = std::min(scrollMax(), to - to % kCell);
}

// ---------------------------------------------------------------------------------- pages


std::string Inventory::text(const std::string& id) const
{
    if (strings_) {
        if (const auto t = strings_->text(id)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return {};
}

int Inventory::weightOf(std::uint16_t typeId) const
{
    if (typeId == 0 || typeId > things_.things.size()) {
        return 0;
    }
    const auto w = things_.things[typeId - 1u].attribute("WEIGHT");
    return w ? std::atoi(std::string(*w).c_str()) : 0;
}

std::array<float, 4> Inventory::classMax(const nox::client::World& world, int cls) const
{
    // The class's Max* values times its multipliers (health, mana, speed, strength).
    std::array<float, 4> out{1, 1, 1, 1};
    if (!gameData_) {
        return out;
    }
    const std::string c = kClassNames[static_cast<std::size_t>(std::clamp(cls, 0, 2))];
    const auto& m = world.self().statMultipliers; // health, mana, strength, speed
    out[0] = nox::things::gameNumber(gameData_, c + "MaxHealth", gameFlags_) * m[0];
    out[1] = nox::things::gameNumber(gameData_, c + "MaxMana", gameFlags_) * m[1];
    out[2] = nox::things::gameNumber(gameData_, c + "MaxSpeed", gameFlags_) * m[3];
    out[3] = nox::things::gameNumber(gameData_, c + "MaxStrength", gameFlags_) * m[2];
    for (auto& v : out) {
        v = std::max(v, 1.0f);
    }
    return out;
}

std::optional<std::pair<std::uint32_t, std::uint8_t>> Inventory::buffAt(const nox::client::World& world, float gx) const
{
    // inventroryOnHovewerSub: the slot under the mouse, counted from x 40 in steps of 35.
    int index = 0;
    for (int t = 0x28; t <= static_cast<int>(gx); t += 0x23) {
        ++index;
    }
    const auto* self = world.drawable(world.self().netCode);
    const std::uint32_t buffs = self ? self->buffs : 0;
    // Count down the shown enchantments (bits 0-29), then the item powers, to the index'th icon.
    for (std::uint32_t i = 0; i < 30; ++i) {
        if ((buffs & (1u << i)) && index-- == 0) {
            return std::pair{kEnchantSpells[i], std::uint8_t{0}};
        }
    }
    for (const auto& p : kItemPowers) {
        if ((world.self().itemBuffs & p.bit) && index-- == 0) {
            return std::pair{std::uint32_t{0}, p.bit};
        }
    }
    return std::nullopt;
}

void Inventory::drawBuffs(SDL_Renderer* r, const nox::client::World& world, float s)
{
    // inventoryDrawAllMB: from x 10, y 0xEA below the panel's top, 35 apart: the enchantments' spells
    // (not the crown's or sneak's), then the item powers.
    const auto* self = world.drawable(world.self().netCode);
    const std::uint32_t buffs = self ? self->buffs : 0;
    float x = 10;
    const float y = static_cast<float>(slide_) + 0xEA + buffsDown(s); // (noxcpp: below a mode's reserved box)
    for (std::uint32_t i = 0; i < 30; ++i) {
        if (!(buffs & (1u << i))) {
            continue;
        }
        if (const auto* spell = kEnchantSpells[i] ? things_.findSpell(kEnchantSpells[i]) : nullptr) {
            drawRef(r, spell->icon, x, y, s);
        }
        x += 0x23;
    }
    for (const auto& p : kItemPowers) {
        if (world.self().itemBuffs & p.bit) {
            drawNamed(r, p.icon, x, y, s);
            x += 0x23;
        }
    }
    // In a quest (game flag 0x1000), the ExtraLives ankh after them (its frame the game frame
    // modulo its count, at x - 0x34, top + 0xBA), with "X <lives>" in the small font, colour
    // 255,255,128, at x + 0x1A, top + 0xF8.
    if (world.game().gameFlags & 0x1000u) {
        const nox::things::ImageDef* ankh = image("ExtraLives");
        const float top = static_cast<float>(slide_);
        float keyX = x + 2; // (without the ankh: its draw's x and y not stepped)
        float keyY = top + 0xB5;
        if (ankh && ankh->animation && !ankh->animation->frames.empty()) {
            const auto& frames = ankh->animation->frames;
            drawRef(r, frames[lastFrame_ % frames.size()], x - 0x34, top + 0xBA, s);
            if (font_) {
                const int small = font_->lineSize(8, s);
                font_->draw("X " + std::to_string(world.self().questLives), (x + 0x1A) * s, (top + 0xF8) * s, small,
                            {255, 255, 128, 255});
            }
            keyX = x + 8;
            keyY = top + 0xBA;
        }
        // While the keys are shared (MSG_GAUNTLET 0x18's state), the SharedKeyMode
        // icon after the ankh (inventoryDrawAllMB: x + 0x42 and y + 5 on from the ankh's, less 0x40 and 0x3A).
        if (world.game().sharedKeys) {
            drawNamed(r, "SharedKeyMode", keyX, keyY, s);
        }
    }
}

void Inventory::drawJournal(SDL_Renderer* r, const nox::client::World& world, float x, float y, float s)
{
    // guiDrawJournal: black, then the entries oldest first, "Journal:<label>"
    // after the type's word, wrapped at 240 from x + 10, scrolled.
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    const SDL_FRect box{x * s, y * s, 0x104 * s, 0x96 * s};
    SDL_RenderFillRect(r, &box);
    if (!font_) {
        return;
    }
    const int size = font_->lineSize(10, s);
    float ly = y - static_cast<float>(scroll_);
    int height = 0;
    for (const auto& e : world.game().journal) {
        std::string label;
        SDL_Color c{0, 255, 0, 255};
        // Entry types: 1 plain (white), 2 quest, 4 completed, 8 hint; anything else green.
        switch (e.type) {
        case 1:
            c = {255, 255, 255, 255};
            break;
        case 2:
            label = text("Journal:QuestLabel");
            c = {255, 0, 0, 255};
            break;
        case 4:
            label = text("Journal:CompletedLabel");
            c = {0x73, 0x73, 0x73, 255};
            break;
        case 8:
            label = text("Journal:HintLabel");
            c = {255, 255, 0, 255};
            break;
        default:
            break;
        }
        const std::string line = label + " " + text("Journal:" + e.entry);
        const auto lines = font_->wrap(line, size, 0xF0 * s);
        const float h = static_cast<float>(lines.size()) * 10;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const float yy = ly + static_cast<float>(i) * 10;
            if (yy + 10 > y && yy < y + 0x96) {
                font_->draw(lines[i], (x + 10) * s, yy * s, size, c);
            }
        }
        ly += h + 10; // (a line between entries)
        height += static_cast<int>(h) + 10;
    }
    journalHeight_ = height;
}

void Inventory::drawStats(SDL_Renderer* r, const nox::client::World& world, std::uint16_t us, float x, float y,
                          float s)
{
    // client_makePlayerStatsDlg.
    const auto me = world.players().find(us);
    if (me == world.players().end() || !font_) {
        return;
    }
    const auto& self = world.self();
    const int cls = me->second.playerClass;
    // The class's most of each stat (0 health, 1 mana, 2 speed, 3 strength), which the bars scale to.
    const auto most = classMax(world, cls);
    const auto warrior = classMax(world, 0);
    const auto fill = [&](float fx, float fy, float fw, float fh, SDL_Color c) {
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        const SDL_FRect rect{fx * s, fy * s, std::max(0.0f, fw) * s, fh * s};
        SDL_RenderFillRect(r, &rect);
    };
    const int big = font_->lineSize(10, s);
    const int small = font_->lineSize(8, s);
    const auto say = [&](const std::string& t, float tx, float ty, bool isSmall, SDL_Color c = {255, 255, 255, 255}) {
        font_->draw(t, tx * s, ty * s, isSmall ? small : big, c);
    };
    const auto width = [&](const std::string& t, bool isSmall) { return font_->measure(t, isSmall ? small : big) / s; };
    constexpr float fh = 10;
    constexpr float smallOff = 1; // (the default font less the small one, halved)
    fill(x + 0xB, y + 0xF, 200, 200, {0, 0, 0, 255});
    const float a3 = x + 0xD;
    float yy = y + 0x12 + fh * 2;
    say(format(text("GuiInv.c:StatsLevel"), {std::to_string(self.level)}), a3, yy, false);
    yy += 1 + fh;
    if (gameFlags_ & 0x800u) {
        // A solo game: the experience and the next level's, gamedata.bin's XPTable (chopped).
        const int next = static_cast<int>(nox::things::gameNumber(gameData_, "XPTable", gameFlags_, self.level + 1u));
        say(format(text("GuiInv.c:StatsEXP"), {std::to_string(self.experience), std::to_string(next)}), a3, yy, false);
    }
    yy += fh * 2 + 2;
    const std::string minMax = text("GuiInv.c:MinMaxFormat");
    // One stat row: its label, a 0x5A-wide bar scaled to the class's most (the maximum in a lighter colour
    // behind the current value), "max / most" right-aligned and the current value at the bar's left.
    const auto bar = [&](const char* label, SDL_Color bg, std::optional<SDL_Color> maxColour, SDL_Color cur, float maxVal,
                         float curVal, float statMax, const std::string& range, const std::string& now) {
        say(text(label), a3, yy, false);
        fill(x + 0x49, yy, 0x5A, fh, bg);
        if (maxColour) {
            fill(x + 0x49, yy, std::floor(maxVal * 0x5A / statMax), fh, *maxColour);
        }
        fill(x + 0x49, yy, std::floor(curVal * 0x5A / statMax), fh, cur);
        say(range, a3 - width(range, true) + 0xC1, yy + smallOff, true);
        say(now, x + 0x3A, yy + smallOff, true);
        yy += 1 + fh;
    };
    bar("GuiInv.c:StatsHealth", {100, 0, 0, 255}, SDL_Color{255, 128, 128, 255}, {255, 0, 0, 255}, self.maxHealth,
        self.health, most[0],
        format(minMax, {std::to_string(self.maxHealth), std::to_string(std::lround(most[0]))}),
        std::to_string(self.health));
    if (cls != 0 || multiclass_) {
        // (noxcpp's multiclass: a conjurer's mana, whatever the class)
        const float manaMost = multiclass_ ? classMax(world, 2)[1] : most[1];
        bar("GuiInv.c:StatsMana", {0, 0, 0x8C, 255}, SDL_Color{0, 0xA0, 0xFF, 255}, {0, 0, 0xFF, 255}, self.maxMana,
            self.mana, manaMost, format(minMax, {std::to_string(self.maxMana), std::to_string(std::lround(manaMost))}),
            std::to_string(self.mana));
    }
    // (noxcpp's multiclass: a warrior's strength, whatever the class)
    const float strengthMost = multiclass_ ? classMax(world, 0)[3] : most[3];
    bar("GuiInv.c:StatsStrength", {0x73, 0x73, 0x73, 255}, std::nullopt, {0xD4, 0xD4, 0xD4, 255}, 0, self.strength,
        strengthMost, format(minMax, {std::to_string(self.strength), std::to_string(std::lround(strengthMost))}),
        std::to_string(self.strength));
    // Speed as a percentage of a warrior's most (the haste and run extras not shown).
    const long speedNow = std::lround(self.speed * 100.0f / warrior[2] + 0.5f);
    const long speedMost = std::lround(most[2] * 100.0f / warrior[2]);
    bar("GuiInv.c:StatsSpeed", {0xF0, 0xB4, 0x2A, 255}, std::nullopt, {255, 255, 0, 255}, 0, self.speed, most[2],
        format(minMax, {std::to_string(speedNow), std::to_string(speedMost)}),
        std::to_string(speedNow));
    yy += fh * 2 + 2 - (1 + fh);
    // Armour (the absorbed fraction in thousandths) and what's carried.
    const std::string armorLabel = text("GuiInv.c:StatsArmorLabel");
    const float labelW = width(armorLabel, false);
    say(armorLabel, a3, yy, false);
    say(format(minMax, {std::to_string(std::lround(self.armorValue * 1000 + 0.5f)), "1000"}), a3 + labelW + 5, yy, false);
    yy += 1 + fh;
    // What's carried: each cell's stack at its type's WEIGHT.
    int carried = 0;
    for (int i = 0; i < kCols * kRows; ++i) {
        const Cell& c = grid_.cell(i);
        if (!c.items.empty()) {
            const auto it = self.inventory.find(c.items.front());
            if (it != self.inventory.end()) {
                carried += weightOf(it->second.typeId) * static_cast<int>(c.items.size());
            }
        }
    }
    const std::string weightLabel = text("GuiInv.c:DollWeight");
    say(weightLabel, a3 + labelW - width(weightLabel, false), yy, false);
    const SDL_Color over = carried > self.carryCapacity ? SDL_Color{255, 0, 0, 255} : SDL_Color{255, 255, 255, 255};
    say(format(minMax, {std::to_string(carried), std::to_string(self.carryCapacity)}), a3 + labelW + 5, yy, false, over);
}

void Inventory::drawIdentify(SDL_Renderer* r, const nox::client::World& world, float x, float y, float s)
{
    // The identify page over identify.wnd (at the panel's (0x33, 0xF)): the title, the item's picture,
    // and the list of what it is.
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    const SDL_FRect box{(x + 0xB) * s, (y + 0xF) * s, 200 * s, 200 * s};
    SDL_RenderFillRect(r, &box);
    if (!font_) {
        return;
    }
    const int size = font_->lineSize(10, s);
    const float wx = x + 0x33;
    const float wy = y + 0xF;
    const auto& self = world.self();
    const auto it = identified_ ? self.inventory.find(*identified_) : self.inventory.end();
    // Nothing chosen yet: how identifying works.
    if (it == self.inventory.end()) {
        const auto lines = font_->wrap(text("thing.db:IdentifyDescription"), size, 140 * s);
        float ly = wy;
        for (const auto& l : lines) {
            font_->draw(l, (wx + 15) * s, ly * s, size, {255, 255, 255, 255});
            ly += 10;
        }
        return;
    }
    const auto& item = it->second;
    const TypeArt* a = art_.get(item.typeId);
    const nox::things::ThingDef* t =
        item.typeId >= 1 && item.typeId <= things_.things.size() ? &things_.things[item.typeId - 1u] : nullptr;
    // The title: "Item: <its name>"; its picture at the window's left.
    const std::string name = names_ ? names_->name(*identified_, item.typeId, item.modifiers, world, {}) : std::string();
    float ly = wy;
    for (const auto& l : font_->wrap(text("GuiInv.c:IdentifyItem") + " " + name, size, 140 * s)) {
        font_->draw(l, (wx + 15) * s, ly * s, size, {255, 255, 255, 255});
        ly += 10;
    }
    drawItem(r, item.typeId, item.modifiers, x + 0xB + 25, y + 0xF + 25, s);

    // The description, a string per line (an empty one is a blank line), wrapped at the end.
    std::vector<std::string> lines;
    // Modifier slot 0 is the effectiveness (the damage or armour multiplier); 2 and 3 are the enchantments.
    const auto mod = [&](std::size_t i) { return modifiers_.modifier(item.modifiers[i]); };
    const auto effect = [&](std::size_t i, std::string_view name, float otherwise) {
        if (const auto* m = mod(i)) {
            if (const auto e = m->effects.find(name); e != m->effects.end()) {
                return e->second;
            }
        }
        return otherwise;
    };
    // Durability (in a network game by how worn it is).
    const int maxHealth = item.maxHealth.value_or(0);
    const int health = item.health.value_or(maxHealth);
    const char* wear = maxHealth == 0                   ? "GuiInv.c:IdentifyDurabilityIndestructable"
                       : health == maxHealth            ? "GuiInv.c:IdentifyDurabilityNoDamage"
                       : health >= maxHealth * 0.5      ? "GuiInv.c:IdentifyDurabilitySlight"
                       : health < maxHealth * 0.25      ? "GuiInv.c:IdentifyDurabilitySevere"
                                                        : "GuiInv.c:IdentifyDurabilityModerate";
    lines.push_back(text(wear));
    lines.emplace_back();
    lines.push_back(format(text("GuiInv.c:IdentifyWeight"), {std::to_string(weightOf(item.typeId))}));
    lines.emplace_back();
    const bool armour = a && (a->classes & oc::Armor);
    const bool weapon = a && (a->classes & (oc::Weapon | oc::Wand));
    const nox::things::EquipDef* def = !t        ? nullptr
                                       : armour ? modifiers_.findArmor(t->name)
                                       : weapon ? modifiers_.findWeapon(t->name)
                                                : nullptr;
    if (armour && def) {
        // ARMOR_VALUE times the effectiveness, in thousandths; shields don't say.
        const long value = std::lround(effect(0, "ArmorMultiplierEffect", 1.0f) * def->armorValue * 1000 + 0.5f);
        lines.push_back((a->subclasses & 2) ? text("guiinv.c:ArmorValueLabelNA")
                                            : format(text("guiinv.c:ArmorValueLabel"), {std::to_string(value)}));
        lines.emplace_back();
    } else if (weapon && def) {
        // calcBoltDamage with the effectiveness and the fire / lightning enchantments.
        const float mult = effect(0, "DamageMultiplierEffect", 1.0f);
        float fire = 0;
        float lightning = 0;
        for (const std::size_t i : {std::size_t{2}, std::size_t{3}}) {
            fire = std::max(fire, effect(i, "FireEffect", 0.0f));
            lightning = std::max(lightning, effect(i, "LightningEffect", 0.0f));
        }
        float total = (static_cast<float>(self.strength - def->requiredStrength) * def->damageCoefficient +
                       static_cast<float>(def->damageMin)) *
                          mult +
                      lightning + fire;
        const float base = static_cast<float>(def->damageMin) * mult;
        // What strength adds; under the required strength it shows 0 and the total doesn't drop below the base.
        float strength = total - base - fire - lightning;
        if (strength < 0) {
            total -= strength;
            strength = 0;
        }
        if (a->subclasses & 0xC) {
            lines.push_back(text("guiinv.c:WeaponDamageLabelNA")); // bows and crossbows: the arrows' business
        } else {
            lines.push_back(format(text("guiinv.c:WeaponDamageLabel"), {fixed2(total)}));
            lines.push_back("  " + format(text("guiinv.c:BaseDamageLabel"), {fixed2(base)}));
            lines.push_back("  " + format(text("guiinv.c:StrengthDamageLabel"), {fixed2(strength)}));
            if (fire > 0) {
                lines.push_back("  " + format(text("guiinv.c:FireDamageLabel"), {fixed2(fire)}));
            }
            if (lightning > 0) {
                lines.push_back("  " + format(text("guiinv.c:ElectricalDamageLabel"), {fixed2(lightning)}));
            }
        }
        lines.emplace_back();
    }
    if (armour || weapon) {
        // The enchantments' IDENTIFYDESC (a definitionless one: unknown).
        if (!def) {
            lines.push_back(text("GuiInv.c:IdentifySpecialAttributes"));
            lines.push_back(text("GuiInv.c:IdentifyUnknown"));
        } else {
            bool said = false;
            for (const std::size_t i : {std::size_t{2}, std::size_t{3}}) {
                if (const auto* m = mod(i); m && !m->identifyDesc.empty()) {
                    if (!said) {
                        lines.push_back(text("GuiInv.c:IdentifySpecialAttributes"));
                        said = true;
                    }
                    lines.push_back("  " + text(m->identifyDesc));
                }
            }
            if (said) {
                lines.emplace_back();
            }
        }
    }
    if (t) {
        const auto d = t->attribute("DESCRIPTION");
        // No DESCRIPTION attribute: the string table's thing.db:<name>Description, only for things with a weight.
        const std::string id = d ? std::string(*d) : weightOf(item.typeId) ? "thing.db:" + t->name + "Description" : "";
        if (!id.empty()) {
            lines.push_back(text(id));
        }
    }
    // The list (148 x 140 at (7, 50)).
    float yy = wy + 50;
    for (const auto& line : lines) {
        if (line.empty()) {
            yy += 10;
            continue;
        }
        for (const auto& l : font_->wrap(line, size, 148 * s)) {
            if (yy + 10 > wy + 190) {
                return;
            }
            font_->draw(l, (wx + 7) * s, yy * s, size, {255, 255, 255, 255});
            yy += 10;
        }
    }
}

void Inventory::stopIdentify()
{
    identify_ = false;
    identified_.reset();
}

std::optional<int> Inventory::identifyCursor(float x, float y, int w, int h) const
{
    if (!identify_ || state_ != 2) {
        return std::nullopt;
    }
    const float s = Hud::scale(w, h);
    const float px = x / s;
    const float py = y / s - static_cast<float>(slide_);
    if (in(kTray, px, py)) {
        return 6; // Identify
    }
    if (in(kSpecial, px, py)) {
        return (static_cast<int>(py) - 13) / kCell == 1 ? 6 : 7;  // the magnifier itself gets the Identify cursor
    }
    if (in(kDoll, px, py)) {
        return std::nullopt;
    }
    return 7; // IdentifyNo
}
} // namespace noxsdl
