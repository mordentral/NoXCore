// Port of OpenNox libs noximage/pcx/pcx.go (MIT, Copyright (c) 2022 NoX World); see THIRD_PARTY_NOTICES.txt.
// Decodes video.bag's image formats to 8-bit RGBA; the formats are listed in sprite.hpp.
#include "noxcore/video/sprite.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/video/channels.hpp"

#include <string>

namespace nox::video {

namespace {

// Floor tiles and edges are 46 x 46 diamonds.
constexpr int kTileSize = 46;

struct Rgba {
    std::uint8_t r, g, b, a;
};

// Red in bits 10-14, green 5-9, blue 0-4.
Rgba fromRgb555(std::uint16_t c)
{
    return {expand5((c >> 10) & 31), expand5((c >> 5) & 31), expand5(c & 31), 0xFF};
}

Rgba fromRgba5551(std::uint16_t c)
{
    Rgba p = fromRgb555(c);
    p.a = (c & 0x8000) ? 0 : 0xFF;
    return p;
}

Rgba fromRgb565(std::uint16_t c)
{
    return {expand5((c >> 11) & 31), expand6((c >> 5) & 63), expand5(c & 31), 0xFF};
}

Rgba fromRgba4444(std::uint16_t c)
{
    return {expand4((c >> 12) & 15), expand4((c >> 8) & 15), expand4((c >> 4) & 15), expand4(c & 15)};
}

// Writes pixels into a DecodedImage; the image starts fully transparent.
class Canvas {
public:
    explicit Canvas(DecodedImage& img)
        : img_(img)
    {
        img_.rgba.assign(static_cast<std::size_t>(img_.width) * img_.height * 4, 0);
    }

    void set(int x, int y, Rgba p)
    {
        auto* px = &img_.rgba[(static_cast<std::size_t>(y) * img_.width + x) * 4];
        px[0] = p.r;
        px[1] = p.g;
        px[2] = p.b;
        px[3] = p.a;
    }

    // Material slots are stored + 1 so that 0 means a plain-colour pixel.
    void setMaterial(int x, int y, std::uint8_t slot)
    {
        if (img_.material.empty()) {
            img_.material.assign(static_cast<std::size_t>(img_.width) * img_.height, 0);
        }
        img_.material[static_cast<std::size_t>(y) * img_.width + x] = static_cast<std::uint8_t>(slot + 1);
    }

private:
    DecodedImage& img_;
};

// The sprite header: i32 width, height, offsetX, offsetY, then one byte (17 bytes).
void readSpriteHeader(ByteReader& r, DecodedImage& img)
{
    img.width = r.i32();
    img.height = r.i32();
    img.offsetX = r.i32();
    img.offsetY = r.i32();
    img.unknown = r.u8();
    // 0x0 sprites exist as placeholders ("NULL0001", "junk").
    if (img.width < 0 || img.width > 1024 || img.height < 0 || img.height > 1024) {
        r.fail("invalid image size " + std::to_string(img.width) + "x" + std::to_string(img.height));
    }
}

/// Row y of the tile diamond: first x and width (initSight).
struct DiamondRow {
    int x;
    int width;
};

// Rows widen by 2 each down to the middle (1, 3, .. 45), then the same rows again in reverse.
DiamondRow diamondRow(int y)
{
    if (y < kTileSize / 2) {
        return {kTileSize / 2 - y, 1 + 2 * y};
    }
    return {y - kTileSize / 2 + 1, 1 + 2 * (kTileSize - 1 - y)};
}

// Type 0: the diamond's pixels row by row, RGB555, nothing between rows.
void decodeTile(ByteReader& r, DecodedImage& img)
{
    img.width = kTileSize;
    img.height = kTileSize;
    Canvas c(img);
    for (int y = 0; y < kTileSize; ++y) {
        const DiamondRow row = diamondRow(y);
        for (int x = 0; x < row.width; ++x) {
            c.set(row.x + x, y, fromRgb555(r.u16()));
        }
    }
}

// Type 1: runs over rows first..last of the diamond (see sprite.hpp); also fills edgeMask.
void decodeEdge(ByteReader& r, DecodedImage& img)
{
    img.width = kTileSize;
    img.height = kTileSize;
    Canvas c(img);
    img.edgeMask.assign(static_cast<std::size_t>(kTileSize) * kTileSize, 0);
    const int first = r.u8();
    const int last = r.u8();
    if (last >= kTileSize) {
        r.fail("edge rows out of range");
    }
    for (int y = first; y <= last; ++y) {
        const DiamondRow row = diamondRow(y);
        for (int x = 0; x < row.width;) {
            const std::uint8_t op = r.u8();
            const int count = r.u8();
            if (x + count > row.width) {
                r.fail("edge run crosses the end of a row");
            }
            for (int i = 0; i < count; ++i) {
                const int px = row.x + x + i;
                auto& mask = img.edgeMask[static_cast<std::size_t>(y) * kTileSize + px];
                switch (op) {
                case 1:
                    break;
                case 2:
                    c.set(px, y, fromRgb555(r.u16()));
                    mask = 1;
                    break;
                case 3:
                    mask = 2;
                    break;
                default:
                    r.fail("invalid edge op " + std::to_string(op));
                }
            }
            x += count;
        }
    }
}

// Types 2 and 7: every pixel, RGBA5551, after the sprite header.
void decodeFull(ByteReader& r, DecodedImage& img)
{
    readSpriteHeader(r, img);
    Canvas c(img);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            c.set(x, y, fromRgba5551(r.u16()));
        }
    }
}

