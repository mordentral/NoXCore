// The Script Reference's Builtins list without ImGui (so the tests can use it): each builtin's
// priority (the user's favourites, kept in the editor settings), the list's sort orders and its
// filter.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl::editor {

/// A builtin's priority, set from its context menu. Normal is every builtin's unless the user
/// set another; only the others are saved.
enum class BuiltinPriority : int { Low = 0, Normal = 1, High = 2, Favourite = 3 };

/// The list's orders. Priority (the default): favourites first, then high, normal, low, each
/// by name.
enum class BuiltinSort : int { Priority = 0, Name = 1, Category = 2, Usage = 3, Table = 4 };
inline constexpr int kBuiltinSortCount = 5;

const char* builtinPriorityName(BuiltinPriority p);
const char* builtinSortName(BuiltinSort s);

/// The user's priorities, by builtin name (absent: Normal).
using BuiltinPriorities = std::map<std::string, int, std::less<>>;

BuiltinPriority priorityOf(const BuiltinPriorities& priorities, std::string_view name);
/// Sets one (Normal takes it out of the map).
void setPriority(BuiltinPriorities& priorities, std::string_view name, BuiltinPriority p);

/// One row's facts for sorting and filtering.
struct BuiltinRow {
    std::uint32_t id = 0;
    std::string name;
    std::string category;
    int usageCalls = 0;
    std::string searchText; ///< lower case: the prototype, the short text, the category
};

/// The rows to show, in order: those matching `query` (lower case, "" for all) and, with
/// `favouritesOnly`, only the favourites.
std::vector<std::uint32_t> builtinListOrder(const std::vector<BuiltinRow>& rows, const BuiltinPriorities& priorities, BuiltinSort sort,
                                            bool favouritesOnly, std::string_view query);

} // namespace noxsdl::editor
