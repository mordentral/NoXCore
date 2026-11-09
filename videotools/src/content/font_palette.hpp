// default.pal is tagged sections, each tag's letters without an end: PALETTE (256 RGB colours, 8
// bits each), LOOKUPTABLE (a 15-bit RGB -> palette index table), SHADEMAP (each colour at 32 light
// levels) and TRANSMAP (the colour half way between any two). The game reads it only in its 8-bit
// mode.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace videotools::content {

struct Palette {
    std::array<std::array<std::uint8_t, 3>, 256> colours{};
    std::vector<std::uint8_t> lookup;   ///< 32768 entries: (r5 << 10 | g5 << 5 | b5) -> index
    std::vector<std::uint8_t> shade;    ///< 256 x 32: [colour * 32 + level]
    std::vector<std::uint8_t> blend;    ///< 256 x 256: [a * 256 + b]
    std::vector<std::string> sections;  ///< the tags in file order

    /// Reads default.pal. Throws std::runtime_error.
    static Palette parse(std::span<const std::uint8_t> file);
};

} // namespace videotools::content
