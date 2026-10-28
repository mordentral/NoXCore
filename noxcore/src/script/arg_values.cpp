// The builtins' string parameters' value sets (arg_values.hpp).
#include "noxcore/script/arg_values.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/mapedit/content.hpp"
#include "noxcore/script/builtins.hpp"
#include "noxcore/script/opcodes.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/script_names.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace nox::script {

namespace {

char lowerChar(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view s)
{
    std::string out(s);
    for (char& c : out) {
        c = lowerChar(c);
    }
    return out;
}

/// A text's first line, its spaces trimmed, cut to `most` bytes at a character's start.
std::string firstLine(std::string_view text, std::size_t most = 90)
{
    const auto nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
    std::string out(line);
    if (out.size() > most) {
        std::size_t cut = most;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out = out.substr(0, cut) + "...";
    } else if (nl != std::string_view::npos) {
        out += " ...";
    }
    return out;
}

/// The string table's text for `label` in UTF-8 (empty when it has none).
std::string tableText(const strings::StringTable* table, std::string_view label)
{
    if (!table || label.empty()) {
        return {};
    }
    const auto t = table->text(label);
    return t ? strings::toUtf8(*t) : std::string();
}

// The registry: each builtin string parameter that takes values from a set. (The string
// parameters left out take free text: the quest variables' names (SetQuestStatus...), which a
// script makes up.)
constexpr ArgParam kParams[] = {
    {"AudioEvent", 0, ValueSetId::Sound},
    {"Print", 0, ValueSetId::TextKey},
    {"PrintToAll", 0, ValueSetId::TextKey},
    {"Chat", 1, ValueSetId::TextKey},
    {"CreateObject", 0, ValueSetId::ThingType},
    {"AwardSpell", 1, ValueSetId::Spell},
    {"GroupAwardSpell", 1, ValueSetId::Spell},
    {"Enchant", 1, ValueSetId::Enchant},
    {"GroupEnchant", 1, ValueSetId::Enchant},
    {"Object", 0, ValueSetId::ObjectName},
    {"HasClass", 1, ValueSetId::ObjectClass},
    {"HasEnchant", 1, ValueSetId::Enchant},
    {"EnchantOff", 1, ValueSetId::Enchant},
    {"Unused5e", 0, ValueSetId::TextKey},
    {"Effect", 0, ValueSetId::Effect},
    {"Waypoint", 0, ValueSetId::WaypointName},
    {"WaypointGroup", 0, ValueSetId::WaypointGroup},
    {"ObjectGroup", 0, ValueSetId::ObjectGroup},
    {"WallGroup", 0, ValueSetId::WallGroup},
    {"ChatTimerSeconds", 1, ValueSetId::TextKey},
    {"ChatTimer", 1, ValueSetId::TextKey},
    {"SetDialog", 1, ValueSetId::DialogType},
    {"StoryPic", 1, ValueSetId::Portrait},
    {"TellStory", 0, ValueSetId::Sound},
    {"TellStory", 1, ValueSetId::TextKey},
    {"CastSpellObjectObject", 0, ValueSetId::Spell},
    {"CastSpellObjectLocation", 0, ValueSetId::Spell},
    {"CastSpellLocationObject", 0, ValueSetId::Spell},
    {"CastSpellLocationLocation", 0, ValueSetId::Spell},
    {"JournalEntry", 1, ValueSetId::JournalEntry},
    {"JournalDelete", 1, ValueSetId::JournalEntry},
    {"JournalEdit", 1, ValueSetId::JournalEntry},
    {"HasSubclass", 1, ValueSetId::Subclass},
    {"TrapSpells", 1, ValueSetId::TrapSpell},
    {"TrapSpells", 2, ValueSetId::TrapSpell},
    {"TrapSpells", 3, ValueSetId::TrapSpell},
    {"SetShopkeeperText", 1, ValueSetId::TextKey},
};

const std::map<std::pair<std::uint32_t, std::size_t>, ValueSetId>& paramIndex()
{
    static const auto index = [] {
        std::map<std::pair<std::uint32_t, std::size_t>, ValueSetId> m;
        for (const ArgParam& p : kParams) {
            if (const auto id = findBuiltin(p.builtin)) {
                m[{*id, p.arg}] = p.set;
            }
        }
        return m;
    }();
    return index;
}

std::string effectNote(server::EffectKind kind)
{
    switch (kind) {
    case server::EffectKind::Point: return "point at (x1, y1)";
    case server::EffectKind::Ray: return "ray from (x1, y1) to (x2, y2)";
    case server::EffectKind::Burst: return "burst at (x1, y1); x2 its size";
    case server::EffectKind::Quake: return "screen shake round (x1, y1); x2 its strength";
    case server::EffectKind::None: break;
    }
    return {};
}

/// A spell's name as the game shows it (its SPEL title's text), else "".
std::string spellTitle(const ArgValueSources& src, std::uint32_t spell)
{
    if (!src.things || !src.strings) {
        return {};
    }
    const things::SpellDef* def = src.things->findSpell(spell);
    return def ? tableText(src.strings, def->title) : std::string();
}

ValueSet effectSet()
{
    std::vector<ArgValue> values;
    std::vector<std::pair<std::string, std::string>> inert;
    for (const server::ScriptEffect& e : server::scriptEffects()) {
        if (e.kind != server::EffectKind::None) {
            values.push_back({std::string(e.name), effectNote(e.kind), -1});
        } else {
            inert.emplace_back(std::string(e.name), e.op == 0x98 ? "the original sends it from leftover coordinates (OB-055); NoX Core sends nothing"
                                                                 : "it is in the game's list of effects, but Effect doesn't send it");
        }
    }
    ValueSet s(ValueSetId::Effect, ValueMatch::NoCase, std::move(values));
    for (auto& [name, why] : inert) {
        s.addInert(std::move(name), std::move(why));
    }
    return s;
}

ValueSet soundSet(const ArgValueSources& src)
{
    std::optional<server::SoundTable> table;
    if (src.things) {
        table.emplace(*src.things);
    }
    std::vector<ArgValue> values;
    for (std::uint32_t id = 1; id < audio::kSoundCount; ++id) {
        const std::string_view name = audio::soundName(id);
        if (name.empty()) {
            continue;
        }
        std::string note = "sound " + std::to_string(id);
        if (table) {
            const server::SoundInfo& info = (*table)[id];
            note += info.files <= 0 ? ", no sound files: plays nothing" : ", heard to " + std::to_string(info.range) + " units";
        }
        values.push_back({std::string(name), std::move(note), -1});
    }
    return {ValueSetId::Sound, ValueMatch::NoCase, std::move(values)};
}

ValueSet spellSet(const ArgValueSources& src, bool trap)
{
    std::vector<ArgValue> values;
    for (std::uint32_t id = 1; id < things::kSpellCount; ++id) {
        const std::string_view name = things::spellName(id);
        if (name.empty()) {
            continue;
        }
        values.push_back({std::string(name), spellTitle(src, id), -1});
    }
    if (trap) {
        values.push_back({"NULL", "an empty slot", -1});
    }
    return {trap ? ValueSetId::TrapSpell : ValueSetId::Spell, ValueMatch::Exact, std::move(values)};
}

ValueSet enchantSet(const ArgValueSources& src)
{
    std::vector<ArgValue> values;
    for (int e = 0; e < 32; ++e) {
        std::string note;
        if (const std::uint32_t spell = server::enchantmentSpell(e)) {
            const std::string title = spellTitle(src, spell);
            note = "from " + (title.empty() ? std::string(things::spellName(spell)) : title);
        } else {
            note = "no spell gives it";
        }
        values.push_back({std::string(server::kEnchantNames[e]), std::move(note), -1});
    }
    return {ValueSetId::Enchant, ValueMatch::Exact, std::move(values)};
}

ValueSet classSet(const mapedit::ContentCatalog* catalog)
{
    std::vector<ArgValue> values;
    for (int bit = 0; bit < 32; ++bit) {
        std::string note = "class bit " + std::to_string(bit);
        if (catalog) {
            const auto n = std::ranges::count_if(catalog->entries, [&](const mapedit::ContentEntry& e) { return (e.classes >> bit) & 1u; });
            note += ", " + std::to_string(n) + (n == 1 ? " thing type" : " thing types");
        }
        values.push_back({std::string(server::kScriptClassNames[bit]), std::move(note), -1});
    }
    return {ValueSetId::ObjectClass, ValueMatch::Exact, std::move(values)};
}

ValueSet subclassSet()
{
    std::vector<ArgValue> values;
    std::set<std::string_view> seen;
    for (const server::ScriptSubclassTable& t : server::scriptSubclassTables()) {
        for (std::size_t bit = 0; bit < t.names.size(); ++bit) {
            if (seen.insert(t.names[bit]).second) { // (HasSubclass takes the first table naming it)
                values.push_back({std::string(t.names[bit]), std::string(t.what) + " subclass, bit " + std::to_string(bit), -1});
            }
        }
    }
    return {ValueSetId::Subclass, ValueMatch::Exact, std::move(values)};
}

ValueSet dialogSet()
{
    std::vector<ArgValue> values;
    for (const server::ScriptDialogType& t : server::scriptDialogTypes()) {
        values.push_back({std::string(t.name), std::string(t.note), -1});
    }
    return {ValueSetId::DialogType, ValueMatch::Exact, std::move(values)};
}

ValueSet thingSet(const things::ThingDb& things, const mapedit::ContentCatalog& catalog)
{
    std::vector<ArgValue> values;
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < things.things.size(); ++i) {
        const std::string& name = things.things[i].name;
        if (name.empty() || !seen.insert(name).second) {
            continue; // (findThing takes the first of a name)
        }
        const mapedit::ContentEntry* e = i < catalog.entries.size() ? &catalog.entries[i] : nullptr;
        std::string note = e ? (!e->displayName.empty() ? e->displayName : e->folder) : std::string();
        values.push_back({name, std::move(note), static_cast<std::int32_t>(i)});
    }
    return {ValueSetId::ThingType, ValueMatch::Exact, std::move(values)};
}

