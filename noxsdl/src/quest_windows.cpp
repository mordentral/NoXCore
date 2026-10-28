// QuestWindows (quest_windows.hpp): the quest's game-over and shared-keys windows, shown and
// filled from the client's World each frame.
#include "quest_windows.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace noxsdl {

namespace {

// GGOver.wnd
constexpr std::int32_t kQuit = 10701, kContinue = 10702, kTitle = 10703, kQuestion = 10709, kConsecutive = 10705, kHighStage = 10706, kSecrets = 10707,
                       kKills = 10708, kGenerators = 10710, kTotalScore = 10711, kTimer = 10712;
// SKey.wnd
constexpr std::int32_t kKeysOk = 10803;

/// swprintf's %d / %s fields filled in order.
std::string format(const std::string& f, std::initializer_list<std::string> args)
{
    std::string out;
    auto next = args.begin();
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%' || i + 1 >= f.size()) {
            out += f[i];
            continue;
        }
        std::size_t j = i + 1;
        while (j < f.size() && (std::isdigit(static_cast<unsigned char>(f[j])) || f[j] == '-')) {
            ++j;
        }
        if (j < f.size() && (f[j] == 'd' || f[j] == 's' || f[j] == 'S')) {
            out += next != args.end() ? *next++ : std::string();
            i = j;
        } else if (j < f.size() && f[j] == '%') {
            out += '%';
            i = j;
        } else {
            out += f[i];
        }
    }
    return out;
}

} // namespace

QuestWindows::QuestWindows(gui::Resources res, const std::filesystem::path& windowDir, Hooks hooks)
    : gui_(std::move(res)), hooks_(std::move(hooks))
{
    // loaded, hidden and disabled.
    gameOver_ = gui_.load(windowDir / "GGOver.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        gui_.resources().sound(0x2FE);
        if (from.id == kQuit) {
            deferred_.push_back([this] {
                if (hooks_.quit) {
                    hooks_.quit();
                }
                gameOver_->show(false);
            });
        } else if (from.id == kContinue) {
            deferred_.push_back([this] {
                if (hooks_.continueQuest) {
                    hooks_.continueQuest();
                }
                gameOver_->show(false);
            });
        }
        return true;
    });
    keys_ = gui_.load(windowDir / "SKey.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        gui_.resources().sound(0x2FE);
        if (from.id == kKeysOk) {
            deferred_.push_back([this] { keys_->show(false); });
        }
        return true;
    });
    for (gui::Window* w : {gameOver_, keys_}) {
        if (w) {
            w->show(false);
        }
    }
}

void QuestWindows::show(gui::Window* w)
{
    // Shown, enabled, centred on the screen (its size halved off the middle), no focus.
    if (!w) {
        return;
    }
    w->show(true);
    w->rect.x = 640 / 2 - w->rect.w / 2;
    w->rect.y = 480 / 2 - w->rect.h / 2;
}

void QuestWindows::update(const nox::client::World& world, std::uint32_t frame, bool host, std::uint64_t nowMs)
{
    gui_.update(nowMs);
    gui::runQueued(deferred_);
    const auto& game = world.game();
    const auto set = [](gui::Window* parent, std::int32_t id, std::string t) {
        if (gui::Window* w = parent ? parent->find(id) : nullptr) {
            w->text = std::move(t);
        }
    };
    if (game.questGameOver.count != overSeen_) {
        // Gui_GameOver_Show: QuestFinalDeath (the world's cue), shown, the tallies in.
        overSeen_ = game.questGameOver.count;
        overFrame_ = game.questGameOver.frame;
        show(gameOver_);
        const auto& over = game.questGameOver;
        set(gameOver_, kGenerators, format(text("GGOver.wnd:GeneratorsDestroyed"), {std::to_string(over.generators)}));
        set(gameOver_, kSecrets, format(text("GGOver.wnd:NumSecretsFound"), {std::to_string(over.secrets)}));
        set(gameOver_, kKills, format(text("GGOver.wnd:Kills"), {std::to_string(over.kills)}));
        set(gameOver_, kConsecutive, ""); // (never written)
        set(gameOver_, kHighStage, "");
        set(gameOver_, kTotalScore, "");  // ("%s" of the empty)
        if (words_) {
            // (noxcpp: a mode's own words, as one's out of a run: no penalty or
            // continuing. A stock client keeps the quest's.)
            set(gameOver_, kTitle, words_->title);
            if (!words_->tallies) {
                set(gameOver_, kGenerators, "");
                set(gameOver_, kSecrets, "");
            }
            set(gameOver_, kQuestion, words_->question);
        } else {
            set(gameOver_, kTitle, text("GGOver.wnd:GameOver"));
            set(gameOver_, kQuestion, text("GeneralPrint:MiscQuestionQDeath"));
        }
    }
    if (gameOverShown()) {
        // 30 s from its coming, in seconds; the host's own player none.
        const auto left = static_cast<std::int32_t>(30 * 30 - (frame - overFrame_));
        set(gameOver_, kTimer, host ? std::string() : format("%s - %d", {text("Rules.c:Time"), std::to_string(std::max(left, 0) / 30)}) /* (L"%s - %d") */);
    }
    if (game.sharedKeysShown != keysSeen_) {
        keysSeen_ = game.sharedKeysShown;
        if (game.sharedKeys) {
            show(keys_);
        }
    }
    if (!game.sharedKeys && keysShown()) {
        keys_->show(false);
    }
}

void QuestWindows::draw(SDL_Renderer* renderer, int width, int height)
{
    if (gameOverShown() || keysShown()) {
        gui_.draw(renderer, width, height);
    }
}

bool QuestWindows::handleEvent(const SDL_Event& event, int width, int height)
{
    if (!gameOverShown() && !keysShown()) {
        return false;
    }
    return gui_.handleEvent(event, width, height);
}

} // namespace noxsdl
