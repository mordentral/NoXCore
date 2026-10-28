// The play-test as the editor shows it: play-in-editor in the Map panel, the toolbar's controls,
// the Play menu's extra items (play_view.hpp).
#include "editor/play_view.hpp"
#include "editor/map_editor.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

namespace noxsdl {

namespace {

/// Help shown over a disabled field too.
void help(const std::string& text)
{
    editor::help(text, ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
}

enum class Icon { Play, Pause, Stop };

/// A toolbar button with a drawn icon (the font has no media glyphs) and its label after it.
bool iconButton(const char* id, Icon icon, const char* label, ImU32 colour, bool enabled)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float side = std::floor(h * 0.46f);
    const float textW = ImGui::CalcTextSize(label).x;
    const ImVec2 size(style.FramePadding.x * 2 + side + (textW > 0 ? style.ItemInnerSpacing.x + textW : 0), h);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(id, size);
    ImGui::EndDisabled();
    const ImVec2 a = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = enabled ? colour : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float x0 = a.x + style.FramePadding.x;
    const float y0 = a.y + (h - side) / 2;
    switch (icon) {
    case Icon::Play:
        dl->AddTriangleFilled(ImVec2(x0, y0), ImVec2(x0, y0 + side), ImVec2(x0 + side, y0 + side / 2), c);
        break;
    case Icon::Pause: {
        const float bar = std::max(2.0f, std::floor(side * 0.34f));
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bar, y0 + side), c);
        dl->AddRectFilled(ImVec2(x0 + side - bar, y0), ImVec2(x0 + side, y0 + side), c);
        break;
    }
    case Icon::Stop:
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + side, y0 + side), c, 1.0f);
        break;
    }
    if (textW > 0) {
        dl->AddText(ImVec2(x0 + side + style.ItemInnerSpacing.x, a.y + style.FramePadding.y),
                    ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
    }
    return pressed;
}

constexpr ImU32 kGreen = IM_COL32(96, 200, 100, 255);
constexpr ImU32 kAmber = IM_COL32(232, 184, 64, 255);
constexpr ImU32 kRed = IM_COL32(222, 78, 64, 255);

} // namespace

PlayView::PlayView(MapEditor& editor)
    : ed_(editor)
{
}

void PlayView::registerCommands()
{
    using editor::Command;
    using editor::Shortcut;
    const auto key = [](const char* text) { return Shortcut::parse(text).value_or(Shortcut{}); };
    const auto add = [&](const char* id, const char* label, const char* shortcut, const char* helpText, std::function<void()> run,
                         std::function<bool()> enabled = {}, std::function<bool()> checked = {}) {
        ed_.commands_.add(Command{id, label, "Play", key(shortcut), helpText, std::move(run), std::move(enabled), std::move(checked)});
    };
    add("play.pause", "Pause Play-Test", "Pause", "Stop the play-test's world where it is, or let it go on.",
        [this] {
            if (ed_.ctx_.playTest.pause) ed_.ctx_.playTest.pause(!state().paused);
        },
        [this] { return running() && !state().session && static_cast<bool>(ed_.ctx_.playTest.pause); }, [this] { return state().paused; });
    add("play.release", "Release Mouse", "Shift+F1",
        "Give the keyboard and mouse back to the editor; the game goes on (click it to play again). Full window: back to the editor.",
        [this] {
            if (ed_.ctx_.playTest.capture) ed_.ctx_.playTest.capture(false);
        },
        [this] { return running() && state().captured; });
    add("play.fullWindow", "Play in Full Window", "",
        "Play-test over the whole window (F2 in the game comes back), instead of in the Map panel with the editor round it.",
        [this] {
            ed_.settings_.playFullWindow = !ed_.settings_.playFullWindow;
            ed_.saveSettings();
        },
        {}, [this] { return ed_.settings_.playFullWindow; });
}

editor::PlayTestView PlayView::state() const
{
    const auto& hooks = ed_.ctx_.playTest;
    if (!running() || !hooks.view) {
        return {};
    }
    return hooks.view();
}

bool PlayView::running() const
{
    const auto& hooks = ed_.ctx_.playTest;
    return hooks.running && hooks.running();
}

bool PlayView::inPanel() const
{
    return running() && !state().fullWindow;
}

void PlayView::beforeUi()
{
    // While the game has the keyboard and mouse ImGui doesn't see the mouse (no hover, and its
    // backend leaves the pointer to the game: shown or hidden every frame otherwise).
    const editor::PlayTestView v = state();
    const bool gameHas = running() && !v.fullWindow && v.captured;
    ImGuiIO& io = ImGui::GetIO();
    if (gameHas) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
    }
    if (gameHas != wasCaptured_) {
        // (What ImGui last saw held stays so otherwise: the keys went to the game meanwhile.)
        io.ClearInputKeys();
        if (gameHas) {
            io.ClearInputMouse();
            capturedMs_ = SDL_GetTicks();
        }
        wasCaptured_ = gameHas;
    }
}

