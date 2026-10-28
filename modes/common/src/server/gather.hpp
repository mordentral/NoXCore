// Gatherables: things lying about that players take up, as generic engine plumbing
// (the frame's continuous work native, the mode's script
// deciding on events). Two ways, the mode's script setting the figures:
//   - Drift: each is drawn toward the nearest player whose reach it's within, a step a frame (its
//     `base`, its age modulo `jitter`, and `pull` over its distance, the distance at least `near`;
//     never past the player), and taken within `take` of it;
//   - Touch: each is taken by the first player (in the frame's order) within its reach.
// Each is gone after `life` frames (or, with dropHeld, as something picks it up); past `most`, the
// oldest go. Taken, the thing is deleted and the mode's handler told (the thing's value, the
// player, where it lay). Nothing here knows any mode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace nox::server {

class GameHost;
class World;
struct ServerObject;

class Gatherables {
public:
    enum class Way : std::uint8_t { Drift, Touch };
    struct Rules {
        Way way = Way::Touch;
        float take = 0;
        std::uint32_t life = 0;
        std::size_t most = 0;
        bool dropHeld = false;
        float base = 0;
        std::uint32_t jitter = 1;
        float pull = 0;
        float near = 1;
    };
    struct Item {
        std::uint32_t extent = 0;
        std::uint32_t value = 0;
        std::uint32_t born = 0;
    };
    /// A player who may take things up this frame, and its reach.
    struct Taker {
        ServerObject* unit = nullptr;
        float reach = 0;
    };
    /// Taken: the thing's item (its thing deleted), the taker's place in the frame's list, where
    /// the thing lay. Told after the pass, in the order taken: it may add or clear this block's items.
    using Taken = std::function<void(const Item&, std::size_t taker, float x, float y)>;

    void setRules(const Rules& rules) { rules_ = rules; }
    const Rules& rules() const noexcept { return rules_; }
    void onTaken(Taken taken) { taken_ = std::move(taken); }

    void add(const Item& item) { items_.push_back(item); }
    std::vector<Item>& items() noexcept { return items_; }
    const std::vector<Item>& items() const noexcept { return items_; }
    std::size_t size() const noexcept { return items_.size(); }
    void clear() { items_.clear(); }
    /// Past `most`, the oldest deleted and dropped.
    void cap(World& world);

    /// The frame's pass at `frame`, the takers given.
    void evaluate(GameHost& host, std::uint32_t frame, std::span<const Taker> takers);

private:
    Rules rules_;
    std::vector<Item> items_;
    Taken taken_;
};

} // namespace nox::server
