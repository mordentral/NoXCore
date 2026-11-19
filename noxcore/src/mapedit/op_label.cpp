// The map editor's op labels: made in English, shown in the language in use (op_label.hpp).
#include "noxcore/mapedit/op_label.hpp"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nox::mapedit {

namespace {

using namespace oplabels;

#define NOX_OPL(k) OpLabelTemplate{&(k), nullptr}
#define NOX_OPLP(k) OpLabelTemplate{nullptr, &(k)}
// Every template of op_label.hpp, once.
constexpr OpLabelTemplate kTemplates[] = {
    // Waypoints
    NOX_OPL(kPlaceWaypoint), NOX_OPL(kPlaceWaypointLinked), NOX_OPL(kLinkWaypointsBoth), NOX_OPL(kLinkWaypoints), NOX_OPL(kUnlinkWaypoints),
    NOX_OPL(kSetLinkFlags), NOX_OPLP(kEnableWaypoints), NOX_OPLP(kDisableWaypoints), NOX_OPL(kEditWaypoint), NOX_OPLP(kSetWaypointFlag),
    NOX_OPLP(kClearWaypointFlag), NOX_OPL(kLinkChain), NOX_OPL(kRenameWaypoint), NOX_OPL(kMoveWaypoint),
    // Polygons
    NOX_OPL(kPolygonNumbered), NOX_OPL(kPolygonNamed), NOX_OPL(kAddPolygon), NOX_OPL(kEditPolygon), NOX_OPL(kMoveCorner), NOX_OPL(kAddCorner),
    NOX_OPL(kRemoveCorner), NOX_OPL(kReorderPolygon), NOX_OPLP(kDeletePolygons), NOX_OPLP(kMovePolygons), NOX_OPL(kRenamePolygon),
    NOX_OPL(kPolygonAmbient), NOX_OPL(kPolygonMinimapGroup), NOX_OPL(kPolygonMinimapNone), NOX_OPL(kPolygonPlayerHandlerNone),
    NOX_OPL(kPolygonPlayerHandler), NOX_OPL(kPolygonMonsterHandlerNone), NOX_OPL(kPolygonMonsterHandler), NOX_OPL(kPolygonFlagSet),
    NOX_OPL(kPolygonFlagClear),
    // Groups
    NOX_OPL(kGroupKindObjects), NOX_OPL(kGroupKindWaypoints), NOX_OPL(kGroupKindWalls), NOX_OPL(kGroupKindGroups), NOX_OPL(kAddGroup),
    NOX_OPL(kEditGroup), NOX_OPL(kDeleteGroup), NOX_OPL(kAddGroupMembers), NOX_OPL(kRemoveGroupMembers), NOX_OPL(kGroupKind), NOX_OPL(kRenameGroup),
    NOX_OPL(kGroupId), NOX_OPL(kGroupRemoveMember),
    // Map info
    NOX_OPL(kMapInfoTitle), NOX_OPL(kMapInfoDescription), NOX_OPL(kMapInfoMapVersion), NOX_OPL(kMapInfoDate), NOX_OPL(kMapInfoCopyright),
    NOX_OPL(kMapInfoAuthor), NOX_OPL(kMapInfoEmail), NOX_OPL(kMapInfoAuthor2), NOX_OPL(kMapInfoEmail2), NOX_OPL(kMapInfoField),
    NOX_OPL(kMapInfoGameTypeAdd), NOX_OPL(kMapInfoGameTypeRemove), NOX_OPL(kMapInfoVersion), NOX_OPL(kMapInfoMinPlayers),
    NOX_OPL(kMapInfoMaxPlayers), NOX_OPL(kMapInfoQuestIntro), NOX_OPL(kMapInfoQuestPicture), NOX_OPL(kMapInfoQuestPictureName),
    NOX_OPL(kMapAmbient), NOX_OPL(kMapIntro),
    // Floors and edges
    NOX_OPLP(kTiles), NOX_OPL(kFloorUnnamed), NOX_OPLP(kAutoEdge), NOX_OPL(kPaintFloor), NOX_OPL(kEraseFloor), NOX_OPL(kReplaceFloor),
    NOX_OPL(kFillFloor), NOX_OPL(kSmoothFloor), NOX_OPL(kRecomputeEdges), NOX_OPL(kRemoveEdges), NOX_OPL(kRemoveEdge), NOX_OPL(kAddEdge),
    // Walls
    NOX_OPL(kWallUnnamed), NOX_OPLP(kPlaceWalls), NOX_OPLP(kRemoveWalls), NOX_OPL(kWallOutline), NOX_OPL(kWallMaterial), NOX_OPL(kWallFacing),
    NOX_OPL(kWallVariant), NOX_OPL(kWallGroup), NOX_OPL(kWallGroupPolygons), NOX_OPL(kMakeWindow), NOX_OPL(kNoWindow), NOX_OPL(kMakeBreakable),
    NOX_OPL(kNotBreakable), NOX_OPL(kMakeSecret), NOX_OPL(kNotSecret), NOX_OPL(kSecretFlags), NOX_OPL(kSecretWait), NOX_OPL(kSecretState),
    NOX_OPL(kSecretOpening), NOX_OPL(kSecretTimer),
    // Objects, the selection
    NOX_OPL(kPlaceOne), NOX_OPL(kPlaceObjects), NOX_OPL(kScatter), NOX_OPL(kPlacePrefab), NOX_OPLP(kDelete), NOX_OPL(kDeleteItems), NOX_OPLP(kMove),
    NOX_OPLP(kPaste), NOX_OPLP(kDuplicate), NOX_OPLP(kRotateObjects), NOX_OPLP(kChangeType), NOX_OPL(kResetTypeData), NOX_OPL(kSetExtent),
    NOX_OPL(kReorderExtents), NOX_OPL(kAddDetailsOf), NOX_OPL(kRemoveDetailsOf), NOX_OPL(kAddItem), NOX_OPL(kCloneItem), NOX_OPL(kMoveItem),
    NOX_OPL(kEquip), NOX_OPL(kUnequip), NOX_OPL(kGeneratorMonsterEdit), NOX_OPL(kSetFieldOf), NOX_OPL(kSetFieldOn),
    // The Properties panel's own
    NOX_OPL(kAddDetails), NOX_OPL(kRemoveDetails), NOX_OPL(kUpgrade), NOX_OPL(kLink), NOX_OPL(kLinkBothWays), NOX_OPL(kTurn), NOX_OPL(kSetSpell),
    NOX_OPL(kSetSpellUse), NOX_OPL(kRemoveSpell), NOX_OPL(kAddSpell), NOX_OPL(kSetEnchantment), NOX_OPL(kSetEnchantmentLevel),
    NOX_OPL(kSetEnchantmentTime), NOX_OPL(kSetShieldHealth), NOX_OPL(kRemoveEnchantment), NOX_OPL(kAddEnchantment), NOX_OPL(kSetShopCount),
    NOX_OPL(kSetShopItem), NOX_OPL(kMoveShopItem), NOX_OPL(kRemoveShopItem), NOX_OPL(kSetShopBook), NOX_OPL(kSetShopModifier),
    NOX_OPL(kAddShopItem), NOX_OPL(kSetGlyphSpell), NOX_OPL(kRemoveGlyphSpell), NOX_OPL(kAddGlyphSpell), NOX_OPL(kRewardSpells),
    NOX_OPL(kRewardAbilities), NOX_OPL(kRewardFieldGuides), NOX_OPL(kRemoveFromList), NOX_OPL(kAddToList), NOX_OPL(kSetGeneratorMonster),
    NOX_OPL(kRemoveGeneratorMonster), NOX_OPL(kSetGeneratorField), NOX_OPL(kAddGeneratorMonster), NOX_OPL(kRemoveItem), NOX_OPL(kSetColourKey),
    NOX_OPL(kRemoveColourKey), NOX_OPL(kSetIntensityKeys), NOX_OPL(kSetConeKeys), NOX_OPL(kSetLightKey),
    // The script
    NOX_OPL(kCompileScript), NOX_OPL(kDecompileScript), NOX_OPL(kEditScript), NOX_OPL(kImportScript), NOX_OPL(kScriptChangedOnDisk),
    NOX_OPL(kAddFunction),
    // A session's
    NOX_OPL(kSnapshot),
};
#undef NOX_OPL
#undef NOX_OPLP

bool placeholderChar(char c, bool first)
{
    return (c >= 'a' && c <= 'z') || c == '_' || (!first && c >= '0' && c <= '9');
}

/// A count's text: digits, a minus first allowed.
std::optional<long long> countOf(std::string_view s)
{
    long long n = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc() || end != s.data() + s.size() || s.empty() || s.front() == '+') return std::nullopt;
    return n;
}

