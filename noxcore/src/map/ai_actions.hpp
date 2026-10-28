// The monster AI action table used to decode ActionData stacks in MonsterXfer/NPCXfer records.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nox::map::detail {

struct AiAction {
    std::string_view name;
    std::uint8_t argCount; // at most 2
    std::uint8_t argTypes[2];
};

inline constexpr std::size_t kAiActionCount = 72;
extern const AiAction kAiActions[kAiActionCount];

/// Bytes an action argument of this type occupies: 0 = location (8), 7 = byte (1),
/// 1..6 = 4; anything else is invalid (0).
constexpr std::size_t aiArgSize(std::uint8_t type)
{
    switch (type) {
    case 0:
        return 8;
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
        return 4;
    case 7:
        return 1;
    default:
        return 0;
    }
}

/// Index of the named action, or -1. The game matches names exactly.
inline int findAiAction(std::string_view name)
{
    for (std::size_t i = 0; i < kAiActionCount; ++i) {
        if (!kAiActions[i].name.empty() && kAiActions[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace nox::map::detail
