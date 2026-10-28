// A party member's class as a record keeps it (DelveRecord::party, TideRecord::Member::cls, the
// noxsdl records message, the online leaderboard): one byte, its class (0 warrior, 1 wizard,
// 2 conjurer, 3 multiclass: a NoX Core player that played every class's spells and abilities) and
// kRecordNoxCore set when the player was on the NoX Core client (it identified itself). A record
// from before these is a plain class, the stock client's.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace nox::server {

inline constexpr std::uint8_t kRecordMulticlass = 3;
inline constexpr std::uint8_t kRecordNoxCore = 0x80;

/// The last line of every in-game records table (Delve's and Undead Tide's boards and "?record",
/// noxsdl's list): the online leaderboard holds the rest.
inline constexpr std::string_view kFullLeaderboard = "Full leaderboard at noxhub.net";

/// The byte for a player of `classId` (0-2) that played multiclass or not, on NoX Core or not.
constexpr std::uint8_t recordClass(std::uint8_t classId, bool multiclass, bool noxCore)
{
    const std::uint8_t cls = multiclass ? kRecordMulticlass : std::min<std::uint8_t>(classId, 2);
    return static_cast<std::uint8_t>(cls | (noxCore ? kRecordNoxCore : 0));
}

/// The byte read back from a record's file: 3 multiclass, any other class kept to 0-2 (a garbled
/// number never reads as multiclass), the client's bit as it was.
constexpr std::uint8_t recordClassFromFile(int value)
{
    const int low = value & 0x7F;
    const int cls = low == kRecordMulticlass ? low : std::clamp(low, 0, 2);
    return static_cast<std::uint8_t>(cls | (value & kRecordNoxCore));
}

/// Whether it played on the NoX Core client.
constexpr bool recordNoxCore(std::uint8_t cls)
{
    return (cls & kRecordNoxCore) != 0;
}

/// "warrior", "wizard", "conjurer", "MultiClass".
constexpr std::string_view recordClassName(std::uint8_t cls)
{
    switch (std::min<int>(cls & 0x7F, kRecordMulticlass)) {
    case 1: return "wizard";
    case 2: return "conjurer";
    case 3: return "MultiClass";
    default: return "warrior";
    }
}

} // namespace nox::server
