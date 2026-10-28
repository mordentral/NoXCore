// The map editor's content browser catalog: every thing.bin object type in a folder of a tree, from
// its CLASS and SUBCLASS (and, for the scenery, its name), plus the user's own categories (the old
// community editor's categories.xml rules).
//
// The folders come from one ordered rule table, first match wins, so each thing is in exactly
// one folder. Things the game's object save never writes (no MENUICON: effects, missiles,
// generators' spawn markers) are "not placeable": they sit in a hidden folder the browser shows
// only with "show all", since a map the game re-saves would lose them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
struct ThingDef;
struct ModifierDb;
} // namespace nox::things

namespace nox::strings {
class StringTable;
}

namespace nox::mapedit {

/// One object type in the catalog.
struct ContentEntry {
    std::size_t thing = 0;     ///< index into ThingDb::things
    std::string name;          ///< thing.bin name
    std::string displayName;   ///< the game's name for it (thingDisplayName, UTF-8); empty: none, or not set
    std::string folder;        ///< the folder path, '/'-separated ("Monsters/Undead")
    std::uint32_t classes = 0; ///< CLASS bits (object_class.hpp)
    std::vector<std::string> subclasses; ///< the SUBCLASS names as written (their bits mean different things per table)
    std::string xfer;          ///< the XFER function (DefaultXfer when none)
    bool placeable = true;     ///< the game's object save writes it (a MENUICON)
};

/// The folder thing.bin's `thing` goes in (the rule table; "Hidden/..." for one not placeable).
std::string contentFolder(const things::ThingDef& thing);
/// The SUBCLASS attribute's names, as written (upper case), in order.
std::vector<std::string> subclassNames(const things::ThingDef& thing);
/// Whether the game's object save writes things of this type (it has a MENUICON).
bool placeableType(const things::ThingDef& thing);
/// The types something can carry (an inventory's items, a shop's stock: Properties' pickers): the
/// placeable things a player picks up (thing.bin's PICKUP function), sorted by name. (Not the
/// CLASS bit PICKUP, which thing.bin's items don't carry.)
std::vector<std::string> carriedTypes(const things::ThingDb& things);
/// The name the game shows for things of this type, in UTF-8 (empty when it shows none):
///   weapons, armour, staffs and flags: their modifier.bin definition's DESC (the base of the
///       tooltip, Gui_Tooltip_GetDrawableText, before modifier words: "Longsword");
///   anything else with a name: its pretty_name (Cli_GetThingPrettyName): the PRETTYNAME string,
///       else for a thing with a non-zero WEIGHT "thing.db:<name>PrettyName";
///   creatures (none of the above): "creature:<name>", the field guide's and summon list's title.
/// Null `strings` or `modifiers` skip what needs them.
std::string thingDisplayName(const things::ThingDef& thing, const strings::StringTable* strings, const things::ModifierDb* modifiers);

/// The catalog: every thing, its folder, and the folders as a sorted tree.
struct ContentCatalog {
    std::vector<ContentEntry> entries; ///< in thing.bin order
    /// Every folder path (parents included), in the tree's display order: the rule table's
    /// top-level order, children sorted by name, "Other" last.
    std::vector<std::string> folders;

    static ContentCatalog build(const things::ThingDb& things);
    /// Gives every entry its display name (thingDisplayName), which search then matches too.
    void setDisplayNames(const things::ThingDb& things, const strings::StringTable* strings, const things::ModifierDb* modifiers);
    /// The entries in `folder` (and, with `recursive`, its sub-folders).
    std::vector<const ContentEntry*> inFolder(std::string_view folder, bool recursive) const;
    const ContentEntry* find(std::string_view name) const;
};

/// What a search query matches: every word of `query` (case-insensitive) is in the name, the
/// display name (`displayName`, or the entry's own when that's empty), the folder, the xfer or a
/// class/subclass name. An empty query matches all.
bool matchesQuery(const ContentEntry& entry, std::string_view query, std::string_view displayName = {});

/// A user-defined category: the old community editor's categories.xml, read as it read it (the
/// rules in order; Include adds without duplicates, Exclude removes what was added so far).
/// Rules: OBJECT_CLASS (a CLASS name), NAME_BEGIN, NAME_ENDING, NAME_CONTAIN, NAME_EQUAL
/// (case-sensitive, as there), ANYTHING (every type; the old reader returned at once on it,
/// ignoring every later rule, Excludes included: here it is a rule like the others).
struct UserCategory {
    struct Rule {
        bool include = true;
        std::string kind;    ///< upper case
        std::string pattern;
    };
    std::string name;
    std::vector<Rule> rules;

    /// The type names it holds, sorted.
    std::vector<std::string> evaluate(const ContentCatalog& catalog) const;
};

/// Parses a categories.xml (<ThemeCategories><Category Name=".."><Include Rule="X">pattern
/// </Include><Exclude Rule="X">..</Exclude>...). Rules before the first Category are ignored (the
/// old reader crashed on them). Throws FormatError for text with no ThemeCategories element.
std::vector<UserCategory> parseCategoriesXml(std::string_view text);
/// The categories as categories.xml text (round-trips through parseCategoriesXml).
std::string categoriesXml(const std::vector<UserCategory>& categories);

} // namespace nox::mapedit
