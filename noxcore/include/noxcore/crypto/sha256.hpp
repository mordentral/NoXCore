// SHA-256 (FIPS 180-4), for the keys a host makes from its clients' secrets (lobby/extended.hpp's
// clientKeyOf). Not used on any game path.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nox::crypto {

class Sha256 {
public:
    Sha256() noexcept;
    void update(std::span<const std::uint8_t> data) noexcept;
    /// The digest (the object then holds nothing useful: make another for more).
    std::array<std::uint8_t, 32> finish() noexcept;

private:
    void block(const std::uint8_t* p) noexcept;
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t used_ = 0;
    std::uint64_t bits_ = 0;
};

/// The digest of `data` at once.
std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data) noexcept;

} // namespace nox::crypto
