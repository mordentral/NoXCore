// A solo save's objects: the live world written as a map's ObjectTOC and ObjectData sections, as
// GAME.EXE's map save (mapSaveMap -> its section writer -> mapWriteSectionsMB, the sections'
// write mode) writes them.
//
//   - ObjectTOC (server_mapRWObjectTOC): version 1; the ids (from 1 in
//     the order met): the object list's things (of a menu-icon type or not) and the things they hold
//     (one level), a monster generator's templates and theirs; the updatable list's
//     (the missiles); the host client's client-only drawables of a type with a menu icon. Then the
//     count and the entries by thing.bin index: id, name.
//   - ObjectData (server_mapRWObjectData): version 1; the object list's things whose
//     type has a MENUICON through xfer_saveObj (one destroyed or
//     pending deletion: nothing; its TOC id, its xfer in write mode, then what it holds, nested,
//     head first); the updatable list's the same, but a thrown chakram (class WEAPON, subclass
//     0x40) writes the chakram it carries at its place; then (not game flag 0x200000)
//     each client-only drawable of a menu-icon type as a fresh object of its type at its
//     truncated place + 0.5, its extent and script id its map number, its flags and xStatus its
//     own (the map record's), a fresh script id taken for each from the world's counter. A 0 ends it.
//   - The object list's order: the exe's list (new things at its head, a frame's batch in the
//     order made): the SaveGameLocation (saveMakePlayerLocation adds it at once), then by the frame made
//     (ServerObject::born) newest first, a frame's in the order made (our extents), the map's
//     last in its file's order (World::loadOrder). The client-only things (not on the exe's
//     server: servMapLoadPlaceObj frees them) come from World::clientOnly's
//     records, the drawable list's order (the file's, reversed).
//   - Every record's header (mapReadWriteObjData, version 0x40): its Extent
//     (ServerObject::fileExtent: 0 for a thing made in play), script id, place; its details when it
//     has any (a script name, things held or owned, a team, flags 0x11408162 or xStatus 0x5E unlike its type's, a
//     pickup script): the flags, name, team, how many it holds, the script ids of what it owns
//     (newest first, of a menu-icon type, not destroyed), xStatus, its pickup handler, its timer less
//     the frame (a decaying thing's end; else the frame it was put in the world).
//   - Each xfer writes its current version (DefaultXfer 0x3C, TriggerXfer 0x3D, ElevatorXfer
//     0x3D, SentryXfer 0x3D, AbilityRewardXfer 0x3D, WeaponXfer 0x40, ArmorXfer 0x3E, ObeliskXfer
//     0x3D, ToxicCloudXfer 0x3D, RewardMarkerXfer 0x3F, MonsterXfer 0x40, NPCXfer 0x3E,
//     MonsterGeneratorXfer 0x3F, the rest 0x3C) from the live object; a script handler names its
//     function only when the script has it (xferReadScriptHandler). What ours doesn't keep comes
//     from the map record the thing was read from (ServerObject::record), else the type's fresh values.
#pragma once

#include "noxcore/map/sections.hpp"
#include "noxcore/server/world.hpp"

#include <cstdint>

namespace nox::server {

struct SavedObjects {
    map::ObjectTOC toc;
    map::ObjectData data;
};

/// The live world's objects as the save writes them (ObjectData's write mode with
/// xfer_saveObj and ObjectTOC alongside), at `frame` (gameFrame). `modifiers`
/// names the items' modifiers (none without). Takes a script id for each client-only record
/// written (the client-only drawables' fresh objects, newObjectWithTypeInd), and puts a thrown
/// chakram's carried chakram at its place, as the exe does.
SavedObjects saveObjects(World& world, const things::ThingDb& things, const things::ModifierDb* modifiers, std::uint32_t frame);
/// One thing's record as its xfer writes it (the header, mapReadWriteObjData, with
/// what it owns and how many it holds, its pickup handler; then its fields), the same as the map
/// save's: what a .plr's Inventory writes for each thing (the things it holds aren't written
/// after it there).
std::vector<std::uint8_t> objectRecord(World& world, const things::ThingDb& things, const things::ModifierDb* modifiers, std::uint32_t frame,
                                       const ServerObject& o);

struct Fight;
/// What MonsterXfer reads into a monster beyond World::load's (XFerMonster's read of a saved
/// record): its enchantments (version 0x3E on, XFer_ReadMonsterBuffs: each one's
/// spell cast on it, spellAccept, then its frames left; the Shield's strength) and
/// its poison (version 0x40, setSomePoisonData). For the map's things in its
/// order, right after the load (the exe: as each record is read).
void readLoadedEnchantments(Fight& fight);
} // namespace nox::server
