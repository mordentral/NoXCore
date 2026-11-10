// TrueType text for overlays, the HUD and the menus, drawn at screen resolution so it stays sharp at
// any zoom or window size.
//
// A Font stands for one of the game's fonts (default.fnt, or large.fnt for the menus' large text)
// and draws in a chain of faces (typeface.hpp): its main face, then the ones a character it lacks
// is taken from, scaled so their capitals match the main face's. GameFonts loads the faces once
// and puts the chain font_choice.hpp picks into each Font in place, so whoever holds a Font keeps
// it when Options' Original Style Fonts or Use HD Images change. Glyphs are rasterised on first use
// into atlas pages kept per face (GlyphAtlas); a Font keeps only which glyph each character is at
// each size, dropped when its faces change.
#pragma once

#include "font_choice.hpp"
#include "text_wrap.hpp"
#include "typeface.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noxsdl {

/// One face's glyphs rasterised into shared atlas textures, shelf by shelf, each glyph at each
/// scale the first time it's asked for and kept.
class GlyphAtlas {
public:
    explicit GlyphAtlas(SDL_Renderer* renderer) : renderer_(renderer) {}
    ~GlyphAtlas();
    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    struct Raster {
        SDL_Texture* page = nullptr; ///< none for a blank glyph (space) or one too big for a page
        SDL_FRect source{};
        float offsetX = 0; ///< bitmap corner relative to the pen on the baseline
        float offsetY = 0;
    };
    /// Glyph `index` of `face` at `scale` (font units to pixels).
    const Raster& get(const Typeface& face, int index, float scale);
    SDL_Renderer* renderer() const { return renderer_; }

private:
    SDL_Renderer* renderer_;
    std::unordered_map<std::uint64_t, Raster> rasters_;
    std::vector<SDL_Texture*> pages_;
    int shelfX_ = 0;
    int shelfY_ = 0;
    int shelfHeight_ = 0;
};

class Font {
public:
    /// The game font a Font stands for, in its pixels: its rows, the baseline's row (the game's
    /// drawString puts a string's cell top-left at the point it's given) and the capitals' height.
    /// default.fnt (and small.fnt, the same file): 9 rows, capitals rows 0-6, baseline at 7.
    /// large.fnt: 15 rows, capitals rows 1-11, baseline at 12.
    struct GameCell {
        float rows;
        float baseline;
        float capHeight;
    };
    static constexpr GameCell kDefaultCell{9, 7, 7};
    static constexpr GameCell kLargeCell{15, 12, 11};

    /// A face of the chain and the atlas its glyphs go in (both GameFonts').
    struct Link {
        const Typeface* face = nullptr;
        GlyphAtlas* atlas = nullptr;
    };

    explicit Font(GameCell cell = kDefaultCell) : cell_(cell) {}
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    /// The faces it draws in, the first the main one. `onGameCell`: the main face is drawn on the
    /// game font's own cell (a Nox-style font: its em the cell's rows, a line a pixel more), not
    /// sized by its own line height.
    void setFaces(std::vector<Link> chain, bool onGameCell);
    bool onGameCell() const { return onGameCell_; }
    /// The main face's name ("" before setFaces).
    const std::string& name() const;

    /// Advance width of a UTF-8 string at `pixelSize` (the em height in screen pixels).
    float measure(std::string_view utf8, int pixelSize);
    /// One glyph's advance at `pixelSize`.
    float advance(char32_t codepoint, int pixelSize);
    /// The glyphs' advances at `pixelSize`, for text_wrap.hpp's line breaking (valid while the
    /// font lives and its faces stay).
    GlyphAdvance advances(int pixelSize);
    /// Splits a UTF-8 string into lines no wider than `maxWidth` as the game's drawStringWrap
    /// does (text_wrap.hpp's wrapLines): at the last space, a word with no space broken one
    /// character early, and at line breaks.
    std::vector<std::string> wrap(std::string_view utf8, int pixelSize, float maxWidth);
    /// Distance from the top of a line to its baseline, and the full line height (on the game's
    /// cell: the rows and a pixel, as the game spaces its lines).
    float ascent(int pixelSize);
    float lineHeight(int pixelSize);
    /// The pixel size whose lines are `gamePixels` game pixels tall at `scale` screen pixels a
    /// game pixel (rounded), at least `least`: the windows' text sizes. On the game's cell a
    /// default.fnt line (10) is its 9 rows at `scale`, a large.fnt one (16) its 15.
    int lineSize(float gamePixels, float scale, int least = 6);

