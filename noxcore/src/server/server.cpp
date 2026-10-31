// The headless game server (server.hpp): map loads and map changes (what carries over from one
// map to the next), the frame in the exe's tick order, its end, and the substeps between frames.
#include "noxcore/server/server.hpp"

#include "noxcore/map/sections.hpp"
#include "noxcore/server/abilities.hpp"
#include "noxcore/server/collide.hpp"
#include "noxcore/server/dialog.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/item_effects.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/npc.hpp"
#include "noxcore/server/object_links.hpp"
#include "noxcore/server/object_save.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/profile.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/solo_save.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace nox::server {

// The world's links to the game's data and the server's own tables (each map's load sets them on
// its new world again).
Server::Server(const things::ThingDb& things, Options options)
    : things_(things)
    , options_(options)
    , clock_(options.multiplier)
    , soundTable_(things)
    , rng_(options.seed)
    , playerAnimations_(PlayerAnimations::load(things))
{
    world_.gameFlags = options_.gameFlags;
    world_.gameData = options_.gameData;
    world_.monsterDb = options_.monsterDb;
    world_.strings = options_.strings;
    world_.soundSets = options_.soundSets;
    world_.soundTable = &soundTable_;
    world_.soundLimit = options_.frameSounds;
    world_.playerAnimations = &playerAnimations_;
    world_.thingDb = &things_;
    world_.modifierDb = options_.modifiers;
    world_.rng = &rng_;
}

HostClient* Server::seenHostClient()
{
    return modeHooks_ ? modeHooks_->hostClient(hostClient_) : hostClient_;
}

void Server::loadMap(const std::filesystem::path& path)
{
    loadMap(map::loadMapFile(path), path.filename().string());
}

void Server::loadMap(const map::MapFile& map, std::string_view fileName)
{
    const util::RoundingScope rounding(options_.exactMaths); // (the game's x87 rounding: x87.hpp)
    loadMapKeeping(map, nullptr, fileName);
}

void Server::switchMap(const map::MapFile& map, std::string_view fileName, std::string_view waypoint)
{
    const util::RoundingScope rounding(options_.exactMaths);
    // The map's MapExit functions first.
    if (script_) {
        script_->setFrame(clock_.frame());
        script_->runEntryPoints("MapExit");
    }
    World old = std::move(world_);
    loadMapKeeping(map, &old, fileName, waypoint);
}