ValueSet textSet(const strings::StringTable& table, bool journal)
{
    constexpr std::string_view kJournal = "Journal:";
    std::vector<ArgValue> values;
    std::set<std::string> seen; // (lower-cased: the lookup ignores case, the first of a label wins)
    for (const strings::StringEntry& e : table.entries()) {
        std::string_view label = e.label;
        if (journal) {
            if (label.size() <= kJournal.size() || lowered(label.substr(0, kJournal.size())) != lowered(kJournal)) {
                continue;
            }
            label.remove_prefix(kJournal.size());
        }
        if (label.empty() || !seen.insert(lowered(label)).second) {
            continue;
        }
        std::string text = e.variants.empty() ? std::string() : strings::toUtf8(e.variants.front().text);
        std::string note = text.empty() ? std::string("(empty text)") : firstLine(text);
        if (e.variants.size() > 1) {
            note += "  (" + std::to_string(e.variants.size()) + " variants)";
        }
        values.push_back({std::string(label), std::move(note), -1});
    }
    return {journal ? ValueSetId::JournalEntry : ValueSetId::TextKey, ValueMatch::NoCase, std::move(values)};
}

ValueSet portraitSet(const things::ThingDb& things)
{
    std::vector<ArgValue> values;
    std::set<std::string_view> seen;
    // (The portraits by the game's naming: "...Pic", or "...Pic" and a number: MaidenPic2,
    // MalePic10. Not the cursors' "CursorPickup".)
    const auto portrait = [](std::string_view n) {
        while (!n.empty() && n.back() >= '0' && n.back() <= '9') n.remove_suffix(1);
        return n.ends_with("Pic");
    };
    for (const things::ImageDef& d : things.images) {
        if (!portrait(d.name) || !seen.insert(d.name).second) {
            continue;
        }
        values.push_back({d.name, d.animation ? "animated portrait" : "portrait", -1});
    }
    return {ValueSetId::Portrait, ValueMatch::Exact, std::move(values)};
}

