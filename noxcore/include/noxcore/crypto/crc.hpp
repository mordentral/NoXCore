// The running CRC GAME.EXE keeps over crypt-file reads and writes (the map header CRC).
#pragma once

#include <cstdint>
#include <span>

namespace nox::crypto {

inline constexpr std::uint32_t kCrcInit = 0xFFFFFFFF;

/// One step of the CRC that GAME.EXE keeps while reading or writing a crypt file
/// (called for every cryptfile_ReadWrite): the standard reflected CRC-32 table
/// run over `data` from `crc` without an initial inversion, then inverted.
///
/// Every call counts, even with no data (which just inverts), so the result depends on how a
/// file's fields were split into calls, not only on its bytes.
std::uint32_t updateCrc(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept;

} // namespace nox::crypto
