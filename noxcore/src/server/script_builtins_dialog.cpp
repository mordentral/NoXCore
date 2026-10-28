// The map script's dialog and journal builtins: ScriptHost::builtin's
// ids 126-130, 157-159, 171 and 177 come here.
#include "noxcore/server/script_host.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/server/dialog.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <random>
#include <vector>

namespace nox::server {

namespace {

/// Plr_GetFirstUnit / Plr_GetNextUnit: every player's unit, by its index.
std::vector<ServerObject*> playerUnitsInOrder(World& world)
{
    return playersInOrder(world, [](const ServerObject& o) { return !o.gone(); });
}

/// MSG_JOURNAL_MSG (0xD5, 68 bytes): `action` (2 remove, 3 update), the name (a strcpy into 64
/// bytes), the type (a remove's: what its stack held, 0 here).
std::vector<std::uint8_t> journalMessage(std::uint8_t action, std::string_view name, std::uint16_t type)
{
    std::vector<std::uint8_t> m(0x44, 0);
    m[0] = 0xD5;
    m[1] = action;
    name = name.substr(0, name.find('\0'));
    for (std::size_t i = 0; i < name.size() && 2 + i < m.size(); ++i) {
        m[2 + i] = static_cast<std::uint8_t>(name[i]);
    }
    m[0x42] = static_cast<std::uint8_t>(type);
    m[0x43] = static_cast<std::uint8_t>(type >> 8);
    return m;
}

/// comJournalEntryRemove: its entry of that name gone (Plr_RemoveJournalEntry: the
/// first, newest first; an exact match), its client told (SrvNet_SendJournalRemove). (The
/// exe's host, slot 0x1F, updates its own journal window instead: ours is a client, told.)
void journalRemove(World& world, ServerObject& unit, std::string_view name)
{
    if (!unit.player) {
        return;
    }
    name = name.substr(0, name.find('\0'));
    auto& journal = unit.player->journal;
    const auto it = std::ranges::find_if(journal, [&](const PlayerControl::JournalEntry& e) { return e.name == name; });
    if (it == journal.end()) {
        return;
    }
    journal.erase(it);
    world.broadcasts().push_back({journalMessage(2, name, 0), true, unit.extent});
}

/// comJournalEntryUpdate: its entry of that name's type (Plr_SetJournalEntryType),
/// its client told (SrvNet_SendJournalUpdate; the host's too, as above).
void journalUpdate(World& world, ServerObject& unit, std::string_view name, std::uint16_t type)
{
    if (!unit.player) {
        return;
    }
    name = name.substr(0, name.find('\0'));
    auto& journal = unit.player->journal;
    const auto it = std::ranges::find_if(journal, [&](const PlayerControl::JournalEntry& e) { return e.name == name; });
    if (it == journal.end()) {
        return;
    }
    it->type = type;
    world.broadcasts().push_back({journalMessage(3, it->name, it->type), true, unit.extent});
}

} // namespace

int ScriptHost::dialogBuiltin(std::uint32_t id, script::Vm& vm)
{
    std::minstd_rand rng; // (none of these draws)
    Fight fight{world_, rng, frame_, &things_, nullptr};
    switch (id) {
    case 126: { // SetDialog(npc, type, start, end)
        const std::int32_t end = vm.popInt();
        const std::int32_t start = vm.popInt();
        const std::uint8_t type = dialogType(vm.string(vm.pop()));
        if (ServerObject* npc = object(vm.popInt())) {
            setDialog(*npc, type, start, end);
        }
        return 0;
    }
    case 127: // CancelDialog(npc)
        if (ServerObject* npc = object(vm.popInt())) {
            cancelDialog(*npc);
        }
        return 0;
    case 128: { // StoryPic(npc, picture)
        const std::string picture = vm.string(vm.pop());
        if (ServerObject* npc = object(vm.popInt())) {
            storyPic(*npc, picture);
        }
        return 0;
    }
    case 129: { // TellStory(sound, text): to the caller from the trigger
        const std::string text = vm.string(vm.pop());
        const std::string soundName = vm.string(vm.pop());
        const std::uint32_t sound = audio::soundId(soundName).value_or(0); // (utilFindSound: 0 unknown)
        ServerObject* player = vm.caller() ? world_.byExtent(vm.caller()) : nullptr;
        ServerObject* npc = vm.trigger() ? world_.byExtent(vm.trigger()) : nullptr;
        openNpcDialog(fight, player, npc, sound, text); // (SrvNet_OpenNpcDialog)
        return 0;
    }
    case 130: { // StartDialog(npc, player)
        ServerObject* player = object(vm.popInt());
        ServerObject* npc = object(vm.popInt());
        if (player && npc) {
            dialogStart(fight, *player, *npc); // (SrvNet_OnDialogStart)
        }
        return 0;
    }
    case 171: { // GetAnswer(npc)
        const ServerObject* npc = object(vm.popInt());
        vm.pushInt(dialogAnswerOf(npc));
        return 0;
    }
    case 177: { // IsTalking(): the host (player 0x1F) talking to an NPC
        const ServerObject* host = hostPlayer(world_);
        vm.pushInt(host && host->player && host->player->dialogWith != 0 ? 1 : 0);
        return 0;
    }
    case 157: { // JournalEntry(object, name, type)
        const auto type = static_cast<std::uint16_t>(vm.popInt());
        const std::string name = vm.string(vm.pop());
        const std::int32_t who = vm.popInt();
        if (who == 0) {
            // every player's (comJournalEntryAdd), no sound.
            for (ServerObject* u : playerUnitsInOrder(world_)) {
                addJournalEntry(world_, *u, name, type);
            }
        } else if (ServerObject* o = object(who)) {
            // (Any object: the exe takes it for a player's unit, ours adds nothing to another.)
            addJournalEntry(world_, *o, name, type);
            if (type & 0xBu) {
                playSound(world_, 0x387u, *o); // (aud(0x387, it, 0, 0))
            }
        }
        return 0;
    }
    case 158: { // JournalDelete(object, name) (script_JournalDelete)
        const std::string name = vm.string(vm.pop());
        const std::int32_t who = vm.popInt();
        if (who == 0) {
            for (ServerObject* u : playerUnitsInOrder(world_)) { // (comRemoveEntryAll)
                journalRemove(world_, *u, name);
            }
        } else if (ServerObject* o = object(who)) {
            journalRemove(world_, *o, name);
        }
        return 0;
    }
    case 159: { // JournalEdit(object, name, type) (script_JournalEdit)
        const auto type = static_cast<std::uint16_t>(vm.popInt());
        const std::string name = vm.string(vm.pop());
        const std::int32_t who = vm.popInt();
        if (who == 0) {
            for (ServerObject* u : playerUnitsInOrder(world_)) { // (comUpdateEntryAll)
                journalUpdate(world_, *u, name, type);
            }
        } else if (ServerObject* o = object(who)) {
            journalUpdate(world_, *o, name, type);
        }
        return 0;
    }
    default:
        return 0;
    }
}

} // namespace nox::server