/// What's wrong with a literal no set value matches, as the lint says it (after `Name("lit"): `).
std::string problem(const ValueSet& set, std::string_view lit)
{
    const std::string l(lit);
    switch (set.id()) {
    case ValueSetId::Effect:
        if (const std::string* why = set.inertReason(lit)) {
            return "Effect doesn't send " + l + ", so nothing shows (" + *why + ")";
        }
        return "no effect is named " + l + ", so nothing shows";
    case ValueSetId::Sound: return "no sound is named " + l + ", so nothing plays";
    case ValueSetId::ThingType: return "no thing type is named " + l + " (case counts), so nothing is made";
    case ValueSetId::Spell: return "no spell is named " + l + " (case counts), so nothing happens";
    case ValueSetId::TrapSpell: return "no spell is named " + l + " (case counts): the slot stays empty";
    case ValueSetId::Enchant: return "no enchantment is named " + l + " (case counts), so nothing happens";
    case ValueSetId::ObjectClass: return "no class is named " + l + " (case counts): HasClass then returns nothing at all (an original bug)";
    case ValueSetId::Subclass: return "no subclass is named " + l + " (case counts): HasSubclass then returns nothing at all (an original bug)";
    case ValueSetId::DialogType: return "no dialog type is named " + l + " (case counts): it counts as NORMAL";
    case ValueSetId::TextKey: return "the string table (nox.csf) has no text named " + l + ": the game shows MISSING:'" + l + "' instead";
    case ValueSetId::JournalEntry: return "the string table (nox.csf) has no journal text Journal:" + l + ": the entry shows as missing text";
    case ValueSetId::Portrait: return "no portrait image is named " + l + " (case counts): the dialog shows no picture";
    case ValueSetId::ObjectName: return "no object on the map is named " + l;
    case ValueSetId::WaypointName: return "no waypoint on the map is named " + l;
    case ValueSetId::ObjectGroup: return "no object group on the map is named " + l;
    case ValueSetId::WaypointGroup: return "no waypoint group on the map is named " + l;
    case ValueSetId::WallGroup: return "no wall group on the map is named " + l;
    }
    return "not a known value";
}

