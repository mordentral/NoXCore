// Kept lists: the things a mode keeps count of for a holder (a unit's summoned creatures, its
// missiles in flight), each list dropping those gone (or dead) natively as it's counted, as generic
// engine plumbing. A mode's script adds what it makes (its
// event); counting, frame by frame where the mode's loop stood, is here. Nothing here knows any
// mode.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace nox::server {

class World;

class KeptLists {
public:
    /// What a list keeps: things in the world and not going (Present), of `type` if it's not 0
    /// (thing.bin's number, from 1); or those also alive (Living: not dead, health not 0).
    enum class Keep : std::uint8_t { Present, Living };
    /// A list key's rule, and the group its lists count in (a group's lists of one holder counted
    /// together).
    struct Rule {
        Keep keep = Keep::Present;
        std::uint16_t type = 0;
        std::uint32_t group = 0;
    };

    /// The rule of every list of `key` (none set: Present, any type, group 0). (Set before the
    /// lists are made: a list keeps the rule it was made with.)
    void setRule(std::uint32_t key, const Rule& rule);
    /// A thing put on the holder's list of `key`.
    void add(std::uint32_t holder, std::uint32_t key, std::uint32_t thing);
    /// The holder's list of `key` settled (those its rule drops gone): how many it keeps.
    int settle(World& world, std::uint32_t holder, std::uint32_t key);
    /// Every list of the holder's in `group` settled: how many they keep in all, and (`have`) how
    /// many its list of `key` keeps.
    int settleGroup(World& world, std::uint32_t holder, std::uint32_t group, std::uint32_t key, int& have);
    /// Every list forgotten (the rules kept).
    void clear() { holders_.clear(); }

private:
    struct List {
        std::uint32_t key = 0;
        Rule rule;
        std::vector<std::uint32_t> things;
    };
    struct Holder {
        std::uint32_t holder = 0;
        std::vector<List> lists;
    };
    Rule rule(std::uint32_t key) const;
    Holder* find(std::uint32_t holder);
    static void settle(World& world, List& list);

    std::vector<std::pair<std::uint32_t, Rule>> rules_;
    std::vector<Holder> holders_;
};

} // namespace nox::server
