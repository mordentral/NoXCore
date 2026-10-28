// The Builtins list's priorities, orders and filter (builtin_list.hpp).
#include "editor/builtin_list.hpp"

#include <algorithm>
#include <cctype>

namespace noxsdl::editor {

namespace {

/// Names compared without case ("Wall" before "WallBreak" before "Wander").
bool nameLess(const std::string& a, const std::string& b)
{
    const auto lo = [](unsigned char c) { return std::tolower(c); };
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [&](char x, char y) { return lo(static_cast<unsigned char>(x)) < lo(static_cast<unsigned char>(y)); });
}

} // namespace

const char* builtinPriorityName(BuiltinPriority p)
{
    switch (p) {
    case BuiltinPriority::Favourite: return "Favourite";
    case BuiltinPriority::High: return "High";
    case BuiltinPriority::Normal: return "Normal";
    case BuiltinPriority::Low: return "Low";
    }
    return "Normal";
}

const char* builtinSortName(BuiltinSort s)
{
    switch (s) {
    case BuiltinSort::Priority: return "Priority";
    case BuiltinSort::Name: return "Name";
    case BuiltinSort::Category: return "Category";
    case BuiltinSort::Usage: return "Use in stock maps";
    case BuiltinSort::Table: return "Builtin number";
    }
    return "Priority";
}

BuiltinPriority priorityOf(const BuiltinPriorities& priorities, std::string_view name)
{
    const auto it = priorities.find(name);
    if (it == priorities.end()) {
        return BuiltinPriority::Normal;
    }
    return static_cast<BuiltinPriority>(std::clamp(it->second, 0, 3));
}

void setPriority(BuiltinPriorities& priorities, std::string_view name, BuiltinPriority p)
{
    if (p == BuiltinPriority::Normal) {
        if (const auto it = priorities.find(name); it != priorities.end()) {
            priorities.erase(it);
        }
    } else {
        priorities[std::string(name)] = static_cast<int>(p);
    }
}

std::vector<std::uint32_t> builtinListOrder(const std::vector<BuiltinRow>& rows, const BuiltinPriorities& priorities, BuiltinSort sort,
                                            bool favouritesOnly, std::string_view query)
{
    std::vector<const BuiltinRow*> shown;
    for (const BuiltinRow& r : rows) {
        if (favouritesOnly && priorityOf(priorities, r.name) != BuiltinPriority::Favourite) continue;
        if (!query.empty() && r.searchText.find(query) == std::string::npos) continue;
        shown.push_back(&r);
    }
    const auto prio = [&](const BuiltinRow* r) { return static_cast<int>(priorityOf(priorities, r->name)); };
    std::ranges::stable_sort(shown, [&](const BuiltinRow* a, const BuiltinRow* b) {
        switch (sort) {
        case BuiltinSort::Priority:
            if (prio(a) != prio(b)) return prio(a) > prio(b);
            return nameLess(a->name, b->name);
        case BuiltinSort::Name:
            return nameLess(a->name, b->name);
        case BuiltinSort::Category:
            if (a->category != b->category) return a->category < b->category;
            if (prio(a) != prio(b)) return prio(a) > prio(b);
            return nameLess(a->name, b->name);
        case BuiltinSort::Usage:
            if (a->usageCalls != b->usageCalls) return a->usageCalls > b->usageCalls;
            return nameLess(a->name, b->name);
        case BuiltinSort::Table:
            return a->id < b->id;
        }
        return a->id < b->id;
    });
    std::vector<std::uint32_t> ids;
    ids.reserve(shown.size());
    for (const BuiltinRow* r : shown) ids.push_back(r->id);
    return ids;
}

} // namespace noxsdl::editor