// Loads `map` into a fresh world. With `old` (a map change) the players, what they hold and the
// game's lasting state carry over from it, renumbered where their extents would clash with the
// new map's things; then the mode is set up, the players placed and the script started.
void Server::loadMapKeeping(const map::MapFile& map, World* old, std::string_view fileName, std::string_view waypoint)
{
    if (&map != &mapFile_) {
        mapFile_ = map;
    }
    // (The script ids' counter goes on from map to map: only a game's start or load sets it.
    // The things are put in the world at this frame.)
    const std::int32_t scriptIds = old ? old->nextScriptId() : world_.nextScriptId();
    // (mapSwitchLevel -> the pending casts' clear, before the map is read) A new
    // ImaginaryCaster takes the next script id ahead of the map's things (made below, once the kept
    // things have their extents).
    const bool caster = things_.findThing("ImaginaryCaster") != nullptr;
    // What stays (the map change's sweep(1)): the players and what they hold (a player its direct holder);
    // out of a network game the creatures friendly to them (isUnit) and a player's Moonglow; in
    // a solo game a player's pixies. What anything else holds, those creatures'
    // too, goes (its holder isn't a player).
    const auto keptOf = [](World& from) {
        std::vector<std::uint32_t> kept;
        for (auto& [extent, o] : from.objects()) {
            if (o.gone() || o.pendingDelete) {
                continue;
            }
            const ServerObject* owner = o.owner ? from.byExtent(o.owner) : nullptr;
            const bool moonglow = o.typeName == "Moonglow" && owner && (owner->classes & things::ObjectClass::Player);
            if (o.player || (o.holder == 0 && (isUnit(from, o) || (!from.network() && moonglow) || playersPixie(from, o)))) {
                kept.push_back(extent);
            }
        }
        for (std::size_t i = 0; i < kept.size(); ++i) { // (what the players hold)
            if (const ServerObject* k = from.byExtent(kept[i]); k && k->player) {
                for (const std::uint32_t e : k->inventory) {
                    if (std::ranges::find(kept, e) == kept.end()) {
                        kept.push_back(e);
                    }
                }
            }
        }
        return kept;
    };
    // A map change (mapSwitchLevel): first, before the new map is read, each player
    // thawed (unfreezeUnit with lock 1: its creatures with it) and its trade cancelled, on the old
    // map (trade.hpp: what it offered back to it; the sessions end with the old map).
    std::vector<std::uint32_t> kept;
    if (old) {
        Fight f{*old, rng_, clock_.frame(), &things_, options_.modifiers};
        mapChangeTrades(f);
        // (mapSwitchLevel's loop: the new things' pass, unitsNewAddToList, ahead of each sweep.)
        // What was made since the last pass goes on the lists and into the motion first, so what
        // of it stays has its place there as the rest does (below).
        queueNewObjects(*old);
        kept = keptOf(*old); // (what the trades gave back among it)
        // What doesn't stay is freed (all of it marked deleted
        // first): what it owns passed up to its owner (unitTransferSlaves: past
        // the deleted), it let go of by its own (unitClearOwner: a player's
        // creature is lost to it, MSG_REPORT_LOSE_CREATURE, important: the client's cage empties
        // at its CLIENT_READY), then out of its team (servFinalizeDelObject:
        // MSG_TEAM_MSG 2, an empty team gone as removeTeamMember has it).
        const std::unordered_set<std::uint32_t> stays(kept.begin(), kept.end());
        std::vector<std::uint32_t> left;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> slaves;
        for (auto& [extent, o] : old->objects()) {
            if (!stays.contains(extent)) {
                left.push_back(extent);
                o.pendingDelete = true;
            }
            if (o.owner != 0) {
                slaves[o.owner].push_back(extent);
            }
        }
        for (const std::uint32_t e : left) {
            ServerObject& o = old->objects().at(e);
            if (const auto it = slaves.find(e); it != slaves.end()) {
                ServerObject* to = o.owner ? old->byExtent(o.owner) : nullptr;
                for (const std::uint32_t s : it->second) {
                    if (ServerObject* slave = old->byExtent(s); slave && slave->owner == e) {
                        setOwner(*old, to, *slave); // (past the deleted: never to one that goes)
                    }
                }
            }
            clearOwner(*old, o);
            removeTeamMember(*old, o);
        }
    }
    // (The exe's things keep their net codes through a map change: the new map's things, a saved
    // copy's made in play among them, don't take the numbers of those that stay, what the trades
    // gave back among them.)
    world_ = World::load(map, things_, old ? old->gameFlags & ~(GameFlag::GameOver | 0x4000000u) : options_.gameFlags,
                         options_.gameData, options_.modifiers, options_.monsterDb, scriptIds + (caster ? 1 : 0), clock_.frame(), kept);
    if (old) {
        // (What the switch told the players on the old map, the trades' and the creatures' ends,
        // goes out with the new map's news, first.)
        world_.broadcasts().insert(world_.broadcasts().begin(), std::make_move_iterator(old->broadcasts().begin()),
                                   std::make_move_iterator(old->broadcasts().end()));
    }
    world_.strings = options_.strings;
    world_.rng = &rng_;
    world_.hostClient = seenHostClient();
    world_.modeHooks = modeHooks_;
    world_.questScreens = options_.questScreens;
    world_.questSaves = options_.questSaves;
    world_.forceClass = options_.forceClass;
    world_.multiclass = options_.multiclass;
    world_.monstersAlert = options_.monstersAlert;
    world_.keepThingsOnDeath = options_.keepThingsOnDeath;
    world_.modeOwnsBook = options_.modeOwnsBook;
    world_.sandboxPlayersCantDie = options_.playersCantDie;
    world_.sandboxNoScoring = options_.noScoring;
    world_.modeOwnsLimits = options_.modeOwnsLimits;
    world_.pickupAnyClass = options_.pickupAnyClass;
    world_.pickupDuplicates = options_.pickupDuplicates;
    world_.soundSets = options_.soundSets;
    world_.soundTable = &soundTable_;
    world_.soundLimit = options_.frameSounds;
    world_.playerAnimations = &playerAnimations_;
    world_.gameplayFlags = old ? old->gameplayFlags : options_.gameplayFlags;
    world_.lessonLimits = old ? old->lessonLimits : options_.lessonLimits;
    world_.timeLimits = old ? old->timeLimits : options_.timeLimits;
    world_.serverOptions = old ? old->serverOptions : options_.serverOptions;
    world_.autoRespawn = old ? old->autoRespawn : options_.autoRespawn;
    world_.keepAutoRespawn = old ? old->keepAutoRespawn : options_.autoRespawn;
    world_.clanTeams = old ? old->clanTeams : false;
    world_.wolGame = old ? old->wolGame : options_.wolGame;
    world_.leaveBones = old ? old->leaveBones : false;
    world_.gestures = old ? old->gestures : true;
    world_.questMaps = &questMaps_;
    world_.questSendSave = questSendSave_;
    world_.questSaveAndKick = questSaveAndKick_;
    {
        // (MonsterXfer's read of a saved record's enchantments and poison: object_save.hpp.)
        Fight f = fight();
        readLoadedEnchantments(f);
    }
    // The map's secret areas (from server_mapRWPolygons) and its quest
    // briefing (server_mapRWMapInfo).
    world_.questSecretsHere = static_cast<int>(std::ranges::count_if(world_.polygons(), [](const MapPolygon& p) { return (p.flags & 1u) != 0; }));
    if (map.find(map::MapInfo::kName)) {
        const auto info = map::decodeSection<map::MapInfo>(map);
        world_.questIntro = info.questIntro;
        world_.questGraphic = info.questGraphic;
        world_.mapTypes = info.gameFlags; // (mapLoad's)
    }
    if (old) {
        // The quest's numbers and the classes' multipliers: the exe's globals.
        world_.questDamage = old->questDamage;
        world_.questHealth = old->questHealth;
        world_.mapDifficulty = old->mapDifficulty;
        world_.questStage = old->questStage;
        world_.questDifficulty = old->questDifficulty;
        world_.warpGatesOn = old->warpGatesOn;
        world_.questBosses = old->questBosses;
        world_.questWarpState = old->questWarpState;
        world_.questWarpGui = old->questWarpGui;
        world_.questWarpEnterFrame = old->questWarpEnterFrame;
        world_.questNextMap = old->questNextMap;
        world_.questChosen = old->questChosen;
        world_.soloQuestCountdown = old->soloQuestCountdown;
        world_.questWarped = old->questWarped;
        world_.questSecretsLast = old->questSecretsHere;
        world_.questSlotSeenAt = old->questSlotSeenAt;
        world_.ticks = old->ticks; // (the solo pause's globals, solo.hpp)
        world_.pauseUntilTick = old->pauseUntilTick;
        world_.levelUp = old->levelUp; // (its LevelUp went with the old map: carryLinks below)
        world_.gameFlags |= old->gameFlags & GameFlag::Paused;
        world_.saveRequest = old->saveRequest; // (requestSave's globals; its exit went with the old map: carryLinks below)
        world_.saveName = old->saveName;
        world_.cameByExit = old->cameByExit;
        world_.saveByExit = old->saveByExit;
        world_.switchToSolo = old->switchToSolo;
        world_.noMultiplayer = old->noMultiplayer;
        world_.soloPath = old->soloPath;
        world_.loadingSavedGame = old->loadingSavedGame;
        world_.savedGameLoadedAt = old->savedGameLoadedAt;
        world_.healthMultiplier = old->healthMultiplier;
        world_.manaMultiplier = old->manaMultiplier;
        world_.strengthMultiplier = old->strengthMultiplier;
        world_.speedMultiplier = old->speedMultiplier;
        world_.multipliersBeforeQuest = old->multipliersBeforeQuest;
        world_.questVariables = old->questVariables; // (the scripts' quest variables, a global list)
        world_.pendingMagicWalls = old->pendingMagicWalls;
        world_.pendingAbility = old->pendingAbility;
        world_.briefingByte = old->briefingByte;
        // The host's spells and types turned off (global tables in the exe: spellEnable)
        // stay off from map to map; a solo game's start turns them all on (below).
        world_.spellDisabled = old->spellDisabled;
        world_.disabledTypes = old->disabledTypes;
        world_.godMode = old->godMode; // (the engine flags' 0x30: the engine's, kept)
    }
    // Map_GetCurrentName: the file's name less its four-letter extension.
    world_.mapName = std::string(fileName.size() > 4 ? fileName.substr(0, fileName.size() - 4) : fileName);
    // What the map put in the world goes on the respawn list (respawnAdd, from
    // createAt while the list is open: until the first frame's pass). Only a thing flagged
    // RESPAWN (its type's FLAGS: potions, weapons, armour, the crown, creatures), and none in a
    // quest, as unitsNewAddToList adds them: a CTF flag (FLAGS = NULL) never comes back as a new
    // one at its base while it's carried, a broken crate never mends. (noxcpp: none at all for a
    // mode that asks, Options::noMapRespawns: its maps' things stay gone once used or broken.)
    for (const auto& [extent, o] : world_.objects()) {
        if (o.holder == 0 && (o.flags & ObjFlag::Respawn) && !(o.classes & things::ObjectClass::Player) && !(world_.gameFlags & GameFlag::Quest) &&
            !options_.noMapRespawns) {
            addRespawn(world_, o);
        }
    }
    // A game started by the character screen's Load (startFromSave): its first load is the save's.
    const bool startedFromSave = !old && std::exchange(firstLoadFromSave_, false);
    if (startedFromSave) {
        world_.switchToSolo = true;
        world_.noMultiplayer = true;
        world_.soloPath = firstSoloPath_;
        world_.loadingSavedGame = true;
    }
    // What came along from the old map (the exe flags them 0x80000000 for the map load's rest).
    std::vector<std::uint32_t> carriedOver;
    const bool fromSave = (old || startedFromSave) && world_.switchToSolo; // (a visited map's saved copy)
    if (old) {
        // (The thaws and trades: mapChangeTrades above, before the load.)
        world_.freezeLock = old->freezeLock; // (a global, as the thaws left it)
        // The players and what they hold stay (the
        // teams with them), their links to the old map's things gone: the wall they touched,
        // frozen, their marks, the crown frame, their last attacker.
        world_.teams = old->teams; // (their bases and crowns the old map's: carryLinks below)
        for (ServerTeam& t : world_.teams) {
            std::erase_if(t.members, [&](std::uint32_t e) {
                const ServerObject* m = old->byExtent(e);
                return !m || !m->player;
            });
        }
        // What stays: `kept`, as it was before the switch let go of the rest.
        // Each kept under its own extent, a map's number too (a staff picked up off the last map):
        // the exe's things stay the same objects with their NetCodes, so the clients see nothing
        // change, their inventory windows' secondary weapon and quiver kept. The new map's thing
        // of such a number took a fresh one at the load (World::load's reserved extents; its map
        // number still names it). A new one only if the number were taken all the same (never
        // expected), its links to the others carried with it and its client told below.
        std::unordered_map<std::uint32_t, std::uint32_t> renumbered;
        for (const std::uint32_t e : kept) {
            if (!world_.byExtent(e)) {
                renumbered[e] = e;
            }
        }
        for (const std::uint32_t e : kept) {
            if (!renumbered.contains(e)) {
                std::uint32_t fresh = world_.newExtent();
                while (renumbered.contains(fresh) || std::ranges::any_of(renumbered, [&](const auto& r) { return r.second == fresh; })) {
                    fresh = world_.newExtent();
                }
                renumbered[e] = fresh;
            }
            carriedOver.push_back(renumbered.at(e));
        }
        // A link to a kept thing goes to its key now; to anything else, none (the exe's pointer to a
        // freed thing, whose checks find it gone), never the new map's thing that has the number
        // now. Every stored link is carried, cleared or kept by its rule in object_links.hpp
        // (carryLinks): the World's carried state here, each lasting spell and each kept thing below.
        const auto carried = [&](ObjectKey e) -> ObjectKey {
            const auto it = renumbered.find(e);
            return it != renumbered.end() ? it->second : 0u;
        };
        carryLinks(world_, carried); // (the teams' members, bases and crowns, the level-up's player and object, the save's exit)
        // The lasting spells: a solo game's on a player go on (their links carried;
        // one already cancelled too: its end runs at the next pass), the rest are over. (The ends of
        // those the exe cancels here aren't run: not yet.)
        if (world_.gameFlags & GameFlag::Solo) {
            for (const DurationSpell& s : old->durationSpells()) {
                const ServerObject* t = s.target ? old->byExtent(s.target) : nullptr;
                if (t && (t->classes & things::ObjectClass::Player) && carried(s.target) != 0) {
                    DurationSpell k = s;
                    carryLinks(k, carried);
                    if (s.spell == 0x36) { // (Moonglow's value: its Moonglow, a key in a number)
                        k.value = static_cast<int>(carried(static_cast<std::uint32_t>(s.value)));
                    }
                    world_.durationSpells().push_back(std::move(k));
                }
            }
        }
        // Where what stays stood on the updatable list and the missiles' list (the order of their
        // walks), before it leaves the old world.
        std::vector<std::uint32_t> keptUpdatable;
        std::vector<std::uint32_t> keptMissiles;
        old->grid().eachUpdatable([&](ServerObject& o) {
            if (renumbered.contains(o.extent)) {
                keptUpdatable.push_back(o.extent);
            }
        });
        old->grid().eachMissile([&](ServerObject& o) {
            if (renumbered.contains(o.extent)) {
                keptMissiles.push_back(o.extent);
            }
        });
        for (const std::uint32_t e : kept) {
            ServerObject o = std::move(old->objects().at(e));
            o.extent = renumbered.at(e);
            // (decayDestroy: the decay list emptied, each one's decay over; what stays stays,
            // its time where the decay left it.)
            if (o.decayAt != 0) {
                o.timer = o.decayAt;
                o.decayAt = 0;
            }
            // Its links: its owner, holder and inventory, its creature's and player's targets, its
            // parts' (object_links.hpp: the weapon in hand, the spell's target, the minimap marks, a
            // creature's generator, a carried glyph's target, a crown's next holder... carried; its
            // hurtBy, cursor, harpoon, teleport marks, gates and what it heard cleared).
            carryLinks(o, carried);
            if (o.monster) {
                o.monster->heardAt = 0; // (with heardFrom)
            }
            if (o.player) {
                // (The pending casts' clear: the list emptied, each player's spell being
                // spoken (hearsOwnPhonemes, castStart) and its trap's spells with it)
                o.player->chant.reset();
                o.player->castStart = 0;
                o.player->hearsOwnPhonemes = false;
                o.player->trapSpells = {};
                o.player->trapCount = 0;
                o.player->lastWallX = o.player->lastWallY = -1;
                o.player->markUses = {}; // (with its marks)
                o.player->crownAt = 0;
                o.player->polygon = kPolygonUnknown; // (servResetPlayers: 0xDEADFACE)
                // A minimap mark of a thing left behind names nothing now (carryLinks) and goes as
                // the list's walk drops a gone thing's: those of bits 1 and 2 alone.
                std::erase_if(o.player->minimapMarks, [](const PlayerControl::MinimapMark& m) { return m.object == 0 && (m.bits & ~3u) == 0; });
            }
            world_.adopt(std::move(o), false);
        }
        // The object lists are the game's too, not the map's: what stays is still in the grid where
        // it stood, and on the updatable list and the missiles' list in its order, behind every
        // new map thing (the load's new things' pass puts those at the lists' heads). It isn't new:
        // the new things' pass doesn't take it again (it would also join the motion, which only a
        // move does for it here: setPosition).
        for (const std::uint32_t e : kept) {
            if (ServerObject* o = world_.byExtent(renumbered.at(e))) {
                world_.gridLink(*o, o->x, o->y);
            }
        }
        const auto relist = [&](const std::vector<std::uint32_t>& order) {
            std::vector<ServerObject*> objects;
            objects.reserve(order.size());
            for (const std::uint32_t e : order) {
                if (ServerObject* o = world_.byExtent(renumbered.at(e))) {
                    objects.push_back(o);
                }
            }
            return objects;
        };
        world_.grid().appendUpdatable(relist(keptUpdatable));
        world_.grid().appendMissiles(relist(keptMissiles));
        // The motion queue is one list for the game, not the map's: the switch leaves it be, and
        // only a thing's deletion takes one out (servFinalizeDelObject). So what was in it and
        // stays is in it still, ahead of anything the new map adds, in its order. (Ours is the
        // World's: without this a thing that stayed marked in the queue never joined the new one,
        // and never moved again: a player who reached the exit running stood frozen on the next
        // map, running on the spot.)
        std::vector<std::pair<std::uint32_t, std::uint64_t>> stillQueued;
        for (const auto& [e, ticket] : old->motionQueue) {
            const auto r = renumbered.find(e);
            ServerObject* o = r != renumbered.end() ? world_.byExtent(r->second) : nullptr;
            if (!o || !o->inMotion || o->motionTicket != ticket) {
                continue; // (gone with the old map, or a stale place: left and joined again since)
            }
            o->motionTicket = ++world_.motionTicket;
            stillQueued.emplace_back(o->extent, o->motionTicket);
        }
        for (const std::uint32_t e : kept) {
            ServerObject* o = world_.byExtent(renumbered.at(e));
            if (o && o->inMotion && std::ranges::none_of(stillQueued, [&](const auto& q) { return q.first == o->extent; })) {
                o->inMotion = false; // (marked but in no queue: free to join again)
            }
        }
        world_.motionQueue.insert(world_.motionQueue.begin(), stillQueued.begin(), stillQueued.end());
        world_.reservedExtents.clear();
        // A player's thing numbered afresh (the guard above; none expected) would be unknown to its
        // client, which knows it by the old number: ours is the extent the net code, the exe's
        // keep their NetCode across the switch. Its client told, a drop of the old and a pickup
        // of the new with its health, charges and wearing; else its potion keys, its inventory's
        // clicks and drops name a thing it no longer holds. (A quest's .plr load numbers them
        // all afresh and tells them: this is for a carry without one, as a mode's.)
        for (const std::uint32_t e : kept) {
            const std::uint32_t code = renumbered.at(e);
            const ServerObject* item = code != e ? world_.byExtent(code) : nullptr;
            const ServerObject* holder = item && item->holder ? world_.byExtent(item->holder) : nullptr;
            if (!holder || !holder->player) {
                continue;
            }
            const auto u8 = [](std::uint32_t v) { return static_cast<std::uint8_t>(v); };
            const auto tell = [&](std::vector<std::uint8_t> m) { world_.broadcasts().push_back({std::move(m), true, holder->extent}); };
            tell({0x4D, u8(e), u8(e >> 8), u8(item->type), u8(item->type >> 8)}); // MSG_REPORT_DROP
            if (item->classes & 0x13001000u) { // (weapons, wands, armour, flags: items.cpp's kModifiable)
                tell({0x4C, u8(code), u8(code >> 8), u8(item->type), u8(item->type >> 8), item->modifiers[0], item->modifiers[1],  // MSG_REPORT_MODIFIABLE_PICKUP
                      item->modifiers[2], item->modifiers[3]});
            } else {
                tell({0x4B, u8(code), u8(code >> 8), u8(item->type), u8(item->type >> 8)});  // MSG_REPORT_PICKUP
            }
            if (item->maxHealth != 0) {
                tell({0x44, u8(code), u8(code >> 8), u8(item->health), u8(item->health >> 8), u8(item->maxHealth), u8(item->maxHealth >> 8)});  // MSG_REPORT_ITEM_HEALTH
            }
            if (item->maxCharges != 0) {
                tell({0x64, u8(code), u8(code >> 8), item->charges, item->maxCharges});  // MSG_REPORT_CHARGES
            }
            if (item->flags & ObjFlag::Equipped) {
                tell({0x60, u8(code), u8(code >> 8)}); // MSG_REPORT_EQUIP
            }
        }
        // noxcpp: a forced class gone with the game that forced it (a mode's lobby again,
        // its quest over): each player its character's own class back (GameHost tells its client
        // before the map loads there), what it carries kept. Only on a server that forces one: a
        // stock game's player keeps the class its .plr gave it.
        if (world_.forceClass && !forcedClass(world_)) {
            Fight back{world_, rng_, clock_.frame(), &things_, options_.modifiers};
            for (auto& [e, o] : world_.objects()) {
                if (o.player && o.player->ownClass <= 2 && o.player->classId != o.player->ownClass) {
                    forceClass(back, o, o.player->ownClass);
                }
            }
        }
        // (Each player was thawed, unitUnFreeze(unit, 1) with its creatures, on the
        // old map before its trade's cancel: mapChangeTrades above, the exe's order.)
    }
    // (The map's load, each player before its start): out of a solo game its glyphs
    // out (CurTraps) are those it carries.
    const auto glyphsCarried = [&](ServerObject& o) {
        if (world_.gameFlags & GameFlag::Solo) {
            return;
        }
        int n = 0;
        for (const std::uint32_t e : o.inventory) {
            if (const ServerObject* i = world_.byExtent(e); i && i->typeName == "Glyph") {
                ++n;
            }
        }
        o.player->curTraps = static_cast<std::uint8_t>(n);
    };
    // A player at a player start (with `teamBases`, a chat game's with teams: its team's base,
    // within 50), its score and deaths cleared and told (not in a co-op game).
    const auto toPlayerStart = [&](ServerObject& o, bool teamBases) {
        glyphsCarried(o);
        auto [x, y] = findPlayerStart(world_, &o, rng_);
        if (teamBases && (world_.gameFlags & GameFlag::Chat) && teamCount(world_) != 0 && !(world_.gameplayFlags & 2u) &&
            !(world_.gameFlags & 0x8000u)) {
            if (const ServerTeam* t = teamById(world_, o.team); t && t->base) {
                if (const ServerObject* base = world_.byExtent(t->base)) {
                    std::tie(x, y) = clearPointNear(world_, rng_, base->x, base->y, 50.0f);
                }
            }
        }
        setPosition(world_, o, x, y, clock_.frame());
        if (!(world_.gameFlags & GameFlag::Coop)) {
            o.player->lessons = 0;
            o.player->deaths = 0;
            reportLesson(world_, o);
        }
    };
    if (caster) {
        // The ImaginaryCaster at (2944, 2944) (createAt), its script id the one kept
        // for it above.
        const std::int32_t next = world_.nextScriptId();
        if (ServerObject* c = world_.createAt(things_, "ImaginaryCaster", 2944.0f, 2944.0f, clock_.frame())) {
            c->scriptId = scriptIds;
            world_.imaginaryCaster = c->extent;
        }
        world_.setNextScriptId(next);
    }
    // Out of a network game, each of the map's things with a team in it as createAt puts it in the
    // world (createAt: game flag 0x800 or gameplay flag 4; a flag not on a chat map),
    // in the file's order, before the map's flags. (A network game's: placeTeamMembers below.)
    if (!world_.network() && ((world_.gameFlags & GameFlag::Solo) || (world_.gameplayFlags & 4u))) {
        for (const std::uint32_t e : world_.loadOrder) {
            ServerObject* o = world_.byExtent(e);
            if (o && o->team != 0 && o->holder == 0 && !((o->classes & things::ObjectClass::Flag) && (world_.mapTypes & 0x80000000u))) {
                addTeamMember(world_, o->team, *o, false, false, rng_, clock_.frame());
            }
        }
    }
    // Out of a network game, the map's flags (mapReadSetFlags, from the map file's
    // load): a solo map's (type bit 1) the modes off, soloTeam (0x200, team 1), 0x800. (Other
    // maps out of a network game: only solo games run here.)
    const bool soloMapRead = !world_.network() && !(world_.gameFlags & GameFlag::SoloOther) && (world_.mapTypes & 1u);
    if (soloMapRead) {
        world_.gameFlags &= ~0x17F0u;
        soloTeam(world_, rng_, clock_.frame());
        world_.gameFlags |= GameFlag::Solo;
    }
    // A solo game's every spell (spellEnableAll) and weapon and
    // armour type on.
    if (world_.gameFlags & GameFlag::Solo) {
        world_.spellDisabled.fill(false);
        world_.disabledTypes.clear();
    }
    if (old) {
        // (The map load's start): each player's outfit anew (not in a solo or co-op
        // game, 0x200), kept when the next map is a quest's (mapLoad's
        // look at its MapInfo types, bit 2). (A "map:waypoint" exit: that look takes the name less
        // its last four letters from the whole string, finds no such map and leaves the types the
        // current map's: an original bug.)
        Fight f = fight();
        const std::uint32_t types = waypoint.empty() ? world_.mapTypes : old->mapTypes;
        for (auto& [extent, o] : world_.objects()) {
            if (o.player) {
                mapChangePlayer(f, o, (types & 2u) != 0);
            }
        }
    }
    // A network game's mode for this map, by its types and the host's choice (setupMode), its
    // objects of a team in their teams first. (mapReadSetFlags runs for every map but
    // with game flag 0x200000.)
    if (world_.network() && !(world_.gameFlags & GameFlag::SoloOther)) {
        const std::uint32_t mapFlags = map.find(map::MapInfo::kName) ? map::decodeSection<map::MapInfo>(map).gameFlags : 0;
        placeTeamMembers(world_, mapFlags, rng_, clock_.frame());
        Fight f = fight();
        const std::uint32_t chosen = old ? old->chosenMode : options_.gameFlags & 0x17F0u;
        setupMode(f, mapFlags, chosen != 0 ? chosen : GameFlag::Arena);
        if (old && !(world_.gameFlags & GameFlag::Chat)) {
            // The timer: running with anyone in play and a limit, at the limit. Told
            // first (Game_SetTimerRunning's SrvNet_SendTimerStatus: the time left of the end it
            // still has, g_gameTimerEndTicks, the old match's), then set: an
            // original bug.
            world_.timerEnd = old->timerEnd;
            setTimerRunning(world_, anyonePlaying(world_) && enforcedTimeLimit(world_) != 0, clock_.frame());
            world_.timerEnd = clock_.frame() + static_cast<std::uint32_t>(enforcedTimeLimit(world_)) * 60u * 30u;
        }
        if (old) {
            // Each player at a player start (a chat game's team base, within 50), its score and
            // deaths cleared and told.
            for (auto& [extent, o] : world_.objects()) {
                if (o.player) {
                    toPlayerStart(o, true);
                }
            }
        }
        // (past the map's checks) Team play (gameplay flag 4) or game flag 0x8000:
        // the teams' scores reset (Team_ResetAllScores, told).
        if ((world_.gameplayFlags & 4u) || (world_.gameFlags & 0x8000u)) {
            resetTeamScores(world_);
        }
        // (the map's start) Team KotR: each team's crown to a random player of it.
        if ((world_.gameFlags & GameFlag::KingOfTheRealm) && (world_.gameplayFlags & 4u)) {
            crownsToTeams(f);
        }
        // A quest map: the stage up, its generators, bosses and difficulty.
        if (world_.quest()) {
            questMapLoaded(f, fileName);
        }
        resetSuddenDeath(f);
    }
    if (old && !world_.network()) {
        // Out of a network game too, each player at a player start, its score
        // cleared (not a co-op game).
        for (auto& [extent, o] : world_.objects()) {
            if (o.player) {
                toPlayerStart(o, false);
            }
        }
    }
    // soloMapLoaded (every load, under game flag 0x8000000; the calls around it are
    // the host's): a saved copy's SaveGameLocation and its creatures, the host's creatures told it
    // again (solo_save.hpp).
    world_.gameFlags |= 0x8000000u;
    const bool hostThere = hostPlayer(world_) != nullptr;
    soloMapLoaded(*this, fromSave, carriedOver);
    world_.gameFlags &= ~0x8000000u;
    // A game's first load without its host yet (ours: the host's player joins after the load;
    // the exe's is made before it): soloMapLoaded done as it comes in
    // (firstLoadHostIn), switch-to-solo kept till then.
    firstLoadAwaitsHost_ = !old && !hostThere;
    firstLoadSoloMap_ = firstLoadAwaitsHost_ && soloMapRead; // (its soloTeam had no host for team 1)
    // An exit's "map:waypoint" (server_switchToWP): every player there.
    // (The exe's is inside the map load's checks, player starts there and the mode's flags or
    // ball: their failing branch isn't ours yet.)
    if (!waypoint.empty()) {
        if (const ServerWaypoint* wp = world_.waypointMatching(waypoint)) {
            for (auto& [extent, o] : world_.objects()) {
                if (o.player) {
                    setPosition(world_, o, wp->x, wp->y, clock_.frame());
                }
            }
        }
    }
    // Come by an exit: each player's creatures that came with it (isUnit, its owned
    // list newest first) within 50 of it, their actions, target and sight forgotten, escorting it
    // (an NPC's colours sent again). (Its pixies' branch tests the player's class
    // for a missile's, never true: an original bug.)
    if (old && world_.cameByExit) {
        Fight f = fight();
        Monsters mons{f};
        for (auto& [pe, p] : world_.objects()) {
            if (!p.player) {
                continue;
            }
            std::vector<ServerObject*> owned;
            for (auto& [e, o] : world_.objects()) {
                if (o.owner == p.extent && &o != &p && isUnit(world_, o)) {
                    owned.push_back(&o);
                }
            }
            std::ranges::sort(owned, [](const ServerObject* a, const ServerObject* b) {
                return a->ownedOrder != b->ownedOrder ? a->ownedOrder > b->ownedOrder : a->extent > b->extent;
            });
            for (ServerObject* o : owned) {
                const auto [x, y] = clearPointNear(world_, rng_, p.x, p.y, 50.0f);
                setPosition(world_, *o, x, y, clock_.frame());
                if (!o->monster) {
                    continue;
                }
                o->monster->heard = 0;
                mons.clear(*o);
                o->hurtBy = 0;
                o->monster->enemy = 0;
                o->monster->focus = 0;
                o->monster->seenCount = 0;
                o->monster->polygon = kPolygonUnknown; // (its polygon 0xDEADFACE: unknown)
                if (auto* it = mons.push(*o, MonsterAction::Escort)) {
                    it->args[0] = std::bit_cast<std::uint32_t>(p.x);
                    it->args[1] = std::bit_cast<std::uint32_t>(p.y);
                    it->args[2] = p.extent;
                }
                // An NPC's (subclass 0x30) colours told again: its first set to itself.
                if (npcReported(*o)) {
                    setNpcColor(*o, 0, o->monster->colors[0]);
                }
            }
        }
        world_.cameByExit = false;
    }
    // A solo game's map loaded (not a saved game's: with the loads): an AUTOSAVE asked for, 30
    // frames on.
    if ((world_.gameFlags & GameFlag::Solo) && !world_.loadingSavedGame) { // (mapLoadRequired)
        world_.saveName = "AUTOSAVE";
        requestSave(world_, true, 0, 0x1E, clock_.frame());
    }
    world_.loadingSavedGame = false; // (mapLoadOrSaveMB(0))
    if (map.find(map::ScriptObject::kName)) {
        lastScriptCode_ = map::decodeSection<map::ScriptObject>(map).code; // (nc.obj)
    }
    // (serverParseEntireMap -> unitsNewAddToList) The map's monsters'
    // INIT, MonsterInit, as they join the object lists at the load: before any script runs, so
    // MapInitialize's orders go over their starting actions. (Those made later: at their first
    // update, as the next frame's unitsNewAddToList would.)
    {
        Fight f = fight();
        Monsters mons{f};
        for (auto& [extent, o] : world_.objects()) {
            if (o.monster && !o.monster->initialised && !o.gone()) {
                mons.init(o);
            }
        }
    }
    scriptFile_ = map.find(map::ScriptObject::kName)
                      ? script::ScriptFile::parse(map::decodeSection<map::ScriptObject>(map).code)
                      : script::ScriptFile{};
    const std::uint32_t timerIds = script_ ? script_->nextTimerId() : 1u; // (script_activatorsLastID: the game's)
    script_ = std::make_unique<ScriptHost>(world_, things_, scriptFile_, options_.seed);
    script_->setNextTimerId(timerIds);
    script_->setModifiers(options_.modifiers);
    world_.script = script_.get();
    script_->setFrame(clock_.frame());
    if (fromSave) {
        // A visited map's saved copy: its ScriptData read with the sections (server_mapRWScriptData: the
        // globals, the timers), then scriptRunFirst sets the language's four but
        // doesn't run GLOBAL; the timers' objects found (soloMapLoaded's script_activatorResolveObjs, with the host
        // only: nothing run between; the first load's without it, as the host comes in); no
        // MapInitialize (its call only without switch-to-solo).
        if (const map::RawSection* s = map.find(map::ScriptData::kName)) {
            script_->loadData(map::ScriptData::parse(s->data), clock_.frame());
        }
        script_->setLanguageGlobals();
        if (hostThere) {
            script_->resolveTimerObjects();
        }
    } else {
        script_->runGlobalInit();
    }
    mapInitPending_ = !fromSave;
    mapEntryPending_ = true;
    // A solo map's load: paused 30 ticks (0x1E).
    if (world_.gameFlags & GameFlag::Solo) {
        pauseFor(world_, 0x1E);
        countNextFrame_ = !(world_.gameFlags & GameFlag::Paused);
    }
    // (noxcpp: a mode's say once the map's in, before its first frame. None: nothing.)
    if (modeHooks_) {
        modeHooks_->mapLoaded(*this);
    }
}

