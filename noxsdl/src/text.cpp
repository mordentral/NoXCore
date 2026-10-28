// Font (text.hpp): stb_truetype rasterising glyphs on first use into shared atlas pages, and the
// measuring, wrapping and drawing of UTF-8 text with them.
#include "text.hpp"

#include "noxcore/io/file.hpp"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

// The built-in Noto Sans, compiled into the exe as a byte array.
extern const unsigned char kEmbeddedFont[];
extern const std::size_t kEmbeddedFontSize;

namespace noxsdl {

namespace {

constexpr int kPageSize = 1024;
constexpr int kPadding = 1; ///< between glyphs, so linear sampling never bleeds

} // namespace

struct Font::Info {
    stbtt_fontinfo font{};
};

char32_t nextCodepoint(std::string_view s, std::size_t& pos)
{
    constexpr char32_t kReplacement = 0xFFFD;
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(s[i]); };
    const std::uint8_t b0 = byte(pos++);
    if (b0 < 0x80) {
        return b0;
    }
    // The lead byte gives the sequence's length (2-4 bytes) and its top bits; each continuation
    // byte (10xxxxxx) adds 6 more.
    int more = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        more = 1;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        more = 2;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        more = 3;
        cp = b0 & 0x07;
    } else {
        return kReplacement;
    }
    for (int i = 0; i < more; ++i) {
        if (pos >= s.size() || (byte(pos) & 0xC0) != 0x80) {
            return kReplacement;
        }
        cp = (cp << 6) | (byte(pos++) & 0x3F);
    }
    return cp;
}

std::unique_ptr<Font> Font::load(SDL_Renderer* renderer, const std::filesystem::path& gameDir)
{
    for (const char* file : {"default.ttf", "default.otf"}) {
        const auto path = gameDir / file;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        try {
            return std::make_unique<Font>(renderer, nox::readFileBytes(path), path.string());
        } catch (const std::exception& ex) {
            SDL_Log("%s: %s (using the built-in font)", path.string().c_str(), ex.what());
        }
    }
    return std::make_unique<Font>(renderer, std::vector<std::uint8_t>(kEmbeddedFont, kEmbeddedFont + kEmbeddedFontSize),
                                  "Noto Sans SemiBold (built in)");
}

Font::Font(SDL_Renderer* renderer, std::vector<std::uint8_t> data, std::string name)
    : renderer_(renderer), data_(std::move(data)), name_(std::move(name)), info_(std::make_unique<Info>())
{
    const int offset = stbtt_GetFontOffsetForIndex(data_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info_->font, data_.data(), offset)) {
        throw std::runtime_error("not a TrueType/OpenType font");
    }
}

Font::~Font()
{
    for (SDL_Texture* page : pages_) {
        SDL_DestroyTexture(page);
    }
}

// A pixel size's metrics, worked out the first time it's asked for.
Font::Face& Font::face(int pixelSize)
{
    pixelSize = std::max(pixelSize, 1);
    auto [it, added] = faces_.try_emplace(pixelSize);
    Face& f = it->second;
    if (added) {
        f.scale = stbtt_ScaleForMappingEmToPixels(&info_->font, static_cast<float>(pixelSize));
        int ascent = 0;
        int descent = 0;
        int gap = 0;
        stbtt_GetFontVMetrics(&info_->font, &ascent, &descent, &gap);
        f.ascent = std::ceil(static_cast<float>(ascent) * f.scale);
        f.lineHeight = std::ceil(static_cast<float>(ascent - descent) * f.scale);
    }
    return f;
}

// A codepoint's glyph at face `f`, rasterised into the atlas the first time and kept: its
// advance, and its page and place (no page when it has no pixels or doesn't fit one).
const Font::Glyph& Font::glyph(Face& f, char32_t codepoint)
{
    if (const auto it = f.glyphs.find(codepoint); it != f.glyphs.end()) {
        return it->second;
    }
    Glyph g;
    if (codepoint < 0x20) {
        // Control characters (some strings start with one) take no room and draw nothing, as in
        // the game's fonts; the font's "missing" box would show otherwise.
        return f.glyphs.emplace(codepoint, g).first->second;
    }
    const int index = stbtt_FindGlyphIndex(&info_->font, static_cast<int>(codepoint));
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&info_->font, index, &advance, &bearing);
    g.advance = static_cast<float>(advance) * f.scale;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    // Its bitmap's box from the pen on the baseline (y0 negative: above it).
    stbtt_GetGlyphBitmapBox(&info_->font, index, f.scale, f.scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0;
    const int h = y1 - y0;
    // Into the atlas: white pixels whose alpha is the glyph's coverage, so a colour mod tints them.
    if (w > 0 && h > 0 && w + kPadding <= kPageSize && h + kPadding <= kPageSize) {
        // Next shelf, or next page, when this one is full.
        if (shelfX_ + w + kPadding > kPageSize) {
            shelfX_ = 0;
            shelfY_ += shelfHeight_;
            shelfHeight_ = 0;
        }
        if (pages_.empty() || shelfY_ + h + kPadding > kPageSize) {
            SDL_Texture* page = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kPageSize,
                                                  kPageSize);
            if (page) {
                SDL_SetTextureBlendMode(page, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(page, SDL_SCALEMODE_NEAREST); // drawn 1:1 on whole pixels
                const std::vector<std::uint8_t> clear(static_cast<std::size_t>(kPageSize) * kPageSize * 4, 0);
                SDL_UpdateTexture(page, nullptr, clear.data(), kPageSize * 4);
                pages_.push_back(page);
            }
            shelfX_ = 0;
            shelfY_ = 0;
            shelfHeight_ = 0;
        }
        if (!pages_.empty()) {
            std::vector<std::uint8_t> coverage(static_cast<std::size_t>(w) * h);
            stbtt_MakeGlyphBitmap(&info_->font, coverage.data(), w, h, w, f.scale, f.scale, index);
            std::vector<std::uint8_t> rgba(coverage.size() * 4);
            for (std::size_t i = 0; i < coverage.size(); ++i) {
                rgba[i * 4 + 0] = 0xFF;
                rgba[i * 4 + 1] = 0xFF;
                rgba[i * 4 + 2] = 0xFF;
                rgba[i * 4 + 3] = coverage[i];
            }
            const SDL_Rect dest{shelfX_, shelfY_, w, h};
            SDL_UpdateTexture(pages_.back(), &dest, rgba.data(), w * 4);
            g.page = static_cast<int>(pages_.size()) - 1;
            g.source = {static_cast<float>(shelfX_), static_cast<float>(shelfY_), static_cast<float>(w),
                        static_cast<float>(h)};
            g.offsetX = static_cast<float>(x0);
            g.offsetY = static_cast<float>(y0);
            shelfX_ += w + kPadding;
            shelfHeight_ = std::max(shelfHeight_, h + kPadding);
        }
    }
    return f.glyphs.emplace(codepoint, g).first->second;
}

