// Ready timers: each holder's weapons (slots of a kind, a level and the frame each is ready at)
// counted down natively, the mode's script asked only when one is ready to fire, as generic engine
// plumbing (the frame's continuous work native, the script
// deciding on events). Each kind goes one of three ways, as the script sets it:
//   - Fire: ready, the script's fire (it says when the slot is ready next);
//   - Orbit: the holder's satellites (satellites.hpp) settled and, while out, turned each frame;
//     none out and ready, the script's fire (it hands the next things over);
//   - Keep: the holder's kept lists of the kind's group (kept.hpp) settled each frame; fewer than
//     its shape's cap of this kind, fewer than the group's most in all, ready, and a foe in sight
//     within the kind's gate, the script's fire (it adds what it makes to the list).
// A slot's shape (an Orbit's places, radius and step; a Keep's cap) is the script's, by kind, level
// and the holder's bonus, asked once for each and kept.
//
// A fire the script declines (nothing to fire at yet) may say why, as a wait: a test of the world
// that, while it holds, means the fire would be declined again the same way. The slot is then
// looked at again when it's ready, as before, but the test is made here: while it holds the slot
// waits its retry again without asking the script; once it doesn't, the script is asked. (A wait
// is the script's promise: it must name the very reason it declined, with nothing done before it.)
// Nothing here knows any mode.
#pragma once

#include "kept.hpp"
#include "satellites.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace nox::server {

class GameHost;
class World;
struct ServerObject;

class ReadyTimers {
public:
    enum class Way : std::uint8_t { Fire, Orbit, Keep };
    /// A kind's way; for Keep, its lists' group, the most the group keeps in all, and the foe it
    /// waits for (one in sight within `gate`, the walls tested for the `tested` nearest).
    struct Kind {
        Way way = Way::Fire;
        std::uint32_t group = 0;
        int most = 0;
        float gate = 0;
        std::uint16_t tested = 0;
    };
    struct Shape {
        Satellites::Shape orbit;
        int cap = 0; ///< (Keep: how many of the kind its list keeps)
    };
    /// A ready slot handed to the script: whose (the holder's place in the frame's list), which,
    /// and for a Keep its list's count and its group's.
    struct Shot {
        std::size_t holder = 0;
        std::size_t slot = 0;
        std::uint32_t kind = 0;
        int level = 0;
        std::uint32_t readyAt = 0;
        int have = 0;
        int total = 0;
    };
    /// Why a fire was declined, a test of the world (holding: declined again). "Foes" are live
    /// enemy creatures of the holder (not dead, not going, health left); "in sight" the nearest
    /// `tested` of them within `range` with no wall between (foes.hpp).
    struct Wait {
        enum class Test : std::uint8_t {
            None,
            NoFoe,           ///< no foe in sight
            NoneNear,        ///< no foe within `range` at all, seen or not
            HolderEnchanted, ///< the holder under `enchant`
            KeptAny,         ///< the holder's kept list `key` holds any
            KeptAtLeast,     ///< it holds `least` or more
            OutOfReach,      ///< the nearest foe in sight is there, its edge farther than the holder's radius and `reach`
            AllUnder,        ///< every foe in sight is the frame's `except` or under `enchant` (or `enchant2`)
        };
        Test test = Test::None;
        /// A second test, its own retry (either holding, the slot waits: each the reason for a
        /// decline of its own, named with its figures below).
        Test also = Test::None;
        std::uint32_t alsoRetry = 0;
        float range = 0;
        std::uint16_t tested = 0;
        float reach = 0;
        std::uint32_t key = 0;
        int least = 0;
        int enchant = -1;
        int enchant2 = -1;
        std::uint32_t retry = 0; ///< frames to the next look while it holds
    };
    struct Result {
        std::uint32_t readyAt = 0;
        Wait wait;
    };
    /// The script's fire: the frame the slot is ready next, and why it was declined (if it was).
    using Fire = std::function<Result(const Shot&)>;
    using ShapeOf = std::function<Shape(std::uint32_t kind, int level, int bonus)>;

    explicit ReadyTimers(const things::ThingDb& things) : satellites_(things) {}

    void setKind(std::uint32_t kind, const Kind& k);
    Way way(std::uint32_t kind) const { return kind < kinds_.size() ? kinds_[kind].way : Way::Fire; }
    void onFire(Fire fire) { fire_ = std::move(fire); }
    void onShape(ShapeOf shape) { shape_ = std::move(shape); }
    Satellites& satellites() noexcept { return satellites_; }
    KeptLists& kept() noexcept { return kept_; }
    /// The satellites, kept lists and waits forgotten (a new map's), the shapes asked afresh.
    void clear();

    /// One holder's slots at `frame`, in order (its unit `unit`, the frame's `holder`-th; its
    /// `bonus`, the shapes' third part; `except`, the thing the waits' AllUnder passes over). A
    /// Slot has kind, level and readyAt.
    template <class Slot>
    void evaluate(GameHost& host, std::uint32_t frame, std::size_t holder, ServerObject& unit, std::span<Slot> slots, int bonus, std::uint32_t except)
    {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            Slot& s = slots[i];
            const auto kind = static_cast<std::uint32_t>(s.kind);
            switch (way(kind)) {
            case Way::Fire:
                if (frame >= s.readyAt) {
                    s.readyAt = fire(host, frame, unit, {holder, i, kind, s.level, s.readyAt, 0, 0}, bonus, except);
                }
                break;
            case Way::Orbit:
                if (orbit(host, frame, unit, kind, s.level, s.readyAt, bonus)) {
                    s.readyAt = fire(host, frame, unit, {holder, i, kind, s.level, s.readyAt, 0, 0}, bonus, except);
                }
                break;
            case Way::Keep: {
                int have = 0;
                int total = 0;
                if (keep(host, frame, unit, kind, s.level, s.readyAt, bonus, have, total)) {
                    s.readyAt = fire(host, frame, unit, {holder, i, kind, s.level, s.readyAt, have, total}, bonus, except);
                }
                break;
            }
            }
        }
    }

private:
    const Shape& shape(std::uint32_t kind, int level, int bonus);
    /// A ready slot: its wait's test made (holding: its retry), else the script's fire. The frame
    /// it's ready next.
    std::uint32_t fire(GameHost& host, std::uint32_t frame, ServerObject& unit, const Shot& shot, int bonus, std::uint32_t except);
    bool holds(World& world, const ServerObject& unit, Wait::Test test, const Wait& w, std::uint32_t except);
    /// An Orbit slot's frame; true when it's to fire.
    bool orbit(GameHost& host, std::uint32_t frame, ServerObject& unit, std::uint32_t kind, int level, std::uint32_t readyAt, int bonus);
    /// A Keep slot's frame; true when it's to fire (its counts in have, total).
    bool keep(GameHost& host, std::uint32_t frame, ServerObject& unit, std::uint32_t kind, int level, std::uint32_t readyAt, int bonus, int& have,
              int& total);

    std::vector<Kind> kinds_;
    Satellites satellites_;
    KeptLists kept_;
    struct Known {
        std::uint32_t key = 0;
        Shape shape;
    };
    std::vector<Known> shapes_; ///< (by key, sorted)
    /// The waits standing: by holder and kind, for the level and bonus they were declined at (a
    /// slot at another is asked afresh).
    struct Held {
        std::uint32_t holder = 0;
        std::uint32_t kind = 0;
        int level = 0;
        int bonus = 0;
        Wait wait;
    };
    std::vector<Held> waits_;
    Fire fire_;
    ShapeOf shape_;
};

} // namespace nox::server
