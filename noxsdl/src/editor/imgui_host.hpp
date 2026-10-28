// Dear ImGui (docking branch) on noxsdl's SDL renderer, for the map editor's panels only: the
// game's own screens stay the .wnd windows. Owns the ImGui context while the editor is open.
//
// Floating panels: with ImGui's multi-viewports a panel dragged out of the main window becomes a
// window of its own (imgui_impl_sdl3 makes the SDL window). imgui_impl_sdlrenderer3 draws only the
// main window, so each extra window gets an SDL_Renderer of its own here, drawing its panels from
// copies of the textures they use: ImGui's font atlas (ImTextureData) and the editor's own images
// (panel_textures.hpp), made on first use and kept up to date (texture_mirror.hpp). The main
// window's drawing is unchanged.
#pragma once

#include "editor/texture_mirror.hpp"
#include "editor/theme.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ImFont;
struct ImGuiViewport;
struct ImGuiWindow;
struct ImDrawData;

namespace noxsdl::editor {

class ImGuiHost {
public:
    /// `iniPath`: where the docked layout is kept (empty: not kept, as a screenshot run).
    /// `floating`: panels may float out of the main window. `hiddenRun`: a --screenshot run, whose
    /// floating windows are made and drawn but never shown (nor take the focus).
    ImGuiHost(SDL_Window* window, SDL_Renderer* renderer, std::string iniPath, float uiScale, Theme theme, bool floating = false,
              bool hiddenRun = false);
    ~ImGuiHost();
    ImGuiHost(const ImGuiHost&) = delete;
    ImGuiHost& operator=(const ImGuiHost&) = delete;

    void processEvent(const SDL_Event& event);
    void newFrame();
    /// Draws the frame's panels onto the renderer's current target, and the floating panels'
    /// windows (each presented here; the main window is presented by its owner).
    void render();
    /// The theme (theme.hpp: Dark, near-black with blue accents; Medium, NoX Core's stone panels
    /// and gold; Light), sized for the display's DPI times the user's scale. Every window follows
    /// it, the floating panels' too (one ImGui style); the panels' own colours with it
    /// (theme_colours.hpp).
    void setStyle(float uiScale, Theme theme);
    Theme theme() const { return theme_; }
    /// The display's content scale times the user's.
    float scale() const { return dpi_ * uiScale_; }
    /// Re-reads the display's scale (the window moved to another monitor).
    void refreshDpi();
    /// A fixed-width font, for the script panel's source (Dear ImGui's own scalable default).
    ImFont* monoFont() const { return mono_; }

    // --- Floating panels ---

    /// Panels may float out of the main window as windows of their own (between frames). Off: the
    /// floating ones come back inside the main window.
    void setFloating(bool on);
    bool floating() const;
    /// The platform can make the windows (not under Wayland, whose SDL backend can't place them).
    bool floatingSupported() const;
    /// Floating windows open now (panels, and popups or tooltips out past the main window's edge).
    int floatingWindows() const;
    /// The floating windows closed while the editor isn't drawn (a full-window play-test, a
    /// session's game in front); they come back where they were with the next frame.
    void closeFloating();
    /// (Tests) each floating window's next frame saved as `base`-N.png (N from 1).
    void shootFloating(std::string base) { shootBase_ = std::move(base); }

    // (The renderer hooks, imgui_host.cpp.)
    void createViewportRenderer(ImGuiViewport* viewport);
    void destroyViewportRenderer(ImGuiViewport* viewport);
    void renderViewport(ImGuiViewport* viewport);
    void presentViewport(ImGuiViewport* viewport);

private:
    void syncTextures();
    void bringBackLostWindows();
    void moveOverMain(ImGuiWindow* window, int index);
    void drawData(ImDrawData* data, SDL_Renderer* renderer);
    void saveShot(SDL_Renderer* renderer);

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    ImFont* mono_ = nullptr;
    std::string iniPath_;
    float dpi_ = 1.0f;
    float uiScale_ = 1.0f;
    Theme theme_ = Theme::Dark;
    bool hiddenRun_ = false;

    std::unique_ptr<TextureUploader> uploader_; ///< SDL's, for the extra renderers
    TextureMirror imguiTextures_;               ///< ImGui's own (the font atlas), by ImTextureData
    std::unordered_map<std::uintptr_t, int> lastStatus_; ///< each ImTextureData's status last frame
    bool suspended_ = false;                    ///< closeFloating: the windows come back next frame
    int quietUntilFrame_ = 0;                   ///< windows made before this frame don't take the focus
    SDL_WindowID mouseWindow_ = 0;              ///< the window the mouse is in (as ImGui was told)
    std::string shootBase_;
    int shot_ = 0;
    std::vector<SDL_FColor> colours_; ///< drawData's vertex colours (kept: no allocation a frame)
};

} // namespace noxsdl::editor
