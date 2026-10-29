// A map sent to a client that hasn't got it (GAME.EXE's Server\Network\mapsend.c). The client
// asks (MSG_REQUEST_MAP 0xB6: it goes observing and leaves its team first, SrvNet_OnPacket's);
// its sender (g_mapSenders, one a player) then sends, a block each even server frame (the tick's
// gameFrame & 1 skips), MSG_MAP_SEND_START (0xB8: the size, the map's file name) and MSG_MAP_SEND_PACKET
// (0xB9: the block's number from 1, its size, up to 512 bytes) of the map's own
// maps\<map>\<map>.nxz, as it is on disk, each reliable and out at once (Net_Send flag 3); a
// player with more than 2 map messages unacknowledged waits. The client says when
// it has the map (MSG_RECEIVED_MAP 0xBF) or gives up (MSG_CANCEL_MAP 0xB7). A map load asked for
// aborts every send (MSG_MAP_SEND_ABORT 0xBA, reason 0).
//
// noxcpp's fast send (a NoX Core client of lobby::kFastMapSendLevel or above, its level taken when
// it asks, for its first kFastMapRequestsPerMap requests after each map load): the same messages
// and the same file, but blocks of kFastMapBlock bytes, sent every frame, up to
// kFastMapBlocksPerFrame of them (kFastMapBlocksPerFrameAll to all fast senders together, round
// robin), while the client has fewer reliable packets out unacknowledged than the window:
// kFastMapStartWindow at first, kFastMapWindow at most, halved (to kFastMapMinWindow at least)
// when a frame's blocks go unacknowledged past kFastMapLateMs and twice the quickest a frame's
// first packet was (kFastMapFirstLateMs before any was), opened again as frames' blocks are
// acknowledged in time (by as many packets till the first late frame, by one a round trip after),
// and within GameHost::reliableBulkRoom. The transport's resends stand as they are (an
// unacknowledged packet again after 2 s, the one an acknowledgement names at once). Every other
// client (stock, or a NoX Core one below that level) and every later request gets the stock stream
// above, byte for byte.
#include "noxcore/server/game_host.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/strings/string_table.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace nox::server {

void GameHost::mapSendPrint(std::string_view label, std::initializer_list<std::string_view> values)
{
    // gui_console_Print(6, ..): Str_GetString's line, %s / %S / %d filled in turn.
    console_.push_back(stringLine(std::string(label), values));
}

bool GameHost::mapSendLoadMap()
{
    // SrvNet_MapSendLoadMap (at a map's load but in a solo game; the send frame's
    // when nothing was loaded): with sends counted under way, a sending one keeps its own copy of
    // what was loaded ("ForceCopy"); then, the map's file name not empty, maps\<map>\<map>.nxz read
    // whole. (Its "not the one loaded" check compares the name's first letter with a byte
    // which nothing writes: never the same, an original bug.) Not there: "CompressFail", what was
    // loaded stays.
    if (mapSendsInProgress_ != 0) {
        for (auto& [peer, s] : mapSenders_) {
            if (s.state == 1) {
                s.copy = mapSendData_;
                s.privateCopy = true;
                s.size = static_cast<std::uint32_t>(mapSendData_.size());
                mapSendPrint("mapsend.c:ForceCopy");
            }
        }
    }
    const std::string current = mapFile_;
    if (!current.empty()) {
        const std::string stem = mapPath_.stem().string();
        std::ifstream in(resolveNoCase(mapsDir() / stem / (stem + ".nxz")), std::ios::binary); // (any case, as on Windows)
        if (in) {
            mapSendData_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            mapSendName_ = current;
            return true;
        }
    }
    mapSendPrint("mapsend.c:CompressFail");
    return false;
}

void GameHost::onRequestMap(std::uint8_t peer)
{
    // SrvNet_OnPacket's 0xB6 first: the player goes observing (Plr_GoObserver(.., 1, 1): free to
    // join, forced) and leaves its team (Team_RemoveMember).
    if (const auto it = players_.find(peer); it != players_.end()) {
        if (ServerObject* unit = server_.world().byExtent(it->second.extent); unit && unit->player) {
            Fight f = server_.fight();
            goObserver(f, *unit, true, true);
            removeTeamMember(server_.world(), *unit);
        }
    }
    // SrvNet_OnRequestMap: counted as under way even when it's turned down (an original bug);
    // an idle sender starts: block 1, 512 bytes a block, the size loaded, the time.
    ++mapSendsInProgress_;
    MapSender& s = mapSenders_[peer];
    if (s.state == 0) {
        ++mapSendsStarted_;
        s.state = 1;
        s.sent = 0;
        s.seq = 1;
        s.blockSize = 0x200;
        s.size = static_cast<std::uint32_t>(mapSendData_.size());
        s.startMs = nowMs_;
        // (noxcpp's fast send, for a NoX Core client that takes it: its first few requests after
        // the map's load.)
        if (transport_.clientLevel(peer) >= lobby::kFastMapSendLevel && fastMapRequests_[peer] < kFastMapRequestsPerMap) {
            ++fastMapRequests_[peer];
            s.fast = true;
            s.blockSize = kFastMapBlock;
            ++mapSendsFast_;
        }
        return;
    }
    mapSendPrint(s.state == 1 ? "mapsend.c:Sending" : "mapsend.c:BadState");
}