bool Server::firstLoadHostIn()
{
    ServerObject* host = hostPlayer(world_);
    if (!firstLoadAwaitsHost_ || !host) {
        return false;
    }
    firstLoadAwaitsHost_ = false;
    // The solo map's read (mapReadSetFlags) as the exe's runs it
    // with its host made: the host in team 1 (Team_GetObjectTeamByNetCode of
    // player_netCode's). (soloTeam again: the team, its name and
    // the flags as that load left them.)
    if (std::exchange(firstLoadSoloMap_, false)) {
        soloTeam(world_, rng_, clock_.frame());
    }
    // The map load's soloMapLoaded(switch-to-solo) as the exe runs it with its host made before
    // the load: the host and what it holds there before the map's things (ObjFlags 0x80000000,
    // set over the object lists before the map's read: not looked up as saved, nor deleted after
    // an exit's save).
    std::vector<std::uint32_t> carried{host->extent};
    carried.insert(carried.end(), host->inventory.begin(), host->inventory.end());
    world_.gameFlags |= 0x8000000u;
    const bool fromSave = world_.switchToSolo;
    soloMapLoaded(*this, fromSave, carried);
    if (fromSave && script_) {
        script_->resolveTimerObjects(); // (its script_activatorResolveObjs, the host on the location's script id)
    }
    world_.gameFlags &= ~0x8000000u;
    return true;
}

