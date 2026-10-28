// The legal screen (legal_screen.hpp): legal.wnd, counted down a frame at a time to the main menu.
#include "legal_screen.hpp"

#include "noxcore/version.hpp"

namespace noxsdl {

namespace {

constexpr std::int32_t kStamp = 9998;      // 0x270E
constexpr std::int32_t kClickArea = 9901;  // 0x26AD: the whole screen's button
constexpr std::int32_t kClickText = 9910;  // 0x26B6: Legal.wnd:ClickMessage, hidden at first
constexpr std::int32_t kLegalText = 9980;  // 0x26FC
constexpr std::int32_t kVersion = 9999;    // 0x270F
constexpr std::int32_t kCounter = 9970;    // 0x26F2: its draw function counts down
constexpr int kInputFrom = 0x10F;          // below this, input is taken (30 frames in)
constexpr int kShowClick = 0x10E;          // below this, the click message shows

} // namespace

LegalScreen::LegalScreen(gui::Resources res, const std::filesystem::path& windowDir, std::function<void()> done)
    : gui_(std::move(res)), done_(std::move(done))
{
    // game_showLegal.
    screen_ = gui_.load(windowDir / "legal.wnd", [this](gui::Window&, int message, gui::Window& from) {
        // windowMainBGProc: the screen's button, once the count is under 0x10F.
        if (count_ < kInputFrom && message == gui::Clicked && from.id == kClickArea) {
            leave();
        }
        return true;
    });
    gui_.setModal(screen_);
    if (gui::Window* v = screen_->find(kVersion)) {
        v->text = std::string("V:") + nox::version(); // ("V:%S", the exe's version)
    }
    // 9998 gets "%S" of eight bytes the exe reads from its own file at 0x1C (a build
    // stamp, if any): left empty here.
    if (gui::Window* stamp = screen_->find(kStamp)) {
        stamp->text.clear();
    }
    if (gui::Window* counter = screen_->find(kCounter)) {
        counter->draw = [this](gui::Window&, gui::Painter& p) {
            if (!placed_) {
                // The legal text's last line at 477 (0x1DD less its height at 640 wide).
                placed_ = true;
                if (gui::Window* legal = screen_->find(kLegalText)) {
                    legal->rect.y = 0x1DD - p.textHeight(legal->text, legal->largeFont, 640);
                }
            }
            // The draw: "TM" in grey at (500, 150).
            p.text("TM", 500, 150, false, nox::gui::Color{150, 150, 150, false});
        };
    }
}

void LegalScreen::leave()
{
    if (over_) {
        return;
    }
    over_ = true;
    gui_.setModal(nullptr);
    if (done_) {
        done_();
    }
}

void LegalScreen::update(std::uint64_t nowMs)
{
    gui_.update(nowMs);
    if (stepMs_ == 0) {
        stepMs_ = nowMs;
    }
    for (; nowMs - stepMs_ >= 1000 / 30 && !over_; stepMs_ += 1000 / 30) {
        // The draw, a frame: the count down; below 0 the main menu; below 0x10E the click
        // message shown.
        --count_;
        if (count_ < 0) {
            leave();
            return;
        }
        if (count_ < kShowClick) {
            if (gui::Window* w = screen_->find(kClickText)) {
                w->show(true);
            }
        }
    }
}

void LegalScreen::draw(SDL_Renderer* renderer, int width, int height)
{
    gui_.draw(renderer, width, height);
}

void LegalScreen::handleEvent(const SDL_Event& event, int width, int height)
{
    if (over_) {
        return;
    }
    // A key (window message 0x15, state 2) once the count is under 0x10F.
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        if (count_ < kInputFrom) {
            leave();
        }
        return;
    }
    gui_.handleEvent(event, width, height);
}

} // namespace noxsdl
