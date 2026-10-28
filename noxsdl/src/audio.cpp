// noxsdl's sound (audio.hpp): the SDL device, its feed from noxcore's mixer, the held cues,
// the per-frame loops, and the movie's sound stream.
#include "audio.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/io/file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <map>
#include <vector>

namespace noxsdl {

namespace {

// A nox.cfg volume (0-0x4000), full when missing.
std::int32_t volumeSetting(const nox::config::Config& config, std::string_view name)
{
    const auto value = config.setting(name);
    if (!value) {
        return 0x4000;
    }
    int v = 0x4000;
    std::from_chars(value->data(), value->data() + value->size(), v);
    return std::clamp(v, 0, 0x4000);
}

} // namespace

std::unique_ptr<Audio> Audio::open(const std::filesystem::path& gameDir, const nox::things::ThingDb& things)
{
    std::optional<nox::audio::AudioBag> bag;
    try {
        bag.emplace(nox::audio::AudioBag::openGameDir(gameDir));
    } catch (const std::exception& e) {
        SDL_Log("audio: no sound effects (%s)", e.what());
        return nullptr;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio: SDL audio didn't start: %s", SDL_GetError());
        return nullptr;
    }
    // Mix at the device's own rate so SDL needn't resample again.
    SDL_AudioSpec device{};
    int deviceFrames = 0;
    std::uint32_t rate = 48000;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, &deviceFrames) && device.freq > 0) {
        rate = static_cast<std::uint32_t>(device.freq);
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(rate)};
    // The callback needs the Audio, which needs the stream: open paused, then hand it over.
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        SDL_Log("audio: no playback device: %s", SDL_GetError());
        return nullptr;
    }
    std::unique_ptr<Audio> audio(new Audio(stream, rate, gameDir, things, std::move(*bag)));
    SDL_SetAudioStreamGetCallback(stream, &Audio::feed, audio.get());
    SDL_ResumeAudioStreamDevice(stream);
    SDL_Log("audio: %u Hz, %zu sound events, %zu samples", rate,
            static_cast<std::size_t>(std::ranges::count_if(audio->events_.events, [](const auto& e) { return e.defined; })),
            audio->bag_.samples().size());
    return audio;
}

Audio::Audio(SDL_AudioStream* stream, std::uint32_t rate, const std::filesystem::path& gameDir,
             const nox::things::ThingDb& things, nox::audio::AudioBag bag)
    : stream_(stream)
    , mixer_(rate)
    , bag_(std::move(bag))
    , events_(nox::audio::buildSoundEvents(things, &bag_))
    , engine_(events_, bag_, mixer_, now())
    , music_(mixer_, gameDir, now())
    , dialog_(mixer_, gameDir, now())
    , rate_(rate)
{
}

Audio::~Audio()
{
// The device stops pulling before the mixer goes.
    SDL_DestroyAudioStream(stream_);
    // --audio-capture: what was played, written as a 16-bit stereo PCM WAV: the RIFF header, the
    // 16-byte fmt chunk (PCM, 2 channels, the rate, 4 bytes a frame), then the data.
    if (!capturePath_.empty()) {
        std::vector<std::uint8_t> out;
        auto put = [&](std::uint32_t v, int bytes) {
            for (int i = 0; i < bytes; ++i) {
                out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
            }
        };
        const auto data = static_cast<std::uint32_t>(captured_.size() * 2);
        out.insert(out.end(), {'R', 'I', 'F', 'F'});
        put(36 + data, 4);
        out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        put(16, 4);
        put(1, 2);
        put(2, 2);
        put(rate_, 4);
        put(rate_ * 4, 4);
        put(4, 2);
        put(16, 2);
        out.insert(out.end(), {'d', 'a', 't', 'a'});
        put(data, 4);
        for (const auto s : captured_) {
            put(static_cast<std::uint16_t>(s), 2);
        }
        try {
            nox::writeFileBytes(capturePath_, out);
        } catch (const std::exception& e) {
            SDL_Log("audio capture: %s", e.what());
        }
    }
}

void Audio::capture(std::filesystem::path path)
{
    std::lock_guard lock(captureMutex_);
    capturePath_ = std::move(path);
}

// SDL's pull, on its audio thread: mixes what it asks for in blocks of up to 1024 stereo
// frames (and keeps a 16-bit copy while capturing, up to ten minutes).
void SDLCALL Audio::feed(void* user, SDL_AudioStream* stream, int additional, int /*total*/)
{
    auto* self = static_cast<Audio*>(user);
    constexpr int kBlock = 1024;
    std::array<float, kBlock * 2> buffer{};
    // `additional` is in bytes: 8 to a stereo float frame.
    int frames = additional / static_cast<int>(sizeof(float) * 2);
    while (frames > 0) {
        const int n = std::min(frames, kBlock);
        self->mixer_.render(buffer.data(), static_cast<std::size_t>(n));
        {
            std::lock_guard lock(self->captureMutex_);
            if (!self->capturePath_.empty() && self->captured_.size() < std::size_t{self->rate_} * 2 * 600) {  // up to ten minutes of 16-bit stereo
                for (int i = 0; i < n * 2; ++i) {
                    self->captured_.push_back(static_cast<std::int16_t>(std::lround(std::clamp(buffer[static_cast<std::size_t>(i)], -1.0f, 1.0f) * 32767.0f)));
                }
            }
        }
        SDL_PutAudioStreamData(stream, buffer.data(), n * static_cast<int>(sizeof(float) * 2));
        frames -= n;
    }
}

