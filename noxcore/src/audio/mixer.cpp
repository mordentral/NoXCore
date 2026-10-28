// The software mixer's channels and streams; see mixer.hpp for how the game drives it.
#include "noxcore/audio/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nox::audio {

namespace {

// 16-bit samples to floats in [-1, 1).
constexpr float kScale = 1.0f / 32768.0f;

/// Catmull-Rom through x1 and x2, at t in [0, 1).
float cubic(float x0, float x1, float x2, float x3, float t)
{
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

/// Left / right gains for a balance pan in [0, 1].
void balance(float gain, float pan, float& left, float& right)
{
    // Full volume on both sides up to the centre, then the far side fades out.
    left = gain * std::min(1.0f, 2.0f * (1.0f - pan));
    right = gain * std::min(1.0f, 2.0f * pan);
}

/// Soft knee above 0.8 so many loud sounds at once round off instead of clipping hard.
float limit(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.8f) {
        return x;
    }
    const float over = a - 0.8f;
    const float y = 0.8f + 0.2f * std::tanh(over / 0.2f);
    return x < 0 ? -y : y;
}

} // namespace

Mixer::Mixer(std::uint32_t outputRate)
    : outputRate_(outputRate)
{
}

void Mixer::play(int channel, SamplePtr sample)
{
    std::lock_guard lock(mutex_);
    Channel& c = channels_.at(static_cast<std::size_t>(channel));
    c.current = std::move(sample);
    c.next.reset();
    c.pos = 0;
    c.playing = c.current && c.current->frames() > 0;
    c.fresh = true;
    c.buffersStarted = c.playing ? 1 : 0;
    // A channel whose rate was never set plays at the sample's own rate.
    if (c.playing && c.rate <= 0) {
        c.rate = c.current->rate;
    }
}

void Mixer::queue(int channel, SamplePtr next)
{
    std::lock_guard lock(mutex_);
    channels_.at(static_cast<std::size_t>(channel)).next = std::move(next);
}

void Mixer::stop(int channel)
{
    std::lock_guard lock(mutex_);
    Channel& c = channels_.at(static_cast<std::size_t>(channel));
    c.playing = false;
    c.current.reset();
    c.next.reset();
}

void Mixer::setParams(int channel, float gain, float pan, double rateHz)
{
    std::lock_guard lock(mutex_);
    Channel& c = channels_.at(static_cast<std::size_t>(channel));
    c.gain = std::clamp(gain, 0.0f, 4.0f);
    c.pan = std::clamp(pan, 0.0f, 1.0f);
    c.rate = std::max(rateHz, 1.0);
}

Mixer::ChannelStatus Mixer::status(int channel) const
{
    std::lock_guard lock(mutex_);
    const Channel& c = channels_.at(static_cast<std::size_t>(channel));
    return {c.playing, c.buffersStarted};
}

void Mixer::playStream(int slot, std::unique_ptr<AudioDecoder> decoder, bool paused)
{
    std::lock_guard lock(mutex_);
    Stream& s = streams_.at(static_cast<std::size_t>(slot));
    s = Stream{};
    s.decoder = std::move(decoder);
    s.paused = paused;
    s.buffer.reserve(8192);
}

void Mixer::stopStream(int slot)
{
    std::lock_guard lock(mutex_);
    streams_.at(static_cast<std::size_t>(slot)) = Stream{};
}

void Mixer::pauseStream(int slot, bool paused)
{
    std::lock_guard lock(mutex_);
    streams_.at(static_cast<std::size_t>(slot)).paused = paused;
}

void Mixer::setStreamGain(int slot, float gain)
{
    std::lock_guard lock(mutex_);
    streams_.at(static_cast<std::size_t>(slot)).gain = std::clamp(gain, 0.0f, 4.0f);
}

Mixer::StreamStatus Mixer::streamStatus(int slot) const
{
    std::lock_guard lock(mutex_);
    const Stream& s = streams_.at(static_cast<std::size_t>(slot));
    if (!s.decoder) {
        return {};
    }
    // Frames decoded but not yet played don't count: what's heard is where it is.
    const std::size_t channels = s.decoder->channels();
    const std::size_t buffered = s.buffer.size() / channels;
    const double ahead = std::max(0.0, static_cast<double>(buffered) - s.pos);
    const std::uint64_t decoded = s.decoder->position();
    return {true, s.ended, decoded - std::min<std::uint64_t>(decoded, static_cast<std::uint64_t>(ahead))};
}

void Mixer::setMasterGain(float gain)
{
    std::lock_guard lock(mutex_);
    master_ = std::clamp(gain, 0.0f, 4.0f);
}

// Runs on the audio thread: clears `out`, adds every playing channel and stream, then applies
// the master gain and the soft limiter.
void Mixer::render(float* out, std::size_t frames)
{
    std::lock_guard lock(mutex_);
    std::fill(out, out + frames * 2, 0.0f);
    for (auto& c : channels_) {
        if (c.playing) {
            mixChannel(c, out, frames);
        }
    }
    for (auto& s : streams_) {
        if (s.decoder && !s.paused && !s.ended) {
            mixStream(s, out, frames);
        }
    }
    for (std::size_t i = 0; i < frames * 2; ++i) {
        out[i] = limit(out[i] * master_);
    }
}

