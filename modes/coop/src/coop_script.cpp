// The co-op campaign's answers to the map scripts' builtins (coop.hpp's rules): where a script
// means "the player" (its builtin given the host, GetHost's), the party. With no guest in, every
// builtin is the game's own.
#include "noxcore/coop/coop.hpp"

#include "noxcore/server/objects.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace nox::coop {

server::ServerObject* CoopDirector::scriptObject(script::Vm& vm, std::int32_t value)
{
    // (server_scriptValToObjectPtr: -1 the caller, -2 the trigger, else a script id; a caller or
    // trigger being deleted is none.)
    if (value == -1 || value == -2) {
        const std::uint32_t extent = value == -1 ? vm.caller() : vm.trigger();
        server::ServerObject* o = extent ? world().byExtent(extent) : nullptr;
        return o && !o->pendingDelete ? o : nullptr;
    }
    return world().byScriptId(value);
}

std::optional<int> CoopDirector::builtin(server::ScriptHost& script, std::uint32_t id, script::Vm& vm)
{
    if (!host_ || party_.size() < 2) {
        return std::nullopt; // (alone: the game's own)
    }
    if (id == ids_.startupScreen || id == ids_.deathScreen) {
        // The game's chapter screen (the host's), then the guests': each NoX Core guest waited for.
        const std::uint32_t arg = vm.pop();
        vm.push(arg);
        const int r = script.gameBuiltin(id, vm);
        if (id == ids_.startupScreen) {
            // (The game's took the host's keys and its journal's 0xE entries: every player's.)
            for (server::ServerObject* u : partyUnits()) {
                if (isHostUnit(u) || !u->player) {
                    continue;
                }
                // (Its client told each entry that goes, MSG_JOURNAL_MSG 2, as the host's is.)
                if (const Member* m = memberOfUnit(*u)) {
                    for (const auto& entry : u->player->journal) {
                        if (entry.type & 0xE) {
                            std::vector<std::uint8_t> msg(0x44, 0);
                            msg[0] = 0xD5;
                            msg[1] = 2;
                            std::copy_n(entry.name.begin(), std::min<std::size_t>(entry.name.size(), 0x40), msg.begin() + 2);
                            tell(*m, std::move(msg));
                        }
                    }
                }
                server::removeJournalEntries(world(), *u, 0xE);
                keysGone(*u);
            }
        }
        chapterToGuests(static_cast<std::uint8_t>(static_cast<std::int32_t>(arg) - 1), id == ids_.startupScreen);
        return r;
    }
    if (id == ids_.tellStory) {
        // The game's talk to its caller; a party member's mirrored to the others.
        const std::uint32_t text = vm.pop();
        const std::uint32_t sound = vm.pop();
        const std::string textId = vm.string(text);
        vm.push(sound);
        vm.push(text);
        const std::size_t mark = world().broadcasts().size();
        const int r = script.gameBuiltin(id, vm);
        if (const server::ServerObject* caller = vm.caller() ? world().byExtent(vm.caller()) : nullptr; caller && memberOfUnit(*caller)) {
            mirrorDialog(*caller, textId);
            if (isHostUnit(caller)) {
                showSceneDialog(*caller, mark); // (a scene's talk: the game's own window for the others too)
            }
        }
        return r;
    }
    if (id == ids_.isCaller) {
        // IsCaller(GetHost()) stays the host's (only the host sets story scenes off); a guest who
        // set it off is told so.
        const std::uint32_t arg = vm.pop();
        server::ServerObject* o = scriptObject(vm, static_cast<std::int32_t>(arg));
        vm.push(arg);
        const int r = script.gameBuiltin(id, vm);
        const std::uint32_t answer = vm.pop();
        vm.push(answer);
        if (answer == 0 && isHostUnit(o)) {
            server::ServerObject* caller = vm.caller() ? world().byExtent(vm.caller()) : nullptr;
            if (caller && caller != o && caller->player) {
                hostOnly(*caller, vm.trigger());
            }
        }
        return r;
    }
    if (id == ids_.getGold || id == ids_.pickup) {
        // In a guest's talk "the player" (GetHost) is the talker for its gold and what it's given:
        // a talk's price is checked against its own purse, its item goes into its own pack.
        server::ServerObject* talker = guestTalker(vm);
        const int n = server::builtinArity(id).args;
        std::vector<std::uint32_t> args(static_cast<std::size_t>(n));
        for (int i = n - 1; i >= 0; --i) {
            args[static_cast<std::size_t>(i)] = vm.pop();
        }
        const bool toHost = !args.empty() && isHostUnit(scriptObject(vm, static_cast<std::int32_t>(args[0])));
        if (talker && toHost) {
            args[0] = static_cast<std::uint32_t>(talker->scriptId);
        }
        // A spell book, ability book or field guide handed "the player" (Pickup(GetHost(), book):
        // used up as it's taken): its award every player's, as the scripts' other rewards are.
        std::optional<RewardBook> book;
        // Any other thing handed "the player" (gold, gear, potions: a deliberate choice) is every
        // player's too, but keys, War01A's letter and the Oblivion staves (isSharedReward).
        std::optional<RewardItem> thing;
        server::ServerObject* taker = toHost ? scriptObject(vm, static_cast<std::int32_t>(args[0])) : nullptr;
        if (id == ids_.pickup && taker && taker->player && args.size() == 2) {
            if (const server::ServerObject* item = scriptObject(vm, static_cast<std::int32_t>(args[1])); item && isRewardBook(*item)) {
                book = RewardBook{item->typeName, item->useFn, item->bookEntry, item->guideCreature, item->subclasses};
            } else if (item && isSharedReward(*item)) {
                thing = RewardItem{item->typeName, item->modifiers, item->charges, item->maxCharges, item->goldAmount};
            }
        }
        if (!talker && !book && !thing) {
            for (const std::uint32_t a : args) {
                vm.push(a);
            }
            return std::nullopt; // (the game's own)
        }
        for (const std::uint32_t a : args) {
            vm.push(a);
        }
        scriptedReward_ = book.has_value();
        const int r = script.gameBuiltin(id, vm);
        scriptedReward_ = false;
        if (book) {
            rewardToParty(*taker, *book);
        }
        if (thing) {
            // (Only what the taker took: Pickup's answer, 1.)
            const std::uint32_t took = vm.pop();
            vm.push(took);
            if (took != 0) {
                itemToParty(*taker, *thing);
            }
        }
        return r;
    }
    const bool fanned = id == ids_.frozen || id == ids_.journalEntry || id == ids_.journalDelete || id == ids_.journalEdit ||
                        id == ids_.hasItem || id == ids_.awardSpell || id == ids_.moveObject || id == ids_.giveXp ||
                        id == ids_.changeGold;
    if (!fanned) {
        return std::nullopt;
    }
    // Its arguments (the first pushed first); the object it's given is the first.
    const int n = server::builtinArity(id).args;
    std::vector<std::uint32_t> args(static_cast<std::size_t>(n));
    for (int i = n - 1; i >= 0; --i) {
        args[static_cast<std::size_t>(i)] = vm.pop();
    }
    // (A script's reward to the host is every player's: GiveXp in full to each, and ChangeGold's
    // gain; a charge, ChangeGold below 0, stays the host's alone, as its GetGold check was.)
    const bool charge = id == ids_.changeGold && args.size() >= 2 && static_cast<std::int32_t>(args[1]) <= 0;
    if (charge && isHostUnit(scriptObject(vm, static_cast<std::int32_t>(args[0])))) {
        if (server::ServerObject* talker = guestTalker(vm)) {
            args[0] = static_cast<std::uint32_t>(talker->scriptId); // (a guest's talk: its own purse)
        }
    }
    if (args.empty() || charge || !isHostUnit(scriptObject(vm, static_cast<std::int32_t>(args[0])))) {
        // (Not "the player": the game's own, its arguments back as they were.)
        for (const std::uint32_t a : args) {
            vm.push(a);
        }
        return std::nullopt;
    }
    if (id == ids_.moveObject) {
        return moveParty(script, id, vm, std::move(args));
    }
    return fanOut(script, id, vm, std::move(args), id == ids_.hasItem);
}

