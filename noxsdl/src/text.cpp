// Font, GlyphAtlas and GameFonts (text.hpp): glyphs found along a Font's chain of faces and
// rasterised by stb_truetype on first use into each face's atlas pages, the measuring, wrapping
// and drawing of UTF-8 text with them, and the faces loaded once and handed to the Fonts.
#include "text.hpp"

#include "noxcore/io/file.hpp"

#include <stb_truetype.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <string>

// The built-in fonts, compiled into the exe as byte arrays: Noto Sans SemiBold and Caudex Bold.
extern const unsigned char kEmbeddedFont[];
extern const std::size_t kEmbeddedFontSize;
extern const unsigned char kCaudexFont[];
extern const std::size_t kCaudexFontSize;

namespace noxsdl {

namespace {

constexpr int kPageSize = 1024;
constexpr int kPadding = 1; ///< between glyphs, so linear sampling never bleeds

} // namespace

// ---------------------------------------------------------------------------------- GlyphAtlas

GlyphAtlas::~GlyphAtlas()
{
    for (SDL_Texture* page : pages_) {
        SDL_DestroyTexture(page);
    }
}

// A glyph at a scale, rasterised into the atlas the first time and kept: its page and place (no
// page when it has no pixels or doesn't fit one).
const GlyphAtlas::Raster& GlyphAtlas::get(const Typeface& face, int index, float scale)
{
    const std::uint64_t key = (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(scale)) << 32) |
                              static_cast<std::uint32_t>(index);
    if (const auto it = rasters_.find(key); it != rasters_.end()) {
        return it->second;
    }
    Raster r;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    // Its bitmap's box from the pen on the baseline (y0 negative: above it).
    stbtt_GetGlyphBitmapBox(&face.info(), index, scale, scale, &x0, &y0, &x1, &y1);
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
            stbtt_MakeGlyphBitmap(&face.info(), coverage.data(), w, h, w, scale, scale, index);
            std::vector<std::uint8_t> rgba(coverage.size() * 4);
            for (std::size_t i = 0; i < coverage.size(); ++i) {
                rgba[i * 4 + 0] = 0xFF;
                rgba[i * 4 + 1] = 0xFF;
                rgba[i * 4 + 2] = 0xFF;
                rgba[i * 4 + 3] = coverage[i];
            }
            const SDL_Rect dest{shelfX_, shelfY_, w, h};
            SDL_UpdateTexture(pages_.back(), &dest, rgba.data(), w * 4);
            r.page = pages_.back();
            r.source = {static_cast<float>(shelfX_), static_cast<float>(shelfY_), static_cast<float>(w),
                        static_cast<float>(h)};
            r.offsetX = static_cast<float>(x0);
            r.offsetY = static_cast<float>(y0);
            shelfX_ += w + kPadding;
            shelfHeight_ = std::max(shelfHeight_, h + kPadding);
        }
    }
    return rasters_.emplace(key, r).first->second;
}

// ---------------------------------------------------------------------------------------- Font

void Font::setFaces(std::vector<Link> chain, bool onGameCell)
{
    chain_ = std::move(chain);
    faces_.clear();
    for (const Link& l : chain_) {
        faces_.push_back(l.face);
    }
    onGameCell_ = onGameCell && !chain_.empty();
    renderer_ = chain_.empty() ? nullptr : chain_.front().atlas->renderer();
    sizes_.clear(); // (the glyphs found and the sizes' metrics were the old faces')
}

const std::string& Font::name() const
{
    static const std::string none;
    return chain_.empty() ? none : chain_.front().face->name();
}

// A pixel size's metrics, worked out the first time it's asked for. On the game's cell the
// baseline is the face's own (the cell's baseline row) and a line is the rows and a pixel.
Font::Face& Font::face(int pixelSize)
{
    pixelSize = std::max(pixelSize, 1);
    auto [it, added] = sizes_.try_emplace(pixelSize);
    Face& f = it->second;
    if (added && !faces_.empty()) {
        const Typeface& main = *faces_.front();
        const auto px = static_cast<float>(pixelSize);
        f.scale = main.scaleForEm(px);
        if (onGameCell_) {
            // (less a hair: 7/9 of 9 pixels is 7, not 8, whatever the float's last bit)
            f.ascent = std::ceil(static_cast<float>(main.ascent()) * f.scale - 0.01f);
            f.lineHeight = std::ceil(px * (cell_.rows + 1) / cell_.rows - 0.01f);
        } else {
            f.ascent = std::ceil(static_cast<float>(main.ascent()) * f.scale);
            f.lineHeight = std::ceil(static_cast<float>(main.ascent() - main.descent()) * f.scale);
        }
        f.scales.push_back(f.scale);
        for (std::size_t i = 1; i < faces_.size(); ++i) {
            f.scales.push_back(fallbackScale(main, f.scale, *faces_[i]));
        }
    }
    return f;
}

