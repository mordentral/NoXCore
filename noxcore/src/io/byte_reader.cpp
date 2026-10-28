// Bounds-checked little-endian reads for the file and message parsers (see byte_reader.hpp).
#include "noxcore/io/byte_reader.hpp"

#include <bit>
#include <cstring>

namespace nox {

ByteReader::ByteReader(std::span<const std::uint8_t> data, std::string_view context)
    : data_(data)
    , context_(context)
{
}

// Every read goes through here, so running past the end always throws with the offset.
std::span<const std::uint8_t> ByteReader::bytes(std::size_t n)
{
    if (n > remaining()) {
        fail("unexpected end of data (need " + std::to_string(n) + " bytes, have " +
             std::to_string(remaining()) + ")");
    }
    auto out = data_.subspan(offset_, n);
    offset_ += n;
    return out;
}

std::uint8_t ByteReader::u8()
{
    return bytes(1)[0];
}

std::uint16_t ByteReader::u16()
{
    auto b = bytes(2);
    return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}

std::uint32_t ByteReader::u32()
{
    auto b = bytes(4);
    return std::uint32_t{b[0]} | (std::uint32_t{b[1]} << 8) | (std::uint32_t{b[2]} << 16) |
           (std::uint32_t{b[3]} << 24);
}

std::uint64_t ByteReader::u64()
{
    const std::uint64_t lo = u32();
    const std::uint64_t hi = u32();
    return lo | (hi << 32);
}

float ByteReader::f32()
{
    return std::bit_cast<float>(u32());
}

std::string ByteReader::rawString(std::size_t n)
{
    auto b = bytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::size_t ByteReader::count(std::uint64_t n, std::size_t minEach)
{
    if (minEach != 0 && n > remaining() / minEach) {
        fail("count " + std::to_string(n) + " runs past the end of the data");
    }
    return static_cast<std::size_t>(n);
}

void ByteReader::align(std::size_t alignment)
{
    const std::size_t rem = offset_ % alignment;
    if (rem != 0) {
        skip(alignment - rem);
    }
}

void ByteReader::fail(std::string_view what) const
{
    std::string msg;
    if (!context_.empty()) {
        msg += context_;
        msg += ": ";
    }
    msg += what;
    msg += " at offset ";
    msg += std::to_string(offset_);
    throw FormatError(msg);
}

void ByteReader::expectEnd() const
{
    if (!atEnd()) {
        fail(std::to_string(remaining()) + " unread trailing bytes");
    }
}

} // namespace nox