void Audio::setVolumes(const nox::config::Config& config)
{
    const auto fx = volumeSetting(config, "FXVolume");
    fxVolume_ = fx;
    const auto speech = volumeSetting(config, "DialogVolume");
    const auto tunes = volumeSetting(config, "MusicVolume");
    engine_.setVolume(fx, now());
    dialog_.setVolume(speech, now());
    music_.setVolume(tunes, now());
}

void Audio::setSwitches(const Switches& on)
{
    on_ = on;
    engine_.setEnabled(on.effects);
    dialog_.setEnabled(on.dialog);
    music_.setEnabled(on.music);
}

Audio::Switches Audio::switches() const
{
    return on_;
}

void Audio::play(std::uint32_t id, int volume)
{
    if (log_) {
        SDL_Log("audio: play %u %s vol %d", id, std::string(nox::audio::soundName(id)).c_str(), volume);
    }
    engine_.play(id, volume, now());
}

void Audio::play(std::string_view name, int volume)
{
    if (const auto id = nox::audio::soundId(name)) {
        play(*id, volume);
    }
}

namespace {

/// A stream's decoder over PCM16 already in memory (a mod's sound), read where it is: `owner`
/// keeps it alive while the audio thread reads it. No copy made.
class HeldPcmDecoder final : public nox::audio::AudioDecoder {
public:
    HeldPcmDecoder(std::shared_ptr<const void> owner, std::span<const std::int16_t> samples, std::uint32_t rate, std::uint16_t channels)
        : owner_(std::move(owner)), samples_(samples), rate_(rate), channels_(channels)
    {
    }
    std::uint32_t rate() const override { return rate_; }
    std::uint16_t channels() const override { return channels_; }
    std::size_t read(std::int16_t* out, std::size_t frames) override
    {
        const std::size_t total = samples_.size() / channels_;
        const std::size_t n = std::min<std::size_t>(frames, total - std::min<std::size_t>(at_, total));
        std::copy_n(samples_.begin() + static_cast<std::ptrdiff_t>(at_ * channels_), n * channels_, out);
        at_ += n;
        return n;
    }
    void seek(std::uint64_t frame) override { at_ = static_cast<std::size_t>(std::min<std::uint64_t>(frame, samples_.size() / channels_)); }
    std::uint64_t position() const override { return at_; }

private:
    std::shared_ptr<const void> owner_;
    std::span<const std::int16_t> samples_;
    std::uint32_t rate_;
    std::uint16_t channels_;
    std::size_t at_ = 0;
};

} // namespace

void Audio::playClip(std::shared_ptr<const void> owner, std::span<const std::int16_t> samples, std::uint32_t rate, std::uint16_t channels,
                     int volume)
{
    if (!on_.effects || samples.empty() || channels < 1 || channels > 2 || rate == 0) {
        return;
    }
    // (Read where it is, `owner` kept alive by the stream: never copied.)
    mixer_.playStream(nox::audio::Mixer::kModUi, std::make_unique<HeldPcmDecoder>(std::move(owner), samples, rate, channels));
    mixer_.setStreamGain(nox::audio::Mixer::kModUi,
                         static_cast<float>(std::clamp(volume, 0, 100)) / 100.0f * static_cast<float>(fxVolume_) / 0x4000);
}

void Audio::cue(const nox::client::AudioCue& cue)
{
    cues_.push_back(cue);
}

void Audio::releaseCues(double gameTime)
{
    while (!cues_.empty() && static_cast<double>(cues_.front().frame) <= gameTime) {
        const auto c = cues_.front();
        cues_.pop_front();
        using Kind = nox::client::AudioCue::Kind;
        if (log_) {
            SDL_Log("audio: cue %d %u %s vol %u pan %d", static_cast<int>(c.kind), c.id,
                    std::string(nox::audio::soundName(c.id)).c_str(), c.volume, c.pan);
        }
        // Positional sounds carry their pan; Local ones play centred at their volume.
        switch (c.kind) {
        case Kind::Sound:
            engine_.playPanned(c.id, c.volume, c.pan, now());
            break;
        case Kind::PlayerSound:
            engine_.playPlayerPanned(c.id, c.volume, c.pan, now());
            break;
        case Kind::Local:
            engine_.play(c.id, c.volume, now());
            break;
        case Kind::Music:
            music_.set(c.id, c.volume);
            break;
        case Kind::MusicPush:
            music_.push();
            break;
        case Kind::MusicPop:
            music_.pop();
            break;
        case Kind::SpellChant:
            if (onChant) {
                onChant(static_cast<std::uint8_t>(c.id), c.frame);
            }
            break;
        }
    }
}

