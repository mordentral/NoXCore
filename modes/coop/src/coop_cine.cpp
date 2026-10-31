// The co-op campaign's cinematics, chapter screens and dialog mirror (coop.hpp): the host's own
// client's calls told to the guests (CineRelay), a chapter screen's pause shared till every NoX
// Core guest is done reading, the world-wide pauses a guest's game doesn't take, and the host's
// talk shown to the others.
#include "noxcore/coop/coop.hpp"

#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace nox::coop {

namespace {

/// A creature type's name as players read it: "AirshipCaptain" -> "Airship Captain".
std::string spaced(std::string_view type)
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

/// MSG_FADE_BEGIN 0xE4 {1 from black / 0 to black, over the menus}: a stock client's fade.
std::vector<std::uint8_t> fadeMessage(bool fromBlack, bool overMenu)
{
    return {0xE4, static_cast<std::uint8_t>(fromBlack ? 1 : 0), static_cast<std::uint8_t>(overMenu ? 1 : 0)};
}

} // namespace

// ---- CineRelay: each call the host's own client's, then the guests'

void CineRelay::blind()
{
    if (own) {
        own->blind();
    }
    director_.cine({coopmsg::CineCall::Blind});
    director_.toGuests(fadeMessage(false, false), true);
}

void CineRelay::unblind()
{
    if (own) {
        own->unblind();
    }
    director_.cine({coopmsg::CineCall::UnBlind});
    director_.toGuests(fadeMessage(true, false), true);
}

void CineRelay::fadeToBlack(int frames, bool overMenu, std::function<void()> done)
{
    // (The server's `done` is the host's client's: a guest's fade ends on its own.)
    if (own) {
        own->fadeToBlack(frames, overMenu, std::move(done));
    } else if (done) {
        done();
    }
    director_.cine({coopmsg::CineCall::FadeToBlack, static_cast<std::uint8_t>(overMenu ? 1 : 0), static_cast<std::uint16_t>(frames)});
    director_.toGuests(fadeMessage(false, overMenu), true);
}

void CineRelay::setMapDrawn(bool drawn)
{
    if (own) {
        own->setMapDrawn(drawn);
    }
    director_.cine({coopmsg::CineCall::MapDrawn, static_cast<std::uint8_t>(drawn ? 1 : 0)});
}

void CineRelay::wideScreen(bool on)
{
    if (own) {
        own->wideScreen(on);
    }
    director_.cine({coopmsg::CineCall::WideScreen, static_cast<std::uint8_t>(on ? 1 : 0)});
}

void CineRelay::playMapIntro(bool immediate)
{
    if (own) {
        own->playMapIntro(immediate);
    }
    director_.cine({coopmsg::CineCall::MapIntro, static_cast<std::uint8_t>(immediate ? 1 : 0)});
}

void CineRelay::endGame(int kind)
{
    if (own) {
        own->endGame(kind);
    }
    director_.cine({coopmsg::CineCall::EndGame, static_cast<std::uint8_t>(kind)});
    // A stock guest: the credits (MSG_CHAPTER_END {0xFF, 1}: the endgame's text, then back to the
    // game; no movie).
    director_.toGuests({0xD6, 0xFF, 1}, true);
}

void CineRelay::musicPush()
{
    if (own) {
        own->musicPush();
    }
    director_.toGuests({0xE6, 0, 0}, false); // (MSG 0xE6: the network game's own)
}

void CineRelay::musicPop()
{
    if (own) {
        own->musicPop();
    }
    director_.toGuests({0xE7, 0, 0}, false);
}

void CineRelay::music(int track, int volume)
{
    if (own) {
        own->music(track, volume);
    }
    director_.toGuests({0xE5, static_cast<std::uint8_t>(track), static_cast<std::uint8_t>(volume)}, false); // (MSG_MUSIC_EVENT)
}

void CineRelay::playVoice(std::string_view file, int volume)
{
    if (own) {
        own->playVoice(file, volume);
    }
    director_.cine({coopmsg::CineCall::Voice, static_cast<std::uint8_t>(volume), 0, 0, std::string(file)});
}

void CineRelay::stopAllFades()
{
    if (own) {
        own->stopAllFades();
    }
    director_.cine({coopmsg::CineCall::StopAllFades});
}

void CineRelay::gameLoaded()
{
    if (own) {
        own->gameLoaded();
    }
    director_.gameLoaded();
}

// ---- the director's part