void Server::step()
{
    const util::RoundingScope rounding(options_.exactMaths); // (the game's x87 rounding: x87.hpp)
    // (A frame counts unless the last tick's end left the game paused: solo.hpp.)
    const bool frameStart = clock_.atFrameStart();
    const int k = clock_.advance(countNextFrame_);
    steppedFrame_ = frameStart;
    lastSubstep_ = k;
    lastMultiplier_ = clock_.multiplier();
    updatedWorld_ = false;
    ranUpdate_ = false;
    if (frameStart) {
        ++world_.ticks;
        ProfileScope timed(ProfileSection::Frame);
        frame();
    } else {
        // (No packets: the exe reads them only in its frame, so whatever came
        // between frames waits for the next one's read.)
        ProfileScope timed(ProfileSection::Substep);
        substep(k, clock_.multiplier());
    }
}

void Server::runFrames(std::uint32_t frames)
{
    const util::RoundingScope rounding(options_.exactMaths);
    // (Ticks: while paused the frame count stands.)
    std::uint32_t ran = 0;
    while (ran < frames || !clock_.atFrameStart()) {
        const bool start = clock_.atFrameStart();
        step();
        ran += start ? 1u : 0u;
    }
}

// One frame (the exe's server tick): a match over only waits out its end, a map load's tick only
// loads; otherwise the frame's checks, the packets and the world's update. Each ends in tickEnd.
void Server::frame()
{
    // What the substeps carried on goes back to its classic place before anything of the frame
    // reads it (the time limit, the chat bases, the packets: the stock server's frame sees the
    // positions its last frame left).
    std::optional<ProfileScope> timedStart;
    timedStart.emplace(ProfileSection::FrameStart);
    const std::uint32_t now = clock_.frame();
    world_.frameNow = now;
    if (script_) {
        script_->setFrame(now); // (the handlers that run this frame see it)
        script_->newFrame();    // (the scripts' frame budget: script::Vm::kMaxFrameSteps)
    }
    // What was made between the ticks (by the host's hand, not a frame's) is as the last tick's
    // end's new things' pass would have found it (unitsNewAddToList), the positions
    // the substeps carried on put back first (the grid has the classic ones: they're not moved).
    for (const std::uint32_t extent : substepsMoved_) {
        if (ServerObject* o = world_.byExtent(extent); o && !o->gone() && o->holder == 0) {
            o->x = o->frameStartX;
            o->y = o->frameStartY;
        }
    }
    substepsMoved_.clear();
    queueNewObjects(world_);
    timedStart.reset();
    if (4096 < world_.sounds().size()) {
        world_.sounds().clear(); // (nobody sending them: a world run without a host)
    }
    // The tick's branches: a match over (game flag 8) first; else a map's
    // load asked for (a global's flag) makes this the load's tick; else the update.
    //
    // A match over: nothing moves; the packets (netlist), then its wait:
    // 10 s on every player drops all it has and is given its outfit again, and the host is asked
    // for the next map (the same one, or the map cycle's: GameHost), which the next tick loads.
    if (world_.gameFlags & GameFlag::GameOver) {
        if (!gameOverEnd_) {
            gameOverStockEnd_ = now + 30u * 10u;
            gameOverEnd_ = std::max(gameOverStockEnd_, gameOverHold_); // (noxcpp's map vote may hold it longer)
        }
        if (packetHook_) {
            packetHook_();
        }
        if (*gameOverEnd_ < now) {
            gameOverEnd_.reset();
            gameOverHold_ = 0;
            world_.gameFlags &= ~GameFlag::GameOver;
            Items items{world_, things_, options_.modifiers, rng_, now};
            for (auto& [extent, o] : world_.objects()) {
                if (o.player && !o.gone()) {
                    items.dropAll(o);
                    items.defaultItems(o);
                }
            }
            nextMapDue_ = true;
        }
        tickEnd();
        return;
    }
    // A map's load: the players' flags dropped, their status 0x10 off, the teams
    // shuffled (beforeMapLoad), then the map's load under game flag 0x8000000 (the host's:
    // GameHost), then the tick's end: no update, no packets, no clients' update.
    if (mapLoadRequested_) {
        mapLoadRequested_ = false;
        {
            Fight f = fight();
            beforeMapLoad(f);
        }
        world_.gameFlags |= 0x8000000u;
        if (mapLoadHook_) {
            mapLoadHook_();
        }
        world_.gameFlags &= ~0x8000000u;
        tickEnd();
        return;
    }
    // The update branch: the host's transfers and clients' update run after the step
    // (GameHost, ranUpdate).
    ranUpdate_ = true;
    // updateServer: the match's time limit (Game_OnTimeLimitExpired), elimination's sudden
    // death, a chat game's team bases, a quest's frame.
    {
        ProfileScope timed(ProfileSection::FrameChecks);
        Fight f = fight();
        checkTimeLimit(f);
        // Not over: in a network game (after the timed bans: the transport's), not a
        // chat game, with the server option (always in CTF and elimination), the
        // camper alarm.
        if (!(world_.gameFlags & GameFlag::GameOver) && (world_.gameFlags & GameFlag::Network) &&
            !(world_.gameFlags & GameFlag::Chat) && world_.serverOption(0x2000u)) {
            camperWatch(f, camperCheckedAt_);
        }
        suddenDeathCheck(f); // (elimination's sudden death: the frame's check)
        // The host's part (the latency report), not with the match over.
        if (updateServerHook_ && !(world_.gameFlags & GameFlag::GameOver)) {
            updateServerHook_();
        }
        // A chat game with teams (not by clan, gameplay flag 2 off): each player in the team whose
        // base is nearest.
        if ((world_.gameFlags & GameFlag::Chat) && !world_.clanTeams && teamCount(world_) != 0 && !(world_.gameplayFlags & 2u)) {
            chatBases(f);
        }
        // A quest's (the update's end): the rejoin, the warp gates.
        if (world_.quest()) {
            questFrame(f);
        }
    }
    // The packets (netlist), at this tick's frame.
    if (packetHook_) {
        ProfileScope timed(ProfileSection::Packets);
        packetHook_();
    }
    // Paused (a solo game's, solo.hpp), the world stands: only the tick's end runs.
    if (!(world_.gameFlags & GameFlag::Paused)) {
        updatedWorld_ = true;
        updateWorld(now);
    }
    tickEnd();
}
void Server::updateWorld(std::uint32_t now)
{
    ProfileScope timedWorld(ProfileSection::UpdateWorld);
    // The units update (updateUnits), from everything's classic position (the
    // substeps carried the important ones on): the players' updates (their own force), the
    // motion of the mobile (physics.hpp), the frame's move of the rest, then the secret walls.
    // (With it, gathered on the way: the players, for the effects pass, which makes none.)
    std::vector<ServerObject*>& players = frameLists_.players;
    players.clear();
    world_.eachObject([&](ServerObject& o) {
        o.x = o.newX = o.frameStartX; // (NewPos where it is till it moves)
        o.y = o.newY = o.frameStartY;
        if (o.player) {
            players.push_back(&o);
        }
    });
    // (In the players' order, Plr_GetFirstUnit's: by player index.)
    std::ranges::sort(players, [](const ServerObject* a, const ServerObject* b) { return a->player->index < b->player->index; });
    // Each player's worn things' UPDATEEFFECTs (item_effects.hpp) and object under its
    // cursor (updateUnits's first pass, Plr_GetFirstUnit's order: cursorObject).
    Fight effects{world_, rng_, now, &things_, options_.modifiers};
    {
        ProfileScope timed(ProfileSection::Effects);
        for (ServerObject* po : players) {
            ServerObject& o = *po;
            if (o.gone()) {
                continue;
            }
            updateEffects(effects, o);
            const ServerObject* at = objectAtCursor(world_, o);
            o.player->cursorObject = at ? at->extent : 0;
        }
    }
    // The updatable list (server_object_updateable: newest first, each one's next
    // taken before its turn), each not DESTROYED (0x20): its UPDATE (not NO_UPDATE's), its fall,
    // then the motion's test: moving, it joins the motion; else it comes to rest, and one without
    // an UPDATE leaves the list (unitRemoveFromUpdatable: a body's poison and
    // enchantments stand till it moves again). What's made meanwhile joins at the tick's end.
    // A frozen player (a dialog's, a shop's, a script's Frozen: NO_UPDATE) skips its update, the only
    // thing that has it sent (unitNeedSync), so the game sends it to nobody and the others' clients
    // stop drawing it 5 frames on: original_bugs.md OB-344, fixed (a deliberate deviation) where
    // several players play a quest or the co-op campaign (a solo game with guests); one player, as
    // the game.
    const bool frozenSent = (world_.quest() || (world_.gameFlags & GameFlag::Solo)) &&
                            std::ranges::count_if(players, [](const ServerObject* p) { return !p->gone(); }) > 1;
    world_.walkPlayers = &players; // (World::playerObjects' list for the walk: none is made in it)
    struct WalkEnd {
        World& w;
        ~WalkEnd() { w.walkPlayers = nullptr; }
    } walkEnd{world_};
    world_.grid().eachUpdatableNextFirst([&](ServerObject& unit) {
        ServerObject* o = &unit;
        if (o->gone() || o->pendingDelete || o->holder != 0) {
            return;
        }
        world_.settleHurtBy(*o); // (its hurtBy on a deleted thing, that one's owner)
        Fight f = fight();
        // (updateUnits: unitNeedSync on each monster of the list after its update.)
        const bool synced = (o->classes & things::ObjectClass::Monster) != 0;
        std::optional<ProfileScope> timedUpdate;
        timedUpdate.emplace(o->player ? ProfileSection::UpdatePlayers : o->monster ? ProfileSection::UpdateMonsters : ProfileSection::UpdateObjects);
        const bool timedRest = frameProfile().on && !o->player && !o->monster;
        const auto restStart = timedRest ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (o->flags & ObjFlag::NoUpdate) {
            // (its Update skipped: still synced, falling and tested; a frozen player sent where
            // several play: frozenSent, OB-344)
            if (frozenSent && o->player) {
                o->touch();
            }
        } else if (o->player && o->player->observing) {
            updateObserver(f, *o); // (Plr_UpdateObserver, its Update while observing)
        } else if (o->player) {
            if (world_.gameFlags & 0x4000000u) {
                suddenDeathDrain(f, *o); // (in updatePlayer)
            }
            updatePlayer(*o, now, playerAnimations_, f);
        } else if (o->classes & things::ObjectClass::Door) {
            updateDoor(world_, *o, now);
        } else if (o->monster && o->updates) {
            Monsters{f}.update(*o); // (MonsterUpdate: monsters.hpp)
        } else if (Objects::acts(*o)) {
            Objects{f, things_}.update(*o);
        }
        // Sent to everyone who sees it each frame, moving or not: clients stop drawing a live
        // creature they've had no word of for 5 frames.
        if (synced) {
            o->touch();
        }
        if (timedRest) {
            frameProfile().addUpdate(Objects::acts(*o) || (o->classes & things::ObjectClass::Door) ? std::string_view(o->updateFn) : std::string_view(),
                                     std::chrono::duration<double>(std::chrono::steady_clock::now() - restStart).count());
        }
        timedUpdate.reset();
        ProfileScope timedFall(ProfileSection::FallSettle);
        Objects::fall(world_, *o, now);
        o->flags &= ~ObjFlag::OnObject; // (updateUnits: an elevator's contact sets it again)
        // Moving, it joins the motion; else it comes to rest (updateUnits: physics.hpp), and without
        // an UPDATE off the list.
        settleOrQueue(world_, *o);
        if ((o->flags & ObjFlag::Resting) && !o->updates) {
            world_.removeUpdatable(*o);
        }
    });
    world_.walkPlayers = nullptr;
    // Each player's control buffer emptied after its update, whichever it is (an observer's too)
    // and whether it ran (updateUnits: playerResetControlBuffer). (The walk's players:
    // none is made in it.)
    for (ServerObject* o : players) {
        if (!o->gone()) {
            o->player->controls.clear();
        }
    }
    // The missiles fly (the units update's second list), then everything else moves.
    {
        ProfileScope timed(ProfileSection::Missiles);
        Missiles{world_, things_, options_.modifiers, rng_, now}.fly();
    }
    // The motion, its touches dispatched at its end (collide).
    {
        std::vector<Contact>& contacts = frameLists_.contacts;
        contacts.clear();
        {
            ProfileScope timed(ProfileSection::Motion);
            moveObjects(world_, rng_, now, &contacts);
        }
        ProfileScope timed(ProfileSection::Contacts);
        Fight f = fight();
        dispatchContacts(f, contacts);
    }
    // The units update's last pass (updateUnits), over the updatable list (each one's next taken after
    // its turn): PrevPos where it was, its NewPos taken, its force spent, its turn taken (Direction1
    // = Direction2: a monster's), its speed for the next frame, then (Fight::unitTail) the slowness
    // and poison counted down, its enchantments (updateUnitBuffs) and the poison's
    // bite. Only what's on the list: a body at rest off it keeps its poison and enchantments.
    {
        ProfileScope timed(ProfileSection::Buffs);
        Fight f = fight();
        world_.grid().eachUpdatable([&](ServerObject& o) {
            if (o.holder != 0) {
                return;
            }
            o.lastX = o.x;
            o.lastY = o.y;
            o.x = o.newX;
            o.y = o.newY;
            o.forceX = o.forceY = 0;
            if (o.monster && o.monster->turnTo >= 0) {
                if (o.direction != o.monster->turnTo) {
                    o.direction = static_cast<std::uint16_t>(o.monster->turnTo);
                    o.touch();
                }
                o.monster->turnTo = -1;
            }
            if (o.slowed > 4) {
                o.slowed = 4;
            }
            // (On the x87: the sum, 5 less the slowness (FILD), their product and x 0.2 all on the
            // stack, one store.)
            o.speedCur = ((util::X87(5.0f) - util::X87::fromDouble(o.slowed)) * (util::X87(o.speedBonus) + o.speedBase) * 0.2f).f();
            if (enchanted(o, Enchant::Slowed)) {
                o.speedCur *= 0.5f;
            }
            if (o.buffs != 0 || o.poison != 0 || o.slowed != 0) {
                f.unitTail(o);
            }
        });
    }
    std::optional<ProfileScope> timedEnd;
    timedEnd.emplace(ProfileSection::EndPass);
    // (What decays is found on the way, marked after the secret walls as before: queueMotion
    // passes over a deleted thing.)
    std::vector<ServerObject*>& decayed = frameLists_.decayed;
    decayed.clear();
    world_.eachObject([&](ServerObject& o) {
        if (o.decayAt != 0 && now >= o.decayAt && o.holder == 0) {
            decayed.push_back(&o);
        }
        if (o.gone() || o.holder != 0) {
            return;
        }
        if (!joinsMotion(o) && !(o.classes & things::ObjectClass::Missile) && (o.vx != 0 || o.vy != 0)) {
            // (noxcpp's: what doesn't join the motion moves by its velocity; the grid kept with it)
            o.x = o.newX = o.frameStartX + o.vx;
            o.y = o.newY = o.frameStartY + o.vy;
            o.touch(); // (unitNeedSync: moved)
            world_.gridRelink(o, o.x, o.y);
        }
        // (A missile's PrevPos is set in its turn, Missiles::flyOne.)
        o.frameStartX = o.x;
        o.frameStartY = o.y;
    });
    world_.stepSecretWalls();
    // What decays goes, the units update's end.
    for (ServerObject* o : decayed) {
        o->pendingDelete = true;
    }
    decayed.clear();
    timedEnd.reset();
    {
        Fight f = fight();
        checkVictory(f); // (server_checkVictory, the units update's last)
    }
    // Then (the tick's order) the respawns; a quest's (the generators' monsters on
    // screen: not yet) difficulty; the spells
    // (spellBookReact: the ones being spoken, then the lasting ones), then the script
    // timers.
    {
        ProfileScope timed(ProfileSection::Respawn);
        Fight f = fight();
        respawnPass(f);
        if (world_.quest()) {
            questDifficultyTick(f);
        }
    }
    {
        ProfileScope timed(ProfileSection::Spells);
        spells().update();
    }
    // The warriors' abilities.
    {
        ProfileScope timed(ProfileSection::Abilities);
        Fight f = fight();
        Abilities{f, things_}.update();
    }
    if (script_) {
        ProfileScope timed(ProfileSection::Scripts);
        script_->setFrame(now);
        script_->runTimers(now);
    }
    ProfileScope timedSweep(ProfileSection::Sweep);
    // The frame's deletions (Delete and the like), their timers with them; one in a
    // team leaves it (delayedDeleteObject's Team_RemoveMember).
    // (The deleted ones gathered on the way, in the world's order, for the sweep's own look; not
    // when a team was left, as a team's going could delete more.)
    // (A mode told may mark more: then the sweep looks again.)
    std::vector<std::uint32_t>& deleted = frameLists_.deleted;
    deleted.clear();
    bool teamLeft = false;
    bool told = false;
    // (Each sweep frees every object marked when its pass looked: when nothing has been marked
    // since (DeleteMark's count), none is, and the pass finds nothing without its walk.)
    const std::uint32_t marks = DeleteMark::marks();
    const bool anyMarked = marks != deletionsLookedAt_;
    deletionsLookedAt_ = marks;
    if (anyMarked) {
        for (auto& [extent, o] : world_.objects()) {
            if (!o.pendingDelete) {
                continue;
            }
            deleted.push_back(extent);
            if (modeHooks_ && (o.modeEvents & ModeEvent::Destroyed)) {
                modeHooks_->destroyed(world_, o); // (noxcpp: told a mode)
                told = true;
            }
            if (o.team != 0) {
                removeTeamMember(world_, o);
                teamLeft = true;
            }
            if (script_) {
                script_->forgetObject(extent);
            }
        }
    }
    world_.sweepDeleted(&rng_, teamLeft ? nullptr : &deleted, !told);
}

