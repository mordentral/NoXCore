// The movie player: GAME.EXE's (noxcore client/movies.hpp says when it runs).
//
//   playing    movies\<name> by noxcore's VQA decoder (video/vqa.hpp), or HDVideo\<NAME>.mkv
//              beside noxcore.exe through FFmpeg when it's there (hd_video.hpp). The game's sounds
//              stop and the music is held (Audio::beginMovie); as the exe waits for the music and
//              speech to go quiet, the movie starts once they have (or after 3 s). Its sound
//              plays on the mixer's movie stream, and the frames follow it: frame n shows once
//              n / fps seconds of the sound have been heard (the exe times its frames by its
//              sound); without sound, by the clock. Every frame is decoded (the exe drops
//              frames only when it falls behind). An HD movie keeps its VQA's time (the VQHD's
//              frame rate and count, when the VQA is there).
//   drawing    over black, where the exe puts it: centred in its 640 x 480 screen (the placement
//              call with -1, -1: 640 x 400 at y 40), that screen scaled to fit the window and centred
//              as noxsdl draws the menus. The pointer isn't drawn (ShowCursor(0)).
//   skipping   movies::skips: Escape stops any, other keys and the
//              left button only those played with Skip::AnyInput. A key's auto-repeats count
//              (Windows repeats WM_KEYDOWN); Alt, F10 and Alt + a key don't (WM_SYSKEYDOWN,
//              movies::keyInput).
//   the end    the screen cleared and the callback run (from update()).
// Shift held as one starts, or noxsdl's --no-movies (as the exe's -safe, engine flag 0x200),
// plays nothing and goes straight on.
#pragma once

#include "audio.hpp"
#include "hd_video.hpp"

#include "noxcore/client/movies.hpp"
#include "noxcore/video/vqa.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace noxsdl {

class MoviePlayer {
public:
    /// `hdDir`: the HDVideo folder beside noxcore.exe (it needn't exist).
    MoviePlayer(SDL_Renderer* renderer, Audio* audio, std::filesystem::path gameDir, std::filesystem::path hdDir);
    ~MoviePlayer();

    /// --no-movies: nothing plays (the chains go on as if each had).
    void setDisabled(bool off) { disabled_ = off; }
    /// The HD versions aren't looked for (--no-hd-movies).
    void setHdAllowed(bool on) { hdAllowed_ = on; }

    /// game_setMovieFile: whether `name` (e.g. "NoxLogo.vqa") is there to play, as a
    /// VQA or in HD.
    bool available(const std::string& name) const;
    /// Plays `name`, then runs `done` (from a later
    /// update(), once the screen has been cleared).
    void play(const std::string& name, nox::client::movies::Skip skip, std::function<void()> done);
    bool active() const { return state_ != State::Idle; }
    /// Whether the movie playing (or last played) is the HD one.
    bool hd() const { return static_cast<bool>(hd_); }
    int frame() const { return shownFrame_; }

    void update(std::uint64_t nowMs);
    void draw(int width, int height);
    /// Keys and buttons while a movie plays; returns whether it took the event (it takes all).
    bool handleEvent(const SDL_Event& event);

private:
    enum class State : std::uint8_t { Idle, Waiting, Playing, Done };

    bool openHd(const std::filesystem::path& file);
    bool openVqa(const std::filesystem::path& file);
    /// An HD movie timed as its VQA (fps and frame count from the VQA's VQHD).
    void vqaTiming(const std::filesystem::path& file);
    void start(std::uint64_t nowMs);
    void finish();
    bool decodeNext();
    void upload();

    SDL_Renderer* renderer_ = nullptr;
    Audio* audio_ = nullptr;
    std::filesystem::path gameDir_;
    std::filesystem::path hdDir_;
    bool disabled_ = false;
    bool hdAllowed_ = true;

    State state_ = State::Idle;
    nox::client::movies::Skip skip_ = nox::client::movies::Skip::AnyInput;
    std::function<void()> done_;
    std::uint64_t waitSince_ = 0;
    std::uint64_t startMs_ = 0;
    double soundAt_ = 0;          ///< the sound's position last seen
    std::uint64_t soundAtMs_ = 0; ///< and when
    bool skipped_ = false;

    std::unique_ptr<nox::video::VqaDecoder> vqa_;
    std::unique_ptr<nox::video::VqaAudioDecoder> vqaSound_;
    std::unique_ptr<HdVideo> hd_;
    std::unique_ptr<nox::audio::AudioDecoder> hdSound_;
    bool hasSound_ = false;
    std::uint32_t soundRate_ = 22050;
    double fps_ = 15;
    int frames_ = 0;
    int decodedFrame_ = -1;
    int shownFrame_ = -1;
    bool decoderDone_ = false;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> rgba_;
    SDL_Texture* texture_ = nullptr;
};

} // namespace noxsdl
