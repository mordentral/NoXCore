// Dear ImGui on noxsdl's SDL renderer, with the editor's themes; the floating panels' windows
// drawn with renderers of their own (imgui_host.hpp).
#include "editor/imgui_host.hpp"
#include "editor/panel_textures.hpp"
#include "editor/theme_colours.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/io/png.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h> // (DestroyPlatformWindow, for closeFloating)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <vector>

extern const unsigned char kEmbeddedFont[]; // Noto Sans SemiBold (embedded_font.cpp)
extern const std::size_t kEmbeddedFontSize;

namespace noxsdl::editor {

namespace {

/// The host whose context is current (one: the editor's), for ImGui's renderer hooks.
ImGuiHost* g_host = nullptr;
/// imgui_impl_sdl3's Platform_ShowWindow, wrapped (a hidden run's windows stay hidden).
void (*g_showWindow)(ImGuiViewport*) = nullptr;
bool g_hiddenRun = false;
int g_quietUntilFrame = 0;

/// An extra window's renderer.
struct ViewportRenderer {
    SDL_Renderer* renderer = nullptr;
};

SDL_Renderer* rendererOf(ImGuiViewport* viewport)
{
    const auto* d = static_cast<ViewportRenderer*>(viewport->RendererUserData);
    return d ? d->renderer : nullptr;
}

/// Makes the extra renderers' copies of textures: SDL textures, static, alpha-blended.
class SdlUploader final : public TextureUploader {
public:
    std::uintptr_t create(std::uintptr_t renderer, const TexturePixels& px) override
    {
        auto* r = reinterpret_cast<SDL_Renderer*>(renderer);
        const SDL_PixelFormat format = px.bytesPerPixel == 4 && px.format != 0 ? static_cast<SDL_PixelFormat>(px.format) : SDL_PIXELFORMAT_RGBA32;
        SDL_Texture* t = SDL_CreateTexture(r, format, SDL_TEXTUREACCESS_STATIC, px.width, px.height);
        if (!t) {
            SDL_Log("map editor: a floating panel's copy of a %dx%d texture can't be made: %s", px.width, px.height, SDL_GetError());
            return 0;
        }
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        upload(t, px, TexelRect{0, 0, px.width, px.height});
        return reinterpret_cast<std::uintptr_t>(t);
    }
    bool update(std::uintptr_t, std::uintptr_t texture, const TexturePixels& px, const TexelRect& rect) override
    {
        return upload(reinterpret_cast<SDL_Texture*>(texture), px, rect);
    }
    void destroy(std::uintptr_t, std::uintptr_t texture) override { SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(texture)); }

private:
    bool upload(SDL_Texture* t, const TexturePixels& px, const TexelRect& rect)
    {
        const SDL_Rect r{rect.x, rect.y, rect.w, rect.h};
        if (px.bytesPerPixel == 4) return SDL_UpdateTexture(t, &r, px.at(rect.x, rect.y), px.pitch);
        // (An alpha-only texture: white with its alpha, as the RGBA atlas would be.)
        expanded_.resize(static_cast<std::size_t>(rect.w) * static_cast<std::size_t>(rect.h) * 4);
        for (int y = 0; y < rect.h; ++y) {
            const std::uint8_t* a = px.at(rect.x, rect.y + y);
            std::uint8_t* o = expanded_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rect.w) * 4;
            for (int x = 0; x < rect.w; ++x, o += 4) {
                o[0] = o[1] = o[2] = 255;
                o[3] = a[x];
            }
        }
        return SDL_UpdateTexture(t, &r, expanded_.data(), rect.w * 4);
    }
    std::vector<std::uint8_t> expanded_;
};

void rendererCreateWindow(ImGuiViewport* viewport)
{
    if (g_host) g_host->createViewportRenderer(viewport);
}

void rendererDestroyWindow(ImGuiViewport* viewport)
{
    if (g_host) {
        g_host->destroyViewportRenderer(viewport);
    } else if (auto* d = static_cast<ViewportRenderer*>(viewport->RendererUserData)) {
        if (d->renderer) SDL_DestroyRenderer(d->renderer);
        delete d;
    }
    viewport->RendererUserData = nullptr;
}

void rendererRenderWindow(ImGuiViewport* viewport, void*)
{
    if (g_host) g_host->renderViewport(viewport);
}

void rendererSwapBuffers(ImGuiViewport* viewport, void*)
{
    if (g_host) g_host->presentViewport(viewport);
}

void showWindow(ImGuiViewport* viewport)
{
    if (g_hiddenRun) {
        return; // (a --screenshot run: made and drawn, never shown)
    }
    if (ImGui::GetFrameCount() < g_quietUntilFrame) {
        viewport->Flags |= ImGuiViewportFlags_NoFocusOnAppearing; // (coming back after a play-test: the editor keeps the focus)
    }
    if (g_showWindow) g_showWindow(viewport);
}