server::HostClient* CoopDirector::hostClient(server::HostClient* own)
{
    relay_.own = own;
    return &relay_;
}

void CoopDirector::tell(const Member& m, std::vector<std::uint8_t> bytes)
{
    if (!host_) {
        return;
    }
    if (const server::ServerObject* u = host_->unitOf(m.peer)) {
        world().broadcasts().push_back({std::move(bytes), true, u->extent});
    }
}

void CoopDirector::cine(const coopmsg::Cine& c)
{
    if (c.call == coopmsg::CineCall::WideScreen) {
        const bool was = std::exchange(bars_, c.a != 0);
        if (!bars_) {
            sceneGathered_ = false;
        } else if (!was || !sceneGathered_) {
            gatherForScene();
        }
    }
    if (!host_) {
        return;
    }
    coopmsg::Cine told = c;
    if (const server::ServerObject* h = server::hostPlayer(world())) {
        told.actor = server::netCodeOf(*h);
    }
    const auto bytes = coopmsg::cineMessage(told);
    for (const Member& m : party_) {
        if (!m.host && m.identified) {
            tell(m, bytes);
        }
    }
}

void CoopDirector::gatherForScene()
{
    if (!host_) {
        return;
    }
    sceneGathered_ = true;
    server::ServerObject* h = server::hostPlayer(world());
    if (!h || (h->flags & server::ObjFlag::Dead)) {
        return; // (a dead host's scene: nobody brought to its body)
    }
    server::World& w = world();
    const std::uint32_t frame = w.script ? w.script->frame() : w.frameNow;
    for (server::ServerObject* u : partyUnits()) {
        const float dx = u->x - h->x;
        const float dy = u->y - h->y;
        if (u == h || (u->flags & server::ObjFlag::Dead) || dx * dx + dy * dy <= kSceneGatherNear * kSceneGatherNear) {
            continue;
        }
        const auto [x, y] = server::clearPointNear(w, rng_, h->x, h->y, 50.0f);
        server::setPosition(w, *u, x, y, frame);
    }
}

void CoopDirector::toGuests(const std::vector<std::uint8_t>& bytes, bool stockOnly)
{
    for (const Member& m : party_) {
        if (!m.host && !(stockOnly && m.identified)) {
            tell(m, bytes);
        }
    }
}

bool CoopDirector::mayPause(server::PauseReason why)
{
    const auto bit = static_cast<std::uint32_t>(1u << static_cast<unsigned>(why));
    if (party_.size() < 2) {
        refused_ &= ~bit;
        return true; // (alone: as the game)
    }
    switch (why) {
    case server::PauseReason::LevelUp:
    case server::PauseReason::BookFlight:
    case server::PauseReason::Menu:
        refused_ |= bit; // (its unpause isn't ours to give either: it would end another's pause)
        return false;
    default:
        refused_ &= ~bit;
        return true;
    }
}

bool CoopDirector::mayUnpause(server::PauseReason why)
{
    if (const auto bit = static_cast<std::uint32_t>(1u << static_cast<unsigned>(why)); !releasing_ && (refused_ & bit)) {
        refused_ &= ~bit;
        return false;
    }
    if (releasing_ || reading_.empty()) {
        return true;
    }
    if (why == server::PauseReason::Chapter) {
        hostDoneReading_ = true; // (let go once the guests are done)
    }
    return false;
}

void CoopDirector::chapterToGuests(std::uint8_t chapter, bool startup)
{
    reading_.clear();
    readingOf_ = 0;
    hostDoneReading_ = false;
    chapterAt_ = static_cast<std::uint32_t>(world().ticks);
    for (const Member& m : party_) {
        if (m.host) {
            continue;
        }
        if (m.identified) {
            tell(m, coopmsg::chapterMessage({chapter, startup}));
            reading_.insert(m.peer);
            ++readingOf_;
        } else if (startup) {
            // A stock guest: MSG_CHAPTER_END itself (its client picks the picture, text and voice by
            // its own class: the campaign's, every player's). It isn't waited for.
            tell(m, {0xD6, chapter, 1});
        } else {
            // A stock guest's loss: never MSG_CHAPTER_END's kind 0 (its own load menu: under the
            // quest's flags it does nothing, the screen never goes). MSG_GAUNTLET 0x0D's lock screen
            // with the chapter's loss picture (Gui_Brief_ShowQuestNextLevel: {u16 stage, u8 flags
            // (1: shown), 32 bytes the image's name, 32 the text's label}; the loss labels have no
            // text), back to the game at its end; the host loads.
            std::vector<std::uint8_t> msg(69, 0);
            msg[0] = 0xF0;
            msg[1] = 0x0D;
            msg[2] = static_cast<std::uint8_t>(chapter + 1);
            msg[4] = 1;
            const std::string image = std::string(className(options_.campaignClass)) + "ChapterLoss" + std::to_string(chapter + 1);
            std::copy_n(image.begin(), std::min<std::size_t>(image.size(), 31), msg.begin() + 5);
            tell(m, std::move(msg));
        }
    }
    tellReadyState();
}

