// Every stored link between objects in the world model (each ObjectRef field: object_key.hpp),
// listed once per struct with what a map change does to it, and the one walk that carries or clears
// them all (carryLinks, clearLinks).
//
// Why: a link is another object's key, and keys don't survive everything. A map change keeps the
// players and what they carry, and anything else they name is left behind; a key that named a thing
// left behind can name the new map's thing of that number (the weapon in hand, the spell target,
// the minimap marks, a creature's generator... once did). Server::loadMapKeeping carries
// every link of what stays through carryLinks, so none can be forgotten:
//  - a field of type ObjectRef (or a list or array of them, or a part holding them) must be listed
//    in its struct's Links<> table below, with its rule: a test ("the object links: every
//    ObjectRef field...") reads the headers and fails for one that isn't;
//  - each table is checked at compile time: the members exist, each rule fits the member's type
//    (a part's rule is Nested, a list's may be CarryDropLost...), no member twice.
// A record read from a map, save or .plr names things by map number or script id, never by key
// (MapNumber can't become an ObjectRef), so a load's renumbering has no links to carry: what a
// record's object has is cleared by clearLinks where a loader wants it so (a .plr's item).
//
// Not links (kept as numbers): a creature's action stack arguments (positions and keys mixed by
// action, Monster::Item::args), a saved record's script ids (Monster::SavedRefs), Moonglow's
// lasting spell's value (its Moonglow's key in an int: carried by loadMapKeeping itself), the
// frame's own lists (World's new list, removed, forgetting, the motion queue, the pending casts: the
// world's bookkeeping, made afresh with it), a message's address (World::Broadcast::to) and a
// sound's source (SoundEvent): this frame's.
#pragma once

#include "noxcore/server/object_key.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <array>
#include <list>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace nox::server {

/// What a map change does to a link (carryLinks).
enum class LinkRule : std::uint8_t {
    Carry,         ///< to what it named, now (its key, kept); none when that stayed behind
    CarryDropLost, ///< (a list) each carried, and those left behind taken out
    CarryOrLost,   ///< carried; one left behind names kLostLink, not none (DialogWith: still talking)
    Clear,         ///< none (a list emptied)
    Keep,          ///< as it is: on what a map change never keeps, or a link it keeps as it stands
    Nested,        ///< a part with links of its own (a struct, a box, a list of them): each by its table
    Fresh,         ///< the new map's own (a World's: made afresh with it, nothing carried)
};

/// What a link to a thing left behind becomes under CarryOrLost.
inline constexpr ObjectKey kLostLink = 0xFFFFFFFFu;

/// A table's entry: the member, its rule and its name (the test matches names with the headers').
template <class S, class M>
struct Link {
    using Member = M;
    M S::*member;
    LinkRule rule;
    std::string_view name;
};
#define NOX_LINK(S, m, rule) ::nox::server::Link<S, decltype(S::m)>{&S::m, ::nox::server::LinkRule::rule, #m}

/// A struct's links: specialised below for each struct holding any (`name`: as the headers spell its
/// scope; `fields`: a tuple of NOX_LINK entries).
template <class S>
struct Links;

template <class T>
concept HasLinks = requires { Links<T>::fields; };