/// The draw data moved to its window's own coordinates: with multi-viewports ImGui draws in the
/// desktop's, each viewport's DisplayPos its window's top left; imgui_impl_sdlrenderer3 passes
/// positions to SDL as they are (it shifts only the clip rectangles), so they're shifted here,
/// once, in place (ImGui makes them anew each frame).
void toWindowCoordinates(ImDrawData* data)
{
    const ImVec2 off = data->DisplayPos;
    if (off.x == 0.0f && off.y == 0.0f) return;
    for (ImDrawList* list : data->CmdLists) {
        for (ImDrawVert& v : list->VtxBuffer) {
            v.pos.x -= off.x;
            v.pos.y -= off.y;
        }
        for (ImDrawCmd& cmd : list->CmdBuffer) {
            cmd.ClipRect.x -= off.x;
            cmd.ClipRect.y -= off.y;
            cmd.ClipRect.z -= off.x;
            cmd.ClipRect.w -= off.y;
        }
    }
    data->DisplayPos = ImVec2(0.0f, 0.0f);
}

SDL_Window* sdlWindowOf(const ImGuiViewport* viewport)
{
    return viewport->PlatformHandle ? SDL_GetWindowFromID(static_cast<SDL_WindowID>(reinterpret_cast<std::intptr_t>(viewport->PlatformHandle))) : nullptr;
}

ImVec4 rgb(int r, int g, int b, float a = 1.0f)
{
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
}

ImU32 rgbU32(int r, int g, int b, int a = 255)
{
    return IM_COL32(r, g, b, a);
}

