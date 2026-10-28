// The crypt-file CRC (see crc.hpp).
#include "noxcore/crypto/crc.hpp"

#include <array>

namespace nox::crypto {

namespace {

// Standard reflected CRC-32 table, built at compile time.
constexpr std::array<std::uint32_t, 256> makeTable()
{
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        }
        t[i] = c;
    }
    return t;
}

constexpr auto kTable = makeTable();

} // namespace

std::uint32_t updateCrc(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept
{
    for (std::uint8_t b : data) {
        crc = kTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    // Inverted on every call, so each call boundary changes the result (see crc.hpp).
    return ~crc;
}

} // namespace nox::crypto