void Server::tickEnd()
{
    // The clients' update (SrvNet_UpdateAllClients: the host's hook), after the frame's
    // deletions (World::sweepDeleted) and before the new things' pass: what
    // the frame made isn't in the grid yet, so it's first sent the next tick.
    if (clientsHook_) {
        clientsHook_();
    }
    ProfileScope timedEnd(ProfileSection::TickEnd);
    // Then the tick's end: the things made this tick into the world's lists,
    // those with a COLLIDE into the motion (unitsNewAddToList), a save asked for, a
    // timed pause over, the next frame counted unless paused, the map's start, the
    // level-up moment's end.
    queueNewObjects(world_);
    //
    // The save: the host free to be saved and alive, the MapShutdown
    // scripts, the save under game flag 0x8000000 (the host's saveGame hook); saved
    // and asked by an exit, its map loaded next (mapLoad's WORKING
    // copy: with the loads); not saved and asked by an exit, the player at the exit's way out. The
    // request over (requestSave(0, the exit, 0)).
    if (world_.saveRequest.pending) {
        ServerObject* host = hostPlayer(world_);
        if (host && mayAct(world_, *host, clock_.frame())) {
            const bool dead = (host->flags & ObjFlag::Dead) != 0;
            bool saved = false;
            ServerObject* exit = world_.saveRequest.exit ? world_.byExtent(world_.saveRequest.exit) : nullptr;
            if (!dead) {
                if (script_) {
                    script_->setFrame(clock_.frame());
                    script_->runEntryPoints("MapShutdown");
                }
                world_.gameFlags |= 0x8000000u;
                saved = saveGame_ ? saveGame_(world_.saveName, exit) : true;
                world_.gameFlags &= ~0x8000000u;
            }
            // (Saved only alive.)
            if (saved) {
                if (exit) {
                    // A saved copy of the exit's map in WORKING: switch-to-solo, not
                    // multiplayer, its path (gameSetSwitchSolo / gameSetNoMPFlag / gameSetSoloSavePath), then mapLoad.
                    if (!saveFolder_.empty()) {
                        if (const auto copy = savedMapCopy(saveFolder_, exit->exitMap)) {
                            world_.switchToSolo = true;
                            world_.noMultiplayer = true;
                            world_.soloPath = copy->string();
                        }
                    }
                    world_.exitTo = exit->exitMap;
                }
            } else if (exit) {
                setPosition(world_, *host, exit->exitX, exit->exitY, clock_.frame());
            }
            requestSave(world_, false, world_.saveRequest.exit, 0, clock_.frame());
        }
    }
    pauseTick(world_);
    countNextFrame_ = !(world_.gameFlags & GameFlag::Paused);
    // (The save's tail) The frame is counted here, before the map's start and the level-up's end:
    // they see the next frame's number (ours moves the clock at the next step's start).
    const std::uint32_t next = clock_.frame() + (countNextFrame_ ? 1u : 0u);
    world_.frameNow = next;
    if (script_) {
        script_->setFrame(next);
    }
    if (script_ && (playersPresent_ || options_.startWithoutPlayers)) {
        if ((mapInitPending_ || mapEntryPending_) && !world_.playerUnits().empty()) {
            // (the map's start, its players in): the screen fades in (MSG_FADE_BEGIN
            // {1, 1}), a quest's briefing instead (quest.hpp's questMapStart); in a network game
            // but a chat one everyone in play is INVULNERABLE for good (till it attacks).
            if (!world_.quest()) {
                world_.broadcasts().push_back({{0xE4, 1, 1}, true, 0});
            } else {
                Fight f{world_, rng_, next, &things_, options_.modifiers};
                questMapStart(f);
            }
            if (world_.network() && !(world_.gameFlags & GameFlag::Chat)) {
                for (auto& [extent, o] : world_.objects()) {
                    // (not the host's own, slot 0x1F: its index isn't 0x1F)
                    if (o.player && o.player->index != 0x1F && !(o.player->statusFlags & 1u)) {
                        enchant(world_, o, Enchant::Invulnerable, 0, 5);
                    }
                }
            }
        }
        if (mapInitPending_) {
            mapInitPending_ = false;
            script_->runEntryPoints("MapInitialize");
        }
        if (mapEntryPending_) {
            mapEntryPending_ = false;
            script_->runEntryPoints("MapEntry");
        }
    }
    // In a solo game (not under game flag 0x80000) the ability a .plr left under
    // way (its Berserker) runs again for the first player (Plr_GetFirstUnit), then is forgotten.
    if ((world_.gameFlags & GameFlag::Solo) && !(world_.gameFlags & 0x80000u) && world_.pendingAbility != 0) {
        ServerObject* first = nullptr;
        for (auto& [extent, o] : world_.objects()) {
            if (o.player && !o.gone() && (!first || o.player->index < first->player->index)) {
                first = &o;
            }
        }
        if (first) {
            Fight f{world_, rng_, next, &things_, options_.modifiers};
            Abilities{f, things_}.tryExecute(*first, world_.pendingAbility);
            world_.pendingAbility = 0;
        }
    }
    // For the host's unit (Plr_GetByIndex(0x1F)'s), each tick: a
    // .plr's walls back up once its Wall is found.
    if (const ServerObject* host = hostPlayer(world_)) {
        restoreMagicWalls(world_, *host);
    }
    {
        Fight f{world_, rng_, next, &things_, options_.modifiers};
        levelUpTick(f);
    }
    // A quest chosen (isQuest): startSoloQuest's countdown
    // run down; at 0 the quest begun (game flags 0x17F0 off, 0x1000 on, quest.cpp's
    // startQuest), its first map loaded (a saved quest's: none kept here; else
    // getQuestMapFile's pick) and the stage 0.
    if (world_.questChosen && world_.soloQuestCountdown != 0 && --world_.soloQuestCountdown == 0) {
        Fight f{world_, rng_, next, &things_, options_.modifiers};
        world_.gameFlags = (world_.gameFlags & ~0x17F0u) | GameFlag::Quest;
        startQuest(f);
        if (world_.questMaps) {
            if (const auto pick = world_.questMaps->pick(rng_)) {
                world_.exitTo = *pick; // (mapLoad)
            }
        }
        world_.questStage = 0;
    }
}

