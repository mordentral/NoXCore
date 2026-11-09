// Items with their modifiers, and a player's own facts (mod_items.hpp).
#include "mod_items.hpp"

#include "mod_parts.hpp"
#include "toolbox.hpp"

#include "noxcore/mapedit/content.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/rewards.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace nox::modes::script {

namespace oc = things::ObjectClass;

namespace {

/// The four slots by name, in the engine's order.
constexpr std::array<const char*, 4> kSlots = {"effectiveness", "material", "primary", "secondary"};
constexpr std::array<std::string_view, 4> kSections = {"EFFECTIVENESS", "MATERIAL", "ENCHANTMENT", "ENCHANTMENT"};

const things::ModifierDb& modifiersArg(lua_State* L)
{
    const things::ModifierDb* db = toolbox::modifiersOf(L);
    if (!db) {
        luaL_error(L, "this host has no modifier.bin");
    }
    return *db;
}

int slotArg(lua_State* L, int idx)
{
    const std::string_view s = luaL_checkstring(L, idx);
    for (std::size_t i = 0; i < kSlots.size(); ++i) {
        if (s == kSlots[i]) {
            return static_cast<int>(i);
        }
    }
    luaL_argerror(L, idx, "a slot: effectiveness, material, primary or secondary");
    return 0;
}

/// A modifier of slot `slot` by its name, its id; an error for one modifier.bin hasn't or of
/// another section.
std::uint8_t modifierIdArg(lua_State* L, int idx, int slot, const char* what)
{
    const things::ModifierDb& db = modifiersArg(L);
    const std::string_view name = luaL_checkstring(L, idx);
    const things::ModifierDef* m = db.findModifier(name);
    if (!m) {
        luaL_error(L, "%s: modifier.bin has no modifier %s", what, std::string(name).c_str());
    }
    if (m->section != kSections[static_cast<std::size_t>(slot)]) {
        luaL_error(L, "%s: %s is an %s modifier, not a %s one", what, m->name.c_str(), m->section.c_str(), kSlots[static_cast<std::size_t>(slot)]);
    }
    return m->id;
}

bool isItem(const server::ServerObject& o)
{
    return !o.player && !(o.classes & oc::Monster) &&
           (o.classes & (oc::Weapon | oc::Armor | oc::Wand | oc::Food | oc::Key | oc::InfoBook | oc::Flag | oc::Treasure | oc::Pickup)) != 0;
}

/// Whether an item with these modifiers is one the game makes (questModifiersValid's rule; a
/// thing that takes none must have none).
bool modifiersFit(const server::ServerObject& item, const things::ModifierDb& db)
{
    const bool none = std::ranges::all_of(item.modifiers, [](std::uint8_t m) { return m == 0xFF; });
    if (none) {
        return true;
    }
    if (!(item.classes & (oc::Weapon | oc::Armor | oc::Wand))) {
        return false;
    }
    return server::questModifiersValid(item, db);
}

void pushModifierName(lua_State* L, const things::ModifierDb* db, std::uint8_t id)
{
    const things::ModifierDef* m = db ? db->modifier(id) : nullptr;
    m ? (void)lua_pushlstring(L, m->name.data(), m->name.size()) : lua_pushnil(L);
}

void setNumber(lua_State* L, const char* name, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, name);
}

/// A string id's text (UTF-8) as field `name`; nothing for no id, no table or no such entry.
void setText(lua_State* L, const strings::StringTable* table, const char* name, const std::string& id)
{
    if (!table || id.empty()) {
        return;
    }
    if (const auto t = table->text(id)) {
        const std::string s = strings::toUtf8(*t);
        lua_pushlstring(L, s.data(), s.size());
        lua_setfield(L, -2, name);
    }
}

/// The damage types by number (the game's table, in order), as a script names them.
constexpr std::array<const char*, 18> kDamageTypes = {"blade",  "flame",   "crush", "impale",    "drain",    "poison",   "dispelUndead", "explosion", "bite",
                                                      "electric", "claw", "impact", "lava", "deathMagic", "plasma", "manaBomb", "zapRay", "airborneElectric"};

// ---- nox.item, nox.modifier, nox.modifiers, nox.modifierFits

