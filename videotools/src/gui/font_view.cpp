// The font (.fnt) and palette (default.pal) viewers in the record editor (panels.hpp).
#include "gui/panels.hpp"

#include "editor/theme_colours.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/strings/string_table.hpp"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace fs = std::filesystem;

namespace videotools::gui {

namespace {

/// `text` in `font` as white pixels on clear (each glyph then a pixel's gap, as the game advances).
std::vector<std::uint8_t> drawText(const content::BitmapFont& font, std::u16string_view text, int& w, int& h)
{
    w = 0;
    for (const char16_t c : text) {
        if (const auto g = font.glyphOf(c)) {
            w += font.width(*g) + 1;
        }
    }
    w = std::max(w, 1);
    h = int(std::max<std::uint32_t>(font.rows, 1));
    std::vector<std::uint8_t> rgba(std::size_t(w) * std::size_t(h) * 4, 0);
    int x0 = 0;
    for (const char16_t c : text) {
        const auto g = font.glyphOf(c);
        if (!g) {
            continue;
        }
        for (int y = 0; y < int(font.rows); ++y) {
            for (int x = 0; x < font.width(*g); ++x) {
                if (font.pixel(*g, x, y)) {
                    std::uint8_t* p = &rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x0 + x)) * 4];
                    p[0] = p[1] = p[2] = p[3] = 255;
                }
            }
        }
        x0 += font.width(*g) + 1;
    }
    return rgba;
}

} // namespace

void RecordPanel::dropViewerTextures()
{
    for (SDL_Texture** t : {&fontSheet_, &fontSample_, &shadeTex_, &blendTex_}) {
        destroyTexture(*t);
        *t = nullptr;
    }
}