void CoopDirector::tellReadyState()
{
    if (!host_) {
        return;
    }
    const auto bytes = coopmsg::readyStateMessage(static_cast<std::uint8_t>(reading_.size()), readingOf_);
    for (const Member& m : party_) {
        if (m.identified) {
            tell(m, bytes);
        }
    }
}

void CoopDirector::releaseChapter()
{
    reading_.clear();
    tellReadyState();
    if (std::exchange(hostDoneReading_, false) && host_) {
        // (The host's screen ended meanwhile: its unpause, held, given now.)
        releasing_ = true;
        server::setPaused(world(), false, server::PauseReason::Chapter);
        releasing_ = false;
    }
}

void CoopDirector::mirrorDialog(const server::ServerObject& talker, const std::string& textId)
{
    if (!talker.player || talker.player->dialogWith == 0) {
        return;
    }
    const server::ServerObject* npc = world().byExtent(talker.player->dialogWith);
    coopmsg::DialogMirror d;
    d.open = true;
    d.talker = server::netCodeOf(talker);
    d.npc = npc ? server::netCodeOf(*npc) : std::uint16_t{0};
    d.text = textId;
    const bool opening = !mirrorOpen_ || mirrorTalker_ != talker.extent || mirrorNpc_ != talker.player->dialogWith;
    mirrorOpen_ = true;
    mirrorTalker_ = talker.extent;
    mirrorNpc_ = talker.player->dialogWith;
    const auto bytes = coopmsg::dialogMessage(d);
    for (const Member& m : party_) {
        if (m.extent == talker.extent) {
            continue; // (the talker has the game's window)
        }
        if (m.identified) {
            tell(m, bytes);
        } else if (opening && npc) {
            // A stock player: who talks to whom, once as it opens.
            std::string name;
            for (const char16_t c : talker.player->name) {
                name += c < 0x80 ? static_cast<char>(c) : '?';
            }
            notice(m, coopmsg::Notice::Info, name + " is talking to " + spaced(npc->typeName) + ".");
        }
    }
}

void CoopDirector::watchDialog()
{
    if (!mirrorOpen_) {
        return;
    }
    const server::ServerObject* talker = world().byExtent(mirrorTalker_);
    if (talker && !talker->gone() && talker->player && talker->player->dialogWith != 0) {
        return;
    }
    mirrorOpen_ = false;
    mirrorNpc_ = 0;
    coopmsg::DialogMirror d; // (closed)
    if (talker) {
        d.talker = server::netCodeOf(*talker);
    }
    const auto bytes = coopmsg::dialogMessage(d);
    for (const Member& m : party_) {
        if (m.extent != mirrorTalker_ && m.identified) {
            tell(m, bytes);
        }
    }
    mirrorTalker_ = 0;
}

void CoopDirector::showSceneDialog(const server::ServerObject& host, std::size_t mark)
{
    if (!host_ || party_.size() < 2 || !host.player || host.player->dialogWith == 0) {
        return;
    }
    const server::ServerObject* npc = world().byExtent(host.player->dialogWith);
    if (!npc || !npc->monster) {
        return;
    }
    // A scene's talk: under the bars, or one that can play a scene (talkPlaysScene: the host's
    // alone). Another talk of the host's stays its own, told to the others as the mirror's line.
    bool scene = bars_;
    if (!scene) {
        if (server::ScriptHost* s = world().script) {
            const script::ScriptFile& file = s->vm().file();
            scene = talkPlaysScene(file, npc->monster->dialogStart) || talkPlaysScene(file, npc->monster->dialogEnd);
        }
    }
    if (!scene) {
        return;
    }
    // The window the game opened for the host (MSG_DIALOG 3: its text, name, voice, portrait and
    // buttons), the same bytes to each other player: a stock client opens it as it comes
    // (Gui_NpcDialog_Open) as a NoX Core one does.
    auto& b = world().broadcasts();
    const std::vector<std::uint8_t>* opened = nullptr;
    for (std::size_t i = std::min(mark, b.size()); i < b.size(); ++i) {
        if (b[i].to == host.extent && b[i].bytes.size() >= 2 && b[i].bytes[0] == 0xD0 && b[i].bytes[1] == 3) {
            opened = &b[i].bytes;
        }
    }
    if (!opened) {
        return;
    }
    const std::vector<std::uint8_t> bytes = *opened;
    sceneTalker_ = host.extent;
    for (const Member& m : party_) {
        if (m.host) {
            continue;
        }
        if (const server::ServerObject* u = host_->unitOf(m.peer)) {
            sceneViewers_.emplace(u->extent);
            tell(m, bytes);
        }
    }
}