bool identStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool identChar(char c)
{
    return identStart(c) || (c >= '0' && c <= '9');
}

} // namespace

// ---- names ----------------------------------------------------------------------------------

std::string_view valueSetName(ValueSetId id)
{
    switch (id) {
    case ValueSetId::Effect: return "effect";
    case ValueSetId::Sound: return "sound";
    case ValueSetId::ThingType: return "thing type";
    case ValueSetId::Spell: return "spell";
    case ValueSetId::TrapSpell: return "spell";
    case ValueSetId::Enchant: return "enchantment";
    case ValueSetId::ObjectClass: return "class";
    case ValueSetId::Subclass: return "subclass";
    case ValueSetId::DialogType: return "dialog type";
    case ValueSetId::TextKey: return "text key";
    case ValueSetId::JournalEntry: return "journal entry";
    case ValueSetId::Portrait: return "portrait";
    case ValueSetId::ObjectName: return "object";
    case ValueSetId::WaypointName: return "waypoint";
    case ValueSetId::ObjectGroup: return "object group";
    case ValueSetId::WaypointGroup: return "waypoint group";
    case ValueSetId::WallGroup: return "wall group";
    }
    return "value";
}

std::string_view valueSetTitle(ValueSetId id)
{
    switch (id) {
    case ValueSetId::Effect: return "Effect names (the ones Effect sends)";
    case ValueSetId::Sound: return "Sound names (the game's sound table)";
    case ValueSetId::ThingType: return "Thing types (thing.bin)";
    case ValueSetId::Spell: return "Spell names";
    case ValueSetId::TrapSpell: return "Spell names, or NULL for an empty slot";
    case ValueSetId::Enchant: return "Enchantment names";
    case ValueSetId::ObjectClass: return "Class names (HasClass's table)";
    case ValueSetId::Subclass: return "Subclass names (HasSubclass's tables)";
    case ValueSetId::DialogType: return "Dialog types";
    case ValueSetId::TextKey: return "Text keys (the string table, nox.csf)";
    case ValueSetId::JournalEntry: return "Journal entries (nox.csf's Journal: texts)";
    case ValueSetId::Portrait: return "Portraits (thing.bin's named images)";
    case ValueSetId::ObjectName: return "Objects on this map (their script names)";
    case ValueSetId::WaypointName: return "Waypoints on this map";
    case ValueSetId::ObjectGroup: return "Object groups on this map";
    case ValueSetId::WaypointGroup: return "Waypoint groups on this map";
    case ValueSetId::WallGroup: return "Wall groups on this map";
    }
    return "Values";
}

