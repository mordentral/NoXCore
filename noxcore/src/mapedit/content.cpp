// The content browser's catalog (content.hpp): the folder rule table, search, and the old
// community editor's categories.xml rules as user categories.
#include "noxcore/mapedit/content.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

namespace nox::mapedit {

namespace {

namespace oc = things::ObjectClass;

std::string upper(std::string_view s)
{
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool has(const std::vector<std::string>& names, std::string_view n)
{
    return std::ranges::find(names, n) != names.end();
}

bool anyOf(std::string_view name, std::initializer_list<std::string_view> words)
{
    for (const auto w : words) {
        if (name.find(w) != std::string_view::npos) return true;
    }
    return false;
}

/// The scenery's folders, by words in the name (case-sensitive), first match wins.
struct Theme {
    std::string_view folder;
    std::initializer_list<std::string_view> words;
};

const std::array<Theme, 19>& themes()
{
    static const std::array<Theme, 19> kThemes = {{
        {"Bodies & Bones", {"Corpse", "Bone", "Skull"}},
        {"Containers", {"Chest", "Barrel", "Crate", "Sack", "Coffin", "CinderBin", "Cauldron", "Spitoon", "WaspNest"}},
        {"Bridges & Stairs", {"Bridge", "Stairs", "Dock", "Portcullis"}},
        {"Water & Shore", {"Water", "Ripples", "Fountain", "Well", "Pool", "Beach", "LillyPad", "Bubbles"}},
        {"Lava, Ice & Mud", {"Lava", "Ice", "Mud", "Snow"}},
        {"Trees & Plants", {"Tree", "Bush", "Plant", "Conifer", "Aspen", "Decidious", "Stump", "Foliage", "Flowers", "Garden", "Grass", "Mushroom",
                            "Leaves", "Log", "Fern", "Forest", "Willow", "Mangrove", "Straw", "Vine"}},
        {"Rocks & Crystals", {"Rock", "Boulder", "Stalagmite", "Pebble", "Brick", "Crystal", "Block"}},
        {"Tombs & Shrines", {"Tomb", "Crypt", "Cross", "Ankh", "Altar", "Shrine", "Grave"}},
        {"Columns & Statues", {"Column", "Pillar", "Statue", "Gargoyle", "Monument", "Ruins", "Tower", "Arch", "Sundial", "StoneHand", "StoneFist"}},
        {"Lights & Fire", {"Torch", "Lantern", "Lamp", "Candle", "Brazier", "Flame", "Fire", "Sconse", "Basin", "Candleabra", "FairyJar", "Vandegraf",
                           "Light"}},
        {"Furniture", {"Chair", "Table", "Bench", "Bed", "Stool", "Desk", "Bookcase", "Book", "Shelves", "Shelf", "Cot", "Nightstand", "Throne",
                       "Rug", "Tapestry", "Painting", "Mirror", "Stove", "Rack", "Hammock", "Pelt", "Bearskin", "Trophy", "Banner", "Cushion",
                       "Alcove", "Stocks", "Maiden", "Torture", "Scroll", "Curtain", "Cabinet", "Dresser"}},
        {"Machines & Tools", {"Gear", "Pulley", "Mech", "Orrery", "Telescope", "Bellows", "Anvil", "Windmill", "Mine", "Mining", "StarChart",
                              "Zodiac", "Workstation", "Alchemist", "Flying", "Hammer", "Tools", "Shovel", "Cart"}},
        {"Traders", {"Trader", "Tent", "Teepee"}},
        {"Ix Temple", {"IxTemple"}},
        {"Land of the Dead", {"LOTD"}},
        {"Dun Mir", {"DunMir"}},
        {"Galava", {"Galava"}},
        {"Ogres & Urchins", {"Ogre", "Urchin"}},
        {"Webs & Debris", {"Web", "Debris", "Bar", "Fence", "Rope", "Pole", "Post", "Sign", "Arrow"}},
    }};
    return kThemes;
}

std::string monsterFolder(const std::vector<std::string>& sub, std::string_view xfer)
{
    if (has(sub, "SHOPKEEPER")) return "NPCs/Shopkeepers";
    if (has(sub, "FEMALE_NPC")) return "NPCs/Maidens";
    if (has(sub, "WOUNDED_NPC")) return "NPCs/Wounded";
    if (has(sub, "NPC") || xfer == "NPCXfer") return "NPCs/NPCs";
    if (has(sub, "BOMBER")) return "Monsters/Bombers";
    if (has(sub, "UNDEAD")) return "Monsters/Undead";
    if (has(sub, "LARGE_MONSTER")) return "Monsters/Large";
    if (has(sub, "MEDIUM_MONSTER")) return "Monsters/Medium";
    if (has(sub, "SMALL_MONSTER")) return "Monsters/Small";
    return "Monsters/Other";
}

std::string weaponFolder(const std::vector<std::string>& sub, std::uint32_t classes)
{
    const auto any = [&](std::initializer_list<std::string_view> names) {
        return std::ranges::any_of(names, [&](std::string_view n) { return has(sub, n); });
    };
    if (any({"SWORD", "LONG_SWORD", "GREAT_SWORD"})) return "Weapons/Swords";
    if (any({"AXE", "OGRE_AXE"})) return "Weapons/Axes";
    if (any({"MACE", "HAMMER"})) return "Weapons/Maces & Hammers";
    if (any({"BOW", "QUIVER", "ARROW"})) return "Weapons/Bows & Arrows";
    if (any({"CROSSBOW", "BOLT"})) return "Weapons/Crossbows";
    if (any({"CHAKRAM", "SHURIKEN"})) return "Weapons/Throwing";
    if ((classes & oc::Wand) || std::ranges::any_of(sub, [](const std::string& s) { return s.starts_with("STAFF"); })) return "Weapons/Staves & Wands";
    return "Weapons/Other";
}

std::string armorFolder(const std::vector<std::string>& sub)
{
    if (has(sub, "HELMET")) return "Armour/Helmets";
    if (has(sub, "SHIELD")) return "Armour/Shields";
    if (has(sub, "BREASTPLATE") || has(sub, "SHIRT")) return "Armour/Body";
    if (has(sub, "ARM_ARMOR")) return "Armour/Arms";
    if (has(sub, "PANTS") || has(sub, "LEG_ARMOR")) return "Armour/Legs";
    if (has(sub, "BOOTS")) return "Armour/Boots";
    if (has(sub, "BACK")) return "Armour/Back";
    return "Armour/Other";
}

/// The rule table for a placeable thing (contentFolder adds "Hidden/" for the others).
std::string folderOf(const things::ThingDef& t)
{
    const std::string_view name = t.name;
    const std::uint32_t c = things::thingClasses(t);
    const auto sub = subclassNames(t);
    const std::string xfer(t.lastAttribute("XFER").value_or("DefaultXfer"));

    // Markers: starts, the game modes' bases, invisible blockers, the scripts' marks.
    if (name == "PlayerStart" || name == "SaveGameLocation") return "Markers/Starts";
    if (name.starts_with("Extent")) return "Markers/Blockers";
    if (name.ends_with("Marker") || name == "TeamBase" || name == "HomeBase" || name == "GameBallStart") return "Markers/Other";
    if (name.starts_with("Amb") && t.attribute("AUDIOLOOP")) return "Ambient Sounds";
    if (c & oc::MonsterGenerator) return "Generators";
    if ((c & oc::Monster) || xfer == "MonsterXfer" || xfer == "NPCXfer") return monsterFolder(sub, xfer);
    if (c & oc::Flag) return "Keys, Treasure & Flags/Flags & Balls";
    if ((c & (oc::Weapon | oc::Wand)) || xfer == "WeaponXfer" || xfer == "AmmoXfer") return weaponFolder(sub, c);
    if ((c & oc::Armor) || xfer == "ArmorXfer") return armorFolder(sub);
    if (c & oc::Food) return std::ranges::any_of(sub, [](const std::string& s) { return s.find("POTION") != std::string::npos; }) ? "Potions & Food/Potions"
                                                                                                                              : "Potions & Food/Food";
    if ((c & oc::InfoBook) || xfer == "SpellRewardXfer" || xfer == "AbilityRewardXfer" || xfer == "FieldGuideXfer") return "Books";
    if (c & oc::Key) return "Keys, Treasure & Flags/Keys";
    if ((c & oc::Treasure) || xfer == "GoldXfer") return "Keys, Treasure & Flags/Treasure & Gold";
    if (xfer == "TeamXfer" || name == "GameBall" || name == "Crown") return "Keys, Treasure & Flags/Flags & Balls";
    if (c & oc::Door) return "Doors";
    if (c & oc::Exit) return "Exits & Travel/Exits";
    if (c & oc::Transporter) return "Exits & Travel/Teleporters";
    if (c & (oc::Elevator | oc::ElevatorShaft)) return "Exits & Travel/Elevators";
    if (c & oc::Hole) return "Exits & Travel/Pits";
    if (c & oc::Trigger) return "Triggers & Plates";
    if (c & oc::Readable) return "Readables & Signs";
    if (xfer == "ObeliskXfer") return "Obelisks";
    if (xfer == "InvisibleLightXfer") return "Lights";
    if (xfer == "SentryXfer" || xfer == "GlyphXfer" || xfer == "ToxicCloudXfer") return "Traps & Hazards";
    if ((c & (oc::Dangerous | oc::Fire)) && !(c & oc::Monster)) return "Traps & Hazards";
    if (xfer == "RewardMarkerXfer" || xfer == "SpellPagePedestalXfer") return "Quest Rewards";
    if (xfer == "MoverXfer") return "Movers";
    if (t.attribute("PICKUP") && !(c & oc::Immobile)) return "Items";
    for (const Theme& th : themes()) {
        if (anyOf(name, th.words)) return "Decorations/" + std::string(th.folder);
    }
    return "Decorations/Other";
}

/// The top-level folders' display order (the rule table's).
constexpr std::string_view kTopOrder[] = {"Monsters", "NPCs", "Generators", "Weapons", "Armour", "Potions & Food", "Books",
                                          "Keys, Treasure & Flags", "Items", "Doors", "Exits & Travel", "Triggers & Plates",
                                          "Traps & Hazards", "Lights", "Readables & Signs", "Obelisks", "Quest Rewards", "Movers",
                                          "Decorations", "Ambient Sounds", "Markers", "Hidden"};

std::size_t topRank(std::string_view folder)
{
    const auto top = folder.substr(0, folder.find('/'));
    for (std::size_t i = 0; i < std::size(kTopOrder); ++i) {
        if (kTopOrder[i] == top) return i;
    }
    return std::size(kTopOrder);
}

/// Folder order: by top-level rank, then path components by name with "Other" last.
bool folderLess(const std::string& a, const std::string& b)
{
    const auto ra = topRank(a), rb = topRank(b);
    if (ra != rb) return ra < rb;
    std::string_view x = a, y = b;
    while (true) {
        const auto ex = x.find('/'), ey = y.find('/');
        const auto px = x.substr(0, ex), py = y.substr(0, ey);
        if (px != py) {
            const bool ox = px == "Other", oy = py == "Other";
            if (ox != oy) return oy;
            return px < py;
        }
        if (ex == std::string_view::npos || ey == std::string_view::npos) return ex == std::string_view::npos && ey != std::string_view::npos;
        x.remove_prefix(ex + 1);
        y.remove_prefix(ey + 1);
    }
}

} // namespace

std::vector<std::string> subclassNames(const things::ThingDef& thing)
{
    std::vector<std::string> out;
    const auto v = thing.attribute("SUBCLASS");
    if (!v) return out;
    std::string_view rest = *v;
    while (!rest.empty()) {
        const auto plus = rest.find_first_of("+ \t|");
        const std::string_view w = rest.substr(0, plus);
        if (!w.empty() && upper(w) != "NULL") out.push_back(upper(w));
        if (plus == std::string_view::npos) break;
        rest.remove_prefix(plus + 1);
    }
    return out;
}

bool placeableType(const things::ThingDef& thing)
{
    return thing.menuIcon && thing.menuIcon->index != -1;
}

std::string contentFolder(const things::ThingDef& thing)
{
    if (placeableType(thing)) return folderOf(thing);
    const std::uint32_t c = things::thingClasses(thing);
    if (c & oc::Missile) return "Hidden/Missiles";
    if (c & oc::MonsterGenerator) return "Hidden/Generators";
    if (c & oc::Monster) return "Hidden/Monsters";
    if (c & (oc::Weapon | oc::Armor | oc::Wand)) return "Hidden/Items";
    return "Hidden/Effects";
}

std::string thingDisplayName(const things::ThingDef& thing, const strings::StringTable* strings, const things::ModifierDb* modifiers)
{
    if (!strings) return {};
    const auto text = [&](const std::string& id) -> std::string {
        if (id.empty()) return {};
        const auto t = strings->text(id);
        return t ? strings::toUtf8(*t) : std::string();
    };
    const std::uint32_t classes = things::thingClasses(thing);
    // Weapons, armour, staffs and flags: the definition's DESC, the tooltip's base.
    if (modifiers && (classes & (oc::Weapon | oc::Armor | oc::Wand | oc::Flag))) {
        const things::EquipDef* def = (classes & (oc::Weapon | oc::Wand | oc::Flag)) ? modifiers->findWeapon(thing.name) : modifiers->findArmor(thing.name);
        if (def) {
            if (auto s = text(def->desc); !s.empty()) return s;
        }
    }
    // pretty_name: PRETTYNAME, else "thing.db:<name>PrettyName" for a thing with a WEIGHT.
    std::string id;
    if (const auto p = thing.attribute("PRETTYNAME")) {
        id = std::string(*p);
    } else if (const auto w = thing.attribute("WEIGHT"); w && std::atoi(std::string(*w).c_str()) != 0) {
        id = "thing.db:" + thing.name + "PrettyName";
    }
    if (auto s = text(id); !s.empty()) return s;
    // Creatures: the field guide's title.
    if (classes & oc::Monster) return text("creature:" + thing.name);
    return {};
}

void ContentCatalog::setDisplayNames(const things::ThingDb& things, const strings::StringTable* strings, const things::ModifierDb* modifiers)
{
    for (auto& e : entries) {
        e.displayName = e.thing < things.things.size() ? thingDisplayName(things.things[e.thing], strings, modifiers) : std::string();
    }
}

std::vector<std::string> carriedTypes(const things::ThingDb& things)
{
    std::vector<std::string> out;
    for (const auto& t : things.things) {
        if (!placeableType(t) || (things::thingClasses(t) & (oc::Monster | oc::Player))) continue;
        const auto pickup = t.lastAttribute("PICKUP");
        if (!pickup || pickup->empty()) continue;
        out.push_back(t.name);
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

ContentCatalog ContentCatalog::build(const things::ThingDb& things)
{
    ContentCatalog cat;
    std::set<std::string> folders;
    cat.entries.reserve(things.things.size());
    for (std::size_t i = 0; i < things.things.size(); ++i) {
        const auto& t = things.things[i];
        ContentEntry e;
        e.thing = i;
        e.name = t.name;
        e.folder = contentFolder(t);
        e.classes = things::thingClasses(t);
        e.subclasses = subclassNames(t);
        e.xfer = std::string(t.lastAttribute("XFER").value_or("DefaultXfer"));
        e.placeable = placeableType(t);
        for (std::size_t at = e.folder.find('/'); ; at = e.folder.find('/', at + 1)) {
            folders.insert(e.folder.substr(0, at));
            if (at == std::string::npos) break;
        }
        cat.entries.push_back(std::move(e));
    }
    cat.folders.assign(folders.begin(), folders.end());
    std::ranges::sort(cat.folders, folderLess);
    return cat;
}

std::vector<const ContentEntry*> ContentCatalog::inFolder(std::string_view folder, bool recursive) const
{
    std::vector<const ContentEntry*> out;
    for (const auto& e : entries) {
        const bool exact = e.folder == folder;
        const bool under = recursive && (folder.empty() || (e.folder.size() > folder.size() && e.folder.starts_with(folder) && e.folder[folder.size()] == '/'));
        if (exact || under) out.push_back(&e);
    }
    return out;
}

const ContentEntry* ContentCatalog::find(std::string_view name) const
{
    for (const auto& e : entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

bool matchesQuery(const ContentEntry& entry, std::string_view query, std::string_view displayName)
{
    std::string hay = util::lowered(entry.name) + ' ' + util::lowered(displayName.empty() ? std::string_view(entry.displayName) : displayName) + ' ' +
                      util::lowered(entry.folder) + ' ' + util::lowered(entry.xfer);
    for (const auto& s : entry.subclasses) hay += ' ' + util::lowered(s);
    static constexpr std::string_view kClassNames[] = {
        "missile", "monster", "player", "obstacle", "food", "exit", "key", "door", "info_book", "trigger", "transporter",
        "hole", "wand", "fire", "elevator", "elevator_shaft", "dangerous", "monstergenerator", "readable", "light", "simple",
        "complex", "immobile", "visible_enable", "weapon", "armor", "not_stackable", "treasure", "flag", "client_persist",
        "client_predict", "pickup"};
    for (std::size_t i = 0; i < std::size(kClassNames); ++i) {
        if (entry.classes & (1u << i)) {
            hay += ' ';
            hay += kClassNames[i];
        }
    }
    const std::string q = util::lowered(query);
    std::size_t at = 0;
    while (at < q.size()) {
        while (at < q.size() && std::isspace(static_cast<unsigned char>(q[at]))) ++at;
        const auto end = q.find(' ', at);
        const std::string word = q.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!word.empty() && hay.find(word) == std::string::npos) return false;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return true;
}

// --- User categories ---------------------------------------------------------------------------

std::vector<std::string> UserCategory::evaluate(const ContentCatalog& catalog) const
{
    std::vector<std::string> out;
    std::set<std::string> in;
    for (const Rule& r : rules) {
        const std::string kind = upper(r.kind);
        const auto matches = [&](const ContentEntry& e) {
            if (kind == "ANYTHING") return true;
            if (kind == "OBJECT_CLASS") return (e.classes & things::parseClassSet(r.pattern)) != 0 && !r.pattern.empty();
            if (kind == "NAME_BEGIN") return e.name.starts_with(r.pattern);
            if (kind == "NAME_ENDING") return e.name.ends_with(r.pattern);
            if (kind == "NAME_CONTAIN") return e.name.find(r.pattern) != std::string::npos;
            if (kind == "NAME_EQUAL") return e.name == r.pattern;
            return false;
        };
        for (const auto& e : catalog.entries) {
            if (!matches(e)) continue;
            if (r.include) {
                if (in.insert(e.name).second) out.push_back(e.name);
            } else if (in.erase(e.name)) {
                std::erase(out, e.name);
            }
        }
    }
    std::ranges::sort(out);
    return out;
}

namespace {

std::string unescapeXml(std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const auto semi = s.find(';', i);
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        const auto ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else out += std::string(s.substr(i, semi - i + 1));
        i = semi;
    }
    return out;
}

std::string escapeXml(std::string_view s)
{
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

/// An attribute's value in a tag's text (`Name="x"`, case-insensitive name).
std::string attributeOf(std::string_view tag, std::string_view name)
{
    const std::string t = util::lowered(tag);
    const std::string n = util::lowered(name) + "=";
    for (std::size_t at = t.find(n); at != std::string::npos; at = t.find(n, at + 1)) {
        if (at > 0 && !std::isspace(static_cast<unsigned char>(t[at - 1]))) continue;
        std::size_t v = at + n.size();
        if (v >= tag.size()) break;
        const char q = tag[v];
        if (q != '"' && q != '\'') continue;
        const auto end = tag.find(q, v + 1);
        if (end == std::string_view::npos) break;
        return unescapeXml(tag.substr(v + 1, end - v - 1));
    }
    return {};
}

} // namespace

std::vector<UserCategory> parseCategoriesXml(std::string_view text)
{
    if (util::lowered(text).find("<themecategories") == std::string::npos) {
        throw FormatError("categories.xml: no ThemeCategories element");
    }
    std::vector<UserCategory> out;
    std::size_t at = 0;
    while ((at = text.find('<', at)) != std::string_view::npos) {
        const auto close = text.find('>', at);
        if (close == std::string_view::npos) break;
        const std::string_view tag = text.substr(at + 1, close - at - 1);
        const std::string name = util::lowered(tag.substr(0, tag.find_first_of(" \t\r\n/")));
        if (name == "category") {
            out.push_back({attributeOf(tag, "Name"), {}});
        } else if ((name == "include" || name == "exclude") && !out.empty() && !tag.ends_with("/")) {
            const auto end = text.find('<', close + 1);
            const std::string pattern = unescapeXml(text.substr(close + 1, end == std::string_view::npos ? std::string_view::npos : end - close - 1));
            out.back().rules.push_back({name == "include", upper(attributeOf(tag, "Rule")), pattern});
        } else if ((name == "include" || name == "exclude") && !out.empty()) {
            out.back().rules.push_back({name == "include", upper(attributeOf(tag, "Rule")), {}});
        }
        at = close + 1;
    }
    return out;
}

std::string categoriesXml(const std::vector<UserCategory>& categories)
{
    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ThemeCategories>\n";
    for (const auto& c : categories) {
        out += "  <Category Name=\"" + escapeXml(c.name) + "\">\n";
        for (const auto& r : c.rules) {
            const char* tag = r.include ? "Include" : "Exclude";
            out += std::string("    <") + tag + " Rule=\"" + escapeXml(r.kind) + "\">" + escapeXml(r.pattern) + "</" + tag + ">\n";
        }
        out += "  </Category>\n";
    }
    out += "</ThemeCategories>\n";
    return out;
}

} // namespace nox::mapedit
