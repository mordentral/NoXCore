// A solo game's save and load: the map written to Save\WORKING\<map>\, Player.plr from
// player_save, the host client's own sections, the AUTOSAVE / SAVE%04d copies, and the fix-ups a
// saved map needs when it is loaded again (script ids back to things). solo_save.hpp has the
// folder layout.
#include "noxcore/server/solo_save.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/map_save.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/map_state.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/object_save.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/player_save.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/things/object_class.hpp"

#include "../map/ai_actions.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

namespace nox::server {

namespace fs = std::filesystem;

namespace {

/// The exe's object list as a saved map's load leaves it: the map's things in its file's order
/// (World::loadOrder), then what was there before (the carried things), ours by extent; then
/// the updatable list's (the missiles).
std::vector<ServerObject*> objectList(World& world, bool missiles)
{
    std::vector<ServerObject*> out;
    for (const std::uint32_t e : world.loadOrder) {
        ServerObject* o = world.byExtent(e);
        if (o && o->holder == 0 && ((o->classes & things::ObjectClass::Missile) != 0) == missiles) {
            out.push_back(o);
        }
    }
    for (auto& [e, o] : world.objects()) {
        if (o.holder == 0 && ((o.classes & things::ObjectClass::Missile) != 0) == missiles && std::ranges::find(out, &o) == out.end()) {
            out.push_back(&o);
        }
    }
    return out;
}

/// The thing with that script id, not destroyed: the object list's things and
/// what each holds (one level), then the updatable list's; none: null. (0 is looked for too.)
ServerObject* byScriptIdFixup(World& world, std::uint32_t id)
{
    const auto live = [&](const ServerObject* o) { return o && !o->gone() && !o->pendingDelete; };
    for (ServerObject* o : objectList(world, false)) {
        if (live(o) && static_cast<std::uint32_t>(o->scriptId) == id) {
            return o;
        }
        for (const std::uint32_t e : o->inventory) {
            ServerObject* i = world.byExtent(e);
            if (live(i) && static_cast<std::uint32_t>(i->scriptId) == id) {
                return i;
            }
        }
    }
    for (ServerObject* o : objectList(world, true)) {
        if (live(o) && static_cast<std::uint32_t>(o->scriptId) == id) {
            return o;
        }
    }
    return nullptr;
}

/// The saved game load's part for a monster: its saved script ids back to things. One
/// that wasn't read from a save (carried over) holds things, not ids: the exe looks its
/// pointers up as ids and finds none, so they're dropped; and its stack's only
/// when it wasn't carried (ObjFlags 0x80000000).
void fixMonster(World& world, ServerObject& o, bool carried)
{
    auto& m = *o.monster;
    const auto refs = m.savedRefs;
    const auto find = [&](std::uint32_t id) -> std::uint32_t {
        const ServerObject* t = byScriptIdFixup(world, id);
        return t ? t->extent : 0u;
    };
    // (A live one's fields: a pointer looked up as a script id finds nothing; a 0 is looked up.)
    const auto liveFind = [&](std::uint32_t extent) -> std::uint32_t { return extent == 0 ? find(0) : 0u; };
    // What it sees (and how many): one not found ends the count at 0.
    for (std::size_t i = 0; i < m.seenCount && i < m.seen.size(); ++i) {
        const std::uint32_t t = refs ? (i < refs->seen.size() ? find(refs->seen[i]) : find(0)) : liveFind(m.seen[i]);
        m.seen[i] = t;
        if (t == 0) {
            m.seenCount = 0;
        }
    }
    m.enemy = refs ? find(refs->enemy) : liveFind(m.enemy);
    if (!carried && refs) {
        // Each stack entry's arguments by its action's types: an object (1) by its script id,
        // a waypoint (2) by its id (server_getWaypointById); 0 stays 0.
        for (int i = 0; i <= m.top && i < static_cast<int>(m.stack.size()); ++i) {
            auto& it = m.stack[static_cast<std::size_t>(i)];
            const auto& def = map::detail::kAiActions[static_cast<std::size_t>(std::clamp(it.action, 0, static_cast<int>(map::detail::kAiActionCount) - 1))];
            for (std::size_t k = 0; k < def.argCount && k < 2; ++k) {
                const std::uint32_t v = refs->stackArgs[static_cast<std::size_t>(i)][k];
                if (def.argTypes[k] == 1) {
                    it.args[2 * k] = v == 0 ? 0u : find(v);
                } else if (def.argTypes[k] == 2) {
                    std::uint32_t index = 0;
                    if (v != 0) {
                        for (std::size_t n = 0; n < world.waypoints().size(); ++n) {
                            if (static_cast<std::uint32_t>(world.waypoints()[n].id) == v) {
                                index = static_cast<std::uint32_t>(n + 1);
                                break;
                            }
                        }
                    }
                    it.args[2 * k] = index;
                }
            }
        }
    }
    // What it heard and its last enemy: the thing's net code (ours its
    // extent); its focus the thing.
    m.heardFrom = refs ? find(refs->heardFrom) : liveFind(m.heardFrom);
    m.lastEnemy = refs ? find(refs->lastEnemy) : liveFind(m.lastEnemy);
    m.focus = refs ? find(refs->focus) : liveFind(m.focus);
    // The transits (and how many): one not found ends the count at 0.
    const std::size_t transits = refs ? refs->transits.size() : m.transits.size();
    std::vector<ObjectRef> kept;
    for (std::size_t i = 0; i < transits; ++i) {
        const std::uint32_t t = refs ? find(refs->transits[i]) : liveFind(m.transits[i]);
        if (t == 0) {
            kept.clear();
            break;
        }
        kept.emplace_back(t);
    }
    m.transits = std::move(kept);
    m.savedRefs.reset();
}

/// The things `owner` owns, its list from the head (newest first: unitSetOwner).
std::vector<ServerObject*> ownedList(World& world, const ServerObject& owner)
{
    std::vector<ServerObject*> out;
    for (auto& [e, o] : world.objects()) {
        if (o.owner == owner.extent && &o != &owner) {
            out.push_back(&o);
        }
    }
    std::ranges::sort(out, [](const ServerObject* a, const ServerObject* b) {
        return a->ownedOrder != b->ownedOrder ? a->ownedOrder > b->ownedOrder : a->extent > b->extent;
    });
    return out;
}

} // namespace

ServerObject* makeSaveLocation(Server& server, ServerObject* exit)
{
    World& world = server.world();
    ServerObject* host = hostPlayer(world);
    if (!host) {
        return nullptr;
    }
    // At the player, or the exit's way out (its exitX / exitY).
    float x = host->x;
    float y = host->y;
    if (exit) {
        x = static_cast<float>(exit->exitX);
        y = static_cast<float>(exit->exitY);
    }
    // (newObjectByTypeID, createAt, in the list at once:
    // unitsNewAddToList)
    ServerObject* location = world.createAt(server.things(), "SaveGameLocation", x, y, server.clock().frame());
    if (!location) {
        return nullptr;
    }
    location->scriptId = host->scriptId;
    const std::uint32_t locationExtent = location->extent;
    for (ServerObject* o : ownedList(world, *host)) {
        if (o->flags & ObjFlag::Active) {
            setOwner(world, world.byExtent(locationExtent), *o);
        }
    }
    return world.byExtent(locationExtent);
}

void restoreFromSaveLocation(Server& server)
{
    World& world = server.world();
    ServerObject* host = hostPlayer(world);
    if (!host) {
        return;
    }
    ServerObject* location = nullptr; // (the first SaveGameLocation in the object list)
    for (auto& [e, o] : world.objects()) {
        if (o.typeName == "SaveGameLocation" && !o.gone()) {
            location = &o;
            break;
        }
    }
    if (!location) {
        return;
    }
    for (ServerObject* o : ownedList(world, *location)) {
        setOwner(world, host, *o);
        if ((o->classes & things::ObjectClass::Monster) && o->monster && (o->monster->status & things::MonsterStatus::Summoned)) {
            o->subclasses |= 0x80u;
            reportAcquired(world, *host, *o); // (SrvNet_SendReportAcquireCreature)
            markMinimap(world, *host, *o, 1); // (netMarkMinimapObject)
        }
    }
    location->scriptId = 0;
    location->pendingDelete = true; // (delayedDeleteObject)
}

map::MapFile saveMap(Server& server)
{
    // mapSaveMap -> its section writer -> mapWriteSectionsMB, the sections'
    // table: the header and MapInfo..MapIntro (map_state.hpp), then ScriptData,
    // ObjectTOC and ObjectData (object_save.hpp).
    map::MapFile out = saveMapSections(server.world(), server.things(), server.mapFile());
    // (server_mapRWScriptObject copies <work dir>\nc.obj, the last script read: a map
    // without one is saved with that one, an original bug.)
    if (!server.mapFile().find(map::ScriptObject::kName) && !server.lastScriptCode().empty()) {
        for (map::RawSection& s : out.sections) {
            if (s.name == map::ScriptObject::kName) {
                map::ScriptObject so = map::ScriptObject::parse(s.data);
                so.code = server.lastScriptCode();
                s.data = so.serialize();
            }
        }
    }
    const map::ScriptData data = server.script() ? server.script()->saveData(server.clock().frame()) : map::ScriptData{};
    out.sections.push_back({std::string(map::ScriptData::kName), data.serialize()});
    SavedObjects objects = saveObjects(server.world(), server.things(), server.modifiers(), server.clock().frame());
    out.sections.push_back({std::string(map::ObjectTOC::kName), objects.toc.serialize()});
    out.sections.push_back({std::string(map::ObjectData::kName), objects.data.serialize()});
    return out;
}

std::int32_t savedScriptCounter(std::span<const std::uint8_t> plr, bool solo)
{
    // In a solo game, GameData's (id 10, version 5) last script id;
    // the script id counter from it, 1000000000 without.
    std::uint32_t firstId = 0;
    if (!plr.empty() && plr.size() % crypto::Blowfish::kBlockSize == 0 && solo) {
        std::vector<std::uint8_t> plain(plr.begin(), plr.end());
        crypto::cipherForKey(crypto::FileKey::PlayerFile).decrypt(plain);
        if (const auto parsed = readLoadedSections(plain)) {
            for (const auto& s : parsed->sections) {
                if (s.id == 10) { // (GameData)
                    try {
                        const auto g = player::GameData::parse(s.data);
                        if (g.version == 5) {
                            firstId = g.lastObjectScriptId;
                        }
                    } catch (const std::exception&) {
                    }
                    break;
                }
            }
        }
    }
    return firstId != 0 ? static_cast<std::int32_t>(firstId) : 1000000000;
}

bool loadHostClientSections(World& world, ServerObject& host, const fs::path& plr)
{
    if (!world.hostClient) {
        return true;
    }
    // Plr_LoadClientPlayerData: its client's sections; the GUI's summon order set by
    // orderUnitLocal (kept, MSG_REPORT_CREATURE_CMD to the host) as
    // plr_rwGUI reads it: a later section failing the load leaves it set.
    HostClient::ClientLoad read;
    const bool clientRead = world.hostClient->loadClientSections(plr, read);
    if (read.summonOrder && (world.gameFlags & GameFlag::Solo) && host.player) {
        host.player->creatureOrder = *read.summonOrder;
        world.broadcasts().push_back({{0xED, *read.summonOrder}, true, host.extent});
    }
    return clientRead;
}

bool loadSoloGame(Server& server, const SoloSaveOptions& options, std::string_view name)
{
    World& world = server.world();
    world.loadingSavedGame = true; // (mapLoadOrSaveMB(1))
    world.gameFlags |= 0x8000000u;  // the saved-game load's flag, cleared again below
    const auto load = [&]() -> bool {
        // The saved game's load. (Game flag 2, a host with a window: the host's.)
        ServerObject* host = hostPlayer(world);
        if (name.empty()) {
            return false;
        }
        // The friendly creatures and the players' pixies.
        for (auto& [e, o] : world.objects()) {
            if (!o.gone() && (isUnit(world, o) || playersPixie(world, o))) {
                o.pendingDelete = true;
            }
        }
        if (name != "WORKING" && !saves::copy(options.gameDir, name, "WORKING")) {
            return false;
        }
        const fs::path plr = options.gameDir / "Save" / "WORKING" / "Player.plr";
        std::error_code ec;
        if (!fs::exists(plr, ec)) {
            world.textToAll(world.text("SaveGame.c:AutoSaveNotFound")); // (Str_GetString qualifies it by SaveGame.c)
            return false;
        }
        std::vector<std::uint8_t> bytes;
        {
            std::ifstream f(plr, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(f), {});
        }
        // The script id counter from the save's GameData (in a solo game).
        world.setNextScriptId(savedScriptCounter(bytes, (world.gameFlags & GameFlag::Solo) != 0));
        world.switchToSolo = true;
        world.noMultiplayer = true;
        if (!host) {
            return false; // (cliPlrInfoLoadFromFile: no player 0x1F)
        }
        Fight fight = server.fight();
        if (!loadPlayerData(fight, *host, bytes)) {
            return false;
        }
        // The map it was saved on (Player->SaveNameBuf), from WORKING.
        const auto& saved = host->player->savedMapName;
        const std::string map(saved.data(), strnlen(saved.data(), saved.size()));
        world.soloPath = (options.gameDir / "Save" / "WORKING" / map / (map + ".map")).string();
        world.exitTo = map + ".map"; // (mapLoad)
        if (world.hostClient) {
            world.hostClient->playMapIntro(false); // (cliPlayMapIntro(0): its own client's)
            if (!loadHostClientSections(world, *host, plr)) {
                return false;
            }
            world.hostClient->gameLoaded(); // (Gui_ClearCenteredMessages)
        }
        world.broadcasts().push_back({{0xCA, 0xAD, 0xDE}, false, 0}); // (SrvNet_BroadcastChatKillAll)
        world.textToAll(world.text("SaveGame.c:GameLoaded")); // (nox.csf's "Savegame.c:gameLoaded")
        return true;
    };
    const bool loaded = load();
    world.gameFlags &= ~0x8000000u;
    world.savedGameLoadedAt = server.clock().frame();
    return loaded;
}

std::optional<fs::path> savedMapCopy(const fs::path& gameDir, std::string_view exitMap)
{
    std::string file(exitMap.substr(0, exitMap.find('\0')));
    if (const auto colon = file.rfind(':'); colon != std::string::npos) {
        file.resize(colon);
    }
    // The folder is the file's name less its last 4 letters (".map").
    const std::string name = file.size() >= 4 ? file.substr(0, file.size() - 4) : std::string();
    const fs::path path = gameDir / "Save" / "WORKING" / name / file;
    std::error_code ec;
    if (!fs::exists(resolveNoCase(path), ec)) { // (_access(path, 0); in any case, as on Windows)
        return std::nullopt;
    }
    return path;
}

void soloMapLoaded(Server& server, bool fromSave, const std::vector<std::uint32_t>& carried)
{
    World& world = server.world();
    ServerObject* host = hostPlayer(world);
    if (!host) {
        return;
    }
    const auto isCarried = [&](std::uint32_t extent) { return std::ranges::find(carried, extent) != carried.end(); };
    if (fromSave) {
        // The object list, from its head (the deleted ones still walked: flag 0x20 is for the
        // lookups).
        for (ServerObject* p : objectList(world, false)) {
            ServerObject& o = *p;
            if (o.typeName == "SaveGameLocation") {
                setPosition(world, *host, o.x, o.y, server.clock().frame());
                host->scriptId = o.scriptId;
                o.scriptId = 0;
                o.pendingDelete = true; // (delayedDeleteObject)
            } else if ((o.classes & things::ObjectClass::Monster) && o.monster) {
                fixMonster(world, o, isCarried(o.extent));
            } else if (!(o.classes & 0xC000u) && (o.classes & things::ObjectClass::Door) && o.doorDirection != o.doorClosed) {
                world.addUpdatable(o); // (an open door: unitAddToUpdatable)
            }
            // (Classes 0x4000 / 0x8000 synced again: ours sends every object anew after a load.)
        }
        // (script_activatorResolveObjs here: the Server does it after this, as
        // loadMapKeeping makes the map's script or as the first load's host comes in
        // (Server::firstLoadHostIn), nothing running between.)
        // the owners the load noted, the newest noted first, each set
        // (unitSetOwner) when both are found; then forgotten.
        for (auto it = world.pendingOwners.rbegin(); it != world.pendingOwners.rend(); ++it) {
            ServerObject* owner = byScriptIdFixup(world, static_cast<std::uint32_t>(it->owner));
            ServerObject* owned = byScriptIdFixup(world, it->owned);
            if (owner && owned) {
                setOwner(world, owner, *owned);
            }
        }
        world.pendingOwners.clear();
        if (world.saveByExit) {
            for (auto& [e, o] : world.objects()) {
                if (isCarried(e) || o.gone() || !isUnit(world, o)) {
                    continue;
                }
                if ((o.classes & things::ObjectClass::Monster) && (o.subclasses & 0x2000u)) {
                    for (const std::uint32_t i : keysOf(o.inventory)) {
                        if (ServerObject* item = world.byExtent(i); item && item->typeName == "Glyph") {
                            deleteObject(world, *item); // (delayedDeleteObject)
                        }
                    }
                }
                o.pendingDelete = true;
            }
            for (auto& [e, o] : world.objects()) {
                if (!isCarried(e) && !o.gone() && playersPixie(world, o)) {
                    o.pendingDelete = true;
                }
            }
        }
    }
    // The host's creatures told it again: those it commands acquired, those it watches watched;
    // its minimap either way.
    std::vector<ServerObject*> owned;
    for (auto& [e, o] : world.objects()) {
        if (o.owner == host->extent && &o != host && (o.classes & things::ObjectClass::Monster)) {
            owned.push_back(&o);
        }
    }
    std::ranges::sort(owned, [](const ServerObject* a, const ServerObject* b) {
        return a->ownedOrder != b->ownedOrder ? a->ownedOrder > b->ownedOrder : a->extent > b->extent;
    });
    for (ServerObject* o : owned) {
        if (o->monster && (o->monster->status & things::MonsterStatus::Summoned)) {
            reportAcquired(world, *host, *o); // (its minimap with it)
        } else if (o->subclasses & 0x80u) {
            reportMonitored(world, *host, *o);
            markMinimap(world, *host, *o, 1);
        }
    }
    world.switchToSolo = false;
    world.saveByExit = false; // (ticks_reset: the host's)
}

bool saveSoloGame(Server& server, const SoloSaveOptions& options, std::string_view name, ServerObject* exit)
{
    World& world = server.world();
    if (world.hostClient) {
        world.hostClient->saveStarting(); // (Gui_CancelDragAndDrop, quickBarClose)
    }
    ServerObject* host = hostPlayer(world);
    if (!host || name.empty() || (host->flags & ObjFlag::Dead)) {
        return false;
    }
    // Save\WORKING\<map>\, one level at a time.
    const fs::path save = options.gameDir / "Save";
    const fs::path working = save / "WORKING";
    const fs::path folder = working / world.mapName;
    if (!saves::makeFolder(save) || !saves::makeFolder(working) || !saves::makeFolder(folder)) {
        return false;
    }
    // The player's things are parked on a SaveGameLocation while the map is written, then given back.
    if (!makeSaveLocation(server, exit)) {
        return false;
    }
    try {
        map::saveMapFileWithCrc(folder / (world.mapName + ".map"), saveMap(server), server.things());
    } catch (const std::exception&) {
        return false; // (the location left as it is: the exe's too)
    }
    restoreFromSaveLocation(server);
    host = hostPlayer(world);
    if (!host) {
        return false;
    }
    // Then the host player's server sections into Player.plr.
    const fs::path plr = working / "Player.plr";
    Fight fight = server.fight();
    const std::vector<std::uint8_t> bytes = savePlayerData(fight, *host);
    if (bytes.empty()) {
        return false;
    }
    {
        std::ofstream f(plr, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) {
            return false;
        }
    }
    // The host's own client's sections appended (the client's: its File Info's stage set just
    // before, the held File Info's bit 8 cleared, its save window's flag never being set).
    std::optional<bool> client;
    if (options.clientSections) {
        client = options.clientSections(plr);
    } else if (world.hostClient) {
        HostClient::ClientSave info;
        info.stage = world.briefingByte;
        info.level = host->player->level;
        info.summonOrder = host->player->creatureOrder;
        info.mapName = world.mapName;
        client = world.hostClient->saveClientSections(plr, info);
    }
    if (!client) {
        // With no client sections the file still ends with its 0 id (the close
        // filling the block: one zero block, the server sections ending on one).
        std::vector<std::uint8_t> end(crypto::Blowfish::kBlockSize, 0);
        crypto::cipherForKey(crypto::FileKey::PlayerFile).encrypt(end);
        std::ofstream f(plr, std::ios::binary | std::ios::app);
        f.write(reinterpret_cast<const char*>(end.data()), static_cast<std::streamsize>(end.size()));
        client = static_cast<bool>(f);
    }
    if (!*client) {
        return false;
    }
    // A named save (an autosave or a slot) gets a copy of WORKING.
    if (name != "WORKING") {
        world.textToAll(world.text("Savegame.c:AutoSaveComplete"));
        if (!saves::copy(options.gameDir, "WORKING", name)) {
            return false;
        }
    }
    world.saveRequest.delay = 0;
    world.saveRequest.at = 0;
    return true;
}

namespace saves {

bool makeFolder(const fs::path& folder)
{
    // _mkdir: only a missing parent (ENOENT) fails; one already there is fine.
    std::error_code ec;
    fs::create_directory(folder, ec);
    return ec != std::errc::no_such_file_or_directory;
}

void remove(const fs::path& gameDir, std::string_view name, bool removeFolder)
{
    const fs::path dir = gameDir / "Save" / name;
    std::error_code ec;
    if (name.empty() || !fs::exists(dir, ec)) {
        return;
    }
    fs::remove(dir / "Player.plr", ec);
    // Each map folder's files, then the folder itself (one level deep).
    std::vector<fs::path> folders;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_directory(ec)) {
            folders.push_back(e.path());
        }
    }
    for (const fs::path& f : folders) {
        for (const auto& e : fs::directory_iterator(f, ec)) {
            if (!e.is_directory(ec)) {
                fs::remove(e.path(), ec);
            }
        }
        fs::remove(f, ec);
    }
    if (removeFolder) {
        fs::remove(dir, ec);
    }
}