void GameHost::mapSendReset(MapSender& s)
{
    if (s.fast && s.state == 1 && mapSendsFast_ != 0) {
        --mapSendsFast_;
    }
    s = MapSender{};
}

void GameHost::mapSendPeerGone(std::uint8_t peer)
{
    // (noxcpp's) A fast send to a player who left ends with it: nothing more to its closed
    // stream, the count down, the sender free for whoever takes the slot next. A stock send goes
    // on as the exe's does (OB-163).
    fastMapRequests_.erase(peer);
    const auto it = mapSenders_.find(peer);
    if (it == mapSenders_.end() || !it->second.fast || it->second.state != 1) {
        return;
    }
    if (mapSendsInProgress_ != 0) {
        --mapSendsInProgress_;
    }
    mapSendReset(it->second);
}

void GameHost::onCancelMap(std::uint8_t peer)
{
    // SrvNet_OnCancelMap: its sender reset (its own copy freed).
    mapSendReset(mapSenders_[peer]);
    if (mapSendsInProgress_ != 0) {
        --mapSendsInProgress_;
    }
    if (const auto it = players_.find(peer); it != players_.end()) {
        mapSendPrint("MapSend.c:downloadcancelled", {it->second.name});
    }
}

void GameHost::onReceivedMap(std::uint8_t peer)
{
    // SrvNet_OnReceivedMap -> forgetObjects(the sender's index: its player's). (SrvNet_OnPacket sets
    // the player's netState to 3 first: already 3 since its MSG_INCOMING_CLIENT; ours
    // keeps none.)
    forgetObjects(static_cast<std::uint8_t>((peer - 1) & 31));
}

void GameHost::forgetObjects(std::uint8_t slot)
{
    // For every object, the slot's bit in its syncMask (owed an update) set,
    // in playersKnowMask cleared but for the always-sent kinds (0x20400000), and its owed bits for the slot
    // cut to its low 12 bits (what it was owed of the object: the reports' bits); those kinds owe
    // it again what differs from their type (1 the animation frame, 4 off where the
    // type is on, 8 an xStatus (the types' are 0 here), 0x40 a height, 0x80 enchantments, 0x400 an
    // NPC's report, 0x200 a weapon's, armour's or flag's modifiers). (The owed bit 2, a health
    // off the type's, has no counterpart here.)
    const std::uint32_t bit = 1u << (slot & 31u);
    const auto playerIt = std::ranges::find_if(players_, [&](const auto& p) { return (p.second.slot & 31u) == (slot & 31u); });
    for (auto& [extent, o] : server_.world().objects()) {
        o.syncMask |= bit;
        o.animOwed &= ~bit;
        o.enableOwed &= ~bit;
        o.xStatusOwed &= ~bit;
        o.heightOwed &= ~bit;
        o.enchantOwed &= ~bit;
        o.npcOwed &= ~bit;
        o.modifierOwed &= ~bit;
        if (!(o.classes & 0x20400000u)) {
            o.knownBy &= ~bit;
            if (playerIt != players_.end()) {
                playerIt->second.knows.erase(extent);
            }
            continue;
        }
        if (o.animFrame != 0) {
            o.animOwed |= bit;
        }
        if (!o.enabled()) {
            o.enableOwed |= bit;
        }
        if (o.xStatus != 0) {
            o.xStatusOwed |= bit;
        }
        if (o.z != 0) {
            o.heightOwed |= bit;
        }
        if (o.buffs != 0) {
            o.enchantOwed |= bit;
        }
        if ((o.classes & 2u) && (o.subclasses & 0x30u)) { // (a monster, an NPC)
            o.npcOwed |= bit;
        }
        if (o.classes & 0x13001000u) { // (0x200: a weapon, armour, a flag)
            o.modifierOwed |= bit;
        }
    }
}