void Server::tryAbility(ServerObject& unit, int ability)
{
    Fight f = fight();
    Abilities{f, things_}.tryExecute(unit, ability);
}

void Server::bookFlightBegins(int kind)
{
    if (world_.gameFlags & GameFlag::Solo) {
        Fight f = fight();
        levelUpMoment(f, nullptr, kind);
    }
}

void Server::bookFlightLands()
{
    if (world_.gameFlags & GameFlag::Solo) {
        Fight f = fight();
        levelUpEnd(f);
        setPaused(world_, false, PauseReason::BookFlight);
    }
}

void Server::giveDefaultItems(ServerObject& unit)
{
    Items{world_, things_, options_.modifiers, rng_, clock_.frame()}.defaultItems(unit);
}

bool Server::pickUp(ServerObject& unit, ServerObject& item)
{
    return Items{world_, things_, options_.modifiers, rng_, clock_.frame()}.pickUp(unit, item, true);
}

void Server::substep(int k, int m)
{
    // Nothing moves while the next frame moves nothing: a solo game paused (its frames don't
    // count, the world stands), a match over, a map's load asked for. (Carried on by their last
    // motion they'd go forward and back each frame.)
    if ((world_.gameFlags & (GameFlag::Paused | GameFlag::GameOver)) || mapLoadRequested_) {
        return;
    }
    // Each important object carried on k / m of the way to where the next frame takes it, while
    // no wall stands in its body's way: the ray from its frame's place to the substep's, its size
    // further on, as the frame's own move traces it (updateObjectsVelocity: walls, closed doors, windows unless
    // it goes through them, only sight-blocking walls for the cleansing flames). Blocked, it stays
    // where its last clear substep had it; the next frame puts it where the game really takes it.
    // Its way is its motion, a frame's; a walking, running or standing player's is the next
    // frame's own move (predictedFrameMove: its start or stop shows as the frame makes it).
    // What can move is on the updatable list (every mover goes back on it after the motion) or
    // the missiles' list: no walk of every object.
    std::vector<Carried>& carried = substepCarried_; // (each with where the last substep had it)
    carried.clear();
    const auto carry = [&](ServerObject& o) {
        if (o.priority != Priority::High || o.gone() || o.holder != 0) {
            return;
        }
        float wayX = o.vx;
        float wayY = o.vy;
        if (const auto move = predictedFrameMove(world_, o)) {
            wayX = move->first;
            wayY = move->second;
        }
        const float x = substepPosition(o.frameStartX, wayX, k, m);
        const float y = substepPosition(o.frameStartY, wayY, k, m);
        const float dx = x - o.frameStartX;
        const float dy = y - o.frameStartY;
        const float along = std::sqrt(dx * dx + dy * dy);
        if (along > 0) {
            // (A missile a mode gave a hit shape meets walls by its type's: ModSize keeps it.)
            const ServerObject::ModSize* own = o.modSize && o.modSize->hit ? &*o.modSize : nullptr;
            const ShapeKind shape = own ? own->typeShape : o.shape;
            const float size = shape == ShapeKind::Circle ? (own ? own->typeRadius : o.radius)
                               : shape == ShapeKind::Box  ? (own ? std::max(own->typeBoxWidth, own->typeBoxHeight)
                                                                 : std::max(o.boxWidth, o.boxHeight)) * 0.5f
                                                          : 0.0f;
            const float reachX = x + dx / along * size;
            const float reachY = y + dy / along * size;
            auto flags = static_cast<std::uint8_t>(((o.flags >> 12) & Trace::ThroughWindows) | Trace::Doors);
            if (o.typeName.has(NameTrait::FlameCleanse)) { // (its name ends with "FlameCleanse")
                flags |= Trace::SightOnly;
            }
            if (!world_.traceRay(o.frameStartX, o.frameStartY, reachX, reachY, flags)) {
                return;
            }
        }
        carried.push_back({&o, o.x, o.y});
        o.x = x;
        o.y = y;
    };
    world_.grid().eachUpdatable(carry);
    world_.grid().eachMissile(carry);
    // Then the bodies, everyone carried first so two moving together are judged where both are
    // going: one this substep would push further into another body than the frame started it
    // stays where its last substep had it (the frame's motion would push it back: shown going
    // on, it would be pulled back at the frame). Likewise one pressing further into a wall
    // (held against it: the ray past its edge is clear, but the motion's wall test pushes it out).
    for (const Carried& c : carried) {
        if (substepPressesBody(world_, *c.o) || substepPressesWall(world_, *c.o)) {
            c.o->x = c.x;
            c.o->y = c.y;
        }
        if (c.o->x != c.o->frameStartX || c.o->y != c.o->frameStartY) {
            substepsMoved_.push_back(c.o->extent);
        }
    }
    carried.clear();
}

} // namespace nox::server