server::ServerObject* CoopDirector::guestTalker(script::Vm& vm)
{
    server::ServerObject* caller = vm.caller() ? world().byExtent(vm.caller()) : nullptr;
    const server::ServerObject* npc = vm.trigger() ? world().byExtent(vm.trigger()) : nullptr;
    if (!caller || !caller->player || isHostUnit(caller) || !memberOfUnit(*caller)) {
        return nullptr;
    }
    // (A talk's functions run with the talker as the caller and the NPC as the trigger.)
    if (!npc || !npc->monster || npc->monster->dialogStart == -1 || npc->monster->dialogEnd == -1) {
        return nullptr;
    }
    return caller;
}

int CoopDirector::fanOut(server::ScriptHost& script, std::uint32_t id, script::Vm& vm, std::vector<std::uint32_t> args, bool anyTrue)
{
    const bool returns = server::builtinArity(id).returns;
    const bool freezing = id == ids_.frozen && args.size() >= 2 && static_cast<std::int32_t>(args[1]) == 1;
    std::optional<std::uint32_t> first;
    bool any = false;
    int result = 0;
    if (id == ids_.journalEntry && args.size() == 3) {
        // The host's entry as the game makes it (its sound at the host, heard about); each guest its
        // entry, its sound to it alone (as the level-up's to its player), not once more about the
        // party standing together.
        const std::string name = vm.string(args[1]);
        const auto type = static_cast<std::uint16_t>(args[2]);
        for (const std::uint32_t a : args) {
            vm.push(a);
        }
        result = script.gameBuiltin(id, vm);
        for (server::ServerObject* u : partyUnits()) {
            if (isHostUnit(u)) {
                continue;
            }
            server::addJournalEntry(world(), *u, name, type);
            if (type & 0xBu) {
                server::playSound(world(), 0x387u, *u, 2, u->extent); // (mode 2: to the listener of that extent)
            }
        }
        return result;
    }
    for (server::ServerObject* u : partyUnits()) {
        // (A scene's freeze holds the living; the dead come back by their respawn.)
        if (freezing && !isHostUnit(u) && (u->flags & server::ObjFlag::Dead)) {
            continue;
        }
        args[0] = static_cast<std::uint32_t>(u->scriptId);
        for (const std::uint32_t a : args) {
            vm.push(a);
        }
        result = std::max(result, script.gameBuiltin(id, vm));
        if (returns) {
            const std::uint32_t v = vm.pop();
            if (!first) {
                first = v;
            }
            any = any || v != 0;
        }
    }
    if (returns) {
        vm.push(anyTrue ? (any ? 1u : 0u) : first.value_or(0u));
    }
    return result;
}

int CoopDirector::moveParty(server::ScriptHost& script, std::uint32_t id, script::Vm& vm, std::vector<std::uint32_t> args)
{
    // The host where the script puts it (the game's own move), then each other living player at a
    // clear point within 50 of it.
    for (const std::uint32_t a : args) {
        vm.push(a);
    }
    const int r = script.gameBuiltin(id, vm);
    server::ServerObject* h = scriptObject(vm, static_cast<std::int32_t>(args[0]));
    if (!h) {
        return r;
    }
    server::World& w = world();
    for (server::ServerObject* u : partyUnits()) {
        if (u == h || (u->flags & server::ObjFlag::Dead)) {
            continue;
        }
        const auto [x, y] = server::clearPointNear(w, rng_, h->x, h->y, 50.0f);
        server::setPosition(w, *u, x, y, script.frame());
    }
    // (A script's move of the host onto an exit, as Wiz03A's after Horvath's talk, is the map's way
    // on: in the solo game it goes as the host lands. exitTouched lets it go for the party at once.)
    scriptMovedAt_ = w.ticks;
    return r;
}

} // namespace nox::coop
