// Kept lists (kept.hpp): the things a mode keeps count of, the gone dropped natively.
#include "kept.hpp"

#include "noxcore/server/world.hpp"

#include <algorithm>

namespace nox::server {

void KeptLists::setRule(std::uint32_t key, const Rule& rule)
{
    for (auto& [k, r] : rules_) {
        if (k == key) {
            r = rule;
            return;
        }
    }
    rules_.emplace_back(key, rule);
}

KeptLists::Rule KeptLists::rule(std::uint32_t key) const
{
    for (const auto& [k, r] : rules_) {
        if (k == key) {
            return r;
        }
    }
    return {};
}

KeptLists::Holder* KeptLists::find(std::uint32_t holder)
{
    for (Holder& h : holders_) {
        if (h.holder == holder) {
            return &h;
        }
    }
    return nullptr;
}

void KeptLists::add(std::uint32_t holder, std::uint32_t key, std::uint32_t thing)
{
    Holder* h = find(holder);
    if (!h) {
        holders_.push_back({holder, {}});
        h = &holders_.back();
    }
    for (List& l : h->lists) {
        if (l.key == key) {
            l.things.push_back(thing);
            return;
        }
    }
    h->lists.push_back({key, rule(key), {thing}});
}

void KeptLists::settle(World& world, List& list)
{
    const Rule& r = list.rule;
    std::erase_if(list.things, [&](std::uint32_t e) {
        const ServerObject* o = world.byExtent(e);
        if (!o || o->gone() || o->pendingDelete) {
            return true;
        }
        if (r.type != 0 && o->type != r.type) {
            return true;
        }
        return r.keep == Keep::Living && ((o->flags & ObjFlag::Dead) || o->health == 0);
    });
}

int KeptLists::settle(World& world, std::uint32_t holder, std::uint32_t key)
{
    if (Holder* h = find(holder)) {
        for (List& l : h->lists) {
            if (l.key == key) {
                settle(world, l);
                return static_cast<int>(l.things.size());
            }
        }
    }
    return 0;
}

int KeptLists::settleGroup(World& world, std::uint32_t holder, std::uint32_t group, std::uint32_t key, int& have)
{
    have = 0;
    Holder* h = find(holder);
    if (!h) {
        return 0;
    }
    int total = 0;
    for (List& l : h->lists) {
        if (l.rule.group == group) {
            settle(world, l);
            const int n = static_cast<int>(l.things.size());
            total += n;
            if (l.key == key) {
                have = n;
            }
        }
    }
    return total;
}

} // namespace nox::server
