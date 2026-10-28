// FormatError and ByteReader, the little-endian reader every file and message parser uses.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nox {

/// Thrown when a file or message doesn't match the expected format.
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Little-endian reader over a byte buffer. Reading past the end throws FormatError.
class ByteReader {
public:
    /// `context` names the data in error messages, e.g. "WallMap".
    explicit ByteReader(std::span<const std::uint8_t> data, std::string_view context = {});

    /// The whole buffer being read.
    std::span<const std::uint8_t> data() const noexcept { return data_; }
    std::size_t offset() const noexcept { return offset_; }
    std::size_t size() const noexcept { return data_.size(); }
    std::size_t remaining() const noexcept { return data_.size() - offset_; }
    bool atEnd() const noexcept { return offset_ == data_.size(); }

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32();

    /// Next `n` bytes, without copying.
    std::span<const std::uint8_t> bytes(std::size_t n);
    /// Next `n` bytes as a string, raw (NULs kept).
    std::string rawString(std::size_t n);

    void skip(std::size_t n) { bytes(n); }
    /// `n`, a count just read, checked against the bytes left before anything is sized by it:
    /// each of the `n` items takes at least `minEach` bytes, so more than remaining() / minEach of
    /// them can't be there (a u32 count would otherwise size a vector of gigabytes). Throws.
    std::size_t count(std::uint64_t n, std::size_t minEach = 1);
    /// Skips to the next multiple of `alignment`, counted from the start of the buffer.
    void align(std::size_t alignment);

    /// Throws FormatError mentioning the context and current offset.
    [[noreturn]] void fail(std::string_view what) const;
    /// Throws unless every byte has been consumed.
    void expectEnd() const;

private:
    std::span<const std::uint8_t> data_;
    std::size_t offset_ = 0;
    std::string context_;
};

} // namespace nox