namespace detail {

template <class T>
struct IsBoxed : std::false_type {};
template <class T, class W>
struct IsBoxed<Boxed<T, W>> : std::true_type {
    using type = T;
};
template <class T>
struct Seq : std::false_type {};
template <class T, class A>
struct Seq<std::vector<T, A>> : std::true_type {
    using type = T;
    static constexpr bool clearable = true;
};
template <class T, class A>
struct Seq<std::list<T, A>> : std::true_type {
    using type = T;
    static constexpr bool clearable = true;
};
template <class T, std::size_t N>
struct Seq<std::array<T, N>> : std::true_type {
    using type = T;
    static constexpr bool clearable = false;
};

/// Whether a link's element type is a key: an ObjectRef, or a pair of them (a bolt's ends).
template <class T>
inline constexpr bool kKeyLike = std::is_same_v<T, ObjectRef> || std::is_same_v<T, std::pair<ObjectRef, ObjectRef>>;

/// Whether `rule` fits a member of type M (each table's entries are checked with it).
template <class M>
consteval bool ruleFits(LinkRule r)
{
    if (r == LinkRule::Fresh) {
        return true; // (never walked)
    }
    if constexpr (std::is_same_v<M, ObjectRef>) {
        return r == LinkRule::Carry || r == LinkRule::CarryOrLost || r == LinkRule::Clear || r == LinkRule::Keep;
    } else if constexpr (IsBoxed<M>::value) {
        return HasLinks<typename IsBoxed<M>::type> && r == LinkRule::Nested;
    } else if constexpr (HasLinks<M>) {
        return r == LinkRule::Nested;
    } else if constexpr (Seq<M>::value) {
        using E = typename Seq<M>::type;
        if constexpr (kKeyLike<E>) {
            return r == LinkRule::Carry || r == LinkRule::Clear || r == LinkRule::Keep ||
                   (r == LinkRule::CarryDropLost && std::is_same_v<E, ObjectRef> && Seq<M>::clearable);
        } else if constexpr (HasLinks<E>) {
            return r == LinkRule::Nested || (r == LinkRule::Clear && Seq<M>::clearable);
        } else {
            return false;
        }
    } else {
        return false;
    }
}

template <class Tuple, std::size_t... I>
consteval bool tableFits(const Tuple& t, std::index_sequence<I...>)
{
    bool ok = true;
    ((ok = ok && ruleFits<typename std::tuple_element_t<I, Tuple>::Member>(std::get<I>(t).rule)), ...);
    // (no member listed twice)
    const std::array<std::string_view, sizeof...(I)> names{std::get<I>(t).name...};
    for (std::size_t a = 0; a < names.size(); ++a) {
        for (std::size_t b = a + 1; b < names.size(); ++b) {
            ok = ok && names[a] != names[b];
        }
    }
    return ok;
}

} // namespace detail

/// Each of a table's members of `s`, as op(member, rule).
template <class S, class Op>
void forEachLinkField(S& s, Op&& op)
{
    std::apply([&](const auto&... f) { (op(s.*(f.member), f.rule), ...); }, Links<std::remove_const_t<S>>::fields);
}

/// The map change's walk (and clearLinks'): `carried` gives a key's thing's key now (0: left
/// behind), or is null to clear every link.
template <class Carried>
class LinkWalk {
public:
    explicit LinkWalk(const Carried* carried) noexcept : carried_(carried) {}

    template <class S>
        requires HasLinks<S>
    void walk(S& s) const
    {
        forEachLinkField(s, [&](auto& m, LinkRule rule) { member(m, rule); });
    }

private:
    void ref(ObjectRef& r, LinkRule rule) const
    {
        if (!carried_) {
            r = 0;
            return;
        }
        switch (rule) {
        case LinkRule::Carry:
        case LinkRule::CarryDropLost:
            r = (*carried_)(r);
            break;
        case LinkRule::CarryOrLost:
            if (r != 0) {
                const ObjectKey now = (*carried_)(r);
                r = now != 0 ? now : kLostLink;
            }
            break;
        case LinkRule::Clear:
            r = 0;
            break;
        case LinkRule::Keep:
        case LinkRule::Nested:
        case LinkRule::Fresh:
            break;
        }
    }
    void element(ObjectRef& r, LinkRule rule) const { ref(r, rule); }
    void element(std::pair<ObjectRef, ObjectRef>& p, LinkRule rule) const
    {
        ref(p.first, rule);
        ref(p.second, rule);
    }
    template <class E>
        requires HasLinks<E>
    void element(E& e, LinkRule) const
    {
        walk(e);
    }

    template <class M>
    void member(M& m, LinkRule rule) const
    {
        if (rule == LinkRule::Fresh || (carried_ && rule == LinkRule::Keep)) {
            return;
        }
        if constexpr (std::is_same_v<M, ObjectRef>) {
            ref(m, rule);
        } else if constexpr (detail::IsBoxed<M>::value) {
            if (m) {
                walk(*m);
            }
        } else if constexpr (HasLinks<M>) {
            walk(m);
        } else if constexpr (detail::Seq<M>::value) {
            using E = typename detail::Seq<M>::type;
            if constexpr (detail::Seq<M>::clearable) {
                if (rule == LinkRule::Clear || (!carried_ && detail::kKeyLike<E>)) {
                    m.clear();
                    return;
                }
            }
            for (auto& e : m) {
                element(e, rule);
            }
            if constexpr (std::is_same_v<M, std::vector<ObjectRef>>) {
                if (rule == LinkRule::CarryDropLost) {
                    std::erase(m, 0u);
                }
            }
        }
    }

