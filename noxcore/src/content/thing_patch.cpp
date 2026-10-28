// thing.bin's patch files (thing_patch.hpp).
#include "noxcore/content/thing_patch.hpp"

#include "noxcore/things/object_class.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <unordered_set>

namespace nox::content {

namespace {

using util::equalsNoCase;
using util::trim;

std::string_view keyOf(std::string_view line)
{
    return trim(line.substr(0, line.find('=')));
}

bool imageKey(std::string_view key)
{
    return equalsNoCase(key, "MENUICON") || equalsNoCase(key, "PRETTYIMAGE");
}

// The keys GAME.EXE's thing.bin readers take: a line's key is its first word, compared exactly
// against these tables (the client's and the server's), so a patch spells them as they do.
constexpr std::string_view kClientKeys[] = {"FLAGS", "CLASS", "SUBCLASS", "EXTENT", "LIGHTINTENSITY", "DRAW", "Z", "ZSIZE",
                                            "SIZE", "MENUICON", "LIGHTCOLOR", "LIGHTDIRECTION", "LIGHTPENUMBRA", "AUDIOLOOP",
                                            "CLIENTUPDATE", "LIFETIME", "WEIGHT", "PRETTYNAME", "DESCRIPTION", "PRETTYIMAGE", "HEALTH"};
constexpr std::string_view kServerKeys[] = {"CARRYCAPACITY", "CLASS", "SUBCLASS", "EXTENT", "FLAGS", "HEALTH", "LIGHTINTENSITY",
                                            "MASS", "MENUICON", "PRETTYIMAGE", "SPEED", "WEIGHT", "MATERIAL", "EXPERIENCE",
                                            "COLLIDE", "DAMAGE", "DAMAGESOUND", "DIE", "DROP", "INIT", "CREATE", "PICKUP",
                                            "UPDATE", "USE", "XFER", "DRAW", "ZSIZE", "WORTH"};

// MATERIAL's names, a bit each (as the server's type reader takes them).
constexpr std::string_view kMaterialNames[] = {"FLESH", "CLOTH", "ANIMAL_HIDE", "WOOD", "METAL", "STONE", "EARTH", "LIQUID",
                                               "GLASS", "PAPER", "SNOW", "MUD", "MAGIC", "DIAMOND", "NONE"};

/// The key as the readers' tables spell it, or nullopt for one neither reads.
std::optional<std::string_view> exactKey(std::string_view key)
{
    for (const auto table : {std::span<const std::string_view>(kClientKeys), std::span<const std::string_view>(kServerKeys)}) {
        for (const std::string_view k : table) {
            if (equalsNoCase(k, key)) {
                return k;
            }
        }
    }
    return std::nullopt;
}

/// A key the game can read: printable ASCII, no blanks, no '='.
bool plainKey(std::string_view key)
{
    return !key.empty() && std::ranges::all_of(key, [](char c) { return c > ' ' && c < 0x7F && c != '='; });
}

/// A flag list's names (CLASS, SUBCLASS, FLAGS, MATERIAL), or an empty span for other keys.
std::vector<std::string_view> namesOf(std::string_view key)
{
    std::vector<std::string_view> names;
    if (key == "CLASS") {
        names.assign(std::begin(things::kClassNames), std::end(things::kClassNames));
    } else if (key == "SUBCLASS") {
        for (const auto& table : things::kSubclassTables) {
            names.insert(names.end(), table.begin(), table.end());
        }
    } else if (key == "FLAGS") {
        names.assign(std::begin(things::kFlagNames), std::end(things::kFlagNames));
    } else if (key == "MATERIAL") {
        names.assign(std::begin(kMaterialNames), std::end(kMaterialNames));
    }
    return names;
}

/// A value's words (split at blanks).
std::vector<std::string_view> wordsOf(std::string_view v)
{
    std::vector<std::string_view> out;
    while (true) {
        const auto b = v.find_first_not_of(" \t");
        if (b == std::string_view::npos) {
            return out;
        }
        v.remove_prefix(b);
        const auto e = v.find_first_of(" \t");
        out.push_back(v.substr(0, e));
        v.remove_prefix(e == std::string_view::npos ? v.size() : e);
    }
}

/// An optional '-' and digits.
bool isInteger(std::string_view w, std::int64_t& v)
{
    const std::string_view digits = w.starts_with('-') ? w.substr(1) : w;
    if (digits.empty() || digits.size() > 10 || !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    std::from_chars(w.data(), w.data() + w.size(), v);
    return true;
}

/// An optional '-', digits, and a '.' with digits (2, -0.5, 1.).
bool isNumber(std::string_view v)
{
    if (v.starts_with('-')) {
        v.remove_prefix(1);
    }
    const auto dot = v.find('.');
    const std::string_view a = v.substr(0, dot);
    const std::string_view b = dot == std::string_view::npos ? std::string_view{} : v.substr(dot + 1);
    const auto digits = [](std::string_view s) { return std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; }); };
    return a.size() + b.size() != 0 && digits(a) && digits(b);
}

/// Checks a value as the game reads `key`'s (an error, or "" with `value` in the tables'
/// spelling). The exe's readers: flag lists split at '+' only and compare each word whole;
/// SPEED, HEALTH and WEIGHT are whole numbers, MASS and EXPERIENCE decimals.
std::string checkValue(std::string_view key, std::string& value, std::string_view stock)
{
    if (value.find('=') != std::string::npos) {
        return "a value can't hold '=' (the game ends it there)";
    }
    if (value.find(" #") != std::string::npos || value.find("\t#") != std::string::npos) {
        return "a value can't hold \" #\" (a patch comment starts there)";
    }
    if (std::ranges::any_of(value, [](char c) { return static_cast<unsigned char>(c) < ' ' && c != '\t'; })) {
        return "a value can't hold control characters";
    }
    const auto names = namesOf(key);
    const bool whole = key == "SPEED" || key == "HEALTH" || key == "WEIGHT";
    const bool decimal = key == "MASS" || key == "EXPERIENCE";
    if (value.empty() && (!names.empty() || whole || decimal)) {
        return "needs a value";
    }
    if (!names.empty()) {
        if (value.find_first_of(" \t") != std::string::npos) {
            return "names are joined with '+' and no blanks (the game compares each word whole)";
        }
        if (equalsNoCase(value, "NULL")) {
            value = "NULL"; // (none: the readers' own word for it)
            return {};
        }
        // A name the tables haven't is one the game ignores: kept only where the type has it already
        // (stock's own lists carry some: DAGGER, NPC_WIZARD).
        const auto stockHas = [&](std::string_view word) {
            for (std::string_view rest = stock; !rest.empty();) {
                const auto plus = rest.find_first_of("+ \t");
                if (equalsNoCase(rest.substr(0, plus), word)) {
                    return true;
                }
                rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
            }
            return false;
        };
        std::string out;
        std::string_view rest = value;
        while (true) {
            const auto plus = rest.find('+');
            const std::string_view word = rest.substr(0, plus);
            const auto it = std::ranges::find_if(names, [&](std::string_view n) { return equalsNoCase(n, word); });
            if (it == names.end() && !stockHas(word)) {
                return std::format("\"{}\" isn't a {} name", word, key);
            }
            out += (out.empty() ? "" : "+") + (it == names.end() ? std::string(word) : std::string(*it));
            if (plus == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(plus + 1);
        }
        value = std::move(out);
        return {};
    }
    if (whole) {
        std::int64_t n = 0;
        const auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
        const std::int64_t most = key == "WEIGHT" ? 255 : key == "HEALTH" ? 65535 : 2147483647;
        if (!std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; }) || ec != std::errc{} || n > most) {
            return std::format("a whole number from 0 to {} (the game reads only the digits)", most);
        }
        return {};
    }
    if (decimal) {
        if (!isNumber(value)) {
            return "a number such as 2 or 0.5";
        }
        return {};
    }
    // The other keys the readers take numbers from: each value's shape (the exe refuses a thing.bin
    // whose reader fails on one).
    const auto words = wordsOf(value);
    const auto ints = [&](std::size_t n, std::int64_t lo, std::int64_t hi, const char* what) -> std::string {
        if (words.size() != n || !std::ranges::all_of(words, [&](std::string_view w) {
                std::int64_t v = 0;
                return isInteger(w, v) && v >= lo && v <= hi;
            })) {
            return what;
        }
        return {};
    };
    if (key == "SIZE") return ints(2, 0, 65535, "two whole numbers: the width and height (SIZE = 30 30)");
    if (key == "ZSIZE") return ints(2, -65535, 65535, "two whole numbers: the lowest and highest point (ZSIZE = 0 30)");
    if (key == "Z") return ints(1, -65535, 65535, "a whole number: how high it's drawn");
    if (key == "LIGHTCOLOR") return ints(3, 0, 255, "three whole numbers 0-255 (LIGHTCOLOR = 255 128 0)");
    if (key == "LIGHTDIRECTION") return ints(1, 0, 359, "degrees, a whole number from 0 to 359");
    if (key == "LIGHTPENUMBRA") return ints(1, 0, 179, "degrees, a whole number from 0 to 179");
    if (key == "LIFETIME") return ints(1, 0, 2147483647, "a whole number of frames");
    if (key == "WORTH") return ints(1, 0, 2147483647, "a whole number of gold");
    if (key == "CARRYCAPACITY") return ints(1, 0, 65535, "a whole number from 0 to 65535");
    if (key == "LIGHTINTENSITY") {
        return words.size() == 1 && isNumber(words[0]) ? std::string() : std::string("a number such as 1 or 0.5");
    }
    if (key == "EXTENT") {
        const bool ok = !words.empty() && ((words[0] == "CIRCLE" && words.size() == 2 && isNumber(words[1])) ||
                                           (words[0] == "BOX" && words.size() == 3 && isNumber(words[1]) && isNumber(words[2])) ||
                                           (words[0] == "CENTER" && words.size() == 1));
        return ok ? std::string() : std::string("CIRCLE r, BOX w h or CENTER");
    }
    if (key == "DAMAGE") {
        const bool ok = !words.empty() && std::ranges::all_of(words[0], [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        return ok ? std::string() : std::string("a damage function's name (and its numbers)");
    }
    return {};
}

/// A patch line with its comment taken off (as the text patches have them: '#' or "//" starting
/// a line, or '#' after a blank).
std::string_view uncommented(std::string_view line)
{
    if (line.starts_with("#") || line.starts_with("//")) {
        return {};
    }
    for (std::size_t i = 1; i < line.size(); ++i) {
        if (line[i] == '#' && (line[i - 1] == ' ' || line[i - 1] == '\t')) {
            return trim(line.substr(0, i));
        }
    }
    return line;
}

} // namespace

bool lockedThingKey(std::string_view key)
{
    // (What decides how every game reads the type's objects.)
    return equalsNoCase(key, "CLASS") || equalsNoCase(key, "XFER") || equalsNoCase(key, "FLAGS") || equalsNoCase(key, "COLLIDE");
}

std::string checkNewThingName(std::string_view name)
{
    if (name.empty() || name.size() > kMaxNewThingName) {
        return std::format("a new type's name is 1 to {} characters", kMaxNewThingName);
    }
    if (!std::isalpha(static_cast<unsigned char>(name[0])) ||
        !std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) {
        return "a new type's name is letters, digits and '_', starting with a letter";
    }
    if (equalsNoCase(name, "NEW") || equalsNoCase(name, "LIKE") || equalsNoCase(name, "END")) {
        return "NEW, LIKE and END are the patch's own words";
    }
    return {};
}

std::optional<std::string_view> thingKeySpelling(std::string_view key)
{
    return plainKey(key) ? exactKey(key) : std::nullopt;
}

std::string checkThingValue(std::string_view key, std::string& value, std::string_view stock)
{
    return checkValue(key, value, stock);
}

ThingPatch parseThingPatch(std::string_view text, std::vector<std::string>* problems)
{
    ThingPatch out;
    ThingPatchRecord* rec = nullptr;
    int lineNo = 0;
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3); // (a UTF-8 byte order mark)
    }
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const std::string_view raw = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        ++lineNo;
        const std::string_view line = uncommented(trim(raw));
        if (line.empty()) {
            continue;
        }
        if (!rec) {
            // A type's name (one word), or NEW <name> LIKE <base>.
            ThingPatchRecord r;
            const auto words = wordsOf(line);
            if (words.size() == 1) {
                r.name = std::string(line);
            } else if (words.size() == 4 && equalsNoCase(words[0], "NEW") && equalsNoCase(words[2], "LIKE")) {
                r.name = std::string(words[1]);
                r.like = std::string(words[3]);
            } else {
                // (Its lines are read into it all the same, so one bad header is one problem.)
                if (problems) problems->push_back(std::format("thing.patch line {}: \"{}\" is neither a type's name nor NEW <name> LIKE <type>", lineNo, line));
                r.name = std::string(line);
            }
            out.records.push_back(std::move(r));
            rec = &out.records.back();
            continue;
        }
        if (equalsNoCase(line, "END")) {
            rec = nullptr;
            continue;
        }
        ThingPatchLine l;
        std::string_view body = line;
        if (body.front() == '-') {
            l.remove = true;
            body.remove_prefix(1);
        }
        const auto eq = body.find('=');
        l.key = std::string(trim(body.substr(0, eq)));
        l.hasEquals = eq != std::string_view::npos;
        l.value = l.hasEquals ? std::string(trim(body.substr(eq + 1))) : std::string();
        if (l.key.empty()) {
            if (problems) problems->push_back(std::format("thing.patch line {}: no key", lineNo));
            continue;
        }
        if (!l.remove && !l.hasEquals) {
            // (A type's name with the END before it missing, most likely.)
            if (problems) problems->push_back(std::format("thing.patch line {}: \"{}\" in {} has no '=' (END missing?)", lineNo, l.key, rec->name));
            continue;
        }
        rec->lines.push_back(std::move(l));
    }
    if (rec && problems) {
        problems->push_back("thing.patch: " + rec->name + " has no END");
    }
    return out;
}

