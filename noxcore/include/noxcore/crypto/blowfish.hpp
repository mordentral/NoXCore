// Blowfish block cipher, the encryption on Nox's .map, .plr and .bin files.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nox::crypto {

/// Standard Blowfish (64-bit blocks, big-endian halves), used by Nox in ECB mode.
class Blowfish {
public:
    static constexpr std::size_t kBlockSize = 8;
    static constexpr std::size_t kMaxKeySize = 56;

    /// Expands a 1..56 byte key. Throws std::invalid_argument for any other length.
    explicit Blowfish(std::span<const std::uint8_t> key);

    void encryptBlock(std::span<std::uint8_t, kBlockSize> block) const noexcept;
    void decryptBlock(std::span<std::uint8_t, kBlockSize> block) const noexcept;

    /// ECB in place. Throws std::invalid_argument if the size isn't a multiple of kBlockSize.
    void encrypt(std::span<std::uint8_t> data) const;
    void decrypt(std::span<std::uint8_t> data) const;

private:
    std::uint32_t f(std::uint32_t x) const noexcept;
    void encryptWords(std::uint32_t& l, std::uint32_t& r) const noexcept;
    void decryptWords(std::uint32_t& l, std::uint32_t& r) const noexcept;

    std::array<std::uint32_t, 18> p_;
    std::array<std::array<std::uint32_t, 256>, 4> s_;
};

} // namespace nox::crypto
