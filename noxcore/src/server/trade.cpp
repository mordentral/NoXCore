// Shops and player-to-player trades: sessions, stock, offers, prices and the MSG_TRADE
// handler (trade.hpp has the overview).
//
// The prices are figured in the exe's x87 arithmetic: 53-bit precision (the CRT's start,
// from the initializer table: _controlfp(_PC_53,
// _MCW_PC); no Direct3D to change it) and rounding in chop mode (each pass of
// mainloop: _controlfp(_RC_CHOP, _MCW_RC)), each FSTP to a float chopped: here util::X87.

#include "noxcore/server/trade.hpp"

#include "noxcore/script/vm.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/rewards.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

constexpr std::uint32_t kModifiable = 0x13001000u; // (weapons, wands, armour, flags: their InitData's modifiers)

using util::hi;
using util::lo;

using util::putU32;

using util::X87;

/// Math_FloatToInt (FISTP, chopped); out of range the integer indefinite.
using util::chop;

bool isPlayer(const ServerObject* o) { return o && (o->classes & oc::Player) && o->player; }

/// Its client's message (Net_QueueImportant / Net_QueueImportantSeq): a player's alone.
void tell(World& world, const ServerObject& to, std::vector<std::uint8_t> bytes)
{
    if (isPlayer(&to)) {
        world.broadcasts().push_back({std::move(bytes), true, to.extent});
    }
}

/// SrvNet_SendInformString: MSG_INFORM 0x0F, not a sign, the string's id.
void inform(World& world, const ServerObject& to, std::string_view id)
{
    tell(world, to, informStringBytes(id));
}

/// SrvNet_SendReportGold: MSG_REPORT_GOLD {u32 its gold}, at once (the player's
/// report, playerReportAnything, tells it again when it sees the change).
void reportGold(World& world, const ServerObject& unit)
{
    std::vector<std::uint8_t> m = {0x4A};
    putU32(m, unit.player->gold);
    tell(world, unit, std::move(m));
}

/// The thing at `extent`: in the world (an inventory too) or held out of it by a trade.
ServerObject* thing(World& world, std::uint32_t extent)
{
    if (const auto it = world.tradeObjects.find(extent); it != world.tradeObjects.end()) {
        return &it->second;
    }
    return extent != 0 ? world.byExtent(extent) : nullptr;
}

/// Out of the world into a trade's keeping (no list holds it: newObjectByTypeID's
/// things, an offer out of its inventory).
void keep(World& world, std::uint32_t extent)
{
    if (auto node = world.objects().extract(extent)) {
        world.gridRelease(node.mapped()); // (in no list)
        world.tradeObjects.insert(std::move(node));
    }
}

/// Back into the world (into an inventory next).
ServerObject* release(World& world, std::uint32_t extent)
{
    if (auto node = world.tradeObjects.extract(extent)) {
        const auto it = world.objects().insert(std::move(node)).position;
        return &it->second;
    }
    return world.byExtent(extent);
}

/// objectFreeMem on a thing a trade keeps.
void freeThing(World& world, std::uint32_t extent)
{
    if (world.tradeObjects.erase(extent) != 0) {
        world.freeExtent(extent);
    }
}

/// A thing a trade keeps that the exe leaves in no list and never frees (a stock thing the full
/// pool turned away, an offer freed with its session): gone, its extent never given back.
void lose(World& world, std::uint32_t extent)
{
    world.tradeObjects.erase(extent);
}

/// The trade pools (registerShopClasses: fixed classes, whose take
/// gives null when used up; only the growing kind grows): "TradeSessions" 0x40, and
/// "TradeItems" 500 for every session's stock and both offer lists (the parked quest shops'
/// too). Their frees (Trade_WithdrawOffer's) give the
/// entry straight back, so what's live is what's counted.
constexpr std::size_t kTradeSessions = 0x40;
constexpr std::size_t kTradeItems = 500;

bool tradeItemLeft(const World& world)
{
    std::size_t live = 0;
    for (const TradeSession& s : world.trades) {
        live += s.stock.size() + s.offersA.size() + s.offersB.size();
    }
    return live < kTradeItems;
}

/// newObjectByTypeID: a thing of `type` in no list (a trade's keeping); null for
/// a type thing.bin lacks.
ServerObject* newThing(Fight& fight, std::string_view type)
{
    if (!fight.things || !fight.things->findThing(type)) {
        return nullptr;
    }
    const std::uint32_t e = fight.world.spawn(*fight.things, type, 0, 0).extent;
    keep(fight.world, e);
    return thing(fight.world, e);
}

/// A thing type's WORTH (the object's Worth, its type's).
std::uint32_t typeWorth(const World& world, std::string_view type)
{
    const things::ThingDef* t = world.thingDb ? world.thingDb->findThing(type) : nullptr;
    const auto w = t ? t->lastAttribute("WORTH") : std::nullopt;
    return w ? static_cast<std::uint32_t>(std::strtoul(std::string(*w).c_str(), nullptr, 10)) : 0;
}

std::string_view xferOf(const World& world, const ServerObject& o)
{
    const things::ThingDef* t = world.thingDb ? world.thingDb->findThing(o.typeName) : nullptr;
    const auto x = t ? t->lastAttribute("XFER") : std::nullopt;
    return x ? *x : std::string_view{};
}

/// A gem (Diamond, Emerald, Ruby: their worth is what a shop pays).
bool isGem(const ServerObject& o) { return o.typeName == "Diamond" || o.typeName == "Emerald" || o.typeName == "Ruby"; }

/// The shopkeeper of a shop session (the side that isn't a player), and its InitData.
ServerObject* keeperOf(World& world, const TradeSession& s)
{
    ServerObject* a = thing(world, s.a);
    return isPlayer(a) ? thing(world, s.b) : a;
}

ServerObject* playerOf(World& world, const TradeSession& s)
{
    ServerObject* a = thing(world, s.a);
    return (a && (a->classes & oc::Player)) ? a : thing(world, s.b);
}

ServerObject::Shopkeeper& keeperData(ServerObject& k)
{
    return k.shopkeeper ? *k.shopkeeper : k.shopkeeper.emplace();
}

/// spellPrice: the spell's price (in a quest times QuestSpellWorthMultiplier).
std::int32_t spellPrice(const World& world, int spell)
{
    if (spell <= 0 || 0x89 <= spell) {
        return 0; // (ind & 0xFFFF0000: the byte's zero)
    }
    const things::SpellDef* def = world.thingDb ? world.thingDb->findSpell(static_cast<std::uint32_t>(spell)) : nullptr;
    const std::int32_t price = def ? def->price : 0;
    float f = X87::fromDouble(price).f(); // (FILD, FSTP float)
    if (world.quest()) {
        f = (X87(world.number("QuestSpellWorthMultiplier")) * f).f();
    }
    return chop(f);
}