int noxItem(lua_State* L)
{
    // nox.item(id): an item's own facts (nil: gone, or not an item).
    const double id = luaL_checknumber(L, 1);
    server::World& world = toolbox::worldOf(L);
    const server::ServerObject* o = id >= 1 && id <= 4294967295.0 ? world.byExtent(static_cast<std::uint32_t>(id)) : nullptr;
    if (!o || o->gone() || !isItem(*o)) {
        lua_pushnil(L);
        return 1;
    }
    const things::ModifierDb* db = toolbox::modifiersOf(L);
    lua_createtable(L, 0, 11);
    lua_pushlstring(L, o->typeName.data(), o->typeName.size());
    lua_setfield(L, -2, "type");
    for (std::size_t i = 0; i < kSlots.size(); ++i) {
        pushModifierName(L, db, o->modifiers[i]);
        lua_setfield(L, -2, kSlots[i]);
    }
    setNumber(L, "durability", o->health);
    setNumber(L, "maxDurability", o->maxHealth);
    setNumber(L, "charges", o->charges);
    setNumber(L, "maxCharges", o->maxCharges);
    lua_pushboolean(L, (o->flags & server::ObjFlag::Equipped) != 0);
    lua_setfield(L, -2, "worn");
    if (o->holder) {
        setNumber(L, "holder", o->holder);
    }
    return 1;
}

int noxModifier(lua_State* L)
{
    // nox.modifier(name): modifier.bin's entry (nil: none of that name).
    const things::ModifierDb& db = modifiersArg(L);
    const things::ModifierDef* m = db.findModifier(luaL_checkstring(L, 1));
    if (!m) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    lua_pushlstring(L, m->name.data(), m->name.size());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, m->section == "EFFECTIVENESS" ? "effectiveness" : m->section == "MATERIAL" ? "material" : "enchantment");
    lua_setfield(L, -2, "section");
    lua_pushstring(L, m->allowedPosition == 1 ? "primary" : m->allowedPosition == 2 ? "secondary" : "any");
    lua_setfield(L, -2, "position");
    lua_createtable(L, 0, 3);
    setNumber(L, "r", m->color[0]);
    setNumber(L, "g", m->color[1]);
    setNumber(L, "b", m->color[2]);
    lua_setfield(L, -2, "color");
    setNumber(L, "worth", m->worth);
    lua_pushlstring(L, m->desc.data(), m->desc.size());
    lua_setfield(L, -2, "desc");
    // Its words as the game shows them (the string table's: none without nox.csf), and its effects'
    // numbers by function name.
    const strings::StringTable* table = toolbox::worldOf(L).strings;
    setText(L, table, "text", m->desc);
    setText(L, table, "identify", m->identifyDesc);
    lua_createtable(L, 0, static_cast<int>(m->effects.size()));
    for (const auto& [name, value] : m->effects) {
        lua_pushnumber(L, value);
        lua_setfield(L, -2, name.c_str());
    }
    lua_setfield(L, -2, "effects");
    return 1;
}

int noxItemStats(lua_State* L)
{
    // nox.itemStats(type): a type's base stats as the game makes and reads it (nil: no such type).
    const things::ThingDb& db = toolbox::thingsOf(L);
    const things::ThingDef* def = nullptr;
    if (lua_type(L, 1) == LUA_TNUMBER) {
        const double n = lua_tonumber(L, 1);
        def = n >= 1 && n <= static_cast<double>(db.things.size()) && n == std::floor(n) ? &db.things[static_cast<std::size_t>(n) - 1] : nullptr;
    } else {
        def = db.findThing(luaL_checkstring(L, 1));
    }
    if (!def) {
        lua_pushnil(L);
        return 1;
    }
    server::World& world = toolbox::worldOf(L);
    const things::ModifierDb* mods = toolbox::modifiersOf(L);
    // (The type as made: its durability and charges as nox.create's would have them.)
    const server::ServerObject o = server::describeType(world, db, def->name);
    lua_createtable(L, 0, 12);
    lua_pushlstring(L, def->name.data(), def->name.size());
    lua_setfield(L, -2, "type");
    if (const std::string name = mapedit::thingDisplayName(*def, world.strings, mods); !name.empty()) {
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "name");
    }
    setNumber(L, "durability", o.maxHealth);
    setNumber(L, "charges", o.maxCharges);
    // Its weapon or armour record (modifier.bin), by the class the game asks it by.
    const bool weaponLike = (o.classes & (oc::Weapon | oc::Wand | oc::Flag)) != 0;
    const things::EquipDef* equip = !mods ? nullptr : weaponLike ? mods->findWeapon(def->name) : (o.classes & oc::Armor) ? mods->findArmor(def->name) : nullptr;
    if (equip && weaponLike) {
        setNumber(L, "damageMin", equip->damageMin);
        setNumber(L, "damageCoefficient", equip->damageCoefficient);
        setNumber(L, "requiredStrength", equip->requiredStrength);
        setNumber(L, "range", equip->range);
        if (const auto type = server::meleeDamageType(things::weaponBitOf(def->name)); type && *type < kDamageTypes.size()) {
            lua_pushstring(L, kDamageTypes[*type]);
            lua_setfield(L, -2, "damageType");
        }
    } else if (equip) {
        setNumber(L, "armor", equip->armorValue);
    }
    // Which class may use or wear it (a multiclass player: any).
    lua_createtable(L, 0, 3);
    static constexpr const char* kClasses[] = {"warrior", "wizard", "conjurer"};
    for (std::uint8_t c = 0; c < 3; ++c) {
        lua_pushboolean(L, server::classMayUse(c, o, mods));
        lua_setfield(L, -2, kClasses[c]);
    }
    lua_setfield(L, -2, "classes");
    return 1;
}