// Resamples one channel into `out` with cubic interpolation, gliding its gains from last
// block's to the current target across the block.
void Mixer::mixChannel(Channel& c, float* out, std::size_t frames)
{
    float targetL = 0;
    float targetR = 0;
    balance(c.gain, c.pan, targetL, targetR);
    // A channel's first block starts at its target gains instead of gliding up from silence.
    if (c.fresh) {
        c.curL = targetL;
        c.curR = targetR;
        c.fresh = false;
    }
    const float stepL = (targetL - c.curL) / static_cast<float>(frames);
    const float stepR = (targetR - c.curR) / static_cast<float>(frames);
    float gl = c.curL;
    float gr = c.curR;
    // Source frames advanced per output frame.
    const double step = c.rate / outputRate_;

    for (std::size_t f = 0; f < frames; ++f) {
        const Pcm& a = *c.current;
        const auto len = static_cast<std::ptrdiff_t>(a.frames());
        const auto ip = static_cast<std::ptrdiff_t>(c.pos);
        const float t = static_cast<float>(c.pos - static_cast<double>(ip));
        // A sample frame by index: before the start repeats frame 0, past the end reads into the queued
        // buffer (so a queued sample continues seamlessly), then silence. Mono reads channel 0 for both.
        auto get = [&](std::ptrdiff_t i, int ch) -> float {
            if (i < 0) {
                i = 0;
            }
            if (i < len) {
                return a.samples[static_cast<std::size_t>(i) * a.channels + static_cast<std::size_t>(std::min<int>(ch, a.channels - 1))];
            }
            if (c.next) {
                const Pcm& b = *c.next;
                const std::size_t j = static_cast<std::size_t>(i - len);
                if (j < b.frames()) {
                    return b.samples[j * b.channels + static_cast<std::size_t>(std::min<int>(ch, b.channels - 1))];
                }
            }
            return 0.0f;
        };
        // Interpolate between source frames ip and ip + 1 (t is the fraction), using ip - 1 and ip + 2 for the curve.
        const float left = cubic(get(ip - 1, 0), get(ip, 0), get(ip + 1, 0), get(ip + 2, 0), t) * kScale;
        const float right = a.channels == 1
                                ? left
                                : cubic(get(ip - 1, 1), get(ip, 1), get(ip + 1, 1), get(ip + 2, 1), t) * kScale;
        gl += stepL;
        gr += stepR;
        out[2 * f] += left * gl;
        out[2 * f + 1] += right * gr;

        c.pos += step;
        // Past the end: move on to the queued buffer, or stop the channel.
        if (c.pos >= static_cast<double>(len)) {
            if (c.next) {
                c.pos -= static_cast<double>(len);
                c.current = std::move(c.next);
                c.next.reset();
                ++c.buffersStarted;
                // A queued sample at another rate: the engine sets the new rate next frame.
            } else {
                c.playing = false;
                c.current.reset();
                break;
            }
        }
    }
    c.curL = targetL;
    c.curR = targetR;
}

// Decodes until the buffer holds enough frames for this block (plus the interpolation's
// neighbours); false if there is nothing to play.
bool Mixer::fillStream(Stream& s, std::size_t frames)
{
    const std::size_t channels = s.decoder->channels();
    const double step = static_cast<double>(s.decoder->rate()) / outputRate_;
    const auto needed = static_cast<std::size_t>(s.pos + static_cast<double>(frames) * step) + 3;
    std::int16_t chunk[2048 * 2];
    while (!s.decoderDone && s.buffer.size() / channels < needed) {
        const std::size_t n = s.decoder->read(chunk, 2048);
        if (n == 0) {
            s.decoderDone = true;
            break;
        }
        s.buffer.insert(s.buffer.end(), chunk, chunk + n * channels);
    }
    return !s.buffer.empty();
}

// Resamples a stream into `out`, gliding its gain like the channels.
void Mixer::mixStream(Stream& s, float* out, std::size_t frames)
{
    if (!fillStream(s, frames)) {
        s.ended = s.decoderDone;
        return;
    }
    const std::size_t channels = s.decoder->channels();
    const auto len = static_cast<std::ptrdiff_t>(s.buffer.size() / channels);
    const double step = static_cast<double>(s.decoder->rate()) / outputRate_;
    if (s.fresh) {
        s.cur = s.gain;
        s.fresh = false;
    }
    const float gstep = (s.gain - s.cur) / static_cast<float>(frames);
    float g = s.cur;
    auto get = [&](std::ptrdiff_t i, std::size_t ch) -> float {
        if (i < 0 || i >= len) {
            return 0.0f;
        }
        return s.buffer[static_cast<std::size_t>(i) * channels + std::min(ch, channels - 1)];
    };
    for (std::size_t f = 0; f < frames; ++f) {
        const auto ip = static_cast<std::ptrdiff_t>(s.pos);
        if (ip >= len) {
            if (s.decoderDone) {
                s.ended = true;
            }
            break;
        }
        const float t = static_cast<float>(s.pos - static_cast<double>(ip));
        const float left = cubic(get(ip - 1, 0), get(ip, 0), get(ip + 1, 0), get(ip + 2, 0), t) * kScale;
        const float right =
            channels == 1 ? left : cubic(get(ip - 1, 1), get(ip, 1), get(ip + 1, 1), get(ip + 2, 1), t) * kScale;
        g += gstep;
        out[2 * f] += left * g;
        out[2 * f + 1] += right * g;
        s.pos += step;
    }
    s.cur = s.gain;
    // Keep one frame before the position for the next block's interpolation.
    const auto drop = std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(s.pos) - 1);
    const auto dropped = std::min(drop, len);
    s.buffer.erase(s.buffer.begin(), s.buffer.begin() + dropped * static_cast<std::ptrdiff_t>(channels));
    s.pos -= static_cast<double>(dropped);
}

} // namespace nox::audio
