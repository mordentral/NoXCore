// Plays a preview sound (audio_player.hpp).
#include "gui/audio_player.hpp"

namespace videotools::gui {

bool AudioPlayer::play(const nox::audio::Pcm& pcm, std::string label)
{
    stop();
    label_ = std::move(label);
    error_.clear();
    if (pcm.channels == 0 || pcm.rate == 0 || pcm.samples.empty()) {
        error_ = "nothing to play";
        return false;
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error_ = SDL_GetError();
        return false;
    }
    spec_.format = SDL_AUDIO_S16;
    spec_.channels = pcm.channels;
    spec_.freq = static_cast<int>(pcm.rate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec_, nullptr, nullptr);
    if (!stream_) {
        error_ = SDL_GetError();
        return false;
    }
    queuedBytes_ = pcm.samples.size() * sizeof(std::int16_t);
    SDL_PutAudioStreamData(stream_, pcm.samples.data(), static_cast<int>(queuedBytes_));
    SDL_FlushAudioStream(stream_);
    length_ = static_cast<double>(pcm.frames()) / pcm.rate;
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void AudioPlayer::stop()
{
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

bool AudioPlayer::playing() const
{
    return stream_ && SDL_GetAudioStreamQueued(stream_) > 0;
}

double AudioPlayer::position() const
{
    if (!stream_ || queuedBytes_ == 0) {
        return 0;
    }
    const double left = static_cast<double>(SDL_GetAudioStreamQueued(stream_)) / static_cast<double>(queuedBytes_);
    return length_ * (1.0 - left);
}

} // namespace videotools::gui
