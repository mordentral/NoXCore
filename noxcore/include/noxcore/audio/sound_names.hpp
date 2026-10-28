// Sound ids: the numbers the protocol (MSG_AUDIO_EVENT's low 10 bits) and the client's own
// Snd_PlaySound calls use, which index the exe's sound name table ("NULL", "AnchorCast",
// ...). thing.bin's AUD and AVNT records, monster sound sets and wall sounds name them, and the
// game resolves the names with a case-insensitive binary search (utilFindSound).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace nox::audio {

/// Ids 0-1022; 0 is "NULL", which the game treats as no sound.
inline constexpr std::uint32_t kSoundCount = 1023;

/// The sound's name ("SwordHit"), or "" past the table.
std::string_view soundName(std::uint32_t id) noexcept;

/// The id for a sound name, compared case-insensitively as the game does, or nullopt. "NULL"
/// gives 0.
std::optional<std::uint32_t> soundId(std::string_view name) noexcept;

} // namespace nox::audio
