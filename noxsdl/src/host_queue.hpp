// What the game noxsdl hosts hands its own client's thread, in order (LocalHost's queue; plain
// data, no SDL, no lock of its own: LocalHost holds one round it). Three kinds of entry:
//   a call    a host client call (GAME.EXE's server calling its client directly) to run on the
//             client's thread;
//   a marker  a datagram the server sent our client (its FNV-1a hash and size): the calls before
//             it run before the client takes that datagram, those after it after;
//   a park    the server waiting for the client's thread to stop for a solo save or load.
// GAME.EXE's server and client share one thread: a call there comes after every message the
// server sent the client before it and before every one it sent after. The client's thread takes
// entries so:
//   takeForDatagram  a datagram arriving: the entries before its marker (the markers before it are
//                    datagrams lost on the way);
//   takeDue          every datagram so far taken (a frame's start, the socket drained): the calls
//                    ahead of the first marker, and a park when one comes before any marker;
//   takeToPark       a park that can't wait for its datagrams (lost, or the client's thread
//                    waiting on the server's lock): every call up to the park and it;
//   takeCalls        every call (the bridge calling the client directly on the client's thread).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace noxsdl {

class HostQueue {
public:
    struct Entry {
        enum class Kind : std::uint8_t { Call, Marker, Park };
        Kind kind = Kind::Call;
        std::uint64_t seq = 0;  ///< a call's number (from 1)
        std::uint32_t hash = 0; ///< a marker's datagram's
        std::size_t size = 0;
        std::function<void()> call;
    };
    /// The most markers kept: a client that stopped taking its datagrams (leaving) lets the oldest go.
    static constexpr std::size_t kMostMarkers = 2048;

    /// FNV-1a over a datagram.
    static std::uint32_t hashOf(std::span<const std::uint8_t> bytes)
    {
        std::uint32_t h = 2166136261u;
        for (const std::uint8_t b : bytes) {
            h = (h ^ b) * 16777619u;
        }
        return h;
    }

    /// A call queued; its number.
    std::uint64_t pushCall(std::function<void()> call)
    {
        Entry e;
        e.kind = Entry::Kind::Call;
        e.seq = ++lastSeq_;
        e.call = std::move(call);
        q_.push_back(std::move(e));
        return lastSeq_;
    }
    void pushMarker(std::span<const std::uint8_t> bytes)
    {
        Entry e;
        e.kind = Entry::Kind::Marker;
        e.hash = hashOf(bytes);
        e.size = bytes.size();
        q_.push_back(std::move(e));
        if (++markers_ > kMostMarkers) {
            // (our client stopped taking its datagrams: the oldest marker let go)
            q_.erase(std::ranges::find_if(q_, isMarker));
            --markers_;
        }
    }
    void pushPark()
    {
        Entry e;
        e.kind = Entry::Kind::Park;
        q_.push_back(std::move(e));
    }

    bool parkQueued() const { return std::ranges::any_of(q_, isPark); }
    std::size_t markers() const { return markers_; }
    std::size_t size() const { return q_.size(); }
    std::uint64_t lastSeq() const { return lastSeq_; }

    void dropParks() { std::erase_if(q_, isPark); }
    void dropCalls() { std::erase_if(q_, isCall); }
    void dropMarkers()
    {
        std::erase_if(q_, isMarker);
        markers_ = 0;
    }

    /// The datagram `bytes` about to be taken: the calls (and a park) before its marker, the
    /// markers before it dropped; nothing when no marker is its.
    void takeForDatagram(std::span<const std::uint8_t> bytes, std::vector<Entry>& out)
    {
        const std::uint32_t hash = hashOf(bytes);
        const auto marker = std::ranges::find_if(q_, [&](const Entry& e) { return isMarker(e) && e.hash == hash && e.size == bytes.size(); });
        if (marker == q_.end()) {
            return;
        }
        for (auto it = q_.begin(); it != marker; ++it) {
            if (isMarker(*it)) {
                --markers_;
            } else {
                out.push_back(std::move(*it));
            }
        }
        --markers_;
        q_.erase(q_.begin(), std::next(marker));
    }
    /// The calls ahead of the first marker, and a park met before any (it last).
    void takeDue(std::vector<Entry>& out)
    {
        while (!q_.empty() && !isMarker(q_.front())) {
            const bool park = isPark(q_.front());
            out.push_back(std::move(q_.front()));
            q_.pop_front();
            if (park) {
                return;
            }
        }
    }
    /// Every call up to the first park and it (markers left); nothing without a park.
    void takeToPark(std::vector<Entry>& out)
    {
        if (!parkQueued()) {
            return;
        }
        for (auto it = q_.begin(); it != q_.end();) {
            if (isMarker(*it)) {
                ++it;
                continue;
            }
            const bool park = isPark(*it);
            out.push_back(std::move(*it));
            it = q_.erase(it);
            if (park) {
                return;
            }
        }
    }
    /// Every call (markers and parks left).
    void takeCalls(std::vector<Entry>& out)
    {
        for (auto it = q_.begin(); it != q_.end();) {
            if (isCall(*it)) {
                out.push_back(std::move(*it));
                it = q_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    static bool isCall(const Entry& e) { return e.kind == Entry::Kind::Call; }
    static bool isMarker(const Entry& e) { return e.kind == Entry::Kind::Marker; }
    static bool isPark(const Entry& e) { return e.kind == Entry::Kind::Park; }

    std::deque<Entry> q_;
    std::size_t markers_ = 0;
    std::uint64_t lastSeq_ = 0;
};

} // namespace noxsdl