/// A stock item's place in the shop's order, its kind's rank (the first of
/// the exe's class / subclass pairs it fits: weapons and armour, wands, then the books, the
/// potions, food; 0xFF less each pair passed) over its price.
std::uint32_t stockKey(const ServerObject& o, std::uint32_t price)
{
    static constexpr std::pair<std::uint32_t, std::uint32_t> kKinds[] = {
        {0x01001000u, 0}, {0x02000000u, 0}, {0x100u, 2}, {0x100u, 1}, {0x100u, 4}, {0x10u, 8}, {0x10u, 0},
    };
    std::uint32_t rank = 0xFF;
    for (const auto& [classes, subclasses] : kKinds) {
        if ((classes & o.classes) && (subclasses == 0 || (o.subclasses & subclasses))) {
            break;
        }
        --rank;
    }
    return rank << 24 | price;  // the kind's rank in the top byte, the price below
}

/// SrvNet_SendShopAddItem: TRADE 0x08 {u16 type, u16 net code, u32 price, u32 its
/// health, the four modifiers}.
void sendStockItem(World& world, const ServerObject& to, const TradeSession::Item& item)
{
    const ServerObject* o = thing(world, item.object);
    if (!o) {
        return;
    }
    std::vector<std::uint8_t> m = {0xC9, 0x08, lo(o->type), hi(o->type), lo(o->extent), hi(o->extent)};
    putU32(m, item.price);
    putU32(m, o->health);
    for (std::size_t i = 0; i < 4; ++i) {
        m.push_back((o->classes & kModifiable) ? o->modifiers[i] : std::uint8_t{0xFF});
    }
    tell(world, to, std::move(m));
}

/// addItemToShopSession: priced as a buy, before the first of the stock whose key
/// isn't below its own; null with the TradeItems pool used up (before the price).
TradeSession::Item* addToStock(Fight& fight, TradeSession& s, ServerObject& o)
{
    World& world = fight.world;
    if (!tradeItemLeft(world)) {
        return nullptr;
    }
    const TradeSession::Item item{o.extent, static_cast<std::uint32_t>(itemCost(world, fight.modifiers, 1, &s, o))};
    const std::uint32_t key = stockKey(o, item.price);
    auto at = s.stock.begin();
    while (at != s.stock.end()) {
        const ServerObject* other = thing(world, at->object);
        if (other && key <= stockKey(*other, at->price)) {
            break;
        }
        ++at;
    }
    return &*s.stock.insert(at, item);
}

