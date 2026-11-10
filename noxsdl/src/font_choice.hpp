// Which face the game's text is drawn in: the one place that picks it, by Options' Original Style
// Fonts and Use HD Images and the font files there are. No SDL (tested on its own).
//
//   roles      default.fnt's text (most of it) and large.fnt's (the menus' large text: the .wnd
//              windows drawn large, gui::Painter's large font).
//   on         the Nox-style fonts beside the program (fonts/): NoxDefault.ttf and NoxLarge.ttf, the
//              game's own letters on its pixel grid; with HD images, NoxDefaultHD.ttf and
//              NoxLargeHD.ttf, the same letters drawn smooth, where they're there. A role whose
//              files are all missing (a build without them) is drawn as off.
//   off        Caudex Bold (embedded), or the game folder's default.ttf / default.otf in its place.
//   fallback   a character the face lacks comes from the next in its chain: a Nox font's is Caudex
//              (or default.ttf), then Noto Sans; Caudex's is Noto Sans (Caudex has no Cyrillic).
#pragma once

#include <array>
#include <cstddef>

namespace noxsdl {

enum class FontRole { Default, Large };

enum class FontFace { NoxDefault, NoxLarge, NoxDefaultHD, NoxLargeHD, Caudex };
inline constexpr std::size_t kNoxFaces = 4; ///< the first four: the Nox-style files

/// Whether each Nox-style file is there and readable (by FontFace, the first four).
using NoxFontFiles = std::array<bool, kNoxFaces>;

/// The face `role` is drawn in.
FontFace chooseFace(FontRole role, bool originalStyle, bool hdImages, const NoxFontFiles& files);
/// A Nox-style face's file name in the fonts folder (nullptr for Caudex).
const char* noxFontFile(FontFace face);
inline bool isNoxFace(FontFace face) { return face != FontFace::Caudex; }

} // namespace noxsdl
