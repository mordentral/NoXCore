// The values the builtins' string parameters accept: one registry mapping each builtin parameter
// that takes a name from a known set to that set, and the sets themselves, made from the game's own
// data and our code, never typed in by hand. The map editor's Script panel reads it three ways:
//   - autocomplete: the caret inside such an argument's string literal (argSiteAt) gives a drop-
//     down of the set's values, filtered as the user types (ValueSet::filter);
//   - Function Info's Values section: every accepted value with its note;
//   - the lint (CompileOptions::argValues): a literal that isn't one of the values is warned,
//     with the closest values (checkArgValue). Computed strings are never checked.
//
// The sets and where they come from:
//   Effect       server/script_names.hpp's scriptEffects: the names ScriptHost::effect sends
//   Sound        audio/sound_names.hpp (the game's sound table, as the server resolves names)
//   ThingType    thing.bin's types (ThingDb), with their display names (mapedit/content.hpp)
//   Spell        things/spell_ids.hpp's names (the server's), titles from thing.bin + nox.csf
//   TrapSpell    the spells and "NULL" (TrapSpells' empty slot)
//   Enchant      server/enchant.hpp's kEnchantNames, each with the spell it comes from
//   ObjectClass  server/script_names.hpp's kScriptClassNames (HasClass's table)
//   Subclass     server/script_names.hpp's scriptSubclassTables (HasSubclass's)
//   DialogType   server/script_names.hpp's scriptDialogTypes (SetDialog's)
//   TextKey      nox.csf's labels, with their text (Print, Chat... look the text up)
//   JournalEntry nox.csf's "Journal:" labels, the prefix left off (JournalEntry's names)
//   Portrait     thing.bin's named images (IMAG) named "...Pic" or "...Pic<n>" (StoryPic's portraits:
//                the dialog window draws its picture by that name)
//   ObjectName, WaypointName, ObjectGroup, WaypointGroup, WallGroup: the open map's names
//                (mapedit/script_doc.hpp's addMapArgValues), made again when the map changes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::things {
struct ThingDb;
struct ModifierDb;
} // namespace nox::things

namespace nox::strings {
class StringTable;
}

namespace nox::script {

enum class ValueSetId : std::uint8_t {
    Effect,
    Sound,
    ThingType,
    Spell,
    TrapSpell,
    Enchant,
    ObjectClass,
    Subclass,
    DialogType,
    TextKey,
    JournalEntry,
    Portrait,
    ObjectName,
    WaypointName,
    ObjectGroup,
    WaypointGroup,
    WallGroup,
};
inline constexpr std::size_t kValueSetCount = 17;

/// "effect", "sound", "thing type"... (what one value of it is called).
std::string_view valueSetName(ValueSetId id);
/// The heading Function Info shows ("Effect names", "Objects on this map"...).
std::string_view valueSetTitle(ValueSetId id);
/// One of the open map's sets (made from the document, not the game's data).
bool isMapValueSet(ValueSetId id);

/// How a literal is matched against a set, as the builtin's own lookup matches it.
enum class ValueMatch : std::uint8_t {
    Exact,     ///< case counts (strcmp: CreateObject, the spells, enchantments, classes...)
    NoCase,    ///< any case (Effect, the sounds, the string table)
    MapScoped, ///< exact, or a name without ':' equal to what follows a map name's own "Scope:"
};

struct ArgValue {
    std::string value;      ///< as the script writes it, inside the quotes
    std::string note;       ///< a short note: an effect's kind, a type's display name, a text's first line...
    std::int32_t thing = -1; ///< the thing type's index in ThingDb::things (its icon), -1 for none
    /// Other spellings the builtin's lookup takes for it. MapScoped: the whole names with a
    /// "Scope:" (the value is what follows it, which a name without ':' matches); a literal with
    /// ':' matches only these.
    std::vector<std::string> aliases;
};

/// One set of values, sorted for showing (case-insensitively by value), indexed for the lint and
/// the filter.
class ValueSet {
public:
    ValueSet() = default;
    ValueSet(ValueSetId id, ValueMatch match, std::vector<ArgValue> values);

    ValueSetId id() const noexcept { return id_; }
    ValueMatch match() const noexcept { return match_; }
    const std::vector<ArgValue>& values() const noexcept { return values_; }
    bool empty() const noexcept { return values_.empty(); }

    /// The value a literal names (as the builtin's lookup would find it), or null.
    const ArgValue* find(std::string_view literal) const;
    bool accepts(std::string_view literal) const { return find(literal) != nullptr; }
    /// The values closest to `literal` (an edit distance, ignoring case; a case-only difference
    /// first), at most `most`, none when nothing is close.
    std::vector<std::string> closest(std::string_view literal, std::size_t most = 3) const;
    /// The values matching `typed` (ignoring case): those starting with it first, then those with
    /// it inside the value, then those with it in the note; each group in the set's order. At
    /// most `most`; an empty `typed` gives the first `most`.
    std::vector<std::size_t> filter(std::string_view typed, std::size_t most = 200) const;