int noxModifiers(lua_State* L)
{
    // nox.modifiers(section): every modifier's name of a section, in modifier.bin's order.
    const std::string_view s = luaL_checkstring(L, 1);
    const std::string_view section = s == "effectiveness" ? "EFFECTIVENESS" : s == "material" ? "MATERIAL" : s == "enchantment" ? "ENCHANTMENT" : "";
    luaL_argcheck(L, !section.empty(), 1, "a section: effectiveness, material or enchantment");
    const things::ModifierDb& db = modifiersArg(L);
    lua_createtable(L, 64, 0);
    int n = 0;
    for (const things::ModifierDef& m : db.modifiers) {
        if (m.section == section) {
            lua_pushlstring(L, m.name.data(), m.name.size());
            lua_rawseti(L, -2, ++n);
        }
    }
    return 1;
}

int noxModifierFits(lua_State* L)
{
    // nox.modifierFits(type, slot, name): whether that modifier in that slot of a new item of the
    // type is one the game makes (the rule giveItem and create hold a spec to).
    const things::ThingDb& db = toolbox::thingsOf(L);
    const std::string_view type = luaL_checkstring(L, 1);
    const int slot = slotArg(L, 2);
    const things::ThingDef* def = db.findThing(type);
    luaL_argcheck(L, def != nullptr, 1, "thing.bin has no such type");
    const things::ModifierDb& mods = modifiersArg(L);
    const things::ModifierDef* m = mods.findModifier(luaL_checkstring(L, 3));
    if (!m || m->section != kSections[static_cast<std::size_t>(slot)]) {
        lua_pushboolean(L, 0);
        return 1;
    }
    server::ServerObject probe;
    probe.typeName = server::Name(def->name);
    probe.classes = things::thingClasses(*def);
    probe.subclasses = things::thingSubclasses(*def);
    probe.modifiers[static_cast<std::size_t>(slot)] = m->id;
    lua_pushboolean(L, modifiersFit(probe, mods));
    return 1;
}

// ---- nox.repairCost, nox.repair, nox.setDurability

server::ServerObject& itemArg(lua_State* L, int idx)
{
    server::ServerObject& item = toolbox::objectArg(L, idx);
    luaL_argcheck(L, isItem(item), idx, "an item");
    return item;
}

/// The unit holding it (its client is told), or null.
const server::ServerObject* holderOf(server::World& world, const server::ServerObject& item)
{
    return item.holder != 0 ? world.byExtent(item.holder) : nullptr;
}

int noxRepairCost(lua_State* L)
{
    // nox.repairCost(item): the game's shop's repair price (a keeper's own multiplier aside: a stock
    // keeper's 1); nil when it needs none.
    const server::ServerObject& item = itemArg(L, 1);
    if (!server::needsRepair(item)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, server::itemCost(toolbox::worldOf(L), toolbox::modifiersOf(L), 2, nullptr, item));
    return 1;
}