std::string writeThingPatch(const ThingPatch& patch, std::string_view header)
{
    if (patch.records.empty()) {
        return {};
    }
    std::string out(header);
    for (const auto& r : patch.records) {
        out += (r.isNew() ? "NEW " + r.name + " LIKE " + r.like : r.name) + "\n";
        for (const auto& l : r.lines) {
            out += "    ";
            if (l.remove) {
                out += "-" + l.key;
            } else if (l.hasEquals) {
                out += l.key + " = " + l.value;
            } else {
                out += l.key;
            }
            out += "\n";
        }
        out += "END\n";
    }
    return out;
}

std::optional<std::string> thingValue(const things::ThingDef& t, std::string_view key)
{
    if (equalsNoCase(key, "MENUICON") || equalsNoCase(key, "PRETTYIMAGE")) {
        const auto& ref = equalsNoCase(key, "MENUICON") ? t.menuIcon : t.prettyImage;
        if (!ref) {
            return std::nullopt;
        }
        return ref->index >= 0 ? std::to_string(ref->index) : ref->name;
    }
    if (const auto v = t.lastAttribute(key)) {
        return std::string(*v);
    }
    return std::nullopt;
}

namespace {

/// One patch line applied to `t` (in its NEW record when `fresh`: its locked keys and MENUICON
/// may be set then). False, with the reason in `result.problems`, when it's refused.
bool applyLine(things::ThingDef& t, ThingPatchLine l, bool fresh, ThingPatchResult& result)
{
    std::string label = t.name + "." + l.key;
    if (!plainKey(l.key)) {
        result.problems.push_back(label + ": a key is one word of plain letters, without blanks or '='");
        return false;
    }
    if (const auto k = exactKey(l.key)) {
        l.key = std::string(*k);
        label = t.name + "." + l.key;
    } else {
        result.warnings.push_back(label + ": not a key GAME.EXE reads (the stock game ignores it)");
    }
    if (l.key == "DRAW") {
        result.problems.push_back(label + ": DRAW isn't changed by a patch");
        return false;
    }
    if (lockedThingKey(l.key) && !fresh) {
        result.problems.push_back(label + ": a type's " + l.key +
                                  " never changes (every player's game and the maps rely on it); a new type chooses its own in its NEW record");
        return false;
    }
    if (!l.remove) {
        if (const std::string error = checkValue(l.key, l.value, thingValue(t, l.key).value_or("")); !error.empty()) {
            result.problems.push_back(label + ": " + error);
            return false;
        }
    }
    if (imageKey(l.key)) {
        auto& ref = l.key == "MENUICON" ? t.menuIcon : t.prettyImage;
        // Only the types with a MENUICON are kept in solo and quest saves: one added or taken
        // away changes which objects a save keeps (a new type's own choice, in its NEW record).
        if (l.key == "MENUICON" && l.remove == ref.has_value() && !fresh) {
            result.problems.push_back(label + (l.remove ? ": a type without its MENUICON drops out of saves (can't be taken away)"
                                                        : ": a type given a MENUICON starts being saved (can't be added)"));
            return false;
        }
        if (l.remove && !ref) {
            result.problems.push_back(label + ": the type has no " + l.key + " to remove");
            return false;
        }
        if (l.remove) {
            ref.reset();
        } else {
            std::int32_t index = 0;
            const auto [p, ec] = std::from_chars(l.value.data(), l.value.data() + l.value.size(), index);
            if (ec != std::errc{} || p != l.value.data() + l.value.size() || index < 0) {
                result.problems.push_back(label + ": an image is a video.bag index");
                return false;
            }
            ref = things::ImageRef{index, 0, {}};
        }
        result.changed.push_back(label);
        return true;
    }
    if (l.remove) {
        if (std::erase_if(t.attributes, [&](const std::string& a) { return equalsNoCase(keyOf(a), l.key); }) == 0) {
            result.problems.push_back(label + ": the type has no " + l.key + " to remove");
            return false;
        }
        result.changed.push_back(label);
        return true;
    }
    // Every line of the key gets it (whichever one a reader takes), the key spelt as the type has it.
    const std::string line = l.hasEquals ? l.key + " = " + l.value : l.key;
    if (line.size() > 255) {
        result.problems.push_back(label + ": longer than thing.bin's 255 bytes a line");
        return false;
    }
    bool set = false;
    for (auto& a : t.attributes) {
        if (equalsNoCase(keyOf(a), l.key)) {
            a = l.hasEquals ? std::string(keyOf(a)) + " = " + l.value : std::string(keyOf(a));
            set = true;
        }
    }
    if (!set) {
        t.attributes.push_back(line);
    }
    result.changed.push_back(label);
    return true;
}

} // namespace

