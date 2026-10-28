// PNG encoder (see png.hpp).
#include "noxcore/io/png.hpp"

#include "noxcore/crypto/crc.hpp"

#include <algorithm>

namespace nox {

namespace {

void be32(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    for (int i = 3; i >= 0; --i) {
        v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
    }
}

} // namespace

std::vector<std::uint8_t> encodePng(int width, int height, std::span<const std::uint8_t> rgba)
{
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 4 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        const auto* row = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(width) * 4);
    }

    // zlib stream of stored blocks (at most 65535 bytes each), then Adler-32.
    // 0x78 0x01: zlib header, deflate with a 32K window and no preset dictionary.
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::size_t off = 0;
    do {
        const std::size_t n = std::min<std::size_t>(0xFFFF, raw.size() - off);
        // Stored block header: final-block flag, then LEN and its one's complement NLEN (little-endian).
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
                 raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
    } while (off < raw.size());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const auto c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& body) {
        be32(png, static_cast<std::uint32_t>(body.size()));
        std::vector<std::uint8_t> typed(type, type + 4);
        typed.insert(typed.end(), body.begin(), body.end());
        png.insert(png.end(), typed.begin(), typed.end());
        // The chunk CRC covers the type and body; PNG's CRC-32 is the same function as the crypt CRC.
        be32(png, crypto::updateCrc(crypto::kCrcInit, typed));
    };
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, static_cast<std::uint32_t>(width));
    be32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no interlace
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

} // namespace nox
