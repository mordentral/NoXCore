// Encodes images into video.bag's formats; the layouts are in sprite.hpp, the choices in
// sprite_encode.hpp.
#include "noxcore/video/sprite_encode.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/video/channels.hpp"

#include <array>
#include <cstdlib>
#include <string>

namespace nox::video {

namespace {

constexpr int kTileSize = 46;
constexpr int kMaxSide = 1024;
// The longest run the shipped sprites have (their encoder's limit; the count byte would take 255).
constexpr int kMaxRun = 253;

// Nearest level of `bits` bits for each 8-bit value, by the widening the decoders use.
template <unsigned Bits>
constexpr std::array<std::uint8_t, 256> nearestTable()
{
    std::array<std::uint8_t, 256> t{};
    for (int c = 0; c < 256; ++c) {
        int best = 0;
        int bestDist = 1 << 20;
        for (unsigned v = 0; v < (1u << Bits); ++v) {
            const int wide = Bits == 5 ? expand5(v) : Bits == 6 ? expand6(v) : expand4(v);
            const int d = wide > c ? wide - c : c - wide;
            if (d < bestDist) {
                bestDist = d;
                best = int(v);
            }
        }
        t[std::size_t(c)] = std::uint8_t(best);
    }
    return t;
}

constexpr auto kNearest5 = nearestTable<5>();
constexpr auto kNearest6 = nearestTable<6>();
constexpr auto kNearest4 = nearestTable<4>();

struct DiamondRow {
    int x;
    int width;
};

// As sprite.cpp: rows widen by 2 down to the middle, then the same rows again in reverse.
DiamondRow diamondRow(int y)
{
    if (y < kTileSize / 2) {
        return {kTileSize / 2 - y, 1 + 2 * y};
    }
    return {y - kTileSize / 2 + 1, 1 + 2 * (kTileSize - 1 - y)};
}

const std::uint8_t* pixel(const DecodedImage& img, int x, int y)
{
    return &img.rgba[(std::size_t(y) * std::size_t(img.width) + std::size_t(x)) * 4];
}

std::uint16_t rgb555(const std::uint8_t* p)
{
    return std::uint16_t((kNearest5[p[0]] << 10) | (kNearest5[p[1]] << 5) | kNearest5[p[2]]);
}

std::uint16_t rgb565(const std::uint8_t* p)
{
    return std::uint16_t((kNearest5[p[0]] << 11) | (kNearest6[p[1]] << 5) | kNearest5[p[2]]);
}

std::uint16_t rgba4444(const std::uint8_t* p)
{
    return std::uint16_t((kNearest4[p[0]] << 12) | (kNearest4[p[1]] << 8) | (kNearest4[p[2]] << 4) | kNearest4[p[3]]);
}

bool exact5(std::uint8_t c) { return expand5(kNearest5[c]) == c; }
bool exact4(std::uint8_t c) { return expand4(kNearest4[c]) == c; }

void writeSpriteHeader(ByteWriter& w, const DecodedImage& img)
{
    w.i32(img.width);
    w.i32(img.height);
    w.i32(img.offsetX);
    w.i32(img.offsetY);
    w.u8(img.unknown);
}

void checkPlanes(const DecodedImage& img)
{
    const std::size_t n = std::size_t(img.width) * std::size_t(img.height);
    if (img.rgba.size() != n * 4) {
        throw FormatError("image: pixels don't match its size");
    }
    if (!img.material.empty() && img.material.size() != n) {
        throw FormatError("image: material plane doesn't match its size");
    }
    if (!img.edgeMask.empty() && img.edgeMask.size() != n) {
        throw FormatError("image: edge mask doesn't match its size");
    }
}

void encodeTile(ByteWriter& w, const DecodedImage& img)
{
    for (int y = 0; y < kTileSize; ++y) {
        const DiamondRow row = diamondRow(y);
        for (int x = 0; x < row.width; ++x) {
            w.u16(rgb555(pixel(img, row.x + x, y)));
        }
    }
}

void encodeEdge(ByteWriter& w, const DecodedImage& img)
{
    // Per pixel: 0 leave, 1 the edge's colour, 2 the floor blended in.
    const auto code = [&](int x, int y) -> int {
        if (!img.edgeMask.empty()) {
            return img.edgeMask[std::size_t(y) * kTileSize + std::size_t(x)];
        }
        return pixel(img, x, y)[3] >= 128 ? 1 : 0;
    };
    int first = -1;
    int last = -1;
    for (int y = 0; y < kTileSize; ++y) {
        const DiamondRow row = diamondRow(y);
        for (int x = 0; x < row.width; ++x) {
            if (code(row.x + x, y) != 0) {
                first = first < 0 ? y : first;
                last = y;
                break;
            }
        }
    }
    if (first < 0) {
        first = last = 0; // nothing to draw: one row left alone
    }
    w.u8(std::uint8_t(first));
    w.u8(std::uint8_t(last));
    for (int y = first; y <= last; ++y) {
        const DiamondRow row = diamondRow(y);
        for (int x = 0; x < row.width;) {
            const int c = code(row.x + x, y);
            if (c > 2) {
                throw FormatError("tile edge: mask code " + std::to_string(c) + " (0-2 only)");
            }
            int n = 1;
            while (x + n < row.width && code(row.x + x + n, y) == c) {
                ++n;
            }
            w.u8(std::uint8_t(c + 1));
            w.u8(std::uint8_t(n));
            if (c == 1) {
                for (int i = 0; i < n; ++i) {
                    w.u16(rgb555(pixel(img, row.x + x + i, y)));
                }
            }
            x += n;
        }
    }
}

void encodeFull(ByteWriter& w, const DecodedImage& img)
{
    writeSpriteHeader(w, img);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const auto* p = pixel(img, x, y);
            w.u16(std::uint16_t(rgb555(p) | (p[3] < 128 ? 0x8000 : 0)));
        }
    }
}