/// SrvNet_SendTradeStartShop: TRADE 0x0D {u16 the keeper's type, its name (24 wide
/// letters and an end), its greeting (from its init data)} to the shop's player (whoever it's
/// called for: its first argument unused).
void sendStartShop(World& world, const TradeSession& s)
{
    const ServerObject* keeper = keeperOf(world, s);
    const ServerObject* player = playerOf(world, s);
    if (!keeper || !player) {
        return;
    }
    // Its name: "NPC:" and its script name (its type's without one) after its last ':', no
    // '_', from the string table.
    std::string name = keeper->scriptName.empty() ? keeper->typeName : keeper->scriptName;
    if (const auto colon = name.rfind(':'); colon != std::string::npos) {
        name = name.substr(colon + 1);
    }
    std::erase(name, '_');
    const std::u16string text = world.text("NPC:" + name);
    std::vector<std::uint8_t> m = {0xC9, 0x0D, lo(keeper->type), hi(keeper->type)};
    for (std::size_t i = 0; i < 25; ++i) {
        const char16_t c = i < 24 && i < text.size() ? text[i] : u'\0';
        m.push_back(static_cast<std::uint8_t>(c));
        m.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    const std::string greeting = keeper->shopkeeper ? keeper->shopkeeper->greeting : std::string{};
    for (std::size_t i = 0; i < 32; ++i) {
        m.push_back(i < greeting.size() && i < 31 ? static_cast<std::uint8_t>(greeting[i]) : std::uint8_t{0});
    }
    tell(world, *player, std::move(m));
}

/// SrvNet_SendTradeStartP2P: TRADE 0x0C {the other's name} to `to`.
void sendStartTrade(World& world, const ServerObject& to, const TradeSession& s)
{
    const ServerObject* other = thing(world, to.extent == s.a ? s.b : s.a);
    std::u16string name;
    if (isPlayer(other)) {
        name = other->player->name;
    }
    std::vector<std::uint8_t> m = {0xC9, 0x0C};
    for (std::size_t i = 0; i < 25; ++i) {
        const char16_t c = i < 24 && i < name.size() ? name[i] : u'\0';
        m.push_back(static_cast<std::uint8_t>(c));
        m.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    tell(world, to, std::move(m));
}

/// tradeSetPlayer: a side takes up the session or lets it go.
void holdSession(World& world, TradeSession& s, std::uint32_t side)
{
    s.active = true;
    if (ServerObject* o = thing(world, side); isPlayer(o)) {
        o->player->trade = s.id;
    }
}

void letGo(World& world, TradeSession& s, std::uint32_t side)
{
    s.active = false;
    if (ServerObject* o = thing(world, side); isPlayer(o) && o->player->trade == s.id) {
        o->player->trade = 0;
    }
}

/// The session freed, its stock with it (and the gold piles); the offers' things
/// are in no list (lost).
void freeSession(World& world, std::uint32_t id)
{
    const auto it = std::ranges::find(world.trades, id, &TradeSession::id);
    if (it == world.trades.end()) {
        return;
    }
    for (const auto& item : it->stock) {
        freeThing(world, item.object);
    }
    for (const auto* offers : {&it->offersA, &it->offersB}) {
        for (const auto& item : *offers) {
            lose(world, item.object); // (the entries freed, not their things)
        }
    }
    world.trades.erase(it);
}

/// createShopStruct: a new session at the list's head; null with the
/// TradeSessions pool used up (before its gold piles).
TradeSession* newSession(World& world)
{
    if (world.trades.size() >= kTradeSessions) {
        return nullptr;
    }
    TradeSession& s = world.trades.emplace_front();
    s.id = world.nextTradeId++;
    // Its two gold piles (newObjectByTypeID("Gold") twice) are goldA /
    // goldB here, but each took a script id (respawnAllow).
    world.newScriptId();
    world.newScriptId();
    return &s;
}

/// A shop record matching a stock thing: its type, a modifiable's modifiers, a
/// field guide's creature, a spell or ability book's entry.
bool recordFits(const World& world, const ServerObject& o, const ServerObject::Shopkeeper::Stock& r)
{
    if (o.typeName != r.type) {
        return false;
    }
    bool fits = true;
    if (o.classes & kModifiable) {
        fits = o.modifiers == r.modifiers;
    }
    if (o.classes & oc::InfoBook) {
        if (!(o.subclasses & 1u) && !(o.subclasses & 4u)) {
            const things::ThingDef* t = world.thingDb ? world.thingDb->findThing(r.guide) : nullptr;
            if (o.guideCreature != (t ? r.guide : std::string{})) { // (getUnitNameByThingType)
                fits = false;
            }
        } else if (o.bookEntry != static_cast<std::uint32_t>(r.parameter)) {
            return false;
        }
    }
    return fits;
}

/// One less of its record in the keeper's stock (getSomeShopData
/// finds it); none left, the record goes.
void takeFromRecords(World& world, TradeSession& s, const ServerObject& o)
{
    ServerObject* keeper = keeperOf(world, s);
    if (!keeper) {
        return;
    }
    auto& stock = keeperData(*keeper).stock;
    for (std::size_t i = 0; i < stock.size(); ++i) {
        if (recordFits(world, o, stock[i])) {
            if (--stock[i].count == 0) {
                stock.erase(stock.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return;
        }
    }
}

/// Whether a sold thing leaves the stock (a quest's gems and ankh never do).
bool leavesStock(const World& world, const ServerObject& o)
{
    return !(world.quest() && (isGem(o) || o.typeName == "AnkhTradable"));
}

/// Its stock entry gone, TRADE 0x09 {u16 its code} to the shop's player.
void removeFromStock(World& world, TradeSession& s, std::uint32_t object)
{
    const auto it = std::ranges::find(s.stock, object, &TradeSession::Item::object);
    if (it == s.stock.end()) {
        return;
    }
    s.stock.erase(it);
    if (ServerObject* p = playerOf(world, s)) {
        tell(world, *p, {0xC9, 0x09, lo(object), hi(object)});
    }
}

/// inventoryCountObjects: those of a type it holds (not the destroyed).
int countHeld(World& world, const ServerObject& unit, std::uint16_t type)
{
    int n = 0;
    for (const std::uint32_t e : unit.inventory) {
        if (const ServerObject* o = world.byExtent(e); o && o->type == type && !(o->flags & ObjFlag::Destroyed)) {
            ++n;
        }
    }
    return n;
}

ServerObject* heldByCode(World& world, const ServerObject& unit, std::uint32_t code)
{
    for (const std::uint32_t e : unit.inventory) {
        if (e == code) {
            return world.byExtent(e);
        }
    }
    return nullptr;
}

/// packetDynamicUnitCode: a code with bit 15 a map object's by its extent.
std::uint32_t dynamicCode(World& world, std::uint32_t code)
{
    if ((code & 0x8000u) == 0x8000u) {
        const ServerObject* o = world.byExtent(code & 0x7FFFu);
        return o ? o->extent : 0;
    }
    return code;
}

} // namespace

bool questItem(const ServerObject& o)
{
    static constexpr std::string_view kKept[] = {
        "BookOfOblivion", "OblivionHalberd", "OblivionHeart", "OblivionWierdling", "OblivionOrb", "AmuletOfClarity",
        "AmuletOfTeleportation", "BridgeGuardsBoots", "Spectacles", "MatildasCloak", "ThaviusStaff",
        "BlueOrbKeyOfTheLich", "RedOrbKeyOfTheLich", "SilverKey", "GoldKey", "SapphireKey", "RubyKey",
        "SponsorshipLetter", "MayorsScepter",
    };
    return std::ranges::find(kKept, std::string_view(o.typeName)) != std::end(kKept);
}

namespace {

Items itemsOf(Fight& fight) { return Items{fight.world, *fight.things, fight.modifiers, fight.rng, fight.frame}; }

/// loadShopItems: the keeper's stock made (out of the world) and priced; a quest's
/// shop its own: an AnkhTradable under ShopAnkhCutoffStage, the gems and supplies of its list,
/// and what a RewardMarker gives two stages on (four weapons, four armours, six spell books,
/// three field guides, an ability book one time in ten).
void loadStock(Fight& fight, TradeSession& s)
{
    World& world = fight.world;
    ServerObject* keeper = nullptr;
    // The keeper is whichever side of the session is a shopkeeper monster.
    for (const std::uint32_t side : {s.a, s.b}) {
        ServerObject* o = thing(world, side);
        if (o && (o->classes & oc::Monster) && (o->subclasses & things::ObjectSubclass::Shopkeeper)) {
            keeper = o;
            break;
        }
    }
    if (!keeper || !fight.things) {
        return;
    }
    // Each thing is made first; the pool used up, it's left made and in no list (each of the ~20
    // calls to addItemToShopSession drops its answer).
    const auto stockOrLose = [&](ServerObject& o) {
        if (!addToStock(fight, s, o)) {
            lose(world, o.extent);
        }
    };
    // Out of a quest: the keeper's own stock records, each made `count` times with its modifiers and book/guide entry.
    if (!world.quest()) {
        const auto records = keeperData(*keeper).stock; // (the stock's records as they stand)
        for (const auto& r : records) {
            for (int n = 0; n < r.count; ++n) {
                ServerObject* o = newThing(fight, r.type);
                if (!o) {
                    continue;
                }
                if (o->classes & kModifiable) {
                    // (modifSetItemAttrs: none set, a staff's aside, it keeps its own)
                    const bool any = std::ranges::any_of(r.modifiers, [](std::uint8_t m) { return m != 0xFF; });  // 0xFF: an empty modifier slot
                    if (any || ((o->classes & oc::Wand) && (o->subclasses & 0x47F0000u))) {  // a magical staff (ObjectSubclass::StaffWands)
                        o->modifiers = r.modifiers;
                    }
                }
                const std::string_view xfer = xferOf(world, *o);
                if (xfer == "SpellRewardXfer" || xfer == "AbilityRewardXfer") {
                    o->bookEntry = static_cast<std::uint8_t>(r.parameter);
                } else if (xfer == "FieldGuideXfer") {
                    const things::ThingDef* t = fight.things->findThing(r.guide);
                    o->guideCreature = t ? r.guide : std::string{};
                }
                stockOrLose(*o);
            }
        }
        return;
    }
    // A quest's shop: an ankh while the stage is under the cutoff, the fixed supplies, then reward-marker finds.
    // (Math_FloatToInt of the gamedata, the stage unsigned)
    const auto cutoff = static_cast<std::uint32_t>(chop(world.number("ShopAnkhCutoffStage")));
    if (static_cast<std::uint32_t>(world.questStage) < cutoff) {
        if (ServerObject* ankh = newThing(fight, "AnkhTradable")) {
            stockOrLose(*ankh);
        }
    }
    static constexpr std::string_view kSupplies[] = {
        "Diamond", "Diamond", "Diamond", "Diamond", "Diamond", "Emerald", "Emerald", "Emerald", "Emerald", "Emerald", "Ruby", "Ruby",
        "Ruby", "Ruby", "Ruby", "RedPotion", "RedPotion", "RedPotion", "BluePotion", "BluePotion", "BluePotion", "CurePoisonPotion",
        "CurePoisonPotion", "CurePoisonPotion", "HastePotion", "InvisibilityPotion", "ShieldPotion", "VampirismPotion",
        "FireProtectPotion", "ShockProtectPotion", "PoisonProtectPotion", "InvulnerabilityPotion", "InfravisionPotion", "Sword",
        "StaffWooden", "LesserFireballWand", "Quiver", "Quiver", "Quiver", "FanChakram", "FanChakram", "LeatherBoots",
        "LeatherArmor", "LeatherHelm", "WizardRobe", "WizardHelm",
    };
    for (const std::string_view type : kSupplies) {
        if (ServerObject* o = newThing(fight, type)) {
            stockOrLose(*o);
        }
    }
    if (!fight.things->findThing("RewardMarker")) {
        return;
    }
    // The marker (a RewardMarker of RewardMarkerCreate's: every list, always), out of the world
    // while it's used, then gone (delayedDeleteObject).
    const std::uint32_t markerExtent = world.spawn(*fight.things, "RewardMarker", 0, 0).extent;
    keep(world, markerExtent);
    const std::uint32_t stage = static_cast<std::uint32_t>(world.questStage) + 2;
    const auto reward = [&](std::uint32_t kinds) {
        ServerObject* marker = thing(world, markerExtent);
        if (!marker || !marker->rewardMarker) {
            return;
        }
        marker->rewardMarker->kinds = kinds;
        const ServerObject copy = *marker;
        if (ServerObject* item = activateRewardMarker(fight, copy, stage, 0, 0)) {
            const std::uint32_t e = item->extent;
            keep(world, e);
            if (ServerObject* kept = thing(world, e)) {
                stockOrLose(*kept);
            }
        }
    };
    // RewardMarker kinds bits: 8 weapons (x4), 0x10 armour (x4), 1 spell books (x6), 4 field guides (x3).
    for (const std::uint32_t kinds : {8u, 8u, 8u, 8u, 0x10u, 0x10u, 0x10u, 0x10u, 1u, 1u, 1u, 1u, 1u, 1u, 4u, 4u, 4u}) {
        reward(kinds);
    }
    // 0x5A (90) < a roll of 0-100: an ability book (kind 2) about one shop in ten.
    if (0x5A < nox::util::uniform_int_distribution<int>(0, 100)(fight.rng)) { // (randInt)
        reward(2u);
    }
    freeThing(world, markerExtent);
}

/// createPlayerShopSession: in a quest the shop the player left (its stock kept),
/// else a new one stocked.
TradeSession* playerShop(Fight& fight, ServerObject& player, ServerObject& keeper)
{
    World& world = fight.world;
    TradeSession* s = nullptr;
    bool kept = false;
    if (world.quest() && player.player) {
        auto& slot = world.questShops[player.player->index & 31u];
        s = tradeSession(world, slot);
        slot = 0;
        kept = s != nullptr;
    }
    if (!s) {
        s = newSession(world);
        if (!s) {
            return nullptr; // (nothing sent: Trade_StartSession returns 0)
        }
    }
    s->a = player.extent;
    s->b = keeper.extent;
    s->shop = true;
    if (!kept) {
        loadStock(fight, *s);
    }
    return s;
}

/// SrvNet_SendTradeAcceptState: TRADE 0x03 {bit 0 it accepted, bit 1 the other}.
void sendAcceptState(World& world, const ServerObject& to, const TradeSession& s)
{
    std::uint8_t bits = 0;
    // Bit 0: `to`'s own side has accepted; bit 1: the other side has.
    if (s.acceptedA) {
        bits = static_cast<std::uint8_t>((s.a != to.extent ? 1 : 0) + 1);
    }
    if (s.acceptedB) {
        bits |= s.b == to.extent ? 1 : 2;
    }
    tell(world, to, {0xC9, 0x03, bits});
}

/// SrvNet_SendTradeGoldValues: TRADE 0x06 {its gold put up, (a non-player trader's
/// side) the other's total less its own, the other's gold put up}.
void sendGoldValues(World& world, const ServerObject& to, const TradeSession& s)
{
    std::uint32_t mine = 0;
    std::uint32_t difference = 0;
    std::uint32_t theirs = 0;
    if (to.extent == s.a) {
        mine = s.goldA;
        difference = isPlayer(thing(world, s.b)) ? 0 : s.totalB - s.totalA;
        theirs = s.goldB;
    } else if (to.extent == s.b) {
        mine = s.goldB;
        difference = isPlayer(thing(world, s.a)) ? 0 : s.totalA - s.totalB;
        theirs = s.goldA;
    }
    std::vector<std::uint8_t> m = {0xC9, 0x06};
    putU32(m, mine);
    putU32(m, difference);
    putU32(m, theirs);
    tell(world, to, std::move(m));
}

/// SrvNet_SendTradeAddItem: TRADE 0x04 {1 its own offer, u16 type, u16 code, u32
/// worth, the modifiers}.
void sendOffer(World& world, const ServerObject& to, std::uint32_t offerer, const ServerObject& item, std::uint32_t value)
{
    std::vector<std::uint8_t> m = {0xC9, 0x04, static_cast<std::uint8_t>(to.extent == offerer ? 1 : 0), lo(item.type), hi(item.type),
                                   lo(item.extent), hi(item.extent)};
    putU32(m, value);
    for (std::size_t i = 0; i < 4; ++i) {
        m.push_back((item.classes & kModifiable) ? item.modifiers[i] : std::uint8_t{0xFF});
    }
    tell(world, to, std::move(m));
}

/// A side's gold put up and its offers' worth.
std::uint32_t sideTotal(const TradeSession& s, bool first)
{
    std::uint32_t total = first ? s.goldA : s.goldB;
    for (const auto& item : first ? s.offersA : s.offersB) {
        total += item.price;
    }
    return total;
}

/// With a non-player trader on a side (both players: nothing), the gold each player
/// put up given back, the totals afresh, and the short side made up: a player's gold put up (what
/// it has, at most), a trader's out of nothing.
void balance(World& world, TradeSession& s)
{
    ServerObject* a = thing(world, s.a);
    ServerObject* b = thing(world, s.b);
    const bool pa = a && (a->classes & oc::Player);
    const bool pb = b && (b->classes & oc::Player);
    if (pa && pb) {
        return;
    }
    if (pa && a->player) {
        addGold(*a, static_cast<std::int32_t>(s.goldA));
    }
    s.goldA = 0;
    if (pb && b->player) {
        addGold(*b, static_cast<std::int32_t>(s.goldB));
    }
    s.goldB = 0;
    s.totalA = sideTotal(s, true);
    s.totalB = sideTotal(s, false);
    if (s.totalA < s.totalB) {
        const std::uint32_t d = s.totalB - s.totalA;
        if (!pa || !a->player) {
            s.totalA += d;
            s.goldA += d;
            return;
        }
        const std::uint32_t put = std::min(d, a->player->gold);
        s.totalA += put;
        s.goldA += put;
        subGold(*a, put);
    } else if (s.totalB < s.totalA) {
        const std::uint32_t d = s.totalA - s.totalB;
        if (pb && b->player) {
            const std::uint32_t put = std::min(d, b->player->gold);
            s.totalB += put;
            s.goldB += put;
            subGold(*b, put);
        } else {
            s.totalB += d;
            s.goldB += d;
        }
    }
}

/// Both accepts off, a non-player trader's on when the totals suit it (Trade_AddOffer's and
/// Trade_WithdrawOffer's).
void resetAccepts(World& world, TradeSession& s)
{
    s.acceptedA = false;
    s.acceptedB = false;
    const ServerObject* a = thing(world, s.a);
    const ServerObject* b = thing(world, s.b);
    if (!(a && (a->classes & oc::Player))) {
        if (s.totalA <= s.totalB) {
            s.acceptedA = true;
        }
    } else if (!(b && (b->classes & oc::Player)) && s.totalB <= s.totalA) {
        s.acceptedB = true;
    }
}

/// Whether an offer list may take `item`: four kinds at most (a modifiable thing
/// always a kind of its own).
bool offerFits(World& world, const std::vector<TradeSession::Item>& offers, const ServerObject& item)
{
    std::array<std::uint32_t, 4> types{};
    std::uint32_t kinds = 0;
    for (const auto& o : offers) {
        const ServerObject* t = thing(world, o.object);
        const std::uint32_t type = t ? t->type : 0;
        if (std::ranges::find(types, type) == types.end() && kinds < 4) {
            types[kinds++] = type;
        }
    }
    if (kinds == 0) {
        return true;
    }
    for (std::uint32_t i = 0; i < kinds; ++i) {
        if (types[i] == item.type && !(item.classes & kModifiable)) {
            return true;
        }
    }
    return kinds < 4;
}

/// Trade_AddOffer: 1 when `who`'s offer took it.
int addOffer(Fight& fight, TradeSession& s, const ServerObject& who, ServerObject& item)
{
    World& world = fight.world;
    std::vector<TradeSession::Item>* offers = who.extent == s.a ? &s.offersA : who.extent == s.b ? &s.offersB : nullptr;
    if (!offers || !offerFits(world, *offers, item)) {
        return 0;
    }
    if (!tradeItemLeft(world)) {
        // (its own pool take failing: each side that's a player told, Trade.c's line)
        for (const std::uint32_t side : {s.a, s.b}) {
            if (const ServerObject* p = thing(world, side); isPlayer(p)) {
                world.textTo(*p, "Trade.c:TradeMaxObjectsReached");
            }
        }
        return 0;
    }
    const TradeSession::Item entry{item.extent, static_cast<std::uint32_t>(itemCost(world, fight.modifiers, 1, &s, item))};
    offers->insert(offers->begin(), entry);
    balance(world, s);
    resetAccepts(world, s);
    for (const std::uint32_t side : {s.a, s.b}) {
        if (const ServerObject* p = thing(world, side); isPlayer(p)) {
            sendGoldValues(world, *p, s);
            sendOffer(world, *p, who.extent, item, entry.price);
            sendAcceptState(world, *p, s);
        }
    }
    return 1;
}

/// Trade_WithdrawOffer: whichever offer holds the code's thing gives it back (a
/// trader's into the shop's stock); no check of who asks.
void withdrawOffer(Fight& fight, TradeSession& s, std::uint32_t code)
{
    World& world = fight.world;
    std::vector<TradeSession::Item>* offers = &s.offersA;
    std::uint32_t owner = s.a;
    auto it = std::ranges::find(s.offersA, code, &TradeSession::Item::object);
    if (it == s.offersA.end()) {
        offers = &s.offersB;
        owner = s.b;
        it = std::ranges::find(s.offersB, code, &TradeSession::Item::object);
        if (it == s.offersB.end()) {
            return;
        }
    }
    const std::uint32_t object = it->object;
    ServerObject* holder = thing(world, owner);
    if (!(holder && (holder->classes & oc::Player))) {
        // (The stock's entry taken while the offer's still holds its own: the pool used up, the
        // thing is left in no list.)
        if (ServerObject* o = thing(world, object)) {
            if (TradeSession::Item* added = addToStock(fight, s, *o)) {
                const TradeSession::Item copy = *added;
                if (ServerObject* p = playerOf(world, s)) {
                    sendStockItem(world, *p, copy);
                }
            } else {
                lose(world, object);
            }
        }
    } else if (ServerObject* o = release(world, object); o && fight.things) {
        itemsOf(fight).inventoryPut(*holder, *o, true);
    }
    offers->erase(std::ranges::find(*offers, object, &TradeSession::Item::object));
    balance(world, s);
    resetAccepts(world, s);
    for (const std::uint32_t side : {s.a, s.b}) {
        if (const ServerObject* p = thing(world, side); isPlayer(p)) {
            sendGoldValues(world, *p, s);
            tell(world, *p, {0xC9, 0x05, lo(object), hi(object)}); // (SrvNet_SendTradeRemoveItem)
            sendAcceptState(world, *p, s);
        }
    }
}

/// `offers` handed to `to`: a player's inventory, a trader's stock (TRADE 0x08 to the
/// player).
void handOver(Fight& fight, TradeSession& s, std::uint32_t to, const std::vector<TradeSession::Item>& offers)
{
    World& world = fight.world;
    ServerObject* recipient = thing(world, to);
    for (const auto& item : offers) {
        if (!(recipient && (recipient->classes & oc::Player))) {
            if (ServerObject* o = thing(world, item.object)) {
                if (TradeSession::Item* added = addToStock(fight, s, *o)) {
                    const TradeSession::Item copy = *added;
                    if (ServerObject* p = playerOf(world, s)) {
                        sendStockItem(world, *p, copy);
                    }
                } else {
                    lose(world, item.object); // (the pool used up: in no list)
                }
            }
        } else if (ServerObject* o = release(world, item.object); o && fight.things) {
            itemsOf(fight).inventoryPut(*recipient, *o, true);
        }
    }
}

/// Trade_OnAccept.
void accept(Fight& fight, TradeSession& s, const ServerObject& who)
{
    World& world = fight.world;
    if (who.extent == s.a) {
        s.acceptedA = true;
    } else if (who.extent == s.b) {
        s.acceptedB = true;
    }
    for (const std::uint32_t side : {s.a, s.b}) {
        if (const ServerObject* p = thing(world, side); isPlayer(p)) {
            sendAcceptState(world, *p, s);
        }
    }
    if (!(s.acceptedA && s.acceptedB)) {
        return;
    }
    // Both handed over with the offers' pool entries still held (frees them after).
    const auto offersA = s.offersA;
    const auto offersB = s.offersB;
    handOver(fight, s, s.b, offersA);
    handOver(fight, s, s.a, offersB);
    s.offersA.clear();
    s.offersB.clear();
    // Each player the other's gold put up.
    if (ServerObject* b = thing(world, s.b); isPlayer(b)) {
        addGold(*b, static_cast<std::int32_t>(s.goldA));
    }
    if (ServerObject* a = thing(world, s.a); isPlayer(a)) {
        addGold(*a, static_cast<std::int32_t>(s.goldB));
    }
    s.goldA = s.goldB = 0;
    s.totalA = s.totalB = 0;
    if (s.shop) {
        if (ServerObject* p = playerOf(world, s)) {
            tell(world, *p, {0xC9, 0x07}); // (SrvNet_SendTradeReset)
        }
        return;
    }
    closeTrade(fight, s);
}

/// Shop_OnBuyItem and Shop_OnBuyStack: the buy of one stock thing, false
/// when refused. `stack` the stack's ways: the pickup function for whatever has one (food too),
/// no sound of its own.
bool buy(Fight& fight, ServerObject& player, TradeSession& s, std::uint32_t gold, ServerObject& stockThing, bool stack)
{
    World& world = fight.world;
    PlayerControl& p = *player.player;
    const std::uint32_t price = static_cast<std::uint32_t>(itemCost(world, fight.modifiers, 1, &s, stockThing));
    if (gold < price) {
        const auto shortBy = static_cast<std::uint16_t>(static_cast<std::uint16_t>(price) - static_cast<std::uint16_t>(gold));
        tell(world, player, {0xC9, 0x1B, lo(shortBy), hi(shortBy)}); // (SrvNet_SendShopNotEnoughGold)
        return false;
    }
    if (stockThing.classes & oc::Food) {
        const int most = (world.gameFlags & 0x1800u) ? 9 : 3;  // solo or quest games: 9 of a food at most, else 3
        if (most <= countHeld(world, player, stockThing.type)) {
            world.textTo(player, "pickup.c:MaxSameItem");
            return false;
        }
    }
    const auto refuse = [&](std::string_view id) {
        inform(world, player, id);
        playSound(world, 0x39D, player);
        return false;
    };
    if (stockThing.typeName == "AnkhTradable" && chop(world.number("MaxExtraLives")) <= static_cast<std::int32_t>(p.questLives)) {
        return refuse("pickup.c:MaxTradableAnkhsReached");
    }
    if (world.quest() && (stockThing.classes & oc::Wand) && (stockThing.subclasses & 0x200000u) &&
        chop(world.number("ForceOfNatureStaffLimit")) <= countHeld(world, player, stockThing.type)) {
        return refuse("pickup.c:MaxSameItem");
    }
    // A quest's gems and ankh are sold over and over: a new one each time
    // (newObjectWithTypeInd).
    ServerObject* item = &stockThing;
    if (world.quest() && (isGem(stockThing) || stockThing.typeName == "AnkhTradable")) {
        item = newThing(fight, stockThing.typeName);
    }
    if (item && fight.things) {
        const std::uint32_t e = item->extent;
        item = release(world, e);
        Items items = itemsOf(fight);
        const bool viaPickup = stack ? !item->pickup.empty() : !(item->classes & 0x110u) && !item->pickup.empty();  // a single buy puts food and books (0x110) straight in the inventory
        if (viaPickup) {
            // Its PICKUP's answer isn't looked at: refused, the thing is in no list (lost) and the
            // gold taken all the same (an original bug).
            const std::uint32_t id = s.id;
            items.pickupFunction(player, *item, true);
            if (item->holder == 0 && !item->pendingDelete) {
                item->pendingDelete = true;
            }
            if (!tradeSession(world, id)) {
                // A solo spell book learnt as it's picked up (a stack's buy) shuts the shop
                // (Spell_GrantToPlayer -> Trade_ExitShop) under the buy: the exe goes on with the
                // freed session (takeFromRecords reads its sides: an original bug). Here the
                // buy ends there, paid for.
                subGold(player, price);
                reportGold(world, player);
                return false;
            }
        } else {
            items.inventoryPut(player, *item, true);
            if (!stack) {
                playSound(world, 0x133, player, 2, player.extent);
            }
        }
    }
    takeFromRecords(world, s, stockThing);
    if (leavesStock(world, stockThing)) {
        removeFromStock(world, s, stockThing.extent);
    }
    subGold(player, price);
    reportGold(world, player);
    return true;
}

/// Shop_OnSellItem / Shop_OnSellStack's sale: out of the inventory, gone,
/// its price added.
void sell(Fight& fight, ServerObject& player, TradeSession& s, ServerObject& item)
{
    World& world = fight.world;
    if (fight.things) {
        itemsOf(fight).removeFromInventory(player, item);
    }
    item.pendingDelete = true; // (delayedDeleteObject)
    addGold(player, itemCost(world, fight.modifiers, 0, &s, item));
    reportGold(world, player);
}

/// Shop_OnSellItem and Shop_OnAskSellPrice's refusals: a quest item ("CantSellQuestItem") or a
/// glyph ("CantSellItem"), 0x39D to the player alone.
bool refuseSale(World& world, ServerObject& player, const ServerObject& item)
{
    const char* id = questItem(item) ? "Trade.c:CantSellQuestItem" : item.typeName == "Glyph" ? "Trade.c:CantSellItem" : nullptr;
    if (!id) {
        return false;
    }
    world.textTo(player, id);
    playSound(world, 0x39D, player, 2, player.extent);
    return true;
}

/// rechargeItem: a staff's charge up by `amount` percent (to 100), its charges
/// with it; whether they changed.
bool recharge(ServerObject& o, int amount)
{
    if (o.chargePercent >= 100) {
        return false;
    }
    o.chargePercent = std::min(o.chargePercent + amount, 100);
    const int charges = o.maxCharges * o.chargePercent / 100;
    if (charges != o.charges) {
        o.charges = static_cast<std::uint8_t>(charges);
        return true;
    }
    return false;
}

} // namespace

int itemCost(const World& world, const things::ModifierDb* modifiers, int mode, const TradeSession* session, const ServerObject& item)
{
    const auto gamedata = [&](std::string_view name) { return X87(world.number(name)); };
    const bool quest = world.quest();
    // The shopkeeper's InitData: a shop's non-player side.
    const ServerObject::Shopkeeper* keeper = nullptr;
    ServerObject::Shopkeeper defaults;
    if (session && session->shop) {
        const ServerObject* k = keeperOf(const_cast<World&>(world), *session);
        keeper = k && k->shopkeeper ? &*k->shopkeeper : &defaults;
    }
    // The base worth: its type's WORTH, a book's spell price or a guide's creature's worth, then its
    // modifiers' worth added.
    float worth = X87::fromDouble(typeWorth(world, item.typeName)).f(); // (Worth, FILD qword)
    if (item.classes & oc::InfoBook) {
        if (item.subclasses & 1u) {
            worth = X87::fromDouble(spellPrice(world, item.bookEntry) & 0xFFFF).f();
        } else if (item.subclasses & 2u) {
            // Its creature's worth (the type the guide names).
            if (world.thingDb && world.thingDb->findThing(item.guideCreature)) {
                worth = X87::fromDouble(typeWorth(world, item.guideCreature)).f();
            }
            if (quest) {
                worth = (gamedata("QuestGuideWorthMultiplier") * worth).f();
            }
        }
    }
    if (item.classes & kModifiable) {
        for (const std::uint8_t id : item.modifiers) {
            if (id == 0xFF) {
                continue;
            }
            const things::ModifierDef* def = modifiers ? modifiers->modifier(id) : nullptr; // (Obj_GetModifierById)
            const float modWorth = X87::fromDouble(def ? def->worth : 0).f(); // (FILD dword, FSTP)
            const X87 added = quest ? gamedata("QuestModifierWorthMultiplier") * modWorth : X87(modWorth);
            worth = (added + worth).f();
        }
    }
    float base = worth; // (the worth before its state: a repair's measure)
    if ((item.classes & oc::Weapon) && (item.subclasses & 0x82u)) {
        // Ammunition by its count against DefaultAmmoAmount (a quest's DefaultAmmoAmountQuest).
        if (!item.infiniteAmmo && item.maxCharges != 0) {
            const std::int32_t full = chop(gamedata(quest ? "DefaultAmmoAmountQuest" : "DefaultAmmoAmount").f());
            const float each = (X87(worth) / X87::fromDouble(full)).f();
            const std::int32_t left = item.charges;
            if (left <= full) {
                worth = (X87::fromDouble(left) / X87::fromDouble(full) * worth).f();
            } else {
                worth = (X87::fromDouble(left - full) * each + worth).f();
            }
        }
    } else if ((item.classes & oc::Wand) && (item.subclasses & 0x47F0000u) && item.maxCharges != 0) {
        worth = (X87::fromDouble(item.charges) / X87::fromDouble(item.maxCharges) * worth).f();
    }
    // Then the shopkeeper's buy multiplier and the item's wear.
    if (keeper && mode != 0) {
        worth = (X87(worth) * keeper->buyMultiplier).f();
        base = (X87(base) * keeper->buyMultiplier).f();
    }
    if (item.maxHealth != 0) {
        worth = (X87::fromDouble(item.health) / X87::fromDouble(item.maxHealth) * worth).f();
    }
    if (keeper && mode == 0) {
        const X87 sale = quest ? gamedata("QuestSellMultiplier") : X87(keeper->sellMultiplier);
        if (!isGem(item)) {
            worth = (sale * worth).f();
            base = (sale * base).f();
        }
    }
    if (quest && mode == 0 && (item.classes & 0x3001000u) && (item.itemFlags & 1u)) {
        base = 0.0f; // (a quest's outfit: sold for 1)
        worth = 1.0f;
    } else if (worth < 1.0f) {
        worth = 1.0f;
    }
    if (base < 1.0f) {
        base = 1.0f;
    }
    // A repair costs RepairCoefficient times what the wear took off its worth.
    if (mode == 2) {
        worth = (gamedata("RepairCoefficient") * (X87(base) - worth)).f();
        if (worth < 1.0f) {
            worth = 1.0f;
        }
    }
    return chop(worth);
}

TradeSession* tradeSession(World& world, std::uint32_t id)
{
    if (id == 0) {
        return nullptr;
    }
    const auto it = std::ranges::find(world.trades, id, &TradeSession::id);
    return it != world.trades.end() ? &*it : nullptr;
}

TradeSession* startTrade(Fight& fight, ServerObject& starter, ServerObject& other)
{
    World& world = fight.world;
    // Either side already trading: refused (the starter told unless it's the same pair again).
    const bool starterPlays = (starter.classes & oc::Player) != 0;
    if (starterPlays && starter.player && starter.player->trade != 0) {
        const TradeSession* busy = tradeSession(world, starter.player->trade);
        if (!busy || busy->b != other.extent) {
            world.textTo(starter, "Trade.c:StarterAlreadyTrading");
        }
        return nullptr;
    }
    const bool otherPlays = (other.classes & oc::Player) != 0;
    if (otherPlays && other.player && other.player->trade != 0) {
        if (starterPlays) {
            world.textTo(starter, "Trade.c:OtherAlreadyTrading", std::u16string_view(other.player->name));
        }
        return nullptr;
    }
    TradeSession* s = nullptr;
    // A player and a non-player: a shop; two players (or neither): a plain trade between them.
    if (starterPlays && !otherPlays) {
        s = playerShop(fight, starter, other);
    } else if (!starterPlays && otherPlays) {
        s = playerShop(fight, other, starter);
    } else {
        // (The exe writes the sides through createShopStruct's answer unchecked: with the pool
        // used up, a crash; not reachable in 1.2b, MSG_TRADE 0x15 wanting a shopkeeper:
        // an original bug. Here no trade.)
        s = newSession(world);
        if (s) {
            s->a = starter.extent;
            s->b = other.extent;
        }
    }
    if (!s) {
        return nullptr;
    }
    s->started = fight.frame;
    holdSession(world, *s, starter.extent);
    holdSession(world, *s, other.extent);
    for (ServerObject* side : {&starter, &other}) {
        if (side->classes & oc::Player) {
            if (s->shop) {
                sendStartShop(world, *s);
            } else {
                sendStartTrade(world, *side, *s);
            }
        }
    }
    if (s->shop) {
        // (servSendShopItems) The stock to its player, in order; a solo player held
        // still while it shops.
        if (ServerObject* p = playerOf(world, *s)) {
            const auto stock = s->stock;
            for (const auto& item : stock) {
                sendStockItem(world, *p, item);
            }
            if (world.gameFlags & GameFlag::Solo) {
                freezeUnit(fight, *p, 0); // (monsters.hpp)
            }
        }
    }
    return s;
}

void exitShop(Fight& fight, TradeSession& s)
{
    World& world = fight.world;
    letGo(world, s, s.a);
    letGo(world, s, s.b);
    ServerObject* p = playerOf(world, s);
    if (p) {
        unfreezeUnit(fight, *p, 0);
        tell(world, *p, {0xC9, 0x02}); // (SrvNet_SendTradeCloseShop)
    }
    if (!world.quest()) {
        freeSession(world, s.id);
        return;
    }
    if (p && p->player) {
        world.questShops[p->player->index & 31u] = s.id; // (kept for it: g_questShopSessionByPlayer)
    }
}

void closeTrade(Fight& fight, TradeSession& s)
{
    World& world = fight.world;
    // Each side's offers back to it, its gold put up, TRADE 0x01 (SrvNet_SendTradeCloseP2P). (The
    // exe gives the gold and the message to a shopkeeper too, through its monster data, when a
    // client sends 0x0E at a shop, an original bug; a player's alone here.)
    const auto giveBack = [&](std::uint32_t side, std::vector<TradeSession::Item>& offers, std::uint32_t gold) {
        ServerObject* holder = thing(world, side);
        for (const auto& item : std::exchange(offers, {})) {
            if (ServerObject* o = release(world, item.object); o && holder && fight.things) {
                itemsOf(fight).inventoryPut(*holder, *o, true);
            }
        }
        if (isPlayer(holder)) {
            addGold(*holder, static_cast<std::int32_t>(gold));
            tell(world, *holder, {0xC9, 0x01});
        }
    };
    giveBack(s.a, s.offersA, s.goldA);
    giveBack(s.b, s.offersB, s.goldB);
    letGo(world, s, s.a);
    letGo(world, s, s.b);
    freeSession(world, s.id);
}

void cancelTrade(Fight& fight, ServerObject& unit)
{
    if (!unit.player) {
        return;
    }
    if (TradeSession* s = tradeSession(fight.world, unit.player->trade)) {
        if (s->shop) {
            exitShop(fight, *s);
        } else {
            closeTrade(fight, *s);
        }
    }
    unit.player->trade = 0;
}

void dropQuestShop(World& world, std::uint8_t index)
{
    auto& slot = world.questShops[index & 31u];
    if (slot != 0) {
        freeSession(world, slot);
    }
    slot = 0;
}

void mapChangeTrades(Fight& fight)
{
    // mapSwitchLevel, each player (Plr_GetFirstUnit's order): thawed (1), its trade
    // cancelled. (Then every session left is freed: the new map starts with none.)
    for (ServerObject* u : playersInOrder(fight.world, [](const ServerObject& o) { return (o.classes & oc::Player) != 0; })) {
        unfreezeUnit(fight, *u, 1);
        cancelTrade(fight, *u);
    }
}

void onTradeMessage(Fight& fight, ServerObject& unit, std::span<const std::uint8_t> m)
{
    World& world = fight.world;
    if (!unit.player || m.size() < 2) {
        return;
    }
    PlayerControl& p = *unit.player;
    const auto u16 = [&](std::size_t at) { return at + 1 < m.size() ? static_cast<std::uint32_t>(m[at] | m[at + 1] << 8) : 0u; };
    TradeSession* s = tradeSession(world, p.trade);
    // MSG_TRADE's sub-code is byte 1; the thing codes after it are u16s from byte 2.
    switch (m[1]) {
    case 0x0E: // cancel (Trade_CloseP2P, whatever the session)
        if (s) {
            closeTrade(fight, *s);
        }
        break;
    case 0x0F: { // offer a thing it holds
        const std::uint32_t code = dynamicCode(world, u16(2));
        ServerObject* item = heldByCode(world, unit, code);
        if (item && s && addOffer(fight, *s, unit, *item) == 1) {
            if (fight.things) {
                itemsOf(fight).removeFromInventory(unit, *item);
            }
            keep(world, item->extent);
        }
        break;
    }
    case 0x10: // take an offer back
        if (const std::uint32_t code = dynamicCode(world, u16(2)); s) {
            withdrawOffer(fight, *s, code);
        }
        break;
    case 0x11: // accept
        if (s) {
            accept(fight, *s, unit);
        }
        break;
    case 0x12: // leave the shop
        if (s) {
            exitShop(fight, *s);
        }
        break;
    case 0x15: { // open a shop: not while a save waits nor observing; a shopkeeper (subclass 8)
        if (world.saveRequest.pending || (p.statusFlags & 3u)) {
            break;
        }
        ServerObject* other = world.byExtent(dynamicCode(world, u16(2)) & 0x7FFFu);
        if (other && !other->gone() && (other->subclasses & 8u)) {
            startTrade(fight, unit, *other);
        }
        break;
    }
    case 0x16: { // buy (Shop_OnBuyItem)
        if (!s) {
            break;
        }
        const std::uint32_t code = u16(2);
        const auto it = std::ranges::find_if(s->stock, [&](const TradeSession::Item& i) { return i.object != 0 && i.object == code; });
        if (it != s->stock.end()) {
            if (ServerObject* o = thing(world, it->object)) {
                buy(fight, unit, *s, p.gold, *o, false);
            }
        }
        break;
    }
    case 0x17: { // buy a stack (Shop_OnBuyStack): by type, its gold read once (the 1.2b bug)
        if (!s) {
            break;
        }
        const std::uint32_t type = u16(2);
        const std::uint32_t count = m.size() > 4 ? m[4] : 0;
        const std::uint32_t gold = p.gold;
        for (std::uint32_t n = 0; n < count; ++n) {
            ServerObject* o = nullptr;
            for (const auto& item : s->stock) {
                if (ServerObject* t = thing(world, item.object); t && t->type == type) {
                    o = t;
                    break;
                }
            }
            if (!o || !buy(fight, unit, *s, gold, *o, true)) {
                break;
            }
        }
        break;
    }
    case 0x18: { // sell (Shop_OnSellItem)
        ServerObject* item = s ? heldByCode(world, unit, u16(2)) : nullptr;
        if (item && !refuseSale(world, unit, *item)) {
            sell(fight, unit, *s, *item);
            playSound(world, 0x133, unit, 2, unit.extent);
        }
        break;
    }
    case 0x19: { // sell a stack (Shop_OnSellStack): by type, anything (equipped, quest items, glyphs)
        if (!s) {
            break;
        }
        const std::uint32_t type = u16(2);
        const std::uint32_t count = m.size() > 4 ? m[4] : 0;
        bool all = count != 0;
        for (std::uint32_t n = 0; n < count; ++n) {
            ServerObject* item = nullptr;
            for (const std::uint32_t e : unit.inventory) {
                if (ServerObject* o = world.byExtent(e); o && o->type == type) {
                    item = o;
                    break;
                }
            }
            if (!item) {
                all = false; // (out of them: returns before the sound)
                break;
            }
            sell(fight, unit, *s, *item);
        }
        if (all) {
            playSound(world, 0x133, unit, 2, unit.extent);
        }
        break;
    }
    case 0x1A: { // repair or recharge (Shop_OnRepairItem): neither its need nor the gold checked
        ServerObject* item = s ? heldByCode(world, unit, u16(2)) : nullptr;
        if (!item) {
            break;
        }
        subGold(unit, static_cast<std::uint32_t>(itemCost(world, fight.modifiers, 2, s, *item)));
        item->health = item->maxHealth; // (unitSetHP)
        item->touch();
        const std::uint16_t code = netCodeOf(*item);
        if (item->maxHealth != 0) { // (SrvNet_SendReportItemHealth)
            tell(world, unit, {0x44, lo(code), hi(code), lo(item->health), hi(item->health), lo(item->maxHealth), hi(item->maxHealth)});
        }
        if ((item->classes & oc::Wand) && (item->subclasses & 0x47F0000u) && recharge(*item, 100)) {  // a magical staff: recharged too
            tell(world, unit, {0x64, lo(code), hi(code), item->charges, item->maxCharges}); // (SrvNet_SendReportCharges)
        }
        reportGold(world, unit);
        playSound(world, 0x323, unit, 2, unit.extent);
        break;
    }
    case 0x1C: { // its price (Shop_OnAskSellPrice): TRADE 0x1D {u16 the code, u32 the price}
        const std::uint32_t code = u16(2);
        ServerObject* item = s ? heldByCode(world, unit, code) : nullptr;
        if (item && !refuseSale(world, unit, *item)) {
            std::vector<std::uint8_t> r = {0xC9, 0x1D, lo(code), hi(code)};
            putU32(r, static_cast<std::uint32_t>(itemCost(world, fight.modifiers, 0, s, *item)));
            tell(world, unit, std::move(r));
        }
        break;
    }
    case 0x1E: { // its repair's cost (Shop_OnAskRepairCost): TRADE 0x1F, or 0x39D when it needs none
        const std::uint32_t code = u16(2);
        ServerObject* item = s ? heldByCode(world, unit, code) : nullptr;
        if (!item) {
            break;
        }
        const bool charge = (item->classes & oc::Wand) && (item->subclasses & 0x47F0000u) && item->charges < item->maxCharges && item->maxCharges != 0;  // a magical staff short of charges
        if ((item->maxHealth != 0 && item->health != item->maxHealth) || charge) {
            std::vector<std::uint8_t> r = {0xC9, 0x1F, lo(code), hi(code)};
            putU32(r, static_cast<std::uint32_t>(itemCost(world, fight.modifiers, 2, s, *item)));
            tell(world, unit, std::move(r));
        } else {
            playSound(world, 0x39D, unit);
        }
        break;
    }
    default:
        break;
    }
}

void builtinIsTrading(World& world, script::Vm& vm)
{
    const ServerObject* host = hostPlayer(world);
    vm.push(host && host->player && host->player->trade != 0 ? 1u : 0u);
}

void builtinSetShopkeeperText(World& world, script::Vm& vm, const std::function<ServerObject*()>& popObject)
{
    (void)world;
    const std::string text = vm.string(vm.pop());
    ServerObject* o = popObject();
    if (o && (o->classes & oc::Monster) && (o->subclasses & things::ObjectSubclass::Shopkeeper) && text.size() < 0x20) {
        keeperData(*o).greeting = text;
    }
}

} // namespace nox::server