/// Near-black panels, light grey text, a blue accent for what's selected and active, and an
/// orange for highlights: a modern 3D editor's dark look, in our own values.
void darkColors(ImGuiStyle& s)
{
    ImVec4* c = s.Colors;
    const ImVec4 blue = rgb(0, 112, 224);       // #0070E0
    const ImVec4 blueLight = rgb(30, 139, 255); // #1E8BFF
    const ImVec4 orange = rgb(255, 165, 0);     // #FFA500
    const ImVec4 border = rgb(10, 10, 10);      // #0A0A0A
    const ImVec4 line = rgb(52, 52, 52);        // #343434
    c[ImGuiCol_Text] = rgb(212, 212, 212);
    c[ImGuiCol_TextDisabled] = rgb(112, 112, 112);
    c[ImGuiCol_WindowBg] = rgb(30, 30, 30);
    c[ImGuiCol_ChildBg] = rgb(26, 26, 26);
    c[ImGuiCol_PopupBg] = rgb(36, 36, 36, 0.99f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = rgb(44, 44, 44);
    c[ImGuiCol_FrameBgHovered] = rgb(54, 54, 54);
    c[ImGuiCol_FrameBgActive] = rgb(62, 62, 62);
    c[ImGuiCol_TitleBg] = rgb(15, 15, 15);
    c[ImGuiCol_TitleBgActive] = rgb(20, 20, 20);
    c[ImGuiCol_TitleBgCollapsed] = rgb(15, 15, 15);
    c[ImGuiCol_MenuBarBg] = rgb(15, 15, 15);
    c[ImGuiCol_ScrollbarBg] = rgb(23, 23, 23);
    c[ImGuiCol_ScrollbarGrab] = rgb(58, 58, 58);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(74, 74, 74);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(90, 90, 90);
    c[ImGuiCol_CheckMark] = blueLight;
    c[ImGuiCol_CheckboxSelectedBg] = rgb(44, 44, 44); // (a ticked box: the blue tick on the field's grey)
    c[ImGuiCol_SliderGrab] = blue;
    c[ImGuiCol_SliderGrabActive] = blueLight;
    c[ImGuiCol_Button] = rgb(51, 51, 51);
    c[ImGuiCol_ButtonHovered] = rgb(64, 64, 64);
    c[ImGuiCol_ButtonActive] = blue;
    c[ImGuiCol_Header] = rgb(0, 96, 192);                  // (selected rows: blue)
    c[ImGuiCol_HeaderHovered] = rgb(30, 139, 255, 0.45f);
    c[ImGuiCol_HeaderActive] = blue;                       // (also the toolbar's active tool)
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = blue;
    c[ImGuiCol_SeparatorActive] = blueLight;
    c[ImGuiCol_ResizeGrip] = rgb(255, 255, 255, 0.05f);
    c[ImGuiCol_ResizeGripHovered] = rgb(0, 112, 224, 0.67f);
    c[ImGuiCol_ResizeGripActive] = blueLight;
    c[ImGuiCol_InputTextCursor] = rgb(224, 224, 224);
    c[ImGuiCol_Tab] = rgb(24, 24, 24);
    c[ImGuiCol_TabHovered] = rgb(46, 46, 46);
    c[ImGuiCol_TabSelected] = rgb(30, 30, 30);             // (the panel's own colour: the tab runs into it)
    c[ImGuiCol_TabSelectedOverline] = blueLight;
    c[ImGuiCol_TabDimmed] = rgb(18, 18, 18);
    c[ImGuiCol_TabDimmedSelected] = rgb(28, 28, 28);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0, 96, 192, 0.70f); // (a panel without the focus: its tab's line dimmer)
    c[ImGuiCol_DockingPreview] = rgb(0, 112, 224, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = rgb(21, 21, 21);
    c[ImGuiCol_PlotLines] = rgb(154, 154, 154);
    c[ImGuiCol_PlotLinesHovered] = orange;
    c[ImGuiCol_PlotHistogram] = blue;
    c[ImGuiCol_PlotHistogramHovered] = orange;
    c[ImGuiCol_TableHeaderBg] = rgb(38, 38, 38);
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = rgb(42, 42, 42);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
    c[ImGuiCol_TextLink] = rgb(61, 155, 255);
    c[ImGuiCol_TextSelectedBg] = rgb(0, 112, 224, 0.45f);
    c[ImGuiCol_TreeLines] = rgb(58, 58, 58);
    c[ImGuiCol_DragDropTarget] = orange;
    c[ImGuiCol_DragDropTargetBg] = rgb(255, 165, 0, 0.12f);
    c[ImGuiCol_UnsavedMarker] = orange;
    c[ImGuiCol_NavCursor] = blueLight;
    c[ImGuiCol_NavWindowingHighlight] = rgb(255, 255, 255, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0, 0, 0, 0.50f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.60f);
}

/// The game's stone greys and gold, on dark panels (the editor's first dark theme).
void mediumColors(ImGuiStyle& s)
{
    ImVec4* c = s.Colors;
    const ImVec4 gold = rgb(214, 168, 64);
    const ImVec4 goldDim = rgb(150, 112, 40);
    c[ImGuiCol_Text] = rgb(226, 218, 200);
    c[ImGuiCol_TextDisabled] = rgb(128, 122, 110);
    c[ImGuiCol_WindowBg] = rgb(28, 26, 24);
    c[ImGuiCol_ChildBg] = rgb(24, 22, 20);
    c[ImGuiCol_PopupBg] = rgb(32, 30, 27, 0.98f);
    c[ImGuiCol_Border] = rgb(70, 62, 50);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = rgb(46, 42, 38);
    c[ImGuiCol_FrameBgHovered] = rgb(62, 56, 48);
    c[ImGuiCol_FrameBgActive] = rgb(78, 68, 54);
    c[ImGuiCol_TitleBg] = rgb(22, 20, 18);
    c[ImGuiCol_TitleBgActive] = rgb(46, 38, 26);
    c[ImGuiCol_TitleBgCollapsed] = rgb(22, 20, 18);
    c[ImGuiCol_MenuBarBg] = rgb(34, 31, 28);
    c[ImGuiCol_ScrollbarBg] = rgb(22, 20, 18);
    c[ImGuiCol_ScrollbarGrab] = rgb(70, 64, 56);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(92, 84, 72);
    c[ImGuiCol_ScrollbarGrabActive] = goldDim;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = goldDim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_Button] = rgb(56, 50, 44);
    c[ImGuiCol_ButtonHovered] = rgb(84, 72, 54);
    c[ImGuiCol_ButtonActive] = rgb(112, 90, 50);
    c[ImGuiCol_Header] = rgb(70, 58, 38);
    c[ImGuiCol_HeaderHovered] = rgb(92, 74, 44);
    c[ImGuiCol_HeaderActive] = rgb(120, 94, 48);
    c[ImGuiCol_Separator] = rgb(70, 62, 50);
    c[ImGuiCol_SeparatorHovered] = goldDim;
    c[ImGuiCol_SeparatorActive] = gold;
    c[ImGuiCol_ResizeGrip] = rgb(90, 76, 52, 0.4f);
    c[ImGuiCol_ResizeGripHovered] = goldDim;
    c[ImGuiCol_ResizeGripActive] = gold;
    c[ImGuiCol_Tab] = rgb(40, 36, 32);
    c[ImGuiCol_TabHovered] = rgb(96, 78, 48);
    c[ImGuiCol_TabSelected] = rgb(70, 58, 38);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_TabDimmed] = rgb(32, 29, 26);
    c[ImGuiCol_TabDimmedSelected] = rgb(52, 44, 32);
    c[ImGuiCol_TabDimmedSelectedOverline] = goldDim;
    c[ImGuiCol_DockingPreview] = rgb(214, 168, 64, 0.6f);
    c[ImGuiCol_DockingEmptyBg] = rgb(18, 17, 16);
    c[ImGuiCol_PlotLines] = goldDim;
    c[ImGuiCol_PlotHistogram] = goldDim;
    c[ImGuiCol_TableHeaderBg] = rgb(40, 36, 32);
    c[ImGuiCol_TableBorderStrong] = rgb(70, 62, 50);
    c[ImGuiCol_TableBorderLight] = rgb(52, 47, 40);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.03f);
    c[ImGuiCol_TextSelectedBg] = rgb(214, 168, 64, 0.35f);
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.55f);
}

void lightColors(ImGuiStyle& s)
{
    ImGui::StyleColorsLight(&s);
    ImVec4* c = s.Colors;
    const ImVec4 gold = rgb(168, 118, 20);
    c[ImGuiCol_WindowBg] = rgb(238, 233, 222);
    c[ImGuiCol_ChildBg] = rgb(244, 240, 232);
    c[ImGuiCol_MenuBarBg] = rgb(226, 219, 204);
    c[ImGuiCol_TitleBgActive] = rgb(214, 198, 160);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_Header] = rgb(222, 200, 150);
    c[ImGuiCol_HeaderHovered] = rgb(230, 206, 150);
    c[ImGuiCol_HeaderActive] = rgb(214, 184, 120);
    c[ImGuiCol_Button] = rgb(222, 212, 192);
    c[ImGuiCol_ButtonHovered] = rgb(230, 206, 150);
    c[ImGuiCol_ButtonActive] = rgb(214, 184, 120);
    c[ImGuiCol_TabSelected] = rgb(222, 200, 150);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_DockingPreview] = rgb(168, 118, 20, 0.5f);
}