// The run kind (op, high nibble included) a pixel goes in.
std::uint8_t runOp(const DecodedImage& img, int x, int y, bool is565)
{
    const auto* p = pixel(img, x, y);
    if (is565) {
        return p[3] < 128 ? 1 : 2;
    }
    if (p[3] == 0) {
        // Transparent; a colour kept under it (op 5 data with alpha 0) stays when RGBA4444 holds it.
        const bool colour = p[0] != 0 || p[1] != 0 || p[2] != 0;
        return colour && exact4(p[0]) && exact4(p[1]) && exact4(p[2]) ? 5 : 1;
    }
    if (!img.material.empty()) {
        const std::uint8_t m = img.material[std::size_t(y) * std::size_t(img.width) + std::size_t(x)];
        if (m != 0) {
            if (m > 16) {
                throw FormatError("sprite: material slot " + std::to_string(m - 1) + " (0-15 only)");
            }
            return std::uint8_t(4 | ((m - 1) << 4));
        }
    }
    if (p[3] < 255) {
        return 5;
    }
    // Opaque: RGB555 unless only RGBA4444 holds the colour exactly.
    const bool in555 = exact5(p[0]) && exact5(p[1]) && exact5(p[2]);
    const bool in4444 = exact4(p[0]) && exact4(p[1]) && exact4(p[2]);
    return !in555 && in4444 ? 5 : 2;
}

std::uint8_t grey(const std::uint8_t* p)
{
    if (p[0] == p[1] && p[1] == p[2]) {
        return p[0];
    }
    return std::uint8_t((p[0] * 299 + p[1] * 587 + p[2] * 114 + 500) / 1000);
}

void encodeRuns(ByteWriter& w, const DecodedImage& img)
{
    writeSpriteHeader(w, img);
    const bool is565 = img.type == 8;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width;) {
            const std::uint8_t op = runOp(img, x, y, is565);
            int n = 1;
            while (x + n < img.width && n < kMaxRun && runOp(img, x + n, y, is565) == op) {
                ++n;
            }
            w.u8(op);
            w.u8(std::uint8_t(n));
            for (int i = 0; i < n; ++i) {
                const auto* p = pixel(img, x + i, y);
                switch (op & 0x0F) {
                case 1:
                    break;
                case 2:
                    w.u16(is565 ? rgb565(p) : rgb555(p));
                    break;
                case 4:
                    w.u8(grey(p));
                    break;
                case 5:
                    w.u16(rgba4444(p));
                    break;
                }
            }
            x += n;
        }
    }
    // The data ends with an empty run (op 0, count 0), as every shipped sprite does.
    w.u8(0);
    w.u8(0);
}

} // namespace

std::uint8_t nearest5(std::uint8_t v) { return kNearest5[v]; }
std::uint8_t nearest6(std::uint8_t v) { return kNearest6[v]; }
std::uint8_t nearest4(std::uint8_t v) { return kNearest4[v]; }

bool canEncodeImage(std::uint8_t type)
{
    return type <= 8;
}

std::vector<std::uint8_t> encodeImage(const DecodedImage& img)
{
    if (!canEncodeImage(img.type)) {
        throw FormatError("can't encode image type " + std::to_string(img.type));
    }
    if (img.width < 0 || img.height < 0 || img.width > kMaxSide || img.height > kMaxSide) {
        throw FormatError("image size " + std::to_string(img.width) + "x" + std::to_string(img.height) +
                          " out of range (0-1024)");
    }
    if ((img.type == 0 || img.type == 1) && (img.width != kTileSize || img.height != kTileSize)) {
        throw FormatError("floor tiles and edges are 46x46, not " + std::to_string(img.width) + "x" +
                          std::to_string(img.height));
    }
    checkPlanes(img);
    ByteWriter w;
    switch (img.type) {
    case 0:
        encodeTile(w, img);
        break;
    case 1:
        encodeEdge(w, img);
        break;
    case 2:
    case 7:
        encodeFull(w, img);
        break;
    default:
        encodeRuns(w, img);
        break;
    }
    return std::move(w).take();
}

} // namespace nox::video