    const Carried* carried_;
};

/// A map change's links of `s` (an object kept, a lasting spell, the World's carried state): each
/// by its rule, `carried(key)` giving a kept thing's key now and 0 for one left behind.
template <class S, class Carried>
void carryLinks(S& s, const Carried& carried)
{
    LinkWalk<Carried>(&carried).walk(s);
}

/// Every link of `s` none, lists emptied, its parts' too (rules aside): a record's object made
/// into a thing of its own (a .plr's item).
template <class S>
void clearLinks(S& s)
{
    struct None {
        ObjectKey operator()(ObjectKey) const noexcept { return 0; }
    };
    LinkWalk<None>(nullptr).walk(s);
}

// --- The tables -----------------------------------------------------------------------------------

template <>
struct Links<PlayerControl::MinimapMark> {
    using S = PlayerControl::MinimapMark;
    static constexpr std::string_view name = "PlayerControl::MinimapMark";
    static constexpr auto fields = std::tuple{NOX_LINK(S, object, Carry)}; // (one left behind goes with the list's walk: loadMapKeeping)
};

template <>
struct Links<PlayerControl> {
    using S = PlayerControl;
    static constexpr std::string_view name = "PlayerControl";
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, cursorObject, Clear),
        NOX_LINK(S, dialogWith, CarryOrLost), // (the exe keeps its pointer to the old map's NPC)
        NOX_LINK(S, harpoonTarget, Clear),
        NOX_LINK(S, harpoonBolt, Clear),
        NOX_LINK(S, marks, Clear),
        NOX_LINK(S, teleportedBy, Clear),
        NOX_LINK(S, minimapMarks, Nested),
        NOX_LINK(S, exitGate, Clear),         // (the new outfit's playerMakeDefItems)
        NOX_LINK(S, warpGate, Clear),
        NOX_LINK(S, soulGate, Clear),         // (its checkpoint)
        NOX_LINK(S, ankhsTaken, Clear),
        NOX_LINK(S, cameraFollow, Clear),
        NOX_LINK(S, assistBy, Clear),
        NOX_LINK(S, currentWeapon, Carry),    // (the exe's ud->currentWeapon: the same thing after the switch)
        NOX_LINK(S, spellTarget, Carry),
    };
};

template <>
struct Links<ServerObject::ModSize> {
    using S = ServerObject::ModSize;
    static constexpr std::string_view name = "ServerObject::ModSize";
    static constexpr auto fields = std::tuple{NOX_LINK(S, ownerCleared, Carry)}; // (a kept pixie's: with its owner)
};

template <>
struct Links<ServerObject::Chakram> {
    using S = ServerObject::Chakram;
    static constexpr std::string_view name = "ServerObject::Chakram";
    // (a chakram in flight is never kept)
    static constexpr auto fields = std::tuple{NOX_LINK(S, target, Keep), NOX_LINK(S, lastHit, Keep)};
};

template <>
struct Links<ServerObject::Trap> {
    using S = ServerObject::Trap;
    static constexpr std::string_view name = "ServerObject::Trap";
    static constexpr auto fields = std::tuple{NOX_LINK(S, target, Carry)}; // (a carried glyph's)
};

template <>
struct Links<ServerObject::Trigger> {
    using S = ServerObject::Trigger;
    static constexpr std::string_view name = "ServerObject::Trigger";
    static constexpr auto fields = std::tuple{NOX_LINK(S, toucher, Keep)}; // (triggers stay with their map)
};

template <>
struct Links<ServerObject::Transport> {
    using S = ServerObject::Transport;
    static constexpr std::string_view name = "ServerObject::Transport";
    static constexpr auto fields = std::tuple{NOX_LINK(S, link, Keep)}; // (pads stay with their map)
};