    /// A name the builtin's table has but does nothing with (Effect's SUMMON, GREEN_BOLT...): the
    /// lint says so instead of "no such name".
    void addInert(std::string name, std::string why);
    const std::string* inertReason(std::string_view literal) const;

private:
    std::string key(std::string_view s) const;

    ValueSetId id_ = ValueSetId::Effect;
    ValueMatch match_ = ValueMatch::Exact;
    std::vector<ArgValue> values_;
    std::vector<std::string> lower_;     ///< each value lower-cased (the filter)
    std::vector<std::string> noteLower_; ///< each note lower-cased
    std::unordered_map<std::string, std::size_t> byKey_;  ///< by key(value) (and, not MapScoped, its aliases)
    std::unordered_map<std::string, std::size_t> byFull_; ///< MapScoped: by each alias (a whole "Scope:Name")
    std::vector<std::pair<std::string, std::string>> inert_; ///< lower-cased name, why
};

/// One builtin string parameter that takes values from a set.
struct ArgParam {
    std::string_view builtin; ///< its name (builtins.hpp)
    std::uint8_t arg = 0;     ///< the parameter, from 0
    ValueSetId set = ValueSetId::Effect;
};
/// Every builtin string parameter with a set, in builtin order (the registry's table; the string
/// parameters it leaves out take free text: quest variable names and the like).
std::span<const ArgParam> argParams();
/// The set builtin `builtin`'s parameter `arg` takes values from, if any.
std::optional<ValueSetId> argValueSet(std::uint32_t builtin, std::size_t arg);

/// The game's data the sets are made from (any may be null: the sets needing it stay empty, or
/// lose their notes).
struct ArgValueSources {
    const things::ThingDb* things = nullptr;
    const strings::StringTable* strings = nullptr;
    const things::ModifierDb* modifiers = nullptr; ///< (weapons' and armour's display names)
};

/// Every set, shared (copies are cheap: the map's sets are put in a copy of the game's).
class ArgValueSets {
public:
    /// The sets that don't need the map: the built-in ones always, those from game data when
    /// `sources` has it.
    static ArgValueSets build(const ArgValueSources& sources);

    void set(ValueSet s);
    /// A set, or null when it isn't there (no game data for it, no map).
    const ValueSet* get(ValueSetId id) const;
    /// A set that's there and not empty (what the lint checks against).
    const ValueSet* usable(ValueSetId id) const;

private:
    std::array<std::shared_ptr<const ValueSet>, kValueSetCount> sets_{};
};

/// The lint's verdict on a string literal given to builtin `builtin`'s parameter `arg`: nothing
/// when it's fine (or the parameter takes free text, or its set isn't there), else the warning:
/// `Effect("SMOKE_BLST"): no effect is named SMOKE_BLST, so nothing shows; did you mean SMOKE_BLAST?`.
std::optional<std::string> checkArgValue(const ArgValueSets& sets, std::uint32_t builtin, std::size_t arg, std::string_view literal);

/// Where a caret stands in a builtin call's argument that takes values from a set (the
/// autocomplete's question).
struct ArgSite {
    std::uint32_t builtin = 0;
    std::size_t arg = 0;
    ValueSetId set = ValueSetId::Effect;
    bool inString = false;  ///< inside the argument's string literal (else at an empty argument)
    std::size_t begin = 0;  ///< what an insert replaces: the literal's text (after its opening
    std::size_t end = 0;    ///< quote, up to its closing one), or the caret (begin == end)
    bool closed = false;    ///< the literal has its closing quote on the line
    std::string typed;      ///< the literal's text before the caret (the filter)
};
/// The argument at byte `caret` of `source` (strings and comments skipped, the calls round it
/// followed back to the innermost one), when it is a builtin parameter with a set and the caret
/// is inside its string literal, or, with `emptyToo`, at an argument with nothing written yet.
std::optional<ArgSite> argSiteAt(std::string_view source, std::size_t caret, bool emptyToo = false);
/// The text an accepted value puts in place of [begin, end): the value, with quotes when the site
/// wasn't in a string, and the closing quote when the literal had none.
std::string argInsertText(const ArgSite& site, std::string_view value);

/// The edit distance (optimal string alignment) between two strings, ignoring ASCII case.
std::size_t editDistanceNoCase(std::string_view a, std::string_view b);

} // namespace nox::script
