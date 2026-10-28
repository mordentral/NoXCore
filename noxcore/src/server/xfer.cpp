// The network file transfers (xfer.hpp): MSG_XFER's sends cut into chunks, resent until each is
// acknowledged, and receives put back together in order. The host uses them for the MOTD and for
// a quest's saved players going to and from the clients.
#include "noxcore/server/xfer.hpp"

#include "noxcore/util/bytes.hpp"

#include <algorithm>
#include <cstring>

namespace nox::server {

namespace {

constexpr std::uint8_t kOp = 0xC2;  // MSG_XFER
constexpr std::size_t kChunk = 0x200;  // 512 bytes a chunk

using util::u16At;

} // namespace

bool Xfers::send(std::uint8_t peer, std::uint8_t action, std::string_view type, std::span<const std::uint8_t> data)
{
    // the first stream neither started nor sized.
    std::size_t id = 0;
    while (id < sends_.size() && (sends_[id].state != 0 || sends_[id].size != 0)) {
        ++id;
    }
    if (id == sends_.size() || data.empty()) {
        return false;
    }
    SendStream& s = sends_[id];
    ++active_;
    std::uint16_t index = 0;
    for (std::size_t at = 0; at < data.size(); at += kChunk) {
        Chunk c;
        c.index = ++index;
        const std::size_t n = std::min(kChunk, data.size() - at);
        c.data.assign(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
        s.chunks.push_back(std::move(c));
    }
    s.peer = peer;
    s.recvId = 0;
    s.state = 1;
    s.size = static_cast<std::uint32_t>(data.size());
    s.action = action;
    s.type = std::string(type);
    // XFER 0: action, size, the type's name, the stream's id (the rest unset there).
    // Layout (0x8C bytes): 0xC2, 0, the action (+2), u32 size (+4), the type's name (+8, at most
    // 0x7F bytes), the stream's id (+0x88).
    std::vector<std::uint8_t> m(0x8C, 0);
    m[0] = kOp;
    m[1] = 0;
    m[2] = action;
    for (int i = 0; i < 4; ++i) {
        m[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(s.size >> (8 * i));
    }
    std::memcpy(m.data() + 8, s.type.data(), std::min<std::size_t>(s.type.size(), 0x7F));
    m[0x88] = static_cast<std::uint8_t>(id);
    out_(peer, std::move(m), true);
    return true;
}

Xfers::SendStream* Xfers::sendStream(std::uint8_t peer, std::uint8_t recvId)
{
    // (by connection and the receiver's id, whatever its state)
    for (SendStream& s : sends_) {
        if (s.peer == peer && s.recvId == recvId) {
            return &s;
        }
    }
    return nullptr;
}

void Xfers::sendChunk(const SendStream& s, const Chunk& c)
{
    // XFER 2: the receiver's id, the chunk's number and size, its bytes; unreliable.
    std::vector<std::uint8_t> m = {kOp, 2, s.recvId, 0, static_cast<std::uint8_t>(c.index), static_cast<std::uint8_t>(c.index >> 8),
                                   static_cast<std::uint8_t>(c.data.size()), static_cast<std::uint8_t>(c.data.size() >> 8)};
    m.insert(m.end(), c.data.begin(), c.data.end());
    out_(s.peer, std::move(m), false);
}

void Xfers::giveUp(std::uint8_t peer, std::uint8_t recvId, std::uint8_t reason)
{
    // XFER 5 with the reason, the send's end, the stream freed.
    SendStream* s = sendStream(peer, recvId);
    if (!s) {
        return;
    }
    out_(peer, {kOp, 5, recvId, reason}, true);
    ended_.push_back({s->peer, s->action, false});
    if (active_ != 0) {
        --active_;
    }
    *s = SendStream{};
}

std::optional<Xfers::Received> Xfers::receive(std::uint8_t peer, std::span<const std::uint8_t> m, std::uint32_t frame)
{
    if (m.size() < 2 || m[0] != kOp) {
        return std::nullopt;
    }
    switch (m[1]) {
    case 0: { // start (Net_Xfer_OnRecvStart)
        if (m.size() < 0x8C) {
            return std::nullopt;
        }
        const std::uint32_t size = m[4] | m[5] << 8 | m[6] << 16 | static_cast<std::uint32_t>(m[7]) << 24;
        const auto free = std::ranges::find_if(recvs_, [](const RecvStream& r) { return r.size == 0; });
        if (size == 0 || size > kMaxReceive || free == recvs_.end()) {
            return std::nullopt;
        }
        RecvStream& r = *free;
        r = RecvStream{};
        r.peer = peer;
        r.action = m[2];
        const char* name = reinterpret_cast<const char*>(m.data() + 8);
        r.type.assign(name, strnlen(name, 0x80));
        r.size = size;
        r.data.assign(size, 0);
        r.lastAt = frame;
        const auto id = static_cast<std::uint8_t>(free - recvs_.begin());
        // XFER 1 (accept): our receive stream's id, then the sender's stream id.
        out_(peer, {kOp, 1, id, m[0x88]}, true); // (accept)
        return std::nullopt;
    }
    case 1: { // accept (Net_Xfer_OnSendAccept: the stream by its id, the connection unchecked)
        if (m.size() >= 4 && m[3] < sends_.size()) {
            sends_[m[3]].recvId = m[2];
            sends_[m[3]].state = 2;
        }
        return std::nullopt;
    }
    case 2: { // data: acknowledged at once (SrvNet_OnPacket), then Net_Xfer_OnRecvData
        if (m.size() < 8) {
            return std::nullopt;
        }
        const std::uint16_t chunk = u16At(m, 4);
        const std::uint16_t size = u16At(m, 6);
        // XFER 3 (ack): the stream's id, 0, u16 the chunk's number.
        out_(peer, {kOp, 3, m[2], 0, static_cast<std::uint8_t>(chunk), static_cast<std::uint8_t>(chunk >> 8)}, true);
        const auto bytes = m.subspan(8, std::min<std::size_t>(size, m.size() - 8));
        if (size == 0 || m[2] >= recvs_.size() || recvs_[m[2]].size == 0) {
            return std::nullopt;
        }
        RecvStream& r = recvs_[m[2]];
        r.lastAt = frame;
        // The next chunk in at the end of what's in (past the file's size the exe writes over its
        // heap: ours writes what fits; either way the file never ends whole and times out).
        const auto append = [&](std::span<const std::uint8_t> b) {
            if (r.have < r.size) {
                std::copy_n(b.begin(), std::min<std::size_t>(b.size(), r.size - r.have), r.data.begin() + r.have);
            }
            r.have += static_cast<std::uint32_t>(b.size());
            ++r.next;
        };
        if (chunk == r.next) {
            append(bytes);
        } else if (chunk > r.next && r.pendingBytes + bytes.size() <= r.size &&
                   std::ranges::none_of(r.pending, [&](const Pending& p) { return p.chunk == chunk; })) {
            // (Held for later: a chunk still to come, once, while what's held fits the file, as a
            // sender's early chunks always do. Past ones and repeats are never used: held, they'd
            // pile up for good.)
            r.pending.push_back({chunk, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
            r.pendingBytes += bytes.size();
        }
        for (auto it = r.pending.begin(); it != r.pending.end();) { // (one walk, in the order they came)
            if (it->chunk == r.next) {
                append(it->data);
                r.pendingBytes -= it->data.size();
                it = r.pending.erase(it);
            } else {
                ++it;
            }
        }
        if (r.have == r.size) {
            // Done to the stream's connection; the file handed over as
            // the data message's sender's (connId - 1): the stream is taken by its id alone, so
            // the two differ when another connection sends the last chunk (an original bug).
            const auto id = static_cast<std::uint8_t>(&r - recvs_.data());
            out_(r.peer, {kOp, 4, id}, true);
            Received done{peer, r.action, r.type, std::move(r.data)};
            r = RecvStream{};
            return done;
        }
        return std::nullopt;
    }
    case 3: { // ack (Net_Xfer_OnSendAck)
        if (m.size() >= 6) {
            if (SendStream* s = sendStream(peer, m[2])) {
                const std::uint16_t chunk = u16At(m, 4);
                const auto it = std::ranges::find_if(s->chunks, [&](const Chunk& c) { return c.index == chunk; });
                if (it != s->chunks.end()) {
                    s->chunks.erase(it);
                }
            }
        }
        return std::nullopt;
    }
    case 4: { // done (Net_Xfer_OnSendDone)
        if (SendStream* s = m.size() >= 3 ? sendStream(peer, m[2]) : nullptr) {
            ended_.push_back({s->peer, s->action, true});
            --active_;
            *s = SendStream{};
        }
        return std::nullopt;
    }
    case 5: { // cancel (Net_Xfer_OnRecvCancel: the stream by its id alone)
        if (m.size() >= 4 && m[2] < recvs_.size()) {
            recvs_[m[2]] = RecvStream{};
        }
        return std::nullopt;
    }
    case 6: { // abort (Net_Xfer_OnSendAbort: the sends under way not counted down)
        if (SendStream* s = m.size() >= 4 ? sendStream(peer, m[2]) : nullptr) {
            ended_.push_back({s->peer, s->action, false});
            *s = SendStream{};
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

void Xfers::frame(std::uint32_t frame)
{
    // the first two chunks of each accepted send not yet acknowledged.
    if (active_ != 0) {
        for (SendStream& s : sends_) {
            if (s.state != 2) {
                continue;
            }
            int n = 0;
            for (auto it = s.chunks.begin(); it != s.chunks.end() && n < 2; ++it, ++n) {
                if (it->sentAt == 0) {
                    sendChunk(s, *it);
                    it->sentAt = frame;
                } else if (it->sentAt + 0x5Au < frame) {  // unacknowledged 90 frames
                    if (it->tries < 0x14) {  // at most 20 resends
                        sendChunk(s, *it);
                        ++it->tries;
                        it->sentAt = frame;
                    } else {
                        giveUp(s.peer, s.recvId, 2);
                        break;
                    }
                }
            }
        }
    }
    // a receive 900 frames without data dropped, its sender told (XFER 6, reason 3).
    for (RecvStream& r : recvs_) {
        if (r.size != 0 && r.lastAt + 900u < frame) {
            out_(r.peer, {kOp, 6, static_cast<std::uint8_t>(&r - recvs_.data()), 3}, true);
            r = RecvStream{};
        }
    }
}

void Xfers::peerGone(std::uint8_t peer)
{
    for (SendStream& s : sends_) {
        if (s.state == 2 && s.peer == peer) {
            giveUp(peer, s.recvId, 1);
        }
    }
}

} // namespace nox::server