void PlayView::panel(const ImVec2& origin, const ImVec2& size)
{
    const editor::PlayTestView v = state();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    dl->AddRectFilled(origin, end, IM_COL32(0, 0, 0, 255)); // (the bars round the game's area)
    // A click on the view gives the game the keyboard and mouse (the click itself is the editor's).
    const bool clicked = ImGui::InvisibleButton("##game", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                                    ImGuiButtonFlags_MouseButtonMiddle);
    if (clicked && !v.captured && ed_.ctx_.playTest.capture) {
        ed_.ctx_.playTest.capture(true);
    }
    const ImVec2 o = ed_.windowOrigin(); // (the area is in window coordinates; ImGui's are the desktop's while panels can float)
    ImVec2 a(o.x + v.area.x, o.y + v.area.y);
    ImVec2 b(o.x + v.area.x + v.area.w, o.y + v.area.y + v.area.h);
    if (v.area.w < 1 || v.area.h < 1) {
        a = origin;
        b = end;
    }
    if (v.texture) {
        dl->AddImage(editor::texId(v.texture), a, b);
    }
    const float s = ed_.imgui_->scale();
    // The game's edge: gold while it has the input.
    dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1),
                v.captured ? IM_COL32(214, 168, 64, 255) : IM_COL32(90, 84, 74, 255), 0, 0, v.captured ? 2.0f * s : 1.0f);
    // The hint, top centre: how to get out (a few seconds after the game takes the input), or in.
    std::string hint;
    if (v.session) {
        // The session's game (the session bar's Play): F2 back to editing, the mouse freed as a play-test's.
        if (!v.captured) {
            hint = "Click the game to play,  F2 edits";
        } else if (SDL_GetTicks() - capturedMs_ < 6000) {
            const std::string release = ed_.commands_.shortcut("play.release").text();
            hint = "F2 edits,  " + (release.empty() ? std::string("Shift+F1") : release) + " frees the mouse";
        }
    } else if (v.captured) {
        if (SDL_GetTicks() - capturedMs_ < 6000) {
            const std::string release = ed_.commands_.shortcut("play.release").text();
            const std::string stop = ed_.commands_.shortcut("play.stop").text();
            hint = (release.empty() ? std::string("F2") : release) + " frees the mouse" + (stop.empty() ? "" : ",  " + stop + " stops");
        }
    } else {
        hint = "Click the game to play";
        const std::string stop = ed_.commands_.shortcut("play.stop").text();
        if (!stop.empty()) hint += ",  " + stop + " stops";
    }
    if (!hint.empty()) {
        const ImVec2 ts = ImGui::CalcTextSize(hint.c_str());
        const ImVec2 p(std::floor((a.x + b.x - ts.x) / 2), a.y + 10 * s); // (top centre: clear of the HUD's corners)
        dl->AddRectFilled(ImVec2(p.x - 6 * s, p.y - 3 * s), ImVec2(p.x + ts.x + 6 * s, p.y + ts.y + 3 * s), IM_COL32(0, 0, 0, 170), 3 * s);
        dl->AddText(p, IM_COL32(236, 226, 200, 255), hint.c_str());
    }
    if (v.paused) {
        const char* text = "PAUSED";
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * 2.0f;
        const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0, text);
        const ImVec2 p((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2);
        dl->AddRectFilled(ImVec2(p.x - 14 * s, p.y - 6 * s), ImVec2(p.x + ts.x + 14 * s, p.y + ts.y + 6 * s), IM_COL32(0, 0, 0, 170), 4 * s);
        dl->AddText(font, fs, p, kAmber, text);
    }
}