template <>
struct Links<ServerObject::Mover> {
    using S = ServerObject::Mover;
    static constexpr std::string_view name = "ServerObject::Mover";
    static constexpr auto fields = std::tuple{NOX_LINK(S, carriedObject, Keep)}; // (movers stay with their map)
};

template <>
struct Links<ServerObject::Ball> {
    using S = ServerObject::Ball;
    static constexpr std::string_view name = "ServerObject::Ball";
    static constexpr auto fields = std::tuple{NOX_LINK(S, toucher, Keep)}; // (the flag ball stays with its map)
};

template <>
struct Links<ServerObject::Crown> {
    using S = ServerObject::Crown;
    static constexpr std::string_view name = "ServerObject::Crown";
    static constexpr auto fields = std::tuple{NOX_LINK(S, returnTo, Carry)};
};

template <>
struct Links<ServerObject::Monster> {
    using S = ServerObject::Monster;
    static constexpr std::string_view name = "ServerObject::Monster";
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, enemy, Carry), // (its AI's targets: mapSwitchLevel resets an isUnit's, the map's load its stack and sight when it came by an exit)
        NOX_LINK(S, seen, Carry),
        NOX_LINK(S, lastEnemy, Carry),
        NOX_LINK(S, focus, Carry),
        NOX_LINK(S, heardFrom, Clear),
        NOX_LINK(S, weapon, Carry),     // (an NPC's weapon in hand: one it carries)
        NOX_LINK(S, transits, Clear),
        NOX_LINK(S, bombTarget, Carry), // (a bomber's glyph's)
    };
};

template <>
struct Links<ServerObject> {
    using S = ServerObject;
    static constexpr std::string_view name = "ServerObject";
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, holder, Carry),
        NOX_LINK(S, hurtBy, Clear), // (mapSwitchLevel: every one's hurtBy cleared)
        NOX_LINK(S, player, Nested),
        NOX_LINK(S, monster, Nested),
        NOX_LINK(S, flyCaster, Carry), // (a turned spell projectile's first caster: a kept pixie's)
        NOX_LINK(S, modSize, Nested),
        NOX_LINK(S, owner, Carry),
        NOX_LINK(S, seekTarget, Clear), // (a homing missile's: of what stays, only a pixie's can be set; its target forgotten)
        NOX_LINK(S, chakram, Nested),
        NOX_LINK(S, trap, Nested),
        NOX_LINK(S, trigger, Nested),
        NOX_LINK(S, transport, Nested),
        NOX_LINK(S, mover, Nested),
        NOX_LINK(S, ball, Nested),
        NOX_LINK(S, crown, Nested),
        NOX_LINK(S, spawnedBy, Carry), // (left behind: its death no longer counts against the new map's generator of that number)
        NOX_LINK(S, elevatorLink, Keep), // (lifts stay with their map)
        NOX_LINK(S, inventory, CarryDropLost),
    };
};

template <>
struct Links<WallCell> {
    using S = WallCell;
    static constexpr std::string_view name = "WallCell";
    static constexpr auto fields = std::tuple{NOX_LINK(S, door, Keep)}; // (walls stay with their map)
};

template <>
struct Links<DurationSpell> {
    using S = DurationSpell;
    static constexpr std::string_view name = "DurationSpell";
    // (A solo game's lasting spell on a kept player goes on with its links carried; Moonglow's value
    // is its Moonglow's key: loadMapKeeping carries it.)
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, source, Carry),   NOX_LINK(S, caster, Carry),    NOX_LINK(S, glyphExtent, Carry), NOX_LINK(S, rayTarget, Carry),
        NOX_LINK(S, target, Carry),   NOX_LINK(S, bolts, Carry),     NOX_LINK(S, lastBolts, Clear),   NOX_LINK(S, object, Carry),
        NOX_LINK(S, staff, Carry),
    };
};

template <>
struct Links<ServerTeam> {
    using S = ServerTeam;
    static constexpr std::string_view name = "ServerTeam";
    // (Its players stay; a member that isn't a player is dropped by the map change itself, one left
    // behind by the walk. Its base and crown were the old map's.)
    static constexpr auto fields = std::tuple{NOX_LINK(S, members, CarryDropLost), NOX_LINK(S, base, Clear), NOX_LINK(S, crown, Clear)};
};