ThingPatchResult applyThingPatch(things::ThingDb& db, const ThingPatch& patch)
{
    ThingPatchResult result;
    bool any = false;
    // The new types, in order, each after the types there are (a LIKE may name an earlier one).
    // Names are compared without case: no type comes to differ from another only in its case.
    std::unordered_set<std::string, util::NoCaseHash, util::NoCaseEqual> names;
    if (std::ranges::any_of(patch.records, &ThingPatchRecord::isNew)) {
        for (const auto& t : db.things) {
            names.insert(t.name);
        }
    }
    for (const auto& r : patch.records) {
        if (!r.isNew()) {
            continue;
        }
        if (const std::string why = checkNewThingName(r.name); !why.empty()) {
            result.problems.push_back("NEW " + r.name + ": " + why);
            continue;
        }
        if (names.contains(r.name)) {
            result.problems.push_back("NEW " + r.name + ": thing.bin has a type of that name already");
            continue;
        }
        const things::ThingDef* base = db.findThing(r.like);
        if (!base) {
            result.problems.push_back("NEW " + r.name + ": thing.bin has no type " + r.like + " to make it like");
            continue;
        }
        if (db.things.size() >= kMaxThingTypes) {
            result.problems.push_back(std::format("NEW {}: thing.bin can't have more than {} types", r.name, kMaxThingTypes));
            continue;
        }
        things::ThingDef t = *base;
        t.name = r.name;
        t.bits.reset();
        bool clean = true;
        const std::size_t changed = result.changed.size();
        for (const auto& l : r.lines) {
            clean = applyLine(t, l, true, result) && clean;
        }
        result.changed.resize(changed); // (a new type's lines are its own, not changes)
        if (!clean) {
            continue; // (its problems are listed: the pack isn't applied)
        }
        names.insert(t.name);
        db.things.push_back(std::move(t));
        result.added.push_back(r.name);
        any = true;
    }
    if (any) {
        db.reindex(); // (the changes below find the new types by name too)
    }
    for (const auto& r : patch.records) {
        if (r.isNew()) {
            continue;
        }
        const things::ThingDef* found = db.findThing(r.name);
        if (!found) {
            result.problems.push_back(r.name + ": thing.bin has no such type (a new one is added with NEW " + r.name + " LIKE <type>)");
            continue;
        }
        things::ThingDef& t = db.things[static_cast<std::size_t>(found - db.things.data())];
        for (const auto& l : r.lines) {
            any = applyLine(t, l, false, result) || any;
        }
    }
    if (any) {
        db.reindex();
    }
    return result;
}

} // namespace nox::content