void GameHost::mapSendFrame(bool even)
{
    // Each even frame (the caller's): with sends under way (nothing loaded yet:
    // loaded now), each sending sender sends its next block unless more than `window` map messages
    // are unacknowledged on its "stream": the unacknowledged count (index + 1) reads the stream from the next
    // sender's first byte (its index, so the player's own stream, index + 1), and for sender 30
    // from the byte past the last, g_mapSendsStarted's low byte (an original bug).
    // (noxcpp's fast senders send every frame, odd ones too, after the stock ones: mapSendFast.)
    if (mapSendsInProgress_ == 0 || (!even && mapSendsFast_ == 0)) {
        return;
    }
    if (even && mapSendData_.empty()) {
        mapSendLoadMap();
    }
    for (auto& [peer, s] : mapSenders_) {
        if (!even || s.state != 1 || s.fast) {
            continue;
        }
        // (A sender's index is its peer - 1; index 30 reads the stream number off g_mapSendsStarted.)
        const auto stream = ((peer - 1) & 31) == 30 ? static_cast<std::uint8_t>(mapSendsStarted_) : peer;
        const std::size_t waiting = stream < 0x80 ? transport_.queuedReliableOf(stream, static_cast<std::uint8_t>(net::Op::MapSendStart),
                                                                                static_cast<std::uint8_t>(net::Op::MapSendPacket))
                                                  : 0;
        if (s.window < waiting) {
            ++s.waits;
        } else {
            mapSendNextBlock(peer, s);
        }
    }
    if (mapSendsFast_ == 0) {
        return;
    }
    // The fast senders, kFastMapBlocksPerFrameAll blocks among them, each frame's turn starting
    // after the one the last frame served last.
    int budget = kFastMapBlocksPerFrameAll;
    auto it = mapSenders_.upper_bound(mapSendFastLast_);
    for (std::size_t n = mapSenders_.size(); n != 0 && budget > 0; --n, ++it) {
        if (it == mapSenders_.end()) {
            it = mapSenders_.begin();
        }
        if (it->second.state == 1 && it->second.fast) {
            mapSendFastLast_ = it->first;
            mapSendFast(it->first, it->second, budget);
        }
    }
}

void GameHost::mapSendFast(std::uint8_t peer, MapSender& s, int& budget)
{
    // (noxcpp's) Blocks while the client's reliable packets out are fewer than the window, at most
    // kFastMapBlocksPerFrame this frame and the frame's `budget`; the last one resets the sender.
    const auto seqs = transport_.reliableSeqs(peer);
    if (!seqs) {
        mapSendPeerGone(peer); // (its stream gone before its leave, or with no player to leave)
        return;
    }
    // A frame's blocks are late when not acknowledged within kFastMapLateMs and twice the round
    // trip (timed by this send alone: a fresh connection's latency is unknown, a reused slot's its
    // last client's), and before any is timed, within kFastMapFirstLateMs: lost or out of order.
    // Those acknowledged in time open the window, by as many packets till the first late one, by
    // one a round trip after; the oldest late halves it (what's in flight is the transport's to
    // resend). (The client acknowledges all before the next it expects; sequence numbers wrap,
    // compared within the window's span.)
    const auto acked = [&](std::uint8_t seqEnd) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(seqs->ack - seqEnd)) >= 0; };
    // (The round trip: the quickest a frame's first packet was acknowledged. A whole frame's can
    // wait on an earlier loss's mending, its first seldom: the first frame's has nothing before it.)
    for (MapSender::Flight& f : s.flights) {
        if (!f.timed && acked(static_cast<std::uint8_t>(f.seqBegin + 1))) {
            f.timed = true;
            const std::uint32_t age = std::max<std::uint32_t>(nowMs_ - f.at, 1);
            s.ackMs = s.ackMs == 0 ? age : std::min(s.ackMs, age);
        }
    }
    const std::uint32_t late = s.ackMs == 0 ? kFastMapFirstLateMs : kFastMapLateMs + 2 * s.ackMs;
    std::size_t done = 0;
    for (; done < s.flights.size() && acked(s.flights[done].seqEnd); ++done) {
        const std::uint32_t age = nowMs_ - s.flights[done].at;
        if (age <= late && (!s.lateSeen || nowMs_ - s.grownAt >= s.ackMs)) {
            s.fastWindow = std::min(s.fastWindow + (s.lateSeen ? 1u : s.flights[done].packets), kFastMapWindow);
            s.grownAt = nowMs_;
        }
    }
    s.flights.erase(s.flights.begin(), s.flights.begin() + static_cast<std::ptrdiff_t>(done));
    if (!s.flights.empty() && nowMs_ - s.flights.front().at > late) {
        s.fastWindow = std::max(kFastMapMinWindow, s.fastWindow / 2);
        s.lateSeen = true;
        s.flights.clear();
    }
    std::uint8_t blocks = 0;
    const std::uint8_t seqBegin = seqs->next; // (unchanged since the frame's start: nothing sent yet)
    for (int n = 0; n < kFastMapBlocksPerFrame && budget > 0 && s.state == 1; ++n) {
        // (Its own window, within kReliableSpan for every paced sender together: reliableBulkRoom.
        // The first block goes with MSG_MAP_SEND_START, a packet of its own.)
        const std::size_t need = s.sent == 0 ? 2 : 1;
        if (transport_.reliableOutstanding(peer) + need > s.fastWindow || reliableBulkRoom(peer) < need) {
            ++s.waits;
            break;
        }
        mapSendNextBlock(peer, s);
        ++blocks;
        --budget;
    }
    if (blocks != 0 && s.state == 1) {
        s.flights.push_back({nowMs_, seqBegin, transport_.reliableSeqs(peer)->next, blocks, false});
    }
}