template <>
struct Links<ActiveAbility> {
    using S = ActiveAbility;
    static constexpr std::string_view name = "ActiveAbility";
    static constexpr auto fields = std::tuple{NOX_LINK(S, unit, Keep)}; // (the World's list is the new map's: Fresh there)
};

template <>
struct Links<TradeSession::Item> {
    using S = TradeSession::Item;
    static constexpr std::string_view name = "TradeSession::Item";
    static constexpr auto fields = std::tuple{NOX_LINK(S, object, Keep)};
};

template <>
struct Links<TradeSession> {
    using S = TradeSession;
    static constexpr std::string_view name = "TradeSession";
    // (Every session ends on the old map: mapChangeTrades. The World's list is Fresh.)
    static constexpr auto fields = std::tuple{NOX_LINK(S, a, Keep), NOX_LINK(S, b, Keep), NOX_LINK(S, stock, Nested), NOX_LINK(S, offersA, Nested),
                                              NOX_LINK(S, offersB, Nested)};
};

template <>
struct Links<World::LevelUpMoment> {
    using S = World::LevelUpMoment;
    static constexpr std::string_view name = "World::LevelUpMoment";
    static constexpr auto fields = std::tuple{NOX_LINK(S, unit, Carry), NOX_LINK(S, object, Clear)}; // (its player kept; its LevelUp went with the old map)
};

template <>
struct Links<World::SaveRequest> {
    using S = World::SaveRequest;
    static constexpr std::string_view name = "World::SaveRequest";
    static constexpr auto fields = std::tuple{NOX_LINK(S, exit, Clear)}; // (its exit went with the old map)
};

template <>
struct Links<World::Respawn> {
    using S = World::Respawn;
    static constexpr std::string_view name = "World::Respawn";
    static constexpr auto fields = std::tuple{NOX_LINK(S, object, Keep)}; // (the World's list is the new map's: Fresh there)
};

template <>
struct Links<World::HeardSound> {
    using S = World::HeardSound;
    static constexpr std::string_view name = "World::HeardSound";
    static constexpr auto fields = std::tuple{NOX_LINK(S, source, Keep)}; // (the World's list is the new map's: Fresh there)
};

template <>
struct Links<World> {
    using S = World;
    static constexpr std::string_view name = "World";
    // What a map change carries of the game's state into the new map's World (loadMapKeeping, once
    // it has copied it over): the rest is the new map's, made with it.
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, durationSpells_, Fresh), // (a solo game's lasting spells on kept players are copied in, each carried by itself)
        NOX_LINK(S, activeAbilities_, Fresh),
        NOX_LINK(S, teams, Nested),
        NOX_LINK(S, levelUp, Nested),
        NOX_LINK(S, saveRequest, Nested),
        NOX_LINK(S, imaginaryCaster, Fresh),
        NOX_LINK(S, respawns, Fresh),
        NOX_LINK(S, heardSounds, Fresh),
        NOX_LINK(S, trades, Fresh),
        NOX_LINK(S, tradeObjects, Fresh),
        NOX_LINK(S, walls_, Fresh),
        NOX_LINK(S, freeing_, Fresh),
    };
};

/// Every struct with a table (the test walks them).
template <class... S>
struct LinkStructs {};
using AllLinkStructs = LinkStructs<PlayerControl::MinimapMark, PlayerControl, ServerObject::ModSize, ServerObject::Chakram, ServerObject::Trap,
                                   ServerObject::Trigger, ServerObject::Transport, ServerObject::Mover, ServerObject::Ball, ServerObject::Crown,
                                   ServerObject::Monster, ServerObject, WallCell, DurationSpell, ServerTeam, ActiveAbility, TradeSession::Item,
                                   TradeSession, World::LevelUpMoment, World::SaveRequest, World::Respawn, World::HeardSound, World>;

namespace detail {
template <class... S>
consteval bool allTablesFit(LinkStructs<S...>)
{
    return (tableFits(Links<S>::fields, std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<decltype(Links<S>::fields)>>>{}) && ...);
}
static_assert(allTablesFit(AllLinkStructs{}), "an object link's rule doesn't fit its member, or a member is listed twice (object_links.hpp)");
} // namespace detail

} // namespace nox::server
