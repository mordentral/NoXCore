// The game's bitmap fonts (*.fnt), read as GAME.EXE's loaders read them.
//
// A font is glyphs of one height: each a width byte, then `rows` rows of `stride` bytes, one bit a
// pixel (the high bit first), set bits drawn in the text's colour. Ranges of character codes map
// to runs of glyphs. Two layouts: "tNoF" (default, small, large: a 28-byte header of u32s) and the
// older one without a magic (number.fnt: u16s and eight range slots).
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nox::video {

struct FontRange {
    std::uint32_t base = 0; ///< the first glyph's number
    std::uint16_t first = 0; ///< character codes first..last
    std::uint16_t last = 0;
};

struct BitmapFont {
    bool tagged = false; ///< "tNoF", else the older layout
    std::uint32_t height = 0; ///< the line's height
    std::uint32_t stride = 0; ///< bytes a glyph row
    std::uint32_t rows = 0;   ///< rows a glyph
    std::uint32_t glyphSize = 0;
    std::vector<FontRange> ranges;
    std::vector<std::uint8_t> glyphs; ///< glyphSize bytes each

    /// Reads a .fnt. Throws std::runtime_error.
    static BitmapFont parse(std::span<const std::uint8_t> file);
    std::size_t glyphCount() const { return glyphSize ? glyphs.size() / glyphSize : 0; }
    /// The glyph for a character code (as the game looks it up), or nullopt.
    std::optional<std::size_t> glyphOf(std::uint16_t ch) const;
    /// A glyph's width in pixels.
    int width(std::size_t glyph) const;
    bool pixel(std::size_t glyph, int x, int y) const;
};

} // namespace nox::video