/// Medium's panel colours: the ones the panels always drew in.
ThemeColours mediumPanelColours(const ImGuiStyle& s)
{
    ThemeColours t{};
    t.error = ImVec4(1, 0.45f, 0.4f, 1);
    t.warning = ImVec4(0.95f, 0.8f, 0.35f, 1);
    t.caution = ImVec4(1, 0.6f, 0.3f, 1);
    t.info = ImVec4(0.55f, 0.8f, 1, 1);
    t.ok = ImVec4(0.5f, 0.85f, 0.5f, 1);
    t.note = ImVec4(0.85f, 0.85f, 0.7f, 1);
    t.param = ImVec4(0.55f, 0.8f, 1, 1);
    t.unknown = ImVec4(0.95f, 0.75f, 0.35f, 1);
    t.highlight = rgb(255, 210, 60);
    t.highlightText = rgb(255, 220, 120);
    t.star = rgb(240, 190, 50);
    t.chatSystem = ImVec4(1.0f, 0.86f, 0.35f, 1.0f);
    t.chatTeam = ImVec4(0.45f, 0.95f, 0.55f, 1.0f);
    t.chatAll = ImVec4(0.55f, 0.8f, 1.0f, 1.0f);
    t.chatTab = ImVec4(0.78f, 0.45f, 0.08f, 1.0f);
    t.chatTabDimmed = ImVec4(0.66f, 0.38f, 0.07f, 1.0f);
    t.chatTabHovered = ImVec4(0.95f, 0.58f, 0.15f, 1.0f);
    t.idle = ImVec4(0.6f, 0.6f, 0.6f, 1);
    t.playing = ImVec4(0.55f, 0.9f, 1, 1);
    t.section = s.Colors[ImGuiCol_Header];
    t.sectionHovered = s.Colors[ImGuiCol_HeaderHovered];
    t.sectionActive = s.Colors[ImGuiCol_HeaderActive];
    t.viewSelection = rgbU32(255, 210, 60);
    t.viewSelectionHot = rgbU32(255, 230, 120);
    t.viewSelectionFaint = rgbU32(255, 210, 60, 90);
    return t;
}

/// Light's: the same meanings, dark enough to read on pale panels. (The Map panel draws over the
/// game's own dark pictures: its selection keeps Medium's gold.)
ThemeColours lightPanelColours(const ImGuiStyle& s)
{
    ThemeColours t = mediumPanelColours(s);
    t.error = rgb(184, 30, 24);
    t.warning = rgb(150, 100, 0);
    t.caution = rgb(190, 88, 10);
    t.info = rgb(24, 92, 176);
    t.ok = rgb(30, 122, 40);
    t.note = rgb(92, 86, 64);
    t.param = ImVec4(0.1f, 0.35f, 0.7f, 1);
    t.unknown = rgb(164, 100, 10);
    t.highlight = rgb(196, 132, 16);
    t.highlightText = rgb(150, 92, 6);
    t.star = rgb(214, 150, 16);
    t.chatSystem = rgb(150, 100, 0);
    t.chatTeam = rgb(26, 128, 50);
    t.chatAll = rgb(24, 92, 176);
    t.idle = rgb(110, 110, 110);
    t.playing = rgb(20, 112, 160);
    return t;
}

/// Dark's: grey section headers (the blue is the selection's), orange highlights.
ThemeColours darkPanelColours(const ImGuiStyle& s)
{
    ThemeColours t = mediumPanelColours(s);
    t.error = rgb(255, 108, 98);
    t.warning = rgb(255, 196, 76);
    t.caution = rgb(255, 150, 60);
    t.info = rgb(110, 182, 255);
    t.ok = rgb(110, 208, 112);
    t.note = rgb(204, 204, 204);
    t.param = rgb(110, 182, 255);
    t.unknown = rgb(255, 184, 76);
    t.highlight = rgb(255, 165, 0);
    t.highlightText = rgb(255, 190, 70);
    t.star = rgb(255, 180, 30);
    t.chatSystem = rgb(255, 190, 76);
    t.chatTeam = rgb(110, 224, 128);
    t.chatAll = rgb(110, 182, 255);
    t.chatTab = rgb(204, 120, 0);
    t.chatTabDimmed = rgb(160, 94, 0);
    t.chatTabHovered = rgb(255, 158, 20);
    t.idle = rgb(140, 140, 140);
    t.playing = rgb(110, 200, 255);
    t.section = rgb(47, 47, 47);
    t.sectionHovered = rgb(58, 58, 58);
    t.sectionActive = rgb(69, 69, 69);
    t.viewSelection = rgbU32(255, 165, 0);
    t.viewSelectionHot = rgbU32(255, 200, 90);
    t.viewSelectionFaint = rgbU32(255, 165, 0, 90);
    return t;
}

Theme g_theme = Theme::Dark;
ThemeColours g_colours{};

} // namespace

Theme activeTheme()
{
    return g_theme;
}

const ThemeColours& themeColours()
{
    return g_colours;
}

