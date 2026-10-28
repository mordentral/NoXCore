// A horde: the creatures a mode sends at the players, steered and reaped natively, as generic
// engine plumbing (the frame's continuous work native, the
// mode deciding on events). Its rules (how often each looks, how far is too far, how near is near
// enough) are the mode's script's; the mode is called only when something happens: one left too
// far that comes back (the far handler: one that doesn't is deleted), and one fallen (the fall
// handler). Nothing here knows any mode.
//   - steer, each frame where the mode's loop stood: each member whose look is due (then due again
//     `period` frames on, and its extent modulo `spread`, so they don't all look on one frame): one
//     left farther than `cull` from every player is the far handler's or gone; one idle (its AI not
//     at a fight, a block, a dodge, its dying, getting up, fleeing or confused), with no enemy of its
//     own and farther than `close` from the nearest player, is sent to walk at that player (as a map
//     script's Walk sends a creature);
//   - reap, each frame: the gone dropped, a live one's most health held to the member's (an unhurt
//     one's health with it), the dead handed to the fall handler and dropped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace nox::server {

class GameHost;
struct ServerObject;

class Horde {
public:
    struct Member {
        std::uint32_t extent = 0;
        std::uint8_t worth = 1;      ///< (the mode's own)
        std::uint32_t steerAt = 0;   ///< its next look
        std::uint16_t most = 0;      ///< its most health held (0: as it is)
        std::uint8_t rank = 0;       ///< (the mode's own)
        bool comesBack = false;      ///< left too far: the far handler's (else deleted)
    };
    struct Rules {
        std::uint32_t period = 45;
        std::uint32_t spread = 16;
        float cull = 1600;
        float close = 60;
    };
    /// One that comes back, left too far: the member, its creature, the nearest player.
    using Far = std::function<void(GameHost& host, Member&, ServerObject& creature, ServerObject& nearest)>;
    /// One fallen (dead), told after the reaping pass has dropped it: a handler may add or clear.
    using Fell = std::function<void(GameHost& host, const Member&, ServerObject& creature)>;

    void setRules(const Rules& rules) { rules_ = rules; }
    void onFar(Far far) { far_ = std::move(far); }
    void onFell(Fell fell) { fell_ = std::move(fell); }

    void add(const Member& m) { members_.push_back(m); }
    /// The last member of that extent (null: none).
    Member* find(std::uint32_t extent);
    std::size_t size() const noexcept { return members_.size(); }
    const std::vector<Member>& members() const noexcept { return members_; }
    void clear() { members_.clear(); }

    /// The frame's steering, at the players given (in play).
    void steer(GameHost& host, std::uint32_t frame, const std::vector<ServerObject*>& players);
    /// The frame's reaping.
    void reap(GameHost& host);

private:
    Rules rules_;
    std::vector<Member> members_;
    Far far_;
    Fell fell_;
};

} // namespace nox::server
