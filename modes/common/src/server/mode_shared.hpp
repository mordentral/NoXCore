// What Delving Mode and Undead Tide share: their maps' names and folders, a chat line, their
// lobbies' things, a creature type's name for players, a run's host options, and the lock that
// closes the game while a run is on. (Internal to noxcore's server sources.)
#pragma once

#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/which_map.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace nox::server::detail {

// (isMap and WhichMap: which_map.hpp.)

/// Whether a map's name is a mode's generated one: `prefix` ("DEL_"), case aside, and at least one
/// more letter.
inline bool hasMapPrefix(std::string_view name, std::string_view prefix)
{
    return name.size() > prefix.size() && util::equalsNoCase(name.substr(0, prefix.size()), prefix);
}

/// Every map folder in `mapsDir` whose name `generated` takes, deleted; how many went.
template <class Pred>
int sweepMaps(const std::filesystem::path& mapsDir, Pred generated)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> doomed;
    for (const auto& e : fs::directory_iterator(mapsDir, ec)) {
        if (e.is_directory(ec) && generated(e.path().filename().string())) {
            doomed.push_back(e.path());
        }
    }
    int removed = 0;
    for (const auto& dir : doomed) {
        if (fs::remove_all(dir, ec) != static_cast<std::uintmax_t>(-1) && !ec) {
            ++removed;
        }
    }
    return removed;
}

/// The named map folders in `mapsDir` deleted (those there); how many went.
inline int sweepMapsNamed(const std::filesystem::path& mapsDir, std::initializer_list<std::string> names)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    int removed = 0;
    for (const std::string& name : names) {
        if (fs::exists(mapsDir / name, ec) && fs::remove_all(mapsDir / name, ec) != static_cast<std::uintmax_t>(-1) && !ec) {
            ++removed;
        }
    }
    return removed;
}

/// A line for MSG_TEXT_MESSAGE: at most 250 characters (its length is a byte, the end counted:
/// a longer one is misread by every client).
inline std::u16string widen(std::string_view s)
{
    return {s.begin(), s.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(s.size(), 250))};
}

/// A line to one player alone, as is (MSG_TEXT_MESSAGE from no one).
inline void tellPlayer(World& world, const ServerObject& player, std::string_view text)
{
    world.broadcasts().push_back({World::textMessage(widen(text), 0), true, player.extent});
}

/// Whether `a` stands within `d` of (x, y): the modes' own reach tests (signs, gates, fires).
inline bool within(const ServerObject& a, float x, float y, float d)
{
    return (a.x - x) * (a.x - x) + (a.y - y) * (a.y - y) < d * d;
}

/// The thing types a mode's director finds on its own maps once they load (its data.luau's
/// `things`: thing.bin's names, the script's, never the code's): the signs, the portal, the
/// records board. Interned as they're read, so each test is a pointer's.
struct ThingTypes {
    std::vector<Name> signs;
    Name portal;
    Name board;
    bool isSign(const Name& type) const noexcept { return std::ranges::find(signs, type) != signs.end(); }
};
inline ThingTypes thingTypes(const modes::script::MapThings& things)
{
    ThingTypes t;
    for (const std::string& s : things.signs) {
        t.signs.emplace_back(s);
    }
    t.portal = Name(things.portal);
    t.board = Name(things.board);
    return t;
}

/// A generated lobby's things as it loads: its signs west to east, its portal and its records
/// board (`types`), each the last found; 0: none.
struct LobbyThings {
    std::vector<std::uint32_t> signs;
    std::uint32_t portal = 0;
    std::uint32_t board = 0;
};
inline LobbyThings lobbyThings(const World& world, const ThingTypes& types)
{
    LobbyThings found;
    std::vector<const ServerObject*> signs;
    for (const auto& [extent, o] : world.objects()) {
        if (o.gone()) {
            continue;
        }
        if (types.isSign(o.typeName)) {
            signs.push_back(&o);
        } else if (o.typeName == types.portal) {
            found.portal = extent;
        } else if (o.typeName == types.board) {
            found.board = extent;
        }
    }
    std::ranges::sort(signs, [](const ServerObject* a, const ServerObject* b) { return a->x < b->x; });
    for (const ServerObject* s : signs) {
        found.signs.push_back(s->extent);
    }
    return found;
}

/// A creature type's name as players read it: "OgreBrute" -> "Ogre Brute".
inline std::string spacedName(std::string_view type)
{
    std::string s;
    for (std::size_t i = 0; i < type.size(); ++i) {
        if (i > 0 && std::isupper(static_cast<unsigned char>(type[i])) && std::islower(static_cast<unsigned char>(type[i - 1]))) {
            s += ' ';
        }
        s += type[i];
    }
    return s;
}

/// The lock while a run is on: a closed game, its allowed list everyone in it now (one who drops
/// may come back), as the Access page's Closed with names Added (the game list shows it so); the
/// host's own Closed and list kept for unlockGame.
inline void lockGame(GameHost& host, bool& savedClosed, std::vector<std::string>& savedAllowed)
{
    GameHost::AccessSettings a = host.accessSettings();
    savedClosed = a.closed;
    savedAllowed = a.allowed;
    a.closed = true;
    for (const auto& [peer, p] : host.players()) {
        if (std::ranges::find(a.allowed, p.name) == a.allowed.end()) {
            a.allowed.push_back(p.name);
        }
    }
    host.setAccessSettings(a);
}

/// A run's host options: the mode picks its maps (no cycle, no map vote), a quest of its one
/// generated map alone, with no rules, stage or results screens, and the players' own characters
/// never overwritten by the quest's.
inline void runHostOptions(GameHost::Options& o, const std::string& mapName)
{
    o.mapCycleOn = false;
    o.mapVote = false;
    o.quest = true;
    o.questScreens = false;
    o.questSaves = false;
    o.questMapsOnly = {mapName};
}

/// The host's own Closed and allowed list back (lockGame's), the rest of its Access as it is now.
inline void unlockGame(GameHost& host, bool savedClosed, const std::vector<std::string>& savedAllowed)
{
    GameHost::AccessSettings a = host.accessSettings();
    a.closed = savedClosed;
    a.allowed = savedAllowed;
    host.setAccessSettings(a);
}

} // namespace nox::server::detail