ImVec4 onPanel(ImVec4 colour)
{
    if (g_theme != Theme::Light) return colour;
    // Pale panels: anything brighter than a mid tone scaled down to one (its hue kept).
    const float lum = 0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z;
    constexpr float kMax = 0.40f;
    if (lum <= kMax) return colour;
    const float k = kMax / lum;
    return ImVec4(colour.x * k, colour.y * k, colour.z * k, colour.w);
}

ImGuiHost::ImGuiHost(SDL_Window* window, SDL_Renderer* renderer, std::string iniPath, float uiScale, Theme theme, bool floating,
                     bool hiddenRun)
    : window_(window)
    , renderer_(renderer)
    , iniPath_(std::move(iniPath))
    , hiddenRun_(hiddenRun)
    , uploader_(std::make_unique<SdlUploader>())
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // (no keyboard navigation: the arrows nudge and pan)
    if (floating) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // (before the first frame: the layout's floating windows read with it)
    }
    io.IniFilename = iniPath_.empty() ? nullptr : iniPath_.c_str();
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImFontConfig font;
    font.FontDataOwnedByAtlas = false; // (compiled into the program)
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kEmbeddedFont), static_cast<int>(kEmbeddedFontSize), 0.0f, &font);
    mono_ = io.Fonts->AddFontDefaultVector(); // (the script panel's fixed-width source font)
    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    // The floating panels' windows: imgui_impl_sdl3 makes them (when the platform can), we draw them.
    g_host = this;
    g_hiddenRun = hiddenRun_;
    imguiTextures_.setUploader(uploader_.get());
    panelTextures().setUploader(uploader_.get());
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) {
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        pio.Renderer_CreateWindow = rendererCreateWindow;
        pio.Renderer_DestroyWindow = rendererDestroyWindow;
        pio.Renderer_RenderWindow = rendererRenderWindow;
        pio.Renderer_SwapBuffers = rendererSwapBuffers;
        g_showWindow = pio.Platform_ShowWindow;
        pio.Platform_ShowWindow = showWindow;
    }
    refreshDpi();
    setStyle(uiScale, theme);
}

ImGuiHost::~ImGuiHost()
{
    // The floating windows (and their renderers and copies) first, while the hooks are ours.
    if (ImGuiContext* g = ImGui::GetCurrentContext()) {
        for (int i = 1; i < g->Viewports.Size; ++i) ImGui::DestroyPlatformWindow(g->Viewports[i]);
    }
    imguiTextures_.clear();
    panelTextures().setUploader(nullptr); // (the panels' sources stay, for the next editor: no copies left)
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g_host = nullptr;
    g_showWindow = nullptr;
}

void ImGuiHost::setFloating(bool on)
{
    ImGuiIO& io = ImGui::GetIO();
    if (on) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        return;
    }
    if (floating()) {
        // The floating panels brought inside the main window first (ImGui would only keep a
        // sliver of one that was off its edge).
        ImGuiContext& g = *ImGui::GetCurrentContext();
        int placed = 0;
        for (int i = 1; i < g.Viewports.Size; ++i) {
            ImGuiWindow* owner = g.Viewports[i]->Window;
            if (owner && !(owner->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))) moveOverMain(owner, placed++);
        }
    }
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
}

bool ImGuiHost::floating() const
{
    return (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
}

bool ImGuiHost::floatingSupported() const
{
    return (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_PlatformHasViewports) != 0;
}

int ImGuiHost::floatingWindows() const
{
    int n = 0;
    const ImGuiContext* g = ImGui::GetCurrentContext();
    for (int i = 1; g && i < g->Viewports.Size; ++i) {
        if (g->Viewports[i]->PlatformWindowCreated) ++n;
    }
    return n;
}

void ImGuiHost::closeFloating()
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    bool any = false;
    for (int i = 1; g && i < g->Viewports.Size; ++i) {
        if (g->Viewports[i]->PlatformWindowCreated) {
            ImGui::DestroyPlatformWindow(g->Viewports[i]);
            any = true;
        }
    }
    if (any) {
        SDL_Log("map editor: floating panels put away while the game has the window");
        suspended_ = true;
    }
}

void ImGuiHost::createViewportRenderer(ImGuiViewport* viewport)
{
    SDL_Window* w = sdlWindowOf(viewport);
    SDL_Renderer* r = nullptr;
    if (w) {
        // SDL's default renderer (not the main window's GPU one: a light one is all a panel needs).
        r = SDL_CreateRenderer(w, nullptr);
        if (!r) {
            SDL_Log("map editor: a floating panel's renderer: %s (software instead)", SDL_GetError());
            r = SDL_CreateRenderer(w, SDL_SOFTWARE_RENDERER);
        }
    }
    if (!r) {
        SDL_Log("map editor: no renderer for a floating panel's window: %s", SDL_GetError());
        viewport->RendererUserData = nullptr;
        return;
    }
    SDL_SetRenderVSync(r, 0); // (the main window's present paces the frame)
    viewport->RendererUserData = new ViewportRenderer{r};
    const ImGuiWindow* owner = static_cast<ImGuiViewportP*>(viewport)->Window;
    SDL_Log("map editor: floating window opened for %s (%s renderer, %.0fx%.0f at %.0f,%.0f)", owner ? owner->Name : "a popup",
            SDL_GetRendererName(r), viewport->Size.x, viewport->Size.y, viewport->Pos.x, viewport->Pos.y);
}