bool isMapValueSet(ValueSetId id)
{
    return id == ValueSetId::ObjectName || id == ValueSetId::WaypointName || id == ValueSetId::ObjectGroup ||
           id == ValueSetId::WaypointGroup || id == ValueSetId::WallGroup;
}

// ---- ValueSet -------------------------------------------------------------------------------

ValueSet::ValueSet(ValueSetId id, ValueMatch match, std::vector<ArgValue> values)
    : id_(id)
    , match_(match)
    , values_(std::move(values))
{
    std::stable_sort(values_.begin(), values_.end(), [](const ArgValue& a, const ArgValue& b) {
        const std::string la = lowered(a.value);
        const std::string lb = lowered(b.value);
        return la != lb ? la < lb : a.value < b.value;
    });
    lower_.reserve(values_.size());
    noteLower_.reserve(values_.size());
    for (std::size_t i = 0; i < values_.size(); ++i) {
        lower_.push_back(lowered(values_[i].value));
        noteLower_.push_back(lowered(values_[i].note));
        byKey_.emplace(key(values_[i].value), i);
        for (const std::string& a : values_[i].aliases) {
            (match_ == ValueMatch::MapScoped ? byFull_ : byKey_).emplace(key(a), i);
        }
    }
}

std::string ValueSet::key(std::string_view s) const
{
    return match_ == ValueMatch::NoCase ? lowered(s) : std::string(s);
}

const ArgValue* ValueSet::find(std::string_view literal) const
{
    // MapScoped, the game's lookups: a name with ':' equal to a whole name; one without equal to
    // what follows a name's own "Scope:", or to a name without one (World::byScriptName,
    // namesMatch). The values are those parts, the whole scoped names their aliases.
    const auto& index = match_ == ValueMatch::MapScoped && literal.find(':') != std::string_view::npos ? byFull_ : byKey_;
    if (const auto it = index.find(key(literal)); it != index.end()) {
        return &values_[it->second];
    }
    return nullptr;
}

std::vector<std::string> ValueSet::closest(std::string_view literal, std::size_t most) const
{
    const std::string want = lowered(literal);
    const std::size_t n = want.size();
    const std::size_t limit = std::clamp<std::size_t>(n / 4 + 1, 1, 3);
    struct Hit {
        std::size_t score;
        std::size_t index;
    };
    std::vector<Hit> hits;
    for (std::size_t i = 0; i < values_.size(); ++i) {
        const std::string_view have = lower_[i];
        if (have == want) {
            hits.push_back({0, i}); // (only the case differs)
            continue;
        }
        const std::size_t diff = have.size() > n ? have.size() - n : n - have.size();
        std::size_t score = SIZE_MAX;
        if (diff <= limit) {
            const std::size_t d = editDistanceNoCase(have, want);
            if (d <= limit) score = d;
        }
        if (score == SIZE_MAX && n >= 3 && (have.starts_with(want) || (have.size() >= 3 && want.starts_with(have)))) {
            score = limit; // (one is the start of the other: SMOKE for SMOKE_BLAST)
        }
        if (score != SIZE_MAX) {
            hits.push_back({score, i});
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score < b.score; });
    std::vector<std::string> out;
    for (const Hit& h : hits) {
        if (out.size() >= most || (!out.empty() && h.score > hits.front().score + 1)) {
            break;
        }
        const std::string& v = values_[h.index].value;
        if (std::ranges::find(out, v) == out.end()) {
            out.push_back(v);
        }
    }
    return out;
}

