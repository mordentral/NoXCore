// The network file transfers (MSG_XFER 0xC2; GAME.EXE's Net_Xfer_*): a file
// sent in 512-byte chunks over a connection, each acknowledged.
//   - Sending: a free send stream (of 0x20: its index is its id), the data cut into
//     chunks numbered from 1, XFER 0 (start: action, size, type name, the stream's id), reliable.
//     The receiver's XFER 1 (accept: its stream's id) opens it (state 2). Each frame
//     the first two chunks not yet acknowledged of each open stream: one never sent
//     goes (XFER 2, unreliable), one sent more than 90 frames ago goes again, at most 20 times;
//     then the transfer is given up (XFER 5 with reason 2). XFER 3 (ack) drops a
//     chunk, XFER 4 (done) ends the stream, XFER 6 (abort) drops it. A connection gone, its open
//     streams are given up with reason 1 (one never accepted stays: an original bug).
//   - Receiving (Net_Xfer_OnRecvStart / Data): a free receive stream (of 0x20) for a start with a
//     size, XFER 1 back; every data message acknowledged at once (SrvNet_OnPacket, whatever the
//     stream); the next chunk appended, any other kept aside and the kept ones walked once in the
//     order they came; all of it in, XFER 4 and the file handed over. 900 frames without data, the
//     stream dropped with XFER 6, reason 3. XFER 5 drops a stream by its id alone.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::server {

class Xfers {
public:
    /// The actions (Start's +2): 1 the MOTD, 2 a saved player sent to its client (SAVEDATA), 3 a
    /// client's saved player sent to the server (SAVE_SERVER).
    enum Action : std::uint8_t { Motd = 1, SaveData = 2, SaveServer = 3 };
    /// The largest file a client may send (noxcpp): what a client sends is its saved player, a few
    /// KB (its size a 16-bit count). A start naming more is refused unanswered, so no connection
    /// can have the host set aside its 32 streams' gigabytes (the exe allocates whatever the
    /// start's u32 says).
    static constexpr std::uint32_t kMaxReceive = 1u << 20;

    /// What goes out: to a connection, the message, reliable (Net_Send's flag 3) or not (flag 0).
    using Out = std::function<void(std::uint8_t peer, std::vector<std::uint8_t> bytes, bool reliable)>;

    /// A file in whole (as the exe hands it on): from whom, its action and type name.
    struct Received {
        std::uint8_t peer = 0;
        std::uint8_t action = 0;
        std::string type;
        std::vector<std::uint8_t> data;
    };
    /// A send that ended: done (the receiver's XFER 4) or given up / aborted.
    struct Ended {
        std::uint8_t peer = 0;
        std::uint8_t action = 0;
        bool done = false;
    };

    explicit Xfers(Out out) : out_(std::move(out)) {}

    /// Sends `data` to `peer` as `type`; false with no free stream or nothing to send.
    bool send(std::uint8_t peer, std::uint8_t action, std::string_view type, std::span<const std::uint8_t> data);
    /// SrvNet_OnPacket's MSG_XFER subs 0-6 (`m` the whole message); a file all in, handed back.
    std::optional<Received> receive(std::uint8_t peer, std::span<const std::uint8_t> m, std::uint32_t frame);
    /// The server's frame (updateServer).
    void frame(std::uint32_t frame);
    /// A connection gone (Plr_OnDisconnect): `peer`'s open sends given up, reason 1.
    void peerGone(std::uint8_t peer);
    /// The sends that ended since the last call.
    std::vector<Ended> takeEnded() { return std::exchange(ended_, {}); }
    /// Sends under way.
    int sending() const noexcept { return active_; }

private:
    struct Chunk {
        std::uint16_t index = 0;
        std::vector<std::uint8_t> data;
        std::uint32_t sentAt = 0; ///< (0: never)
        std::int16_t tries = 0;
    };
    struct SendStream {
        std::uint8_t peer = 0;
        std::uint8_t recvId = 0;
        std::uint16_t state = 0; ///< 0 free, 1 started, 2 accepted
        std::uint32_t size = 0;
        std::uint8_t action = 0;
        std::string type;
        std::list<Chunk> chunks; ///< not yet acknowledged, in order
    };
    struct Pending {
        std::uint16_t chunk = 0;
        std::vector<std::uint8_t> data;
    };
    struct RecvStream {
        std::uint8_t peer = 0;
        std::uint8_t action = 0;
        std::string type;
        std::uint32_t size = 0; ///< (0: free)
        std::uint32_t have = 0; ///< bytes in
        std::uint32_t next = 1;
        std::vector<std::uint8_t> data;
        std::uint32_t lastAt = 0;
        std::list<Pending> pending; ///< chunks out of order, as they came
        std::size_t pendingBytes = 0; ///< (theirs together: never more than the file's size)
    };

    SendStream* sendStream(std::uint8_t peer, std::uint8_t recvId);
    void giveUp(std::uint8_t peer, std::uint8_t recvId, std::uint8_t reason);
    void sendChunk(const SendStream& s, const Chunk& c);

    Out out_;
    std::array<SendStream, 0x20> sends_{};
    std::array<RecvStream, 0x20> recvs_{};
    int active_ = 0;
    std::vector<Ended> ended_;
};

} // namespace nox::server
