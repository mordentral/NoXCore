// The bitmap fonts and default.pal (font_palette.hpp).
#include "content/font_palette.hpp"

#include <cstring>
#include <format>
#include <stdexcept>

namespace videotools::content {

namespace {

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t at = 0;
    void need(std::size_t n) const
    {
        if (at + n > data.size()) {
            throw std::runtime_error(std::format("cut short at {} (needs {} more bytes)", at, at + n - data.size()));
        }
    }
    std::uint32_t u32()
    {
        need(4);
        const std::uint32_t v = data[at] | data[at + 1] << 8 | data[at + 2] << 16 | std::uint32_t(data[at + 3]) << 24;
        at += 4;
        return v;
    }
    std::uint16_t u16()
    {
        need(2);
        const auto v = static_cast<std::uint16_t>(data[at] | data[at + 1] << 8);
        at += 2;
        return v;
    }
};

} // namespace

BitmapFont BitmapFont::parse(std::span<const std::uint8_t> file)
{
    BitmapFont f;
    Reader r{file};
    std::uint32_t rangeCount = 0;
    if (file.size() >= 4 && std::memcmp(file.data(), "tNoF", 4) == 0) {
        f.tagged = true;
        r.at = 4;
        r.u32(); // (a version the game never reads)
        f.height = r.u32();
        f.stride = r.u32();
        f.rows = r.u32();
        rangeCount = r.u32();
        f.glyphSize = r.u32();
        if (rangeCount > 4096) {
            throw std::runtime_error("implausible range count");
        }
        for (std::uint32_t i = 0; i < rangeCount; ++i) {
            FontRange g;
            g.base = r.u32();
            g.first = r.u16();
            g.last = r.u16();
            f.ranges.push_back(g);
        }
    } else {
        // The older layout: u16 height, stride, rows, range count, eight range slots, u32 glyph size.
        f.height = r.u16();
        f.stride = r.u16();
        f.rows = r.u16();
        rangeCount = r.u16();
        if (rangeCount > 8) {
            throw std::runtime_error("not a font (more than 8 ranges in the old layout)");
        }
        for (std::uint32_t i = 0; i < 8; ++i) {
            FontRange g;
            g.base = r.u32();
            g.first = r.u16();
            g.last = r.u16();
            if (i < rangeCount) {
                f.ranges.push_back(g);
            }
        }
        f.glyphSize = r.u32();
    }
    if (f.glyphSize != 1 + f.stride * f.rows || f.stride == 0 || f.rows == 0) {
        throw std::runtime_error(std::format("glyph size {} isn't 1 + {} x {}", f.glyphSize, f.stride, f.rows));
    }
    std::size_t count = 0;
    for (const auto& g : f.ranges) {
        if (g.last < g.first) {
            throw std::runtime_error("a range ends before it starts");
        }
        count += std::size_t(g.last - g.first) + 1;
    }
    r.need(count * f.glyphSize);
    f.glyphs.assign(file.begin() + std::ptrdiff_t(r.at), file.begin() + std::ptrdiff_t(r.at + count * f.glyphSize));
    return f;
}

std::optional<std::size_t> BitmapFont::glyphOf(std::uint16_t ch) const
{
    for (const auto& g : ranges) {
        if (g.first <= ch && ch <= g.last) {
            const std::size_t i = std::size_t(ch - g.first) + g.base;
            return i < glyphCount() ? std::optional<std::size_t>(i) : std::nullopt;
        }
    }
    return std::nullopt;
}

int BitmapFont::width(std::size_t glyph) const
{
    return glyph < glyphCount() ? glyphs[glyph * glyphSize] : 0;
}

bool BitmapFont::pixel(std::size_t glyph, int x, int y) const
{
    if (glyph >= glyphCount() || x < 0 || y < 0 || x >= width(glyph) || std::uint32_t(y) >= rows || std::uint32_t(x / 8) >= stride) {
        return false;
    }
    const std::uint8_t b = glyphs[glyph * glyphSize + 1 + std::size_t(y) * stride + std::size_t(x / 8)];
    return (b >> (7 - x % 8)) & 1;
}

Palette Palette::parse(std::span<const std::uint8_t> file)
{
    Palette p;
    std::size_t at = 0;
    const auto take = [&](std::size_t n) {
        if (at + n > file.size()) {
            throw std::runtime_error(std::format("{} cut short", p.sections.empty() ? std::string("the file") : p.sections.back()));
        }
        std::vector<std::uint8_t> out(file.begin() + std::ptrdiff_t(at), file.begin() + std::ptrdiff_t(at + n));
        at += n;
        return out;
    };
    struct Section {
        const char* tag;
        std::size_t size;
    };
    static constexpr Section kSections[] = {{"PALETTE", 768}, {"LOOKUPTABLE", 32768}, {"SHADEMAP", 8192}, {"TRANSMAP", 65536}};
    while (at < file.size()) {
        const Section* s = nullptr;
        for (const auto& k : kSections) {
            const std::size_t n = std::strlen(k.tag);
            if (at + n <= file.size() && std::memcmp(file.data() + at, k.tag, n) == 0) {
                s = &k;
                break;
            }
        }
        if (!s) {
            throw std::runtime_error(std::format("an unknown section at {}", at));
        }
        at += std::strlen(s->tag);
        p.sections.emplace_back(s->tag);
        auto bytes = take(s->size);
        if (s->size == 768) {
            for (std::size_t i = 0; i < 256; ++i) {
                p.colours[i] = {bytes[i * 3], bytes[i * 3 + 1], bytes[i * 3 + 2]};
            }
        } else if (s->size == 32768) {
            p.lookup = std::move(bytes);
        } else if (s->size == 8192) {
            p.shade = std::move(bytes);
        } else {
            p.blend = std::move(bytes);
        }
    }
    if (p.sections.empty() || p.sections.front() != "PALETTE") {
        throw std::runtime_error("no PALETTE section first");
    }
    return p;
}

} // namespace videotools::content