std::vector<std::size_t> ValueSet::filter(std::string_view typed, std::size_t most) const
{
    std::vector<std::size_t> out;
    const std::string want = lowered(typed);
    if (want.empty()) {
        for (std::size_t i = 0; i < values_.size() && out.size() < most; ++i) out.push_back(i);
        return out;
    }
    std::vector<std::uint8_t> taken(values_.size(), 0);
    const auto pass = [&](auto matches) {
        for (std::size_t i = 0; i < values_.size() && out.size() < most; ++i) {
            if (!taken[i] && matches(i)) {
                taken[i] = 1;
                out.push_back(i);
            }
        }
    };
    pass([&](std::size_t i) { return lower_[i].starts_with(want); });
    pass([&](std::size_t i) { return lower_[i].find(want) != std::string::npos; });
    if (want.size() >= 2) {
        pass([&](std::size_t i) { return noteLower_[i].find(want) != std::string::npos; });
    }
    return out;
}

void ValueSet::addInert(std::string name, std::string why)
{
    inert_.emplace_back(lowered(name), std::move(why));
}

const std::string* ValueSet::inertReason(std::string_view literal) const
{
    const std::string want = lowered(literal);
    for (const auto& [name, why] : inert_) {
        if (name == want) return &why;
    }
    return nullptr;
}

// ---- the registry -----------------------------------------------------------------------------

std::span<const ArgParam> argParams()
{
    return kParams;
}

std::optional<ValueSetId> argValueSet(std::uint32_t builtin, std::size_t arg)
{
    const auto& index = paramIndex();
    const auto it = index.find({builtin, arg});
    return it == index.end() ? std::nullopt : std::optional<ValueSetId>(it->second);
}

ArgValueSets ArgValueSets::build(const ArgValueSources& src)
{
    ArgValueSets out;
    std::optional<mapedit::ContentCatalog> catalog;
    if (src.things) {
        catalog = mapedit::ContentCatalog::build(*src.things);
        catalog->setDisplayNames(*src.things, src.strings, src.modifiers);
    }
    out.set(effectSet());
    out.set(soundSet(src));
    out.set(spellSet(src, false));
    out.set(spellSet(src, true));
    out.set(enchantSet(src));
    out.set(classSet(catalog ? &*catalog : nullptr));
    out.set(subclassSet());
    out.set(dialogSet());
    if (src.things) {
        out.set(thingSet(*src.things, *catalog));
        out.set(portraitSet(*src.things));
    }
    if (src.strings) {
        out.set(textSet(*src.strings, false));
        out.set(textSet(*src.strings, true));
    }
    return out;
}

void ArgValueSets::set(ValueSet s)
{
    const auto id = static_cast<std::size_t>(s.id());
    sets_[id] = std::make_shared<const ValueSet>(std::move(s));
}

const ValueSet* ArgValueSets::get(ValueSetId id) const
{
    return sets_[static_cast<std::size_t>(id)].get();
}

const ValueSet* ArgValueSets::usable(ValueSetId id) const
{
    const ValueSet* s = get(id);
    return s && !s->empty() ? s : nullptr;
}

std::optional<std::string> checkArgValue(const ArgValueSets& sets, std::uint32_t builtin, std::size_t arg, std::string_view literal)
{
    const auto id = argValueSet(builtin, arg);
    const ValueSet* set = id ? sets.usable(*id) : nullptr;
    if (!set || set->accepts(literal)) {
        return std::nullopt;
    }
    std::string msg = std::string(builtinSignature(builtin).name) + "(\"" + std::string(literal) + "\"): ";
    if (literal.empty()) {
        msg += "an empty " + std::string(valueSetName(*id)) + " name matches nothing";
        return msg;
    }
    msg += problem(*set, literal);
    if (set->inertReason(literal)) {
        return msg;
    }
    const auto near = set->closest(literal);
    if (!near.empty()) {
        msg += "; did you mean ";
        for (std::size_t i = 0; i < near.size(); ++i) {
            msg += (i == 0 ? "" : i + 1 == near.size() ? " or " : ", ") + near[i];
        }
        msg += "?";
    }
    return msg;
}

// ---- the caret ----------------------------------------------------------------------------------

