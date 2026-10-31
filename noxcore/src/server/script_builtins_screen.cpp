// The cinematic builtins: what they do on the server, and their calls into the host's
// own client (host_client.hpp: GAME.EXE makes them directly, no message). Builtin ids checked
// against the exe's builtin table (115, 135-137, 176, 187, 196, 202, 203, 207, 208).
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/object_class.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

/// The chapter screen (StartupScreen / DeathScreen): the chapter (the script's argument less one), the
/// game paused, the host's screen to black over 25 frames over the GUI; then the map
/// no longer drawn and the host told the chapter's over (SrvNet_SendChapterEnd:
/// MSG_CHAPTER_END 0xD6 {chapter, kind == 1}, reliable).
void chapterScreen(World& world, int chapter, bool startup)
{
    setPaused(world, true, PauseReason::Chapter); // (Game_SetPaused(1): a solo game's)
    World* w = &world;
    const auto done = [w, chapter, startup] {
        if (w->hostClient) {
            w->hostClient->setMapDrawn(false); // (whoever's there)
        }
        ServerObject* host = hostPlayer(*w);
        if (!host) {
            // (No player 0x1F: the exe's search leaves the last player's record, or garbage with
            // none, and sends it there: an original bug. Nothing here.)
            return;
        }
        w->broadcasts().push_back({{0xD6, static_cast<std::uint8_t>(chapter), static_cast<std::uint8_t>(startup ? 1 : 0)}, true, host->extent});
    };
    if (world.hostClient) {
        world.hostClient->fadeToBlack(25, true, done);
    } else {
        done();
    }
}

} // namespace

int ScriptHost::screenBuiltin(std::uint32_t id, script::Vm& vm)
{
    HostClient* host = world_.hostClient;
    switch (id) {
    case 135: // UnBlind
        if (host) {
            host->unblind();
        }
        return 0;
    case 136: // Blind
        if (host) {
            host->blind();
        }
        return 0;
    case 137: { // WideScreen (WideScreenDo)
        const std::int32_t on = vm.popInt();
        if (on == 1) {
            const bool moving = host && host->wideScreenMoving();
            if (host) {
                host->wideScreen(true); // (the bars' slide in (0x10, 10))
            }
            if (moving) {
                return 0;
            }
            // The missiles (the updatable list) but pixies gone; on the object list, an owned
            // cloud, meteor or fist gone, the rest: a telekinesis hand NO_COLLIDE, these lasting
            // spells each cancelled.
            static constexpr std::array<std::string_view, 6> kGone = {"ToxicCloud", "SmallToxicCloud", "Meteor",
                                                                      "SmallFist",  "MediumFist",      "LargeFist"};
            static constexpr std::array<std::uint32_t, 14> kCancelled = {0x84, 0x38, 0x18, 0x2B, 0x16, 0x1F, 0x23,
                                                                         8,    9,    4,    0x73, 0x15, 0x3B, 0x4B};
            for (auto& [e, o] : world_.objects()) {
                if (!o.gone() && o.holder == 0 && (o.classes & oc::Missile) && o.typeName != "Pixie") {
                    o.pendingDelete = true; // (delayedDeleteObject)
                }
            }
            for (auto& [e, o] : world_.objects()) {
                if (o.gone() || o.holder != 0 || (o.classes & oc::Missile)) {
                    continue;
                }
                if (o.owner != 0 && std::ranges::find(kGone, o.typeName) != kGone.end()) {
                    o.pendingDelete = true;
                    continue;
                }
                if (o.typeName == "TelekinesisHand" && !(o.flags & ObjFlag::NoCollide)) {
                    o.flags |= ObjFlag::NoCollide;
                }
                for (const std::uint32_t spell : kCancelled) {
                    cancelLastingSpells(world_, spell, o);
                }
            }
        } else {
            if (host) {
                host->wideScreen(false); // (the bars' slide out (0x10, 0x1E))
            }
            for (auto& [e, o] : world_.objects()) {
                if (o.typeName == "TelekinesisHand" && (o.flags & ObjFlag::NoCollide)) {
                    o.flags &= ~ObjFlag::NoCollide;
                }
            }
        }
        return 0;
    }
    case 176: { // StartupScreen
        const std::int32_t chapter = vm.popInt() - 1;
        if (ServerObject* p = hostPlayer(world_)) {
            removeJournalEntries(world_, *p, 0xE);
            // Its keys gone (delayedDeleteObject: out of the inventory at once,
            // Obj_RemoveFromInventory telling its client, MSG_REPORT_DROP).
            Items items{world_, things_, modifiers_, creatureRng_, frame_};
            for (const std::uint32_t e : keysOf(p->inventory)) {
                if (ServerObject* item = world_.byExtent(e); item && !item->pendingDelete && (item->classes & oc::Key)) {
                    items.removeFromInventory(*p, *item);
                    item->pendingDelete = true;
                }
            }
        }
        chapterScreen(world_, chapter, true);
        return 0;
    }
    case 187: { // DeathScreen
        const std::int32_t chapter = vm.popInt() - 1;
        chapterScreen(world_, chapter, false);
        return 0;
    }
    case 202: // MusicPushEvent: a solo game's host's, else MSG 0xE6 to everyone.
    case 203: // MusicPopEvent: MSG 0xE7.
        if (world_.gameFlags & GameFlag::Solo) {
            if (host) {
                id == 202 ? host->musicPush() : host->musicPop();
            }
        } else {
            // (3 bytes queued; the 2 after the id are the stack's: zeros here)
            world_.broadcasts().push_back({{static_cast<std::uint8_t>(id == 202 ? 0xE6 : 0xE7), 0, 0}, true, 0});
        }
        return 0;
    case 204: // The music off: a solo game's host's (Snd_SetMusic(0, 0)), else
              // MSG_MUSIC_EVENT {0, (stack)} to everyone (its third byte the stack's: zero here).
        if (world_.gameFlags & GameFlag::Solo) {
            if (host) {
                host->music(0, 0);
            }
        } else {
            world_.broadcasts().push_back({{0xE5, 0, 0}, true, 0});
        }
        return 0;
    case 207: { // EndGame:
        const std::int32_t kind = vm.popInt();
        if (host) {
            host->endGame(kind);
        }
        return 0;
    }
    case 208: // ImmediateBlind
        if (host) {
            host->playMapIntro(true);
        }
        return 0;
    case 196: // ImmediateUnBlind (the community's Unknownc4): video_stopAllFades (ImmediateBlind's
              // black too), then Game_SetPaused(0)
        if (host) {
            host->stopAllFades();
        }
        setPaused(world_, false, PauseReason::Cinematic);
        return 0;
    case 114: // ChatTimerSeconds: as ChatTimer, `duration` in seconds (x gameFPS, 30)
    case 115: { // ChatTimer: Chat for `duration`; a solo game's host hears its voice
        const auto duration = static_cast<std::uint16_t>(vm.popInt() * (id == 114 ? 30 : 1));
        const std::uint32_t text = vm.pop();
        if (ServerObject* o = object(vm.popInt())) {
            // (Str_GetString's one variant: its text said, its speech file the voice.)
            const World::Line line = world_.line(vm.string(text));
            world_.unitSay(*o, line.text, duration);
            if (!line.speech.empty() && (world_.gameFlags & GameFlag::Solo) && host) {
                host->playVoice(line.speech, 100);
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

} // namespace nox::server