void Audio::music(int track, int volume)
{
    music_.set(track, volume);
}

void Audio::dialog(std::string name, int volume)
{
    dialog_.play(std::move(name), volume);
}

void Audio::stopDialog()
{
    dialog_.stop();
}

void Audio::beginLoops(const Listener& listener)
{
    listener_ = listener;
    for (auto& [_, l] : loops_) {
        l.seen = false;
    }
}

void Audio::loop(std::uint64_t key, std::uint32_t soundId, float x, float y, bool enabled)
{
    // In range, the volume falls off in a straight line to nothing at the event's
    // range; the pan follows the thing's place across the view.
    int volume = 0;
    int pan = 0;
    const int range = engine_.range(soundId);
    if (enabled && range > 0) {
        // The listener's offset from the thing, in world units.
        const auto dx = static_cast<int>(listener_.x - x);
        const auto dy = static_cast<int>(listener_.y - y);
        if (dx < range && dy < range) { // (the game's quick test, one-sided as it is)
            const auto distance = static_cast<int>(std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy + 1));
            if (distance < range) {
                volume = std::clamp((range - distance) * 100 / range, 0, 100);
                pan = static_cast<int>((x - listener_.viewCentreX) * 50.0f / std::max(1.0f, listener_.halfViewWidth));
            }
        }
    }
    Loop& l = loops_[key];
    l.seen = true;
    l.soundId = soundId;
    l.volume = volume;
    l.pan = pan;
}

void Audio::loopAt(std::uint64_t key, std::uint32_t soundId, int volume)
{
    Loop& l = loops_[key];
    l.seen = true;
    l.soundId = soundId;
    l.volume = std::clamp(volume, 0, 100);
    l.pan = 0;
}

void Audio::endLoops()
{
    const auto t = now();
    // Loops not named this frame stop: their thing went away.
    for (auto it = loops_.begin(); it != loops_.end();) {
        if (!it->second.seen) {
            engine_.stopLoop(it->second.handle, t);
            it = loops_.erase(it);
        } else {
            ++it;
        }
    }
    // Per sound, the kLoopsPerSound loudest heard play; one already playing counts a little
    // louder, so near-equal ones don't swap back and forth (each swap restarts the loop).
    std::map<std::uint32_t, std::vector<Loop*>> bySound;
    for (auto& [_, l] : loops_) {
        if (l.volume > 0) {
            bySound[l.soundId].push_back(&l);
        } else if (l.handle.serial != 0) {
            engine_.driveLoop(l.handle, l.soundId, 0, l.pan, t); // out of hearing: stops
        }
    }
    for (auto& [_, group] : bySound) {
        const auto score = [](const Loop* l) { return l->volume + (l->handle.serial != 0 ? 15 : 0); };  // the playing bonus: 15 volume points
        std::ranges::stable_sort(group, [&](const Loop* a, const Loop* b) { return score(a) > score(b); });
        for (std::size_t i = 0; i < group.size(); ++i) {
            Loop& l = *group[i];
            if (i < kLoopsPerSound) {
                engine_.driveLoop(l.handle, l.soundId, l.volume, l.pan, t);
            } else if (l.handle.serial != 0) {
                engine_.stopLoop(l.handle, t); // (one of many the same, further off: the near ones carry it)
            }
        }
    }
}

void Audio::stopEffects()
{
    engine_.stopAll();
    loops_.clear();
    cues_.clear();
    dialog_.stop();
    mixer_.stopStream(nox::audio::Mixer::kModUi);
}

void Audio::enterGame()
{
    stopEffects();
    music_.pushLevel();
    music_.stop();
}

void Audio::leaveGame()
{
    stopEffects();
    music_.stop();
    music_.popLevel();
}

void Audio::beginMovie()
{
    // The movie's start: the samples stopped, the music held, the speech stopped.
    stopEffects();
    music_.hold(true);
    holdingForMovie_ = true;
}

bool Audio::quietForMovie() const
{
    // The movie waits while the music is playing or fading, or speech is.
    return !music_.busy() && !dialog_.busy();
}

void Audio::playMovieSound(std::unique_ptr<nox::audio::AudioDecoder> decoder)
{
    mixer_.playStream(nox::audio::Mixer::kMovie, std::move(decoder));
    mixer_.setStreamGain(nox::audio::Mixer::kMovie, 1.0f);
}

Audio::MovieSound Audio::movieSound() const
{
    const auto st = mixer_.streamStatus(nox::audio::Mixer::kMovie);
    return {st.open, st.ended, st.position};
}

void Audio::endMovie()
{
    // The movie's end: VQA_Close's sound gone, the music let go.
    mixer_.stopStream(nox::audio::Mixer::kMovie);
    if (std::exchange(holdingForMovie_, false)) {
        music_.hold(false);
    }
}

void Audio::update()
{
    const auto t = now();
    engine_.update(t);
    music_.update(t);
    dialog_.update(t);
}

} // namespace noxsdl
