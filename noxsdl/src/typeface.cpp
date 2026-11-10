// Typeface (typeface.hpp): a font's bytes kept for stb_truetype, its line metrics and capital height
// read once, and the fallback chain's glyph lookup and scale.
#include "typeface.hpp"

#include <stb_truetype.h>

#include <stdexcept>

namespace noxsdl {

Typeface::Typeface(std::vector<std::uint8_t> data, std::string name)
    : data_(std::move(data)), name_(std::move(name)), info_(std::make_unique<stbtt_fontinfo>())
{
    const int offset = data_.empty() ? -1 : stbtt_GetFontOffsetForIndex(data_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(info_.get(), data_.data(), offset)) {
        throw std::runtime_error("not a TrueType/OpenType font");
    }
    stbtt_GetFontVMetrics(info_.get(), &ascent_, &descent_, &lineGap_);
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    const float perEm = stbtt_ScaleForMappingEmToPixels(info_.get(), 1.0f);
    capUnits_ = stbtt_FindGlyphIndex(info_.get(), 'H') != 0 && stbtt_GetCodepointBox(info_.get(), 'H', &x0, &y0, &x1, &y1) && y1 > 0
                    ? static_cast<float>(y1)
                    : 0.7f / perEm; // (no 'H': about the usual)
}

Typeface::~Typeface() = default;

int Typeface::glyphIndex(char32_t codepoint) const
{
    return stbtt_FindGlyphIndex(info_.get(), static_cast<int>(codepoint));
}

float Typeface::scaleForEm(float pixels) const
{
    return stbtt_ScaleForMappingEmToPixels(info_.get(), pixels);
}

int Typeface::advanceUnits(int glyph) const
{
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(info_.get(), glyph, &advance, &bearing);
    return advance;
}

ChainGlyph findInChain(std::span<const Typeface* const> chain, char32_t codepoint)
{
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (const int index = chain[i]->glyphIndex(codepoint); index != 0) {
            return {i, index};
        }
    }
    return {};
}

float fallbackScale(const Typeface& main, float mainScale, const Typeface& fallback)
{
    return fallback.capUnits() > 0 ? main.capUnits() * mainScale / fallback.capUnits() : mainScale;
}

} // namespace noxsdl