void PlayView::toolbarButtons()
{
    const auto& hooks = ed_.ctx_.playTest;
    if (!hooks.available()) {
        return;
    }
    const auto key = [this](const char* id) {
        const std::string k = ed_.commands_.shortcut(id).text();
        return k.empty() ? std::string() : "  (" + k + ")";
    };
    if (!running()) {
        if (ed_.inSession()) {
            // (The session's game is Play (F2) on the session bar; this tests our working copy in a
            // game of our own, the others undisturbed.)
            if (iconButton("##play", Icon::Play, "Private Test", kGreen, ed_.commands_.enabled("play.start"))) ed_.commands_.run("play.start");
            help("Test the working copy in a game of your own, in its own window: its real mode, nobody else in it, the session undisturbed" +
                 key("play.start"));
            ImGui::SameLine();
            return;
        }
        if (iconButton("##play", Icon::Play, "Play", kGreen, ed_.commands_.enabled("play.start"))) ed_.commands_.run("play.start");
        help("Play-test the map " + std::string(ed_.settings_.playFullWindow ? "over the whole window (F2 in the game comes back)" : "in the Map panel") +
             key("play.start") + "\nIts game mode, players dying and Play in Full Window are in the Play menu.");
        ImGui::SameLine();
        return;
    }
    const editor::PlayTestView v = state();
    if (v.session) {
        // The session's game in the Map panel (the session bar's Play): back to editing; it can't
        // be paused or stopped from here (everyone plays in it).
        if (iconButton("##edit", Icon::Stop, "Edit (F2)", kAmber, static_cast<bool>(hooks.stop))) hooks.stop();
        help("Back to editing: your character stands aside as an observer till you play again.");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(editor::onPanel(ImGui::ColorConvertU32ToFloat4(kGreen)), "Playing in the session");
        ImGui::SameLine(0, 4);
        if (v.captured) {
            ImGui::TextDisabled("(%s frees the mouse)", ed_.commands_.shortcut("play.release").text().c_str());
        } else {
            ImGui::TextDisabled("(click the game to play)");
        }
        ImGui::SameLine();
        return;
    }
    if (v.fullWindow) {
        // The test runs behind the editor (F2): back into it.
        if (iconButton("##resume", Icon::Play, "Back to Game", kGreen, true)) ed_.commands_.run("play.start");
        help("Back into the play-test's game" + key("play.start"));
        ImGui::SameLine();
    }
    if (v.paused) {
        if (iconButton("##pause", Icon::Play, "Resume", kGreen, ed_.commands_.enabled("play.pause"))) ed_.commands_.run("play.pause");
        help("Let the play-test's world go on" + key("play.pause"));
    } else {
        if (iconButton("##pause", Icon::Pause, "Pause", kAmber, ed_.commands_.enabled("play.pause"))) ed_.commands_.run("play.pause");
        help("Stop the play-test's world where it is" + key("play.pause"));
    }
    ImGui::SameLine();
    if (iconButton("##stop", Icon::Stop, "Stop", kRed, true)) ed_.commands_.run("play.stop");
    help("End the play-test" + key("play.stop"));
    ImGui::SameLine();
    if (ImGui::Button("Reload")) ed_.commands_.run("script.reload");
    help("Hot-reload the script into the play-test" + key("script.reload"));
    ImGui::SameLine();
    // The play state.
    ImGui::AlignTextToFramePadding();
    if (v.paused) {
        ImGui::TextColored(editor::onPanel(ImGui::ColorConvertU32ToFloat4(kAmber)), "Paused");
    } else {
        ImGui::TextColored(editor::onPanel(ImGui::ColorConvertU32ToFloat4(kGreen)), "Playing");
    }
    ImGui::SameLine(0, 4);
    if (v.fullWindow) {
        ImGui::TextDisabled("(full window, behind the editor)");
    } else if (v.captured) {
        ImGui::TextDisabled("(%s frees the mouse)", ed_.commands_.shortcut("play.release").text().c_str());
    } else {
        ImGui::TextDisabled("(click the game to play)");
    }
    ImGui::SameLine();
}

void PlayView::menus()
{
    if (!ImGui::BeginMenu("Play")) {
        return;
    }
    ImGui::Separator();
    ed_.menuItem("play.pause");
    ed_.menuItem("play.release");
    ImGui::Separator();
    ed_.menuItem("play.fullWindow");
    ImGui::EndMenu();
}

bool PlayView::gameKey(const SDL_Event& event)
{
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return false;
    }
    const SDL_Keycode k = event.key.key;
    if (k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT || k == SDLK_RSHIFT || k == SDLK_LALT || k == SDLK_RALT || k == SDLK_LGUI ||
        k == SDLK_RGUI) {
        return false;
    }
    editor::Shortcut pressed;
    pressed.ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
    pressed.shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
    pressed.alt = (event.key.mod & SDL_KMOD_ALT) != 0;
    pressed.key = SDL_GetKeyName(k);
    const editor::Command* c = ed_.commands_.match(pressed);
    if (!c || (c->id != "play.stop" && c->id != "play.pause" && c->id != "play.release")) {
        return false; // (everything else is the game's: F5 too, which would only come back in)
    }
    ed_.commands_.run(c->id);
    return true;
}

} // namespace noxsdl