bool nested(std::string_view name)
{
    return std::ranges::find(kNestedNames, name) != std::end(kNestedNames);
}

/// One English form of a template, cut at its placeholders: lits[0] {names[0]} lits[1] ... {names[k-1]} lits[k].
struct Matcher {
    const OpLabelTemplate* t = nullptr;
    std::vector<std::string> lits;
    std::vector<std::string> names;
    std::size_t literal = 0; ///< the literal text's length (longer: tried first)
    std::size_t counts = 0;  ///< {n} placeholders (more: tried first)
};

Matcher cut(const OpLabelTemplate& t, std::string_view en)
{
    Matcher m;
    m.t = &t;
    std::string lit;
    for (std::size_t i = 0; i < en.size();) {
        if (en[i] == '{') {
            std::size_t j = i + 1;
            while (j < en.size() && placeholderChar(en[j], j == i + 1)) ++j;
            if (j > i + 1 && j < en.size() && en[j] == '}') {
                m.lits.push_back(std::move(lit));
                lit.clear();
                m.names.emplace_back(en.substr(i + 1, j - i - 1));
                if (m.names.back() == "n") ++m.counts;
                i = j + 1;
                continue;
            }
        }
        lit += en[i++];
    }
    m.lits.push_back(std::move(lit));
    for (const auto& l : m.lits) m.literal += l.size();
    return m;
}