bool copy(const fs::path& gameDir, std::string_view from, std::string_view to)
{
    const fs::path src = gameDir / "Save" / from;
    const fs::path dst = gameDir / "Save" / to;
    if (!makeFolder(dst)) {
        return false;
    }
    remove(gameDir, to, false);
    std::error_code ec;
    if (!fs::copy_file(src / "Player.plr", dst / "Player.plr", fs::copy_options::overwrite_existing, ec)) {
        return false;
    }
    if (!fs::is_directory(src, ec)) {
        return false; // (SetCurrentDirectoryA)
    }
    // Every map folder is made first, then their files copied; a file that fails to copy fails the
    // copy but the rest still go.
    std::vector<fs::path> folders;
    for (const auto& e : fs::directory_iterator(src, ec)) {
        if (e.is_directory(ec)) {
            folders.push_back(e.path().filename());
        }
    }
    for (const fs::path& f : folders) {
        if (!fs::create_directory(dst / f, ec)) {
            return false; // (CreateDirectoryA: one already there fails too)
        }
    }
    bool ok = true;
    for (const fs::path& f : folders) {
        for (const auto& e : fs::directory_iterator(src / f, ec)) {
            if (!e.is_directory(ec) && !fs::copy_file(e.path(), dst / f / e.path().filename(), fs::copy_options::overwrite_existing, ec)) {
                ok = false;
            }
        }
    }
    return ok;
}

} // namespace saves

} // namespace nox::server
