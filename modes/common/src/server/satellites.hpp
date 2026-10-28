// Satellites: groups of things turning about a unit (an orbit group), each harming about it as
// they go, as generic engine plumbing (the frame's continuous
// work native, the mode's script deciding on events). A mode's script makes a group's things and
// hands them over with their life and harm (its event); each frame, where the mode's own loop
// stood, the group is settled (its things gone dropped, its life's end put away) and turned (the
// harm about each, the turn, each thing placed). Nothing here knows any mode.
//
// A group is kept by its centre's unit while the mode keeps it: its angle goes on from one set of
// things to the next.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::server {

class GameHost;
class World;
struct ServerObject;

class Satellites {
public:
    /// The harm about each thing each frame (Missiles::damageAt's): out to `reach`, the whole of
    /// `damage` within `full`, of `type`, from the thing, the centre spared.
    struct Harm {
        float reach = 0;
        float full = 0;
        int damage = 0;
        int type = 0;
    };
    /// How the things stand about the centre: `count` places round it (the i-th at the angle plus
    /// a full turn times i over count), `radius` out, the angle `step` more each frame.
    struct Shape {
        int count = 1;
        float radius = 0;
        float step = 0;
    };
    struct Group {
        std::vector<std::uint32_t> things;
        float angle = 0;
        std::uint32_t born = 0;      ///< the frame its things were handed over
        std::uint32_t life = 0;      ///< past this many frames from `born`, put away
        std::uint32_t harmAfter = 0; ///< no harm till past this many frames from `born`
        std::uint16_t type = 0;      ///< its things' type (thing.bin's, from 1): one of another dropped
        Harm harm;
    };

    explicit Satellites(const things::ThingDb& things) : things_(things) {}

    /// The centre's group (made, its angle 0, when it has none).
    Group& group(std::uint32_t centre) { return groups_[centre]; }
    const Group* find(std::uint32_t centre) const;
    /// Things handed to the centre's group from `frame` (its things before them let go).
    void start(std::uint32_t centre, std::vector<std::uint32_t> things, std::uint32_t frame, std::uint32_t life, std::uint32_t harmAfter,
               std::uint16_t type, const Harm& harm);

    /// The frame's settling: the things gone (or another type) dropped; its life over, those left
    /// deleted. Whether any are out.
    bool settle(World& world, Group& g, std::uint32_t frame);
    /// The frame's turn of a group with things out: the harm about each (past harmAfter), the
    /// angle on a step (a full turn taken off past it), each thing stopped and placed.
    void turn(GameHost& host, ServerObject& centre, Group& g, const Shape& shape, std::uint32_t frame);

    /// Each group whose centre `keep` turns down, its things put to deletion and the group gone, in
    /// the centres' order.
    template <class Keep>
    void tidy(World& world, Keep&& keep)
    {
        for (auto it = groups_.begin(); it != groups_.end();) {
            if (keep(it->first)) {
                ++it;
                continue;
            }
            letGo(world, it->second);
            it = groups_.erase(it);
        }
    }
    /// Every group's things put to deletion, every group gone.
    void putAway(World& world);
    /// Every group forgotten (its things left as they are: a new map's).
    void clear() { groups_.clear(); }
    bool empty() const noexcept { return groups_.empty(); }

private:
    void letGo(World& world, const Group& g);

    const things::ThingDb& things_;
    std::map<std::uint32_t, Group> groups_; ///< by centre
};

} // namespace nox::server