    /// Draws a UTF-8 string with its line's top-left corner at (x, y), in screen pixels.
    void draw(std::string_view utf8, float x, float y, int pixelSize, SDL_Color color);

    /// The game font's own placement (GameCell): at `scale` output pixels per game pixel, the
    /// pixel size whose capitals are the game font's capitals' height (on the game's cell, its
    /// rows at `scale`), a string's width in game pixels at it, and the string drawn with its
    /// cell's top-left at game pixel (gx, gy), the baseline the cell's baseline row below.
    int gameFontSize(float scale);
    float measureGameText(std::string_view utf8, float scale);
    void drawGameText(std::string_view utf8, float gx, float gy, float scale, SDL_Color color);

private:
    struct Glyph {
        SDL_Texture* page = nullptr; ///< none for a blank glyph (space)
        SDL_FRect source{};
        float offsetX = 0; ///< bitmap corner relative to the pen on the baseline
        float offsetY = 0;
        float advance = 0;
    };
    struct Face {
        float scale = 0;
        float ascent = 0;
        float lineHeight = 0;
        std::vector<float> scales; ///< each face of the chain's scale, the fallbacks' capitals matched
        std::unordered_map<char32_t, Glyph> glyphs;
    };

    Face& face(int pixelSize);
    const Glyph& glyph(Face& face, char32_t codepoint);
    void drawRun(std::string_view utf8, float x, float y, Face& face, SDL_Color color);
    /// A line's height for a 1-pixel em (lineSize): on the game's cell, exact.
    float linePerEm();

    GameCell cell_;
    SDL_Renderer* renderer_ = nullptr; ///< the atlases' (drawRun)
    std::vector<Link> chain_;
    std::vector<const Typeface*> faces_; ///< chain_'s faces (findInChain)
    bool onGameCell_ = false;
    std::map<int, Face> sizes_;
};

/// The game text's faces, each loaded once on first use and kept with its atlas: the Nox-style
/// fonts from `fontDir` (font_choice.hpp), Caudex Bold (or the game folder's default.ttf /
/// default.otf in its place) and Noto Sans, those two compiled in.
class GameFonts {
public:
    GameFonts(SDL_Renderer* renderer, const std::filesystem::path& gameDir, std::filesystem::path fontDir);
    ~GameFonts();
    GameFonts(const GameFonts&) = delete;
    GameFonts& operator=(const GameFonts&) = delete;

    /// default.fnt's and large.fnt's Fonts set to the faces font_choice.hpp picks (a role whose
    /// face is unchanged left as it is).
    void apply(Font& standard, Font& large, bool originalStyle, bool hdImages);

private:
    struct Loaded {
        std::unique_ptr<Typeface> face;
        std::unique_ptr<GlyphAtlas> atlas;
        Font::Link link() const { return {face.get(), atlas.get()}; }
    };
    Loaded make(std::vector<std::uint8_t> data, std::string name);
    /// A Nox-style face loaded the first time it's chosen; one that won't load is marked missing.
    bool loadNox(FontFace face);
    void applyRole(Font& font, FontRole role, bool originalStyle, bool hdImages);

    SDL_Renderer* renderer_;
    std::filesystem::path fontDir_;
    NoxFontFiles files_{};                ///< the Nox-style files there (and, once tried, readable)
    std::array<Loaded, kNoxFaces> nox_{}; ///< by FontFace
    Loaded plain_;                        ///< Caudex Bold, or the game folder's default.ttf
    Loaded noto_;
    std::array<int, 2> chosen_{-1, -1};   ///< by FontRole: the FontFace each Font has (-1 none yet)
};

} // namespace noxsdl
