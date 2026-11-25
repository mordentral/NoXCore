// (noxcpp) The question which the player plays with (input_mode_ask.hpp).
#include "input_mode_ask.hpp"

#include "noxcore/strings/l10n.hpp"

#include <algorithm>
#include <optional>

namespace noxsdl {

namespace {

namespace st = nox::gui::status;

// dlg.wnd
constexpr std::int32_t kDlgText = 4004, kDlgTitle = 4005, kDlgNo = 4006, kDlgYes = 4007;
// QuitMenu.wnd's large button, the art the two choices take.
constexpr std::int32_t kQuitButton = 9001;
// The layout (the 640 x 480 screen's units).
constexpr int kWidth = 240, kHeight = 158;
constexpr int kButtonW = 132, kButtonH = 24;
constexpr int kControllerY = 84, kMouseY = 116;

} // namespace

InputModeAsk::InputModeAsk(gui::Resources res, std::filesystem::path windowDir) : gui_(std::move(res)), windowDir_(std::move(windowDir))
{
}

void InputModeAsk::open(std::function<void(bool pad)> onAnswer)
{
    close();
    onAnswer_ = std::move(onAnswer);
    nox::gui::WindowDef def;
    std::optional<nox::gui::WindowDef> button;
    try {
        def = nox::gui::loadWindowFile(windowDir_ / "dlg.wnd");
        const auto quit = nox::gui::loadWindowFile(windowDir_ / "QuitMenu.wnd");
        if (const auto* b = quit.find(kQuitButton)) {
            button = *b;
        }
    } catch (const std::exception& ex) {
        SDL_Log("input mode question: %s", ex.what());
        if (def.children.empty()) {
            // (No dlg.wnd: no question; the mouse's mode as before pad support.)
            const auto answer = std::move(onAnswer_);
            if (answer) {
                answer(false);
            }
            return;
        }
    }
    // The message box made wider and taller for two big buttons, one over the other.
    def.width = kWidth;
    def.height = kHeight;
    // Its title and text kept; its buttons and entry fields (all hidden till a dialog shows some) not.
    std::erase_if(def.children, [](const nox::gui::WindowDef& c) { return c.id != kDlgTitle && c.id != kDlgText; });
    for (auto& c : def.children) {
        if (c.id == kDlgTitle) {
            c.x = 5;
            c.y = 4;
            c.width = kWidth - 10;
        } else if (c.id == kDlgText) {
            c.x = 12;
            c.y = 30;
            c.width = kWidth - 24;
            c.height = 44;
        }
    }
    const auto choice = [&](std::int32_t id, int y) {
        nox::gui::WindowDef b;
        if (button) {
            b = *button;
        } else {
            b.type = nox::gui::WindowType::PushButton;
            b.typeName = "PUSHBUTTON";
            b.status = st::Enabled;
            b.style = nox::gui::style::MouseTrack;
        }
        b.id = id;
        b.x = (kWidth - kButtonW) / 2;
        b.y = y;
        b.width = kButtonW;
        b.height = kButtonH;
        b.status = (b.status | st::Enabled) & ~st::Hidden;
        b.text.clear();
        b.tooltip.clear();
        b.children.clear();
        return b;
    };
    def.children.push_back(choice(kDlgYes, kControllerY));
    def.children.push_back(choice(kDlgNo, kMouseY));

    dialog_ = gui_.load(def, [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked || (from.id != kDlgYes && from.id != kDlgNo)) {
            return false;
        }
        gui_.resources().sound(0x2FE); // ButtonPress
        const bool pad = from.id == kDlgYes;
        deferred_.push_back([this, pad] {
            auto answer = std::move(onAnswer_);
            close();
            if (answer) {
                answer(pad);
            }
        });
        return true;
    });
    gui::centreOnScreen(*dialog_);
    dialog_->status |= st::Above;
    if (gui::Window* t = dialog_->find(kDlgTitle)) {
        t->text = NOX_TR("input_mode.title", "Game Controller");
    }
    if (gui::Window* t = dialog_->find(kDlgText)) {
        t->text = NOX_TR("input_mode.text", "A game controller is connected. Which will you play with?");
    }
    gui::Painter measure(nullptr, gui_.resources(), 640, 480);
    for (const auto& [id, text] : {std::pair{kDlgYes, NOX_TR("input_mode.controller", "Controller")},
                                   std::pair{kDlgNo, NOX_TR("input_mode.mouse", "Mouse and Keyboard")}}) {
        if (gui::Window* b = dialog_->find(id)) {
            b->text = text;
            // (Words too long for the art: the large font drawn smaller to fit, as the WOL screens'.)
            if (const int w = b->largeFont ? measure.textWidth(b->text, true) : 0; w > kButtonW - 10) {
                b->largeScale = std::max(0.55f, static_cast<float>(kButtonW - 10) / static_cast<float>(w));
            }
        }
    }
    gui_.setModal(dialog_);
    gui_.setNavDefault(dialog_->find(kDlgYes));
}

void InputModeAsk::close()
{
    if (!dialog_) {
        return;
    }
    gui_.setModal(nullptr);
    gui_.destroyAll();
    dialog_ = nullptr;
}

void InputModeAsk::update(std::uint64_t nowMs)
{
    if (!dialog_) {
        return;
    }
    gui_.update(nowMs);
    gui::runQueued(deferred_);
}

void InputModeAsk::draw(SDL_Renderer* renderer, int width, int height)
{
    if (dialog_) {
        gui_.draw(renderer, width, height);
    }
}

void InputModeAsk::handleEvent(const SDL_Event& event, int width, int height)
{
    if (dialog_) {
        gui_.handleEvent(event, width, height);
    }
}

} // namespace noxsdl
