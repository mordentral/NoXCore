// (noxcpp) The pad's drawn shapes, in output pixels, crisp at any size: filled and outlined convex
// shapes (circles, ellipses, rounded boxes, lines) through SDL_RenderGeometry with a pixel's soft
// edge, and the button glyphs of pad_labels.hpp drawn with them (the HUD's prompts, A's prompt,
// the Controller screen and its picture of a pad).
#pragma once

#include "pad/pad_labels.hpp"

#include <SDL3/SDL.h>

#include <span>
#include <vector>

namespace noxsdl {
class Font;
}

namespace noxsdl::pad {

namespace shape {

/// A convex outline's points (either winding).
void fill(SDL_Renderer* r, std::span<const SDL_FPoint> points, SDL_FColor c);
/// A band `width` wide along a closed convex outline, centred on it.
void stroke(SDL_Renderer* r, std::span<const SDL_FPoint> points, float width, SDL_FColor c);
/// The outline of a circle, an ellipse (turned by `turn` radians) and a box with round corners,
/// points enough for its size; into `out` (cleared first).
void circlePoints(std::vector<SDL_FPoint>& out, float cx, float cy, float radius);
void ellipsePoints(std::vector<SDL_FPoint>& out, float cx, float cy, float rx, float ry, float turn);
void roundBoxPoints(std::vector<SDL_FPoint>& out, float x, float y, float w, float h, float radius);
/// A straight line `width` wide.
void line(SDL_Renderer* r, SDL_FPoint a, SDL_FPoint b, float width, SDL_FColor c);

} // namespace shape

/// A glyph's colours: its ink (outline, letter, the meant dot or arm), what it's filled with, and
/// the parts not meant (the other dots and arms).
struct GlyphInk {
    SDL_FColor ink{0.94f, 0.71f, 0.16f, 1};
    SDL_FColor fill{0.08f, 0.06f, 0.04f, 0.9f};
    SDL_FColor dim{0.45f, 0.40f, 0.32f, 1};
};

/// How wide `label` draws at height `h` (output pixels).
float glyphWidth(Font* font, const ButtonLabel& label, float h);
/// Draws `label` `h` tall, its left edge at x, its middle at cy; returns its width.
float drawGlyph(SDL_Renderer* r, Font* font, const ButtonLabel& label, float x, float cy, float h, const GlyphInk& ink);
/// A prompt (the modifier's glyph, a "+", the button's), as drawGlyph; 0 wide with no button.
float promptWidth(Font* font, const PromptLabels& prompt, float h);
float drawPrompt(SDL_Renderer* r, Font* font, const PromptLabels& prompt, float x, float cy, float h, const GlyphInk& ink);

} // namespace noxsdl::pad