// Types 3-6 and 8: per row, { op, count } runs until the row is covered; ops in sprite.hpp.
void decodeRuns(ByteReader& r, DecodedImage& img)
{
    readSpriteHeader(r, img);
    Canvas c(img);
    const bool rgb565 = img.type == 8;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width;) {
            const std::uint8_t op = r.u8();
            const int count = r.u8();
            if (x + count > img.width) {
                r.fail("run crosses the end of a row");
            }
            switch (op & 0x0F) {
            case 1: // transparent
            case 6: // no data
                break;
            case 2:
            case 7:
                for (int i = 0; i < count; ++i) {
                    const std::uint16_t v = r.u16();
                    c.set(x + i, y, rgb565 ? fromRgb565(v) : fromRgb555(v));
                }
                break;
            case 4:
                if (rgb565) {
                    r.fail("material run in an RGB565 sprite");
                }
                for (int i = 0; i < count; ++i) {
                    const std::uint8_t v = r.u8();
                    c.set(x + i, y, {v, v, v, 0xFF});
                    // The high nibble of the op picks the material slot.
                    c.setMaterial(x + i, y, static_cast<std::uint8_t>(op >> 4));
                }
                break;
            case 5:
                if (rgb565) {
                    r.fail("RGBA4444 run in an RGB565 sprite");
                }
                for (int i = 0; i < count; ++i) {
                    c.set(x + i, y, fromRgba4444(r.u16()));
                }
                break;
            default:
                r.fail("invalid run op " + std::to_string(op));
            }
            x += count;
        }
    }
}

} // namespace

DecodedImage decodeImage(std::span<const std::uint8_t> data, std::uint8_t type)
{
    ByteReader r(data, "image");
    DecodedImage img;
    img.type = type;
    switch (type) {
    case 0:
        decodeTile(r, img);
        break;
    case 1:
        decodeEdge(r, img);
        break;
    case 2:
    case 7:
        decodeFull(r, img);
        break;
    case 3:
    case 4:
    case 5:
    case 6:
    case 8:
        decodeRuns(r, img);
        break;
    default:
        throw FormatError("unsupported image type " + std::to_string(type));
    }
    img.unusedBytes = r.remaining();
    return img;
}

} // namespace nox::video