void ImGuiHost::destroyViewportRenderer(ImGuiViewport* viewport)
{
    auto* d = static_cast<ViewportRenderer*>(viewport->RendererUserData);
    viewport->RendererUserData = nullptr;
    if (!d) return;
    if (d->renderer) {
        const auto key = reinterpret_cast<std::uintptr_t>(d->renderer);
        imguiTextures_.dropRenderer(key);
        panelTextures().dropRenderer(key);
        SDL_DestroyRenderer(d->renderer);
    }
    delete d;
}

void ImGuiHost::renderViewport(ImGuiViewport* viewport)
{
    SDL_Renderer* r = rendererOf(viewport);
    if (!r || !viewport->DrawData) return;
    SDL_SetRenderTarget(r, nullptr);
    if (!(viewport->Flags & ImGuiViewportFlags_NoRendererClear)) {
        SDL_SetRenderDrawColor(r, 16, 15, 14, 255);
        SDL_RenderClear(r);
    }
    toWindowCoordinates(viewport->DrawData);
    drawData(viewport->DrawData, r);
    if (!shootBase_.empty()) saveShot(r);
}

void ImGuiHost::presentViewport(ImGuiViewport* viewport)
{
    if (SDL_Renderer* r = rendererOf(viewport)) SDL_RenderPresent(r);
}

void ImGuiHost::saveShot(SDL_Renderer* renderer)
{
    SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
    SDL_Surface* rgba = shot ? SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGBA32) : nullptr;
    if (shot) SDL_DestroySurface(shot);
    if (!rgba) {
        SDL_Log("map editor: a floating window's shot can't be read: %s", SDL_GetError());
        return;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rgba->w) * static_cast<std::size_t>(rgba->h) * 4);
    for (int y = 0; y < rgba->h; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(rgba->pixels) + static_cast<std::ptrdiff_t>(y) * rgba->pitch;
        std::copy(row, row + static_cast<std::ptrdiff_t>(rgba->w) * 4, pixels.begin() + static_cast<std::ptrdiff_t>(y) * rgba->w * 4);
    }
    const std::string path = shootBase_ + "-" + std::to_string(++shot_) + ".png";
    try {
        nox::writeFileBytes(path, nox::encodePng(rgba->w, rgba->h, pixels));
        SDL_Log("map editor: saved floating window %d as %s (%dx%d)", shot_, path.c_str(), rgba->w, rgba->h);
    } catch (const std::exception& ex) {
        SDL_Log("map editor: floating window shot %s: %s", path.c_str(), ex.what());
    }
    SDL_DestroySurface(rgba);
}

void ImGuiHost::syncTextures()
{
    // ImGui's textures (the font atlas) as their status says, read before the main window's
    // backend acts on it: the extra renderers' copies made again, updated or freed to match.
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        const auto key = reinterpret_cast<std::uintptr_t>(tex);
        const int status = static_cast<int>(tex->Status);
        const auto last = lastStatus_.find(key);
        const bool changed = last == lastStatus_.end() || last->second != status;
        switch (tex->Status) {
        case ImTextureStatus_WantCreate:
            imguiTextures_.setExternal(key, tex->Width, tex->Height);
            if (changed) imguiTextures_.invalidate(key);
            break;
        case ImTextureStatus_WantUpdates:
            imguiTextures_.setExternal(key, tex->Width, tex->Height);
            imguiTextures_.markDirty(key, TexelRect{tex->UpdateRect.x, tex->UpdateRect.y, tex->UpdateRect.w, tex->UpdateRect.h});
            break;
        case ImTextureStatus_OK:
            imguiTextures_.setExternal(key, tex->Width, tex->Height);
            break;
        case ImTextureStatus_WantDestroy:
        case ImTextureStatus_Destroyed:
            imguiTextures_.remove(key);
            break;
        }
        lastStatus_[key] = status;
    }
}