int noxRepair(lua_State* L)
{
    // nox.repair(item): the shop's repair, no gold taken (durability and a staff's charges to their
    // most, told to its holder's client); whether it needed one (nothing done when not).
    toolbox::refuseInsideAsked(L, "nox.repair");
    server::ServerObject& item = itemArg(L, 1);
    if (!server::needsRepair(item)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    server::World& world = toolbox::worldOf(L);
    server::repairItem(world, item, holderOf(world, item));
    lua_pushboolean(L, 1);
    return 1;
}

int noxSetDurability(lua_State* L)
{
    // nox.setDurability(item, n): its durability, 1 to its most (more is its most), told to its
    // holder's client as a repair tells it; false for a thing without durability.
    toolbox::refuseInsideAsked(L, "nox.setDurability");
    server::ServerObject& item = itemArg(L, 1);
    const double n = luaL_checknumber(L, 2);
    luaL_argcheck(L, n >= 1 && n <= 65535 && n == std::floor(n), 2, "a whole number from 1");
    if (item.maxHealth == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    server::World& world = toolbox::worldOf(L);
    server::setItemHealth(world, item, static_cast<std::uint16_t>(std::min<double>(n, item.maxHealth)), holderOf(world, item));
    lua_pushboolean(L, 1);
    return 1;
}

// ---- nox.equip, nox.unequip

int noxEquip(lua_State* L)
{
    // nox.equip(unit, item): put on (the engine's own: Obj_PlayerEquipWeapon, a staff too, or
    // ...Armor), told to everyone. true, or false and why (the engine's refusal: its class, its
    // strength, already worn, not its own).
    toolbox::refuseInsideAsked(L, "nox.equip");
    server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    server::ServerObject& item = toolbox::objectArg(L, 2);
    const char* why = nullptr;
    if (item.holder != u.extent) {
        why = "not its own";
    } else if (item.flags & server::ObjFlag::Equipped) {
        why = "worn already";
    } else if (!(item.classes & (oc::Weapon | oc::Wand | oc::Armor))) {
        why = "not a weapon or armour";
    }
    if (!why) {
        server::Items items = toolbox::hostOf(L)->server().items();
        const bool worn = (item.classes & oc::Armor) ? items.equipArmor(u, item, true) : items.equipWeapon(u, item, true);
        if (worn) {
            lua_pushboolean(L, 1);
            return 1;
        }
        why = !items.classMay(u, item) ? "its class can't use it" : !items.strongEnough(u, item) ? "not strong enough" : "refused";
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, why);
    return 2;
}

int noxUnequip(lua_State* L)
{
    // nox.unequip(unit, item): taken off (the engine's), told to everyone; whether it was worn.
    toolbox::refuseInsideAsked(L, "nox.unequip");
    server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    server::ServerObject& item = toolbox::objectArg(L, 2);
    if (item.holder != u.extent || !(item.flags & server::ObjFlag::Equipped)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    toolbox::hostOf(L)->server().items().takeOff(u, item, true, true);
    lua_pushboolean(L, (item.flags & server::ObjFlag::Equipped) == 0);
    return 1;
}

int noxSecondaryWeapon(lua_State* L)
{
    // nox.secondaryWeapon(unit): the item its client holds as its secondary weapon (nil: none, or
    // one no longer carried, or in hand now).
    const server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    const std::uint16_t code = u.player->secondaryWeapon;
    const server::ServerObject* item = code != 0 ? toolbox::worldOf(L).byPacketCode(server::NetCode{code}) : nullptr;
    if (!item || item->gone() || item->holder != u.extent || (item->flags & server::ObjFlag::Equipped)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, item->extent);
    return 1;
}

// ---- a player's level and experience

int noxSetLevel(lua_State* L)
{
    // nox.setLevel(unit, level): the stock level (1-10) as the engine sets it (its values read
    // again: health and mana full; told to its client).
    toolbox::refuseInsideAsked(L, "nox.setLevel");
    server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    const double level = luaL_checknumber(L, 2);
    luaL_argcheck(L, level >= 1 && level <= 10 && level == std::floor(level), 2, "a level, 1-10");
    u.player->level = static_cast<std::uint8_t>(level);
    server::readVals(toolbox::worldOf(L), u, true);
    return 0;
}

int noxSetExperience(lua_State* L)
{
    // nox.setExperience(unit, xp): its experience set and told to its client
    // (MSG_REPORT_EXPERIENCE); no level up.
    toolbox::refuseInsideAsked(L, "nox.setExperience");
    server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    const double xp = luaL_checknumber(L, 2);
    luaL_argcheck(L, xp >= 0 && xp <= 2147483647.0, 2, "an amount from 0");
    u.experience = static_cast<float>(xp);
    server::reportExperience(toolbox::worldOf(L), u);
    return 0;
}

int noxGiveExperience(lua_State* L)
{
    // nox.giveExperience(unit, xp): the engine's Plr_GiveExperience (its text, a level up when due).
    toolbox::refuseInsideAsked(L, "nox.giveExperience");
    server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    const double xp = luaL_checknumber(L, 2);
    luaL_argcheck(L, xp >= 0 && xp <= 1'000'000'000.0, 2, "an amount from 0");
    server::Fight f = toolbox::fightOf(L);
    server::giveExperience(f, u, static_cast<float>(xp));
    return 0;
}

} // namespace

ItemSpec itemSpecArg(lua_State* L, int idx)
{
    ItemSpec spec;
    if (lua_isnoneornil(L, idx)) {
        return spec;
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    idx = lua_absindex(L, idx);
    for (std::size_t i = 0; i < kSlots.size(); ++i) {
        lua_rawgetfield(L, idx, kSlots[i]);
        if (!lua_isnil(L, -1) && !(lua_isboolean(L, -1) && !lua_toboolean(L, -1))) {
            spec.mods[i] = modifierIdArg(L, lua_gettop(L), static_cast<int>(i), kSlots[i]);
        }
        lua_pop(L, 1);
    }
    const auto whole = [&](const char* key, double lo, double hi) -> std::optional<int> {
        lua_rawgetfield(L, idx, key);
        std::optional<int> v;
        if (!lua_isnil(L, -1)) {
            const double n = luaL_checknumber(L, -1);
            if (!(n >= lo && n <= hi && n == std::floor(n))) {
                luaL_error(L, "the spec's %s is a whole number, %d-%d", key, static_cast<int>(lo), static_cast<int>(hi));
            }
            v = static_cast<int>(n);
        }
        lua_pop(L, 1);
        return v;
    };
    spec.durability = whole("durability", 1, 65535);
    spec.charges = whole("charges", 0, 255);
    return spec;
}

bool finishItem(lua_State* L, server::ServerObject& item, const ItemSpec& spec, std::string& why)
{
    if (spec.mods != std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0xFF}) {
        const things::ModifierDb* db = toolbox::modifiersOf(L);
        if (item.modifiers != spec.mods) {
            why = std::string(item.typeName.view()) + " takes no modifiers";
            return false;
        }
        if (!db || !modifiersFit(item, *db)) {
            why = "those modifiers don't fit " + std::string(item.typeName.view()) + " (nox.modifierFits)";
            return false;
        }
    }
    if (spec.durability && item.maxHealth != 0) {
        item.health = static_cast<std::uint16_t>(std::clamp<int>(*spec.durability, 1, item.maxHealth));
    }
    if (spec.charges && item.maxCharges != 0) {
        item.charges = static_cast<std::uint8_t>(std::min<int>(*spec.charges, item.maxCharges));
    }
    return true;
}

void addPlayerFacts(lua_State* L, const server::HostedPlayer& p, const server::ServerObject* unit)
{
    // The account as the join gave it (ASCII, up to its end or 10 letters, trailing spaces off).
    std::string account;
    for (const std::uint8_t c : p.accept.account) {
        if (c == 0) {
            break;
        }
        account.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
    while (!account.empty() && account.back() == ' ') {
        account.pop_back();
    }
    lua_pushlstring(L, account.data(), account.size());
    lua_setfield(L, -2, "account");
    if (unit && unit->player) {
        static constexpr const char* kClasses[] = {"warrior", "wizard", "conjurer"};
        const std::uint8_t c = unit->player->classId;
        lua_pushstring(L, c < 3 ? kClasses[c] : "warrior");
        lua_setfield(L, -2, "class");
        setNumber(L, "level", unit->player->level);
        setNumber(L, "experience", unit->experience);
    }
}

void addItemsToolbox(lua_State* L)
{
    static constexpr std::pair<const char*, lua_CFunction> kFns[] = {
        {"item", noxItem},           {"modifier", noxModifier},     {"modifiers", noxModifiers},  {"itemStats", noxItemStats},
        {"modifierFits", noxModifierFits}, {"equip", noxEquip},     {"unequip", noxUnequip}, {"secondaryWeapon", noxSecondaryWeapon},
        {"repairCost", noxRepairCost}, {"repair", noxRepair},       {"setDurability", noxSetDurability},
        {"setLevel", noxSetLevel},   {"setExperience", noxSetExperience}, {"giveExperience", noxGiveExperience},
    };
    for (const auto& [name, fn] : kFns) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
}

} // namespace nox::modes::script
