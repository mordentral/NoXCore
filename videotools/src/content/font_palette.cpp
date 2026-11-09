// The default.pal reader (font_palette.hpp).
#include "content/font_palette.hpp"

#include <cstring>
#include <format>
#include <stdexcept>

namespace videotools::content {

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