bool RecordPanel::fontForm(const content::Record& r)
{
    if (r.ref.list != content::RecordList::File || (r.ref.kind != content::FileKind::Fonts && r.ref.kind != content::FileKind::Palette)) {
        return false;
    }
    const fs::path* file = recordFile(ws_.files, r.ref);
    if (!file) {
        return false;
    }
    const auto& theme = noxsdl::editor::themeColours();
    SDL_Renderer* renderer = SDL_GetRenderer(ws_.session.window());
    const float scale = ws_.session.scale();
    if (viewerFile_ != *file) {
        viewerFile_ = *file;
        viewerError_.clear();
        font_.reset();
        palette_.reset();
        dropViewerTextures();
        sampleShown_.clear();
        try {
            const auto bytes = nox::readFileBytes(*file);
            if (r.ref.kind == content::FileKind::Fonts) {
                font_ = content::BitmapFont::parse(bytes);
            } else {
                palette_ = content::Palette::parse(bytes);
            }
        } catch (const std::exception& ex) {
            viewerError_ = std::string("Can't read it: ") + ex.what();
        }
    }
    ImGui::TextDisabled("%s", utf8(*file).c_str());
    if (!viewerError_.empty()) {
        ImGui::TextColored(theme.error, "%s", viewerError_.c_str());
        return true;
    }
    if (font_) {
        const content::BitmapFont& f = *font_;
        ImGui::TextWrapped("The original game's bitmap font: %zu glyphs, %u pixels high (lines %u apart), %s layout. NoX Core draws its own "
                           "text with a TrueType font; these are what GAME.EXE shows.",
                           f.glyphCount(), f.rows, f.height, f.tagged ? "the \"tNoF\"" : "the older");
        std::string ranges;
        for (const auto& g : f.ranges) {
            ranges += std::format("{}{:#06x}-{:#06x}", ranges.empty() ? "" : ", ", g.first, g.last);
        }
        ImGui::TextDisabled("Characters: %s", ranges.c_str());
        // Every glyph, 16 a row, each in a cell of the widest glyph's width.
        if (!fontSheet_) {
            int cellW = 1;
            for (std::size_t i = 0; i < f.glyphCount(); ++i) {
                cellW = std::max(cellW, f.width(i));
            }
            cellW += 2;
            const int cellH = int(f.rows) + 2;
            const int cols = 16;
            const int rowsN = int((f.glyphCount() + cols - 1) / cols);
            const int w = cols * cellW, h = std::max(1, rowsN * cellH);
            std::vector<std::uint8_t> rgba(std::size_t(w) * std::size_t(h) * 4, 0);
            for (std::size_t i = 0; i < f.glyphCount(); ++i) {
                const int ox = int(i % cols) * cellW + 1, oy = int(i / cols) * cellH + 1;
                for (int y = 0; y < int(f.rows); ++y) {
                    for (int x = 0; x < f.width(i); ++x) {
                        if (f.pixel(i, x, y)) {
                            std::uint8_t* p = &rgba[(std::size_t(oy + y) * std::size_t(w) + std::size_t(ox + x)) * 4];
                            p[0] = p[1] = p[2] = p[3] = 255;
                        }
                    }
                }
            }
            fontSheet_ = makeTexture(renderer, rgba, w, h);
            SDL_SetTextureScaleMode(fontSheet_, SDL_SCALEMODE_NEAREST);
            fontSheetW_ = w;
            fontSheetH_ = h;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##sample", "Type to see it in this font", sampleText_, sizeof sampleText_);
        if (sampleShown_ != sampleText_) {
            sampleShown_ = sampleText_;
            destroyTexture(fontSample_);
            fontSample_ = nullptr;
            if (!sampleShown_.empty()) {
                int w = 0, h = 0;
                const auto rgba = drawText(f, nox::strings::fromUtf8(sampleShown_), w, h);
                fontSample_ = makeTexture(renderer, rgba, w, h);
                SDL_SetTextureScaleMode(fontSample_, SDL_SCALEMODE_NEAREST);
                fontSampleW_ = w;
                fontSampleH_ = h;
            }
        }
        const float k = 3 * scale;
        if (fontSample_) {
            ImGui::Image(texRef(fontSample_), ImVec2(fontSampleW_ * k, fontSampleH_ * k));
        }
        ImGui::Separator();
        ImGui::BeginChild("sheet", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::Image(texRef(fontSheet_), ImVec2(fontSheetW_ * k, fontSheetH_ * k));
        ImGui::EndChild();
        return true;
    }
    const content::Palette& p = *palette_;
    ImGui::TextWrapped("The original game's 256 colours and its tables for them (%s). GAME.EXE reads them only in its 8-bit mode; "
                       "NoX Core and the 16-bit game don't use them.",
                       [&] {
                           std::string s;
                           for (const auto& t : p.sections) s += (s.empty() ? "" : ", ") + t;
                           return s;
                       }().c_str());
    // The colours, 16 a row; hover for the numbers.
    const float cell = 20 * scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    for (int i = 0; i < 256; ++i) {
        const auto& c = p.colours[std::size_t(i)];
        const ImVec2 a(at.x + float(i % 16) * cell, at.y + float(i / 16) * cell);
        dl->AddRectFilled(a, ImVec2(a.x + cell - 1, a.y + cell - 1), IM_COL32(c[0], c[1], c[2], 255));
    }
    ImGui::InvisibleButton("colours", ImVec2(16 * cell, 16 * cell));
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetMousePos();
        const int i = int((m.y - at.y) / cell) * 16 + int((m.x - at.x) / cell);
        if (i >= 0 && i < 256) {
            const auto& c = p.colours[std::size_t(i)];
            ImGui::SetTooltip("%d: %d %d %d (#%02X%02X%02X)", i, c[0], c[1], c[2], c[0], c[1], c[2]);
        }
    }
    // The shade map (each colour down, its 32 light levels across) and the blend table.
    if (!shadeTex_ && p.shade.size() == 8192) {
        std::vector<std::uint8_t> rgba(32 * 256 * 4);
        for (int colour = 0; colour < 256; ++colour) {
            for (int level = 0; level < 32; ++level) {
                const auto& c = p.colours[p.shade[std::size_t(colour * 32 + level)]];
                std::uint8_t* d = &rgba[(std::size_t(colour) * 32 + std::size_t(level)) * 4];
                d[0] = c[0], d[1] = c[1], d[2] = c[2], d[3] = 255;
            }
        }
        shadeTex_ = makeTexture(renderer, rgba, 32, 256);
        SDL_SetTextureScaleMode(shadeTex_, SDL_SCALEMODE_NEAREST);
    }
    if (!blendTex_ && p.blend.size() == 65536) {
        std::vector<std::uint8_t> rgba(256 * 256 * 4);
        for (std::size_t i = 0; i < 65536; ++i) {
            const auto& c = p.colours[p.blend[i]];
            rgba[i * 4] = c[0], rgba[i * 4 + 1] = c[1], rgba[i * 4 + 2] = c[2], rgba[i * 4 + 3] = 255;
        }
        blendTex_ = makeTexture(renderer, rgba, 256, 256);
        SDL_SetTextureScaleMode(blendTex_, SDL_SCALEMODE_NEAREST);
    }
    if (shadeTex_ || blendTex_) {
        ImGui::SeparatorText("Shade map (32 light levels a colour) and blend table (each pair's mix)");
        if (shadeTex_) {
            ImGui::Image(texRef(shadeTex_), ImVec2(64 * scale, 256 * scale));
            ImGui::SameLine();
        }
        if (blendTex_) {
            ImGui::Image(texRef(blendTex_), ImVec2(256 * scale, 256 * scale));
        }
    }
    return true;
}

} // namespace videotools::gui