bool fits(std::string_view name, std::string_view value)
{
    return name != "n" || countOf(value).has_value();
}

/// Whether s[pos...] matches m from its placeholder i on (lits[i + 1] after it), filling caps.
bool matchFrom(const Matcher& m, std::string_view s, std::size_t pos, std::size_t i, std::vector<std::string_view>& caps)
{
    if (i == m.names.size()) return pos == s.size();
    const std::string& next = m.lits[i + 1];
    if (i + 1 == m.names.size()) {
        // The last placeholder: up to the closing literal at the end.
        if (s.size() < pos + next.size() || s.substr(s.size() - next.size()) != next) return false;
        caps[i] = s.substr(pos, s.size() - next.size() - pos);
        return fits(m.names[i], caps[i]);
    }
    for (std::size_t at = pos;; ++at) {
        at = next.empty() ? at : s.find(next, at);
        if (at == std::string_view::npos || at > s.size()) return false;
        caps[i] = s.substr(pos, at - pos);
        if (fits(m.names[i], caps[i]) && matchFrom(m, s, at + next.size(), i + 1, caps)) return true;
        if (at == s.size()) return false;
    }
}

struct Shown {
    std::mutex mutex;
    std::vector<Matcher> matchers;                         // (made on first use)
    std::unordered_map<std::string_view, const OpLabelTemplate*> exact; // fixed templates without placeholders
    std::uint64_t revision = ~0ull;
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, std::string, Hash, std::equal_to<>> cache;
};

Shown& shown()
{
    static Shown s;
    return s;
}

void compile(Shown& s)
{
    for (const OpLabelTemplate& t : kTemplates) {
        if (t.key) {
            Matcher m = cut(t, t.key->en);
            if (m.names.empty()) s.exact.emplace(t.key->en, &t);
            else s.matchers.push_back(std::move(m));
        } else {
            s.matchers.push_back(cut(t, t.plural->one));
            s.matchers.push_back(cut(t, t.plural->other));
        }
    }
    // The most particular first: more literal text, then more counts, then a counted template.
    std::ranges::stable_sort(s.matchers, [](const Matcher& a, const Matcher& b) {
        if (a.literal != b.literal) return a.literal > b.literal;
        if (a.counts != b.counts) return a.counts > b.counts;
        return (a.t->plural != nullptr) > (b.t->plural != nullptr);
    });
}

/// The label in the language in use (the shown lock held); nothing when no template matches.
std::optional<std::string> translate(const Shown& s, std::string_view english, int depth)
{
    if (const auto it = s.exact.find(english); it != s.exact.end()) return nox::l10n::tr(*it->second->key);
    std::vector<std::string_view> caps;
    for (const Matcher& m : s.matchers) {
        if (m.literal > english.size() || !english.starts_with(m.lits.front())) continue;
        caps.assign(m.names.size(), {});
        if (!matchFrom(m, english, m.lits.front().size(), 0, caps)) continue;
        std::vector<nox::l10n::Arg> args;
        args.reserve(caps.size());
        long long count = 1;
        for (std::size_t i = 0; i < caps.size(); ++i) {
            if (m.names[i] == "n") count = *countOf(caps[i]);
            std::optional<std::string> inner;
            if (depth < 3 && nested(m.names[i])) inner = translate(s, caps[i], depth + 1);
            args.emplace_back(m.names[i], inner ? std::string_view(*inner) : caps[i]);
        }
        const std::string& pattern = m.t->key ? nox::l10n::tr(*m.t->key) : nox::l10n::plural(nox::l10n::slotOf(*m.t->plural), count);
        return nox::l10n::format(pattern, std::span<const nox::l10n::Arg>(args));
    }
    return std::nullopt;
}

} // namespace

std::string opLabel(const nox::l10n::Key& text, std::initializer_list<nox::l10n::Arg> args)
{
    return nox::l10n::format(text.en, args);
}

std::string opLabel(const nox::l10n::PluralKey& text, std::initializer_list<nox::l10n::Arg> args)
{
    long long n = 0;
    for (const auto& a : args) {
        if (a.name == "n") n = countOf(a.value).value_or(0);
    }
    return nox::l10n::format(n == 1 ? text.one : text.other, args);
}

const std::string& shownOpLabel(std::string_view english)
{
    Shown& s = shown();
    std::lock_guard lock(s.mutex);
    const std::uint64_t revision = nox::l10n::languageRevision();
    if (s.revision != revision || s.cache.size() > 4096) {
        s.cache.clear();
        s.revision = revision;
    }
    if (const auto it = s.cache.find(english); it != s.cache.end()) return it->second;
    std::string text;
    if (nox::l10n::language() == "en") {
        text = std::string(english);
    } else {
        if (s.matchers.empty()) compile(s);
        text = translate(s, english, 0).value_or(std::string(english));
    }
    return s.cache.emplace(std::string(english), std::move(text)).first->second;
}

std::span<const OpLabelTemplate> opLabelTemplates()
{
    return kTemplates;
}

} // namespace nox::mapedit