void ImGuiHost::drawData(ImDrawData* data, SDL_Renderer* renderer)
{
    // imgui_impl_sdlrenderer3's RenderDrawData for an extra renderer: the same drawing, each
    // command's texture swapped for this renderer's copy of it.
    float rsx = 1.0f, rsy = 1.0f;
    SDL_GetRenderScale(renderer, &rsx, &rsy);
    const ImVec2 scale(rsx == 1.0f ? data->FramebufferScale.x : 1.0f, rsy == 1.0f ? data->FramebufferScale.y : 1.0f);
    const int fbW = static_cast<int>(data->DisplaySize.x * scale.x);
    const int fbH = static_cast<int>(data->DisplaySize.y * scale.y);
    if (fbW <= 0 || fbH <= 0) return;
    SDL_SetRenderViewport(renderer, nullptr);
    SDL_SetRenderClipRect(renderer, nullptr);
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    ImGui_ImplSDLRenderer3_RenderState state{renderer};
    pio.Renderer_RenderState = &state;
    SDL_ScaleMode scaleMode = SDL_SCALEMODE_LINEAR;
    const ImVec2 off = data->DisplayPos;
    const auto key = reinterpret_cast<std::uintptr_t>(renderer);
    std::vector<SDL_FColor>& colours = colours_;
    for (const ImDrawList* list : data->CmdLists) {
        const ImDrawVert* vtx = list->VtxBuffer.Data;
        const ImDrawIdx* idx = list->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback == pio.DrawCallback_ResetRenderState) {
                    SDL_SetRenderViewport(renderer, nullptr);
                    SDL_SetRenderClipRect(renderer, nullptr);
                    scaleMode = SDL_SCALEMODE_LINEAR;
                } else if (cmd.UserCallback == pio.DrawCallback_SetSamplerLinear) {
                    scaleMode = SDL_SCALEMODE_LINEAR;
                } else if (cmd.UserCallback == pio.DrawCallback_SetSamplerNearest) {
                    scaleMode = SDL_SCALEMODE_NEAREST;
                } else {
                    cmd.UserCallback(list, &cmd);
                }
                continue;
            }
            ImVec2 c0((cmd.ClipRect.x - off.x) * scale.x, (cmd.ClipRect.y - off.y) * scale.y);
            ImVec2 c1((cmd.ClipRect.z - off.x) * scale.x, (cmd.ClipRect.w - off.y) * scale.y);
            c0.x = std::max(c0.x, 0.0f);
            c0.y = std::max(c0.y, 0.0f);
            c1.x = std::min(c1.x, static_cast<float>(fbW));
            c1.y = std::min(c1.y, static_cast<float>(fbH));
            if (c1.x <= c0.x || c1.y <= c0.y) continue;
            // The texture: ImGui's own (the atlas) from its pixels, or a panel image's copy; a panel
            // image with no copy (unregistered, or it can't be made) as a grey placeholder.
            SDL_Texture* tex = nullptr;
            bool placeholder = false;
            if (ImTextureData* td = cmd.TexRef._TexData) {
                TexturePixels px;
                px.data = static_cast<const std::uint8_t*>(td->GetPixels());
                px.width = td->Width;
                px.height = td->Height;
                px.pitch = td->GetPitch();
                px.bytesPerPixel = td->BytesPerPixel;
                px.format = static_cast<std::uint32_t>(SDL_PIXELFORMAT_RGBA32);
                tex = reinterpret_cast<SDL_Texture*>(imguiTextures_.resolve(key, reinterpret_cast<std::uintptr_t>(td), &px));
                if (!tex) continue; // (glyphs as solid boxes would be worse than none)
            } else if (cmd.TexRef._TexID != ImTextureID_Invalid) {
                tex = reinterpret_cast<SDL_Texture*>(panelTextures().resolve(key, static_cast<std::uintptr_t>(cmd.TexRef._TexID)));
                placeholder = tex == nullptr;
            }
            const SDL_Rect clip{static_cast<int>(c0.x), static_cast<int>(c0.y), static_cast<int>(c1.x - c0.x), static_cast<int>(c1.y - c0.y)};
            SDL_SetRenderClipRect(renderer, &clip);
            const int count = list->VtxBuffer.Size - static_cast<int>(cmd.VtxOffset);
            const ImDrawVert* v = vtx + cmd.VtxOffset;
            colours.resize(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const ImU32 c = v[i].col;
                const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
                if (placeholder) {
                    colours[static_cast<std::size_t>(i)] = SDL_FColor{0.30f, 0.28f, 0.25f, a};
                } else {
                    colours[static_cast<std::size_t>(i)] = SDL_FColor{static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                                                                      static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                                                                      static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, a};
                }
            }
            if (tex) SDL_SetTextureScaleMode(tex, scaleMode);
            SDL_RenderGeometryRaw(renderer, tex, &v->pos.x, static_cast<int>(sizeof(ImDrawVert)), colours.data(), static_cast<int>(sizeof(SDL_FColor)),
                                  &v->uv.x, static_cast<int>(sizeof(ImDrawVert)), count, idx + cmd.IdxOffset, static_cast<int>(cmd.ElemCount),
                                  static_cast<int>(sizeof(ImDrawIdx)));
        }
    }
    pio.Renderer_RenderState = nullptr;
}

void ImGuiHost::refreshDpi()
{
    const float s = SDL_GetWindowDisplayScale(window_);
    dpi_ = s > 0 ? std::clamp(s, 0.5f, 4.0f) : 1.0f;
}

void ImGuiHost::setStyle(float uiScale, Theme theme)
{
    uiScale_ = std::clamp(uiScale, 0.5f, 3.0f);
    theme_ = theme;
    ImGuiStyle style;
    style.WindowRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.GrabRounding = 2.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(7, 5);
    style.FramePadding = ImVec2(7, 4);
    style.WindowPadding = ImVec2(8, 8);
    switch (theme_) {
    case Theme::Light:
        lightColors(style);
        g_colours = lightPanelColours(style);
        break;
    case Theme::Medium:
        mediumColors(style);
        g_colours = mediumPanelColours(style);
        break;
    case Theme::Dark:
    default:
        // Flat: small corners, thin dark outlines round the fields, a brighter line over the
        // selected tab.
        style.WindowRounding = 2.0f;
        style.FrameRounding = 2.0f;
        style.TabRounding = 2.0f;
        style.GrabRounding = 2.0f;
        style.PopupRounding = 2.0f;
        style.ScrollbarRounding = 2.0f;
        style.ChildRounding = 2.0f;
        style.FrameBorderSize = 1.0f;
        style.TabBarOverlineSize = 2.0f;
        darkColors(style);
        g_colours = darkPanelColours(style);
        break;
    }
    g_theme = theme_;
    style.ScaleAllSizes(scale());
    style.FontSizeBase = 15.0f;
    style.FontScaleDpi = dpi_;
    style.FontScaleMain = uiScale_;
    ImGui::GetStyle() = style;
}