// A codepoint's glyph at face `f`, from the first face of the chain that has it, kept.
const Font::Glyph& Font::glyph(Face& f, char32_t codepoint)
{
    if (const auto it = f.glyphs.find(codepoint); it != f.glyphs.end()) {
        return it->second;
    }
    Glyph g;
    // Control characters (some strings start with one) take no room and draw nothing, as in the
    // game's fonts; the font's "missing" box would show otherwise.
    if (codepoint >= 0x20 && !chain_.empty()) {
        const ChainGlyph at = findInChain(faces_, codepoint);
        const Link& l = chain_[at.face];
        const float scale = f.scales[at.face];
        g.advance = static_cast<float>(l.face->advanceUnits(at.index)) * scale;
        const GlyphAtlas::Raster& r = l.atlas->get(*l.face, at.index, scale);
        g.page = r.page;
        g.source = r.source;
        g.offsetX = r.offsetX;
        g.offsetY = r.offsetY;
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

float Font::advance(char32_t codepoint, int pixelSize)
{
    return glyph(face(pixelSize), codepoint).advance;
}

GlyphAdvance Font::advances(int pixelSize)
{
    Face& f = face(pixelSize);
    return [this, &f](char32_t c) { return glyph(f, c).advance; };
}

std::vector<std::string> Font::wrap(std::string_view utf8, int pixelSize, float maxWidth)
{
    // drawStringWrap's breaks (text_wrap.hpp), at this size's widths.
    return wrapLines(utf8, maxWidth, advances(pixelSize));
}

float Font::ascent(int pixelSize)
{
    return face(pixelSize).ascent;
}

float Font::lineHeight(int pixelSize)
{
    return face(pixelSize).lineHeight;
}

float Font::linePerEm()
{
    return onGameCell_ ? (cell_.rows + 1) / cell_.rows : lineHeight(100) / 100.0f;
}

int Font::lineSize(float gamePixels, float scale, int least)
{
    return std::max(least, static_cast<int>(std::lround(gamePixels * scale / linePerEm())));
}

// Glyphs placed from the pen on the baseline, on whole pixels so the 1:1 atlas stays sharp.
void Font::drawRun(std::string_view utf8, float x, float y, Face& f, SDL_Color color)
{
    const float baseline = std::round(y) + f.ascent;
    float pen = std::round(x);
    for (std::size_t pos = 0; pos < utf8.size();) {
        const Glyph& g = glyph(f, nextCodepoint(utf8, pos));
        if (g.page) {
            SDL_SetTextureColorMod(g.page, color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(g.page, color.a);
            const SDL_FRect dest{std::round(pen) + g.offsetX, baseline + g.offsetY, g.source.w, g.source.h};
            SDL_RenderTexture(renderer_, g.page, &g.source, &dest);
        }
        pen += g.advance;
    }
}

void Font::draw(std::string_view utf8, float x, float y, int pixelSize, SDL_Color color)
{
    drawRun(utf8, x, y, face(pixelSize), color);
}

int Font::gameFontSize(float scale)
{
    // On the game's cell the capitals are the cell's (its em is the rows); else the face's 'H'.
    const float capPerEm = onGameCell_ ? cell_.capHeight / cell_.rows : faces_.empty() ? 0.7f : faces_.front()->capPerEm();
    return std::max(6, static_cast<int>(std::lround(cell_.capHeight * scale / capPerEm)));
}

float Font::measureGameText(std::string_view utf8, float scale)
{
    return measure(utf8, gameFontSize(scale)) / scale;
}

void Font::drawGameText(std::string_view utf8, float gx, float gy, float scale, SDL_Color color)
{
    const int size = gameFontSize(scale);
    // The baseline on a whole pixel, the cell's baseline row below the given top.
    const float baseline = std::round((gy + cell_.baseline) * scale);
    drawRun(utf8, gx * scale, baseline - face(size).ascent, face(size), color);
}

// ----------------------------------------------------------------------------------- GameFonts

GameFonts::GameFonts(SDL_Renderer* renderer, const std::filesystem::path& gameDir, std::filesystem::path fontDir)
    : renderer_(renderer), fontDir_(std::move(fontDir))
{
    // The plain face: the game folder's default.ttf / default.otf, the names OpenNox looks for when
    // it swaps a TrueType font in for default.fnt, else Caudex Bold.
    for (const char* file : {"default.ttf", "default.otf"}) {
        const auto path = gameDir / file;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        try {
            plain_ = make(nox::readFileBytes(path), path.string());
            SDL_Log("font: %s in place of Caudex Bold", path.string().c_str());
            break;
        } catch (const std::exception& ex) {
            SDL_Log("%s: %s (using Caudex Bold)", path.string().c_str(), ex.what());
        }
    }
    if (!plain_.face) {
        plain_ = make(std::vector<std::uint8_t>(kCaudexFont, kCaudexFont + kCaudexFontSize), "Caudex Bold (built in)");
    }
    noto_ = make(std::vector<std::uint8_t>(kEmbeddedFont, kEmbeddedFont + kEmbeddedFontSize), "Noto Sans SemiBold (built in)");
    // Which Nox-style files there are, looked for once (read when first chosen).
    std::string missing;
    for (std::size_t i = 0; i < kNoxFaces; ++i) {
        const char* file = noxFontFile(static_cast<FontFace>(i));
        std::error_code ec;
        files_[i] = std::filesystem::is_regular_file(fontDir_ / file, ec);
        if (!files_[i]) {
            missing += missing.empty() ? file : std::string(", ") + file;
        }
    }
    if (!missing.empty()) {
        SDL_Log("fonts: no %s in %s (Original Style Fonts draws in Caudex Bold where its file is missing)", missing.c_str(),
                fontDir_.string().c_str());
    }
}

GameFonts::~GameFonts() = default;

GameFonts::Loaded GameFonts::make(std::vector<std::uint8_t> data, std::string name)
{
    Loaded l;
    l.face = std::make_unique<Typeface>(std::move(data), std::move(name));
    l.atlas = std::make_unique<GlyphAtlas>(renderer_);
    return l;
}

bool GameFonts::loadNox(FontFace face)
{
    const auto i = static_cast<std::size_t>(face);
    if (nox_[i].face) {
        return true;
    }
    if (!files_[i]) {
        return false;
    }
    const auto path = fontDir_ / noxFontFile(face);
    try {
        nox_[i] = make(nox::readFileBytes(path), path.filename().string());
        return true;
    } catch (const std::exception& ex) {
        SDL_Log("%s: %s (not used)", path.string().c_str(), ex.what());
        files_[i] = false; // (not tried again)
        return false;
    }
}

void GameFonts::applyRole(Font& font, FontRole role, bool originalStyle, bool hdImages)
{
    FontFace face = chooseFace(role, originalStyle, hdImages, files_);
    while (isNoxFace(face) && !loadNox(face)) {
        face = chooseFace(role, originalStyle, hdImages, files_); // (it's marked missing now)
    }
    int& chosen = chosen_[role == FontRole::Large ? 1 : 0];
    if (chosen == static_cast<int>(face)) {
        return;
    }
    chosen = static_cast<int>(face);
    std::vector<Font::Link> chain;
    if (isNoxFace(face)) {
        chain.push_back(nox_[static_cast<std::size_t>(face)].link());
    }
    chain.push_back(plain_.link());
    chain.push_back(noto_.link());
    font.setFaces(std::move(chain), isNoxFace(face));
    SDL_Log("font: %s for %s's text", font.name().c_str(), role == FontRole::Large ? "large.fnt" : "default.fnt");
}

void GameFonts::apply(Font& standard, Font& large, bool originalStyle, bool hdImages)
{
    applyRole(standard, FontRole::Default, originalStyle, hdImages);
    applyRole(large, FontRole::Large, originalStyle, hdImages);
}

} // namespace noxsdl
