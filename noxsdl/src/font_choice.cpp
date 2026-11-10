// The game text's face (font_choice.hpp).
#include "font_choice.hpp"

namespace noxsdl {

FontFace chooseFace(FontRole role, bool originalStyle, bool hdImages, const NoxFontFiles& files)
{
    if (!originalStyle) {
        return FontFace::Caudex;
    }
    const bool large = role == FontRole::Large;
    const FontFace pixel = large ? FontFace::NoxLarge : FontFace::NoxDefault;
    const FontFace hd = large ? FontFace::NoxLargeHD : FontFace::NoxDefaultHD;
    const auto there = [&](FontFace f) { return files[static_cast<std::size_t>(f)]; };
    if (hdImages && there(hd)) {
        return hd;
    }
    return there(pixel) ? pixel : FontFace::Caudex;
}

const char* noxFontFile(FontFace face)
{
    switch (face) {
    case FontFace::NoxDefault:
        return "NoxDefault.ttf";
    case FontFace::NoxLarge:
        return "NoxLarge.ttf";
    case FontFace::NoxDefaultHD:
        return "NoxDefaultHD.ttf";
    case FontFace::NoxLargeHD:
        return "NoxLargeHD.ttf";
    case FontFace::Caudex:
        break;
    }
    return nullptr;
}

} // namespace noxsdl