void CoopDirector::watchSceneDialog()
{
    if (sceneTalker_ == 0) {
        return;
    }
    const server::ServerObject* talker = world().byExtent(sceneTalker_);
    if (talker && !talker->gone() && talker->player && talker->player->dialogWith != 0) {
        return;
    }
    // The host's talk over (its answer, or the talk cancelled): the others' windows closed
    // (MSG_DIALOG 4, as the host's answer closes its own).
    for (const Member& m : party_) {
        if (const server::ServerObject* u = host_ ? host_->unitOf(m.peer) : nullptr; u && sceneViewers_.contains(u->extent)) {
            tell(m, {0xD0, 4});
        }
    }
    sceneViewers_.clear();
    sceneTalker_ = 0;
}

void CoopDirector::tellPartyHealth()
{
    if (!host_ || party_.size() < 2) {
        healthTold_.clear();
        return;
    }
    // As a quest's server tells its players each other's health (SrvNet_SendTeamHealthPercent:
    // MSG_TEAM_MSG 0x0C, its code and health x 100 / its most), whenever a share changes: a stock
    // guest's client (told a quest's flags) draws the bar over each other player from it, as a NoX
    // Core one does in a co-op game. (The share worked out again only as the health or its most
    // changed; what's told kept in place, those not seen this pass let go.)
    ++healthPass_;
    for (const Member& to : party_) {
        if (!host_->unitOf(to.peer)) {
            continue;
        }
        for (const Member& of : party_) {
            const server::ServerObject* u = of.peer == to.peer ? nullptr : host_->unitOf(of.peer);
            if (!u || !u->player) {
                continue;
            }
            const auto [it, fresh] = healthTold_.try_emplace(std::pair{to.peer, u->extent});
            HealthTold& told = it->second;
            if (fresh || told.health != u->health || told.maxHealth != u->maxHealth) {
                auto msg = server::questTeamHealthMessage(*u);
                const int share = msg[4] + 1;
                if (fresh || told.share != share) {
                    tell(to, std::move(msg));
                }
                told.share = share;
                told.health = u->health;
                told.maxHealth = u->maxHealth;
            }
            told.pass = healthPass_;
        }
    }
    std::erase_if(healthTold_, [&](const auto& e) { return e.second.pass != healthPass_; }); // (those gone: told afresh if they come back)
}

void CoopDirector::endUnpausedLevelUp()
{
    server::World& w = world();
    if (!w.levelUp.active || !w.levelUp.timed || (w.gameFlags & server::GameFlag::Paused) || party_.size() < 2) {
        return;
    }
    // (Its 5 s were the world's pause; unpaused, the player's level-up state would hold it while
    // the others play on: a second of it.)
    if (w.ticks - w.levelUp.startTick >= 30u) {
        server::Fight f = host_->server().fight();
        server::levelUpEnd(f);
    }
}

void CoopDirector::joinScene(const Member& m)
{
    if (!host_ || m.host) {
        return;
    }
    if (bars_ && m.identified) {
        coopmsg::Cine c{coopmsg::CineCall::WideScreen, 1};
        if (const server::ServerObject* h = server::hostPlayer(world())) {
            c.actor = server::netCodeOf(*h);
        }
        tell(m, coopmsg::cineMessage(c));
    }
    // Held as the party is (a script's Frozen on the host holds the others too).
    const server::ServerObject* h = server::hostPlayer(world());
    server::ServerObject* u = host_->unitOf(m.peer);
    if (h && u && world().freezeLock == 1 && (h->flags & server::ObjFlag::NoUpdate) && !(u->flags & server::ObjFlag::NoUpdate)) {
        server::Fight f = host_->server().fight();
        server::freezeUnit(f, *u, 1);
    }
}

} // namespace nox::coop