float Font::measure(std::string_view utf8, int pixelSize)
{
    Face& f = face(pixelSize);
    float width = 0;
    for (std::size_t pos = 0; pos < utf8.size();) {
        width += glyph(f, nextCodepoint(utf8, pos)).advance;
    }
    return width;
}

std::vector<std::string> Font::wrap(std::string_view utf8, int pixelSize, float maxWidth)
{
    Face& f = face(pixelSize);
    std::vector<std::string> lines;
    std::string line;
    float width = 0;
    std::size_t lastSpace = std::string::npos; // in `line`
    // Glyph by glyph: a line full breaks at its last space, or mid-word when it has none.
    for (std::size_t pos = 0; pos < utf8.size();) {
        const std::size_t start = pos;
        const char32_t c = nextCodepoint(utf8, pos);
        if (c == '\n') {
            lines.push_back(std::move(line));
            line.clear();
            width = 0;
            lastSpace = std::string::npos;
            continue;
        }
        const float advance = glyph(f, c).advance;
        // Past the width: a break (a space never breaks the line itself; it marks where the next may).
        if (width + advance > maxWidth && !line.empty() && c != ' ') {
            if (lastSpace != std::string::npos) {
                // Break at the last space; the rest of the word starts the next line.
                std::string rest = line.substr(lastSpace + 1);
                line.resize(lastSpace);
                lines.push_back(std::move(line));
                line = std::move(rest);
                width = measure(line, pixelSize);
            } else {
                lines.push_back(std::move(line)); // one long word: break it anywhere
                line.clear();
                width = 0;
            }
            lastSpace = std::string::npos;
        }
        if (c == ' ') {
            lastSpace = line.size();
        }
        line.append(utf8.substr(start, pos - start));
        width += advance;
    }
    if (!line.empty() || lines.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

float Font::ascent(int pixelSize)
{
    return face(pixelSize).ascent;
}

float Font::lineHeight(int pixelSize)
{
    return face(pixelSize).lineHeight;
}

int Font::lineSize(float gamePixels, float scale, int least)
{
    return std::max(least, static_cast<int>(std::lround(gamePixels * scale / (lineHeight(100) / 100.0f))));
}

// Glyphs placed from the pen on the baseline, on whole pixels so the 1:1 atlas stays sharp.
void Font::drawRun(std::string_view utf8, float x, float y, Face& f, SDL_Color color)
{
    const float baseline = std::round(y) + f.ascent;
    float pen = std::round(x);
    for (std::size_t pos = 0; pos < utf8.size();) {
        const Glyph& g = glyph(f, nextCodepoint(utf8, pos));
        if (g.page >= 0) {
            SDL_Texture* page = pages_[static_cast<std::size_t>(g.page)];
            SDL_SetTextureColorMod(page, color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(page, color.a);
            const SDL_FRect dest{std::round(pen) + g.offsetX, baseline + g.offsetY, g.source.w, g.source.h};
            SDL_RenderTexture(renderer_, page, &g.source, &dest);
        }
        pen += g.advance;
    }
}

void Font::draw(std::string_view utf8, float x, float y, int pixelSize, SDL_Color color)
{
    drawRun(utf8, x, y, face(pixelSize), color);
}

namespace {
/// The game fonts' capitals: rows 0-6 of their 9 (default.fnt, small.fnt).
constexpr float kGameCapHeight = 7;
} // namespace

int Font::gameFontSize(float scale)
{
    if (capPerEm_ <= 0) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        const float perEm = stbtt_ScaleForMappingEmToPixels(&info_->font, 1.0f);
        capPerEm_ = stbtt_GetCodepointBox(&info_->font, 'H', &x0, &y0, &x1, &y1) && y1 > 0
                        ? static_cast<float>(y1) * perEm
                        : 0.7f; // (no 'H': about the usual)
    }
    return std::max(6, static_cast<int>(std::lround(kGameCapHeight * scale / capPerEm_)));
}

float Font::measureGameText(std::string_view utf8, float scale)
{
    return measure(utf8, gameFontSize(scale)) / scale;
}

void Font::drawGameText(std::string_view utf8, float gx, float gy, float scale, SDL_Color color)
{
    const int size = gameFontSize(scale);
    // The baseline on a whole pixel, the capitals' height below the given top.
    const float baseline = std::round((gy + kGameCapHeight) * scale);
    drawRun(utf8, gx * scale, baseline - face(size).ascent, face(size), color);
}

} // namespace noxsdl