void GameHost::mapSendNextBlock(std::uint8_t peer, MapSender& s)
{
    // SrvNet_MapSendNextBlock.
    const auto player = players_.find(peer);
    if (s.sent == 0) {
        if (player != players_.end()) {
            mapSendPrint("MapSend.c:DownloadingMap", {mapSendName_, player->second.name});
        }
        // MSG_MAP_SEND_START, 88 bytes: +4 the size (u32), +8 the map's file name (79 letters at most,
        // NUL-padded).
        std::vector<std::uint8_t> start(88, 0); // (bytes 1-3 left as they were: 0 here)
        start[0] = static_cast<std::uint8_t>(net::Op::MapSendStart);
        for (int i = 0; i < 4; ++i) {
            start[4 + i] = static_cast<std::uint8_t>(s.size >> (8 * i));
        }
        std::copy_n(mapSendName_.begin(), std::min<std::size_t>(mapSendName_.size(), 79), start.begin() + 8);
        transport_.sendReliableNow(peer, start, nowMs_);
    }
    const std::uint32_t n = std::min<std::uint32_t>(s.blockSize, s.size - s.sent);
    const std::vector<std::uint8_t>& from = s.privateCopy ? s.copy : mapSendData_;
    // MSG_MAP_SEND_PACKET: {0xB9, 0, u16 the block's number, u16 its size}, then the block's bytes.
    std::vector<std::uint8_t> block{static_cast<std::uint8_t>(net::Op::MapSendPacket), 0, static_cast<std::uint8_t>(s.seq),
                                    static_cast<std::uint8_t>(s.seq >> 8), static_cast<std::uint8_t>(n),
                                    static_cast<std::uint8_t>(n >> 8)};
    if (s.sent + n <= from.size()) {
        block.insert(block.end(), from.begin() + s.sent, from.begin() + s.sent + n);
    } else {
        block.resize(block.size() + n, 0); // (the exe reads past a shorter buffer)
    }
    transport_.sendReliableNow(peer, block, nowMs_);
    ++s.seq;
    s.sent += n;
    if (s.size <= s.sent) {
        // Done: the figures to the console, the count down, the sender reset.
        const std::uint64_t seconds = (nowMs_ - s.startMs) / 1000;
        const std::uint64_t rate = seconds == 0 ? s.size : s.size / seconds;
        if (player != players_.end()) {
            mapSendPrint("MapSend.c:downloaddone", {std::to_string(s.sent), std::to_string(s.seq - 1), std::to_string(rate),
                                                    std::to_string(seconds), player->second.name});
        }
        if (mapSendsInProgress_ != 0) {
            --mapSendsInProgress_;
        }
        mapSendPrint("mapsend.c:InProgress", {std::to_string(mapSendsInProgress_)});
        mapSendReset(s);
    }
}

void GameHost::mapSendAbortAll(std::uint8_t reason)
{
    // Each send under way aborted (SrvNet_MapSendAbort: MSG_MAP_SEND_ABORT, the
    // count down) and its sender reset.
    for (auto& [peer, s] : mapSenders_) {
        if (s.state != 1) {
            continue;
        }
        transport_.sendReliableNow(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::MapSendAbort), reason}, nowMs_);
        if (mapSendsInProgress_ != 0) {
            --mapSendsInProgress_;
        }
        // (the sender's index, its player's, whether it's still in)
        mapSendPrint("MapSend.c:downloadaborted", {std::to_string((peer - 1) & 31), std::to_string(reason)});
        mapSendReset(s);
    }
}

} // namespace nox::server
