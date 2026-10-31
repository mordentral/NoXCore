// TrueType text for overlays (name plates now, the HUD later), drawn at screen resolution so it
// stays sharp at any zoom or window size.
//
// The font is the game folder's default.ttf or default.otf, the names OpenNox looks for when it
// swaps a TrueType font in for default.fnt, else Noto Sans compiled into the exe. Glyphs are
// rasterised on first use per pixel size into shared atlas textures.
#pragma once

#include "text_wrap.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noxsdl {

class Font {
public:
    /// default.ttf / default.otf from `gameDir` if present and valid, else the embedded font.
    static std::unique_ptr<Font> load(SDL_Renderer* renderer, const std::filesystem::path& gameDir);

    /// Throws std::runtime_error if `data` isn't a font stb_truetype can read.
    Font(SDL_Renderer* renderer, std::vector<std::uint8_t> data, std::string name);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    const std::string& name() const { return name_; }

    /// Advance width of a UTF-8 string at `pixelSize` (the em height in screen pixels).
    float measure(std::string_view utf8, int pixelSize);
    /// One glyph's advance at `pixelSize`.
    float advance(char32_t codepoint, int pixelSize);
    /// The glyphs' advances at `pixelSize`, for text_wrap.hpp's line breaking (valid while the
    /// font lives).
    GlyphAdvance advances(int pixelSize);
    /// Splits a UTF-8 string into lines no wider than `maxWidth` as the game's drawStringWrap
    /// does (text_wrap.hpp's wrapLines): at the last space, a word with no space broken one
    /// character early, and at line breaks.
    std::vector<std::string> wrap(std::string_view utf8, int pixelSize, float maxWidth);
    /// Distance from the top of a line to its baseline, and the full line height.
    float ascent(int pixelSize);
    float lineHeight(int pixelSize);
    /// The pixel size whose lines are `gamePixels` game pixels tall at `scale` screen pixels a
    /// game pixel (rounded), at least `least`: the windows' text sizes.
    int lineSize(float gamePixels, float scale, int least = 6);

    /// Draws a UTF-8 string with its line's top-left corner at (x, y), in screen pixels.
    void draw(std::string_view utf8, float x, float y, int pixelSize, SDL_Color color);

    /// The game's own fonts as this one: default.fnt and small.fnt (the same file) are 9 rows
    /// high with the capitals in the top 7, and the game's drawString puts a string's cell
    /// top-left at the point it's given, so the capitals' tops are there and the baseline is 7
    /// below. These match that at `scale` output pixels per game pixel: the pixel size whose
    /// capitals are 7 game pixels tall, a string's width in game pixels at it, and the
    /// string drawn with its capitals' tops at game pixel (gx, gy).
    int gameFontSize(float scale);
    float measureGameText(std::string_view utf8, float scale);
    void drawGameText(std::string_view utf8, float gx, float gy, float scale, SDL_Color color);

private:
    struct Glyph {
        int page = -1; ///< atlas texture, -1 for a blank glyph (space)
        SDL_FRect source{};
        float offsetX = 0; ///< bitmap corner relative to the pen on the baseline
        float offsetY = 0;
        float advance = 0;
    };
    struct Face {
        float scale = 0;
        float ascent = 0;
        float lineHeight = 0;
        std::unordered_map<char32_t, Glyph> glyphs;
    };

    Face& face(int pixelSize);
    const Glyph& glyph(Face& face, char32_t codepoint);
    void drawRun(std::string_view utf8, float x, float y, Face& face, SDL_Color color);

    SDL_Renderer* renderer_;
    std::vector<std::uint8_t> data_;
    std::string name_;
    struct Info;
    std::unique_ptr<Info> info_;
    std::map<int, Face> faces_;
    float capPerEm_ = 0; ///< the capitals' height for a 1-pixel em ('H'), found on first use

    // Atlas pages filled shelf by shelf.
    std::vector<SDL_Texture*> pages_;
    int shelfX_ = 0;
    int shelfY_ = 0;
    int shelfHeight_ = 0;
};

} // namespace noxsdl
