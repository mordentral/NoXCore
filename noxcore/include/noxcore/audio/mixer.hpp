// The software mixer that stands in for Miles: kChannels sample channels (the game allocates 16)
// and two streams (music and dialog speech), mixed to float stereo at the output
// device's rate. The game's logic (sound_engine.hpp, music.hpp) drives it from the main thread
// once a frame; render() runs on the audio thread. Every call takes the mixer's lock.
//
// Unlike Miles it mixes in floating point and resamples with cubic interpolation, and it glides
// each channel's gain over a render block instead of stepping it, so volume and pan ramps don't
// click. Pan is a balance: centred, both sides play at full volume (Miles' 0-127 pan).
#pragma once

#include "noxcore/audio/pcm.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

namespace nox::audio {

using SamplePtr = std::shared_ptr<const Pcm>;

class Mixer {
public:
    static constexpr int kChannels = 16;
    static constexpr int kStreams = 4;
    static constexpr int kMusic = 0;
    static constexpr int kDialog = 1;
    static constexpr int kMovie = 2; ///< a movie's sound (the exe's VQA player has its own DirectSound)
    static constexpr int kModUi = 3; ///< (noxcpp's) a mod UI's own sound, one at a time

    explicit Mixer(std::uint32_t outputRate);

    std::uint32_t outputRate() const { return outputRate_; }

    // ---- sample channels
    /// Starts `sample` from its beginning (dropping whatever played).
    void play(int channel, SamplePtr sample);
    /// What plays straight after the current buffer (nullptr: nothing, the channel then ends).
    void queue(int channel, SamplePtr next);
    void stop(int channel);
    /// Gain 0-1, pan 0 (left) - 1 (right), playback rate in Hz of the sample's own data.
    void setParams(int channel, float gain, float pan, double rateHz);

    struct ChannelStatus {
        bool playing = false;
        std::uint32_t buffersStarted = 0; ///< since play(): 1 for the first buffer, +1 per queued one
    };
    ChannelStatus status(int channel) const;

    // ---- streams
    /// Starts a stream (replacing the slot's) paused or playing.
    void playStream(int slot, std::unique_ptr<AudioDecoder> decoder, bool paused = false);
    void stopStream(int slot);
    void pauseStream(int slot, bool paused);
    void setStreamGain(int slot, float gain);

    struct StreamStatus {
        bool open = false;
        bool ended = false;          ///< decoded to its end and played out
        std::uint64_t position = 0;  ///< frames decoded
    };
    StreamStatus streamStatus(int slot) const;

    /// Overall output gain (1 = as mixed).
    void setMasterGain(float gain);

    /// Mixes `frames` stereo frames (interleaved L, R) into `out`, replacing its contents.
    void render(float* out, std::size_t frames);

private:
    struct Channel {
        SamplePtr current;
        SamplePtr next;
        double pos = 0;   ///< frame position in `current`
        double rate = 0;  ///< source frames per second
        float gain = 0, pan = 0.5f;
        float curL = 0, curR = 0; ///< gains applied at the end of the last block
        bool playing = false;
        bool fresh = true; ///< no block played yet: start at the target gain
        std::uint32_t buffersStarted = 0;
    };
    struct Stream {
        std::unique_ptr<AudioDecoder> decoder;
        std::vector<std::int16_t> buffer; ///< decoded frames (interleaved), from one before `pos`
        double pos = 0;                   ///< frame position in `buffer`
        bool paused = false;
        bool decoderDone = false;
        bool ended = false;
        float gain = 0, cur = 0;
        bool fresh = true;
    };

    void mixChannel(Channel& c, float* out, std::size_t frames);
    void mixStream(Stream& s, float* out, std::size_t frames);
    bool fillStream(Stream& s, std::size_t frames);

    mutable std::mutex mutex_;
    std::uint32_t outputRate_;
    float master_ = 1.0f;
    std::array<Channel, kChannels> channels_{};
    std::array<Stream, kStreams> streams_{};
};

} // namespace nox::audio
