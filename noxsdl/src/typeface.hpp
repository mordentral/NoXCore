// One TrueType font read by stb_truetype: its metrics and its glyphs' lookup, with no SDL, so the
// fallback chain (a character the main face lacks taken from the next) and the choice of faces
// (font_choice.hpp) can be tested on their own. text.hpp rasterises and draws with them.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct stbtt_fontinfo;

namespace noxsdl {

class Typeface {
public:
    /// Throws std::runtime_error if `data` isn't a font stb_truetype can read.
    Typeface(std::vector<std::uint8_t> data, std::string name);
    ~Typeface();
    Typeface(const Typeface&) = delete;
    Typeface& operator=(const Typeface&) = delete;

    const std::string& name() const { return name_; }
    const stbtt_fontinfo& info() const { return *info_; }

    /// The glyph for a character, 0 (the "missing" glyph) when the font has none.
    int glyphIndex(char32_t codepoint) const;
    /// Font units to pixels for an em `pixels` tall.
    float scaleForEm(float pixels) const;
    /// The line's metrics (hhea), in font units: ascent up (positive), descent down (negative).
    int ascent() const { return ascent_; }
    int descent() const { return descent_; }
    int lineGap() const { return lineGap_; }
    /// The capitals' height in font units ('H''s top), 70% of the em without an 'H'.
    float capUnits() const { return capUnits_; }
    /// The capitals' height for a 1-pixel em.
    float capPerEm() const { return capUnits_ * scaleForEm(1.0f); }
    /// A glyph's advance in font units.
    int advanceUnits(int glyph) const;

private:
    std::vector<std::uint8_t> data_;
    std::string name_;
    std::unique_ptr<stbtt_fontinfo> info_;
    int ascent_ = 0;
    int descent_ = 0;
    int lineGap_ = 0;
    float capUnits_ = 0;
};

/// Where a character's glyph comes from in a chain of faces: the first face that has it, else the
/// first face's own "missing" glyph (index 0).
struct ChainGlyph {
    std::size_t face = 0;
    int index = 0;
};
ChainGlyph findInChain(std::span<const Typeface* const> chain, char32_t codepoint);

/// The scale (font units to pixels) a fallback face is drawn at beside `main` drawn at
/// `mainScale`: its capitals as tall as the main face's.
float fallbackScale(const Typeface& main, float mainScale, const Typeface& fallback);

} // namespace noxsdl
