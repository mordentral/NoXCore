// ByteWriter, the little-endian writer the file encoders use (with the game's CRC when asked).
#pragma once

#include "noxcore/crypto/crc.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace nox {

/// Little-endian writer appending to a growable byte buffer.
///
/// Optionally tracks GAME.EXE's crypt-file CRC (crypto::updateCrc): each write call below is
/// one CRC step, mirroring one cryptfile_ReadWrite call. Padding and the `uncounted*`
/// calls mirror the game's lower-level writes, which the CRC doesn't see.
class ByteWriter {
public:
    ByteWriter() = default;
    /// Appending to `into` (what it holds kept, and its room: take() hands it back).
    explicit ByteWriter(std::vector<std::uint8_t> into) noexcept : buf_(std::move(into)) {}

    std::size_t size() const noexcept { return buf_.size(); }
    const std::vector<std::uint8_t>& data() const& noexcept { return buf_; }
    std::vector<std::uint8_t> take() && noexcept { return std::move(buf_); }

    /// Starts CRC tracking from `init`.
    void trackCrc(std::uint32_t init = crypto::kCrcInit)
    {
        crcOn_ = true;
        crc_ = init;
    }
    std::uint32_t crc() const noexcept { return crc_; }

    void u8(std::uint8_t v) { put(std::array{v}); }
    void u16(std::uint16_t v) { put(le<2>(v)); }
    void u32(std::uint32_t v) { put(le<4>(v)); }
    void u64(std::uint64_t v) { put(le<8>(v)); }
    void i8(std::int8_t v) { u8(static_cast<std::uint8_t>(v)); }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }

    /// One call; an empty span still counts as a (CRC-inverting) call.
    void bytes(std::span<const std::uint8_t> b) { put(b); }
    void rawString(std::string_view s)
    {
        put({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
    }

    /// Not seen by the CRC.
    void uncountedBytes(std::span<const std::uint8_t> b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
    void uncountedU32(std::uint32_t v) { uncountedBytes(le<4>(v)); }
    void uncountedU64(std::uint64_t v) { uncountedBytes(le<8>(v)); }
    void zeros(std::size_t n) { buf_.insert(buf_.end(), n, 0); }
    /// Zero-pads to the next multiple of `alignment`, counted from the start of the buffer.
    void align(std::size_t alignment) { zeros((alignment - buf_.size() % alignment) % alignment); }

    /// Overwrites 4 bytes at `offset`, which must already have been written (not counted).
    void patchU32(std::size_t offset, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            buf_.at(offset + i) = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }

private:
    template <std::size_t N, class T>
    static std::array<std::uint8_t, N> le(T v)
    {
        std::array<std::uint8_t, N> b{};
        for (std::size_t i = 0; i < N; ++i) {
            b[i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(v) >> (8 * i));
        }
        return b;
    }

    void put(std::span<const std::uint8_t> b)
    {
        buf_.insert(buf_.end(), b.begin(), b.end());
        if (crcOn_) {
            crc_ = crypto::updateCrc(crc_, b);
        }
    }

    std::vector<std::uint8_t> buf_;
    bool crcOn_ = false;
    std::uint32_t crc_ = crypto::kCrcInit;
};

} // namespace nox