std::optional<ArgSite> argSiteAt(std::string_view s, std::size_t caret, bool emptyToo)
{
    caret = std::min(caret, s.size());
    struct Frame {
        std::optional<std::uint32_t> builtin;
        std::size_t arg = 0;
        std::size_t argStart = 0;
    };
    std::vector<Frame> stack;
    std::string ident; // the last identifier (cleared by any other token)
    const auto blank = [&](std::size_t a, std::size_t b) {
        for (std::size_t k = a; k < b; ++k) {
            if (s[k] != ' ' && s[k] != '\t' && s[k] != '\r' && s[k] != '\n') return false;
        }
        return true;
    };
    const auto siteFor = [&](const Frame& f) -> std::optional<ArgSite> {
        if (!f.builtin) return std::nullopt;
        const auto set = argValueSet(*f.builtin, f.arg);
        if (!set) return std::nullopt;
        ArgSite site;
        site.builtin = *f.builtin;
        site.arg = f.arg;
        site.set = *set;
        return site;
    };
    std::size_t i = 0;
    while (i < caret) {
        const char c = s[i];
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            const auto nl = s.find('\n', i);
            const std::size_t e = nl == std::string_view::npos ? s.size() : nl;
            if (caret <= e) return std::nullopt; // (in a comment)
            i = e;
            ident.clear();
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            const auto close = s.find("*/", i + 2);
            const std::size_t e = close == std::string_view::npos ? s.size() : close + 2;
            if (caret < e) return std::nullopt;
            i = e;
            ident.clear();
            continue;
        }
        if (c == '"') {
            // A literal: to its closing quote, or (unclosed) to the line's end.
            std::size_t j = i + 1;
            while (j < s.size() && s[j] != '"' && s[j] != '\n') {
                j += s[j] == '\\' && j + 1 < s.size() && s[j + 1] != '\n' ? 2 : 1;
            }
            j = std::min(j, s.size());
            const bool closed = j < s.size() && s[j] == '"';
            if (caret <= j) {
                // The caret is in it: an argument with a set, the literal its first thing?
                if (stack.empty() || !blank(stack.back().argStart, i)) return std::nullopt;
                auto site = siteFor(stack.back());
                if (!site) return std::nullopt;
                site->inString = true;
                site->begin = i + 1;
                site->end = j;
                site->closed = closed;
                site->typed = std::string(s.substr(i + 1, caret - (i + 1)));
                return site;
            }
            i = closed ? j + 1 : j;
            ident.clear();
            continue;
        }
        if (identStart(c)) {
            std::size_t j = i;
            while (j < s.size() && identChar(s[j])) ++j;
            ident = std::string(s.substr(i, j - i));
            i = j;
            continue;
        }
        switch (c) {
        case '(': {
            Frame f;
            if (!ident.empty()) f.builtin = findBuiltin(ident);
            f.argStart = i + 1;
            stack.push_back(f);
            break;
        }
        case ')':
            if (!stack.empty()) stack.pop_back();
            break;
        case ',':
            if (!stack.empty()) {
                ++stack.back().arg;
                stack.back().argStart = i + 1;
            }
            break;
        case '{': case '}': case ';':
            stack.clear();
            break;
        default:
            break;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            ident.clear();
        }
        ++i;
    }
    if (!emptyToo || stack.empty() || !blank(stack.back().argStart, caret)) {
        return std::nullopt;
    }
    auto site = siteFor(stack.back());
    if (!site) return std::nullopt;
    site->inString = false;
    site->begin = site->end = caret;
    return site;
}

std::string argInsertText(const ArgSite& site, std::string_view value)
{
    if (!site.inString) {
        return "\"" + std::string(value) + "\"";
    }
    return std::string(value) + (site.closed ? "" : "\"");
}

std::size_t editDistanceNoCase(std::string_view a, std::string_view b)
{
    // Optimal string alignment: insertions, deletions, substitutions and swaps of neighbours.
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    std::vector<std::size_t> prev2(m + 1), prev(m + 1), cur(m + 1);
    for (std::size_t j = 0; j <= m; ++j) prev[j] = j;
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= m; ++j) {
            const bool same = lowerChar(a[i - 1]) == lowerChar(b[j - 1]);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (same ? 0 : 1)});
            if (i > 1 && j > 1 && lowerChar(a[i - 1]) == lowerChar(b[j - 2]) && lowerChar(a[i - 2]) == lowerChar(b[j - 1])) {
                cur[j] = std::min(cur[j], prev2[j - 2] + 1);
            }
        }
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[m];
}

} // namespace nox::script