void ImGuiHost::processEvent(const SDL_Event& event)
{
    if (floating()) {
        // Which window has the mouse, as ImGui is told it (it picks the hovered panel window by
        // it). SDL3 doesn't always say: a window minimised with the mouse in it never sends its
        // leave, and ImGui then thinks the mouse is still over it (no hover, no wheel elsewhere);
        // and a motion in another window can come before its enter. Told here: the leave on
        // minimising or hiding, the enter on the first motion in a window (no button held: a drag
        // has the mouse captured by the window it began in).
        const auto tell = [](SDL_EventType type, SDL_WindowID id) {
            SDL_Event e{};
            e.type = type;
            e.window.windowID = id;
            e.window.timestamp = SDL_GetTicksNS();
            ImGui_ImplSDL3_ProcessEvent(&e);
        };
        switch (event.type) {
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            mouseWindow_ = event.window.windowID;
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (mouseWindow_ == event.window.windowID) mouseWindow_ = 0;
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
            if (mouseWindow_ == event.window.windowID) {
                tell(SDL_EVENT_WINDOW_MOUSE_LEAVE, event.window.windowID);
                mouseWindow_ = 0;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.windowID != 0 && event.motion.windowID != mouseWindow_ && event.motion.state == 0) {
                tell(SDL_EVENT_WINDOW_MOUSE_ENTER, event.motion.windowID);
                mouseWindow_ = event.motion.windowID;
            }
            break;
        default:
            break;
        }
    }
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED && event.window.windowID == SDL_GetWindowID(window_)) {
        refreshDpi();
        setStyle(uiScale_, theme_);
    }
}

void ImGuiHost::newFrame()
{
    if (suspended_) {
        // Back from the game: the floating windows are made again this frame, without the focus.
        suspended_ = false;
        quietUntilFrame_ = ImGui::GetFrameCount() + 3;
        SDL_Log("map editor: floating panels back");
    }
    g_quietUntilFrame = quietUntilFrame_;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    bringBackLostWindows();
}

void ImGuiHost::bringBackLostWindows()
{
    // A floating window whose middle is on no monitor (its monitor unplugged since the layout was
    // saved, or the desktop rearranged) comes back over the main window. (ImGui alone only keeps a
    // corner of it on the first monitor.) Not while it's being dragged.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (!floating() || pio.Monitors.empty()) return;
    int placed = 0;
    for (int i = 1; i < g.Viewports.Size; ++i) {
        ImGuiWindow* owner = g.Viewports[i]->Window;
        if (!owner || (owner->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0) continue;
        if (g.MovingWindow && g.MovingWindow->RootWindowDockTree == owner->RootWindowDockTree) continue;
        const ImVec2 mid(owner->Pos.x + owner->Size.x / 2, owner->Pos.y + owner->Size.y / 2); // (the window's: moved at once by SetWindowPos)
        bool seen = false;
        for (const ImGuiPlatformMonitor& m : pio.Monitors) {
            if (mid.x >= m.MainPos.x && mid.y >= m.MainPos.y && mid.x < m.MainPos.x + m.MainSize.x && mid.y < m.MainPos.y + m.MainSize.y) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        SDL_Log("map editor: floating window %s was off every monitor (at %.0f,%.0f): back over the main window", owner->Name, owner->Pos.x, owner->Pos.y);
        moveOverMain(owner, placed++);
    }
}

void ImGuiHost::moveOverMain(ImGuiWindow* window, int index)
{
    // Over the main window, a step down and right per window, and no bigger than it.
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const float step = 40.0f * scale();
    const ImVec2 to(main->WorkPos.x + step * static_cast<float>(1 + index), main->WorkPos.y + step * static_cast<float>(1 + index));
    const ImVec2 size(std::min(window->SizeFull.x, std::max(main->WorkSize.x - step * 2, 200.0f)),
                      std::min(window->SizeFull.y, std::max(main->WorkSize.y - step * 2, 150.0f)));
    if (ImGuiDockNode* node = window->DockNodeAsHost) {
        node->Pos = to; // (a floating group of docked panels: its node has the place)
        node->Size = node->SizeRef = size;
    }
    ImGui::SetWindowPos(window, to, ImGuiCond_Always);
    ImGui::SetWindowSize(window, size, ImGuiCond_Always);
}

void ImGuiHost::render()
{
    ImGui::Render();
    syncTextures(); // (before the backend below acts on the textures' status)
    toWindowCoordinates(ImGui::GetDrawData());
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    // The floating windows: made, moved or closed to match the panels, then drawn and presented.
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
    if (!shootBase_.empty() && shot_ > 0) {
        shootBase_.clear(); // (once: the windows drawn this frame were saved)
        shot_ = 0;
    }
}

} // namespace noxsdl::editor
