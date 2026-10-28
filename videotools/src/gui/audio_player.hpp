// Plays a sound for the preview: decoded 16-bit PCM (an Audio.bag sample, a .wav) through SDL's
// default output, one at a time.
#pragma once

#include "noxcore/audio/pcm.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace videotools::gui {

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer() { stop(); }
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    /// Plays `pcm` from the start (stopping what's playing); false (and error() says why) when the
    /// output can't be opened.
    bool play(const nox::audio::Pcm& pcm, std::string label);
    void stop();
    bool playing() const;
    /// What's playing (or played last).
    const std::string& label() const { return label_; }
    /// Seconds played and in all.
    double position() const;
    double length() const { return length_; }
    const std::string& error() const { return error_; }

private:
    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioSpec spec_{};
    std::string label_;
    std::string error_;
    double length_ = 0;
    std::size_t queuedBytes_ = 0;
};

} // namespace videotools::gui
