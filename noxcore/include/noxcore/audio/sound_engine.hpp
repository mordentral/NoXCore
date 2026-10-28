// The client's sound-effect engine, as GAME.EXE's AudEvent.c runs it over Miles, on our mixer.
//
// A play request (Snd_PlaySound and friends) makes a voice for the sound event. Once a
// frame:
//   1. each event's voices are ranked by volume and anything over its maxInstances is dropped:
//      the quietest, and among voices of about the same volume the newest, unless
//      the event's flag 0x10 says the newest displaces an old one;
//   2. new voices claim one of the 16 channels: a free one, else the lowest-priority
//      one (the quietest of those that are playing), but never one of higher priority
//      than the new sound's (the event's priority, +2 for the sounds a player makes);
//   3. voices that lost their channel stop, and when the events playing add up loud (their
//      volumes x 33 / 0x4000 summed past 100) every sound effect is turned down to fit, gliding
//      over up to a second;
//   4. voices with a channel load their files and start.
// A voice plays one file of its event (a random one with flag 2), or with flag 4 all of them in
// turn; flag 1 loops (`loops` times, or forever), and a delay range puts a random pause between
// plays (flag 8 also before the first). Files follow each other without a gap when there's no
// pause (Miles' end-of-buffer callback).
//
// Volume, pan and pitch are the exe's ramps (ramp.hpp): a voice's volume and pan, the channel's
// random pitch, the sound-effects master (FXVolume in nox.cfg) and the ducking level multiply
// into what the mixer plays.
//
// Not copied: Miles' 1 MB sample cache, and the eviction of lower-priority sounds when it is
// full: samples are decoded once and kept while used.
#pragma once

#include "noxcore/audio/mixer.hpp"
#include "noxcore/audio/ramp.hpp"
#include "noxcore/audio/sound_events.hpp"

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <random>
#include <vector>

namespace nox::audio {

class AudioBag;

class SoundEngine {
public:
    /// `bag` and `mixer` must outlive the engine. `nowMs` is the millisecond clock every call
    /// takes (the game's).
    SoundEngine(const SoundEventTable& events, AudioBag& bag, Mixer& mixer, std::uint64_t nowMs);

    /// The sound-effects master, 0-0x4000 (nox.cfg FXVolume). Applies at once.
    void setVolume(std::int32_t volume, std::uint64_t nowMs);
    /// Sound effects on or off (Options' checkbox). Off stops every voice.
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    /// Snd_PlaySound: `volume` 0-100, centred.
    void play(std::uint32_t id, int volume, std::uint64_t nowMs);
    /// MSG_AUDIO_EVENT: `pan` -50 (left) to 50 (right).
    void playPanned(std::uint32_t id, int volume, int pan, std::uint64_t nowMs);
    /// MSG_AUDIO_PLAYER_EVENT: a sound we made ourselves, two priorities higher.
    void playPlayerPanned(std::uint32_t id, int volume, int pan, std::uint64_t nowMs);

    /// A looping sound tied to something that updates it every frame (a map object's AUDIOLOOP).
    /// Holds the voice's serial; a stopped voice's serial is 0, so the handle lapses.
    struct LoopHandle {
        std::uint32_t serial = 0;
        std::uint32_t id = 0;
    };
    /// Starts the loop at `volume` (0-100) and `pan` (-50-50), or glides an existing one there;
    /// volume 0 stops it.
    void driveLoop(LoopHandle& handle, std::uint32_t id, int volume, int pan, std::uint64_t nowMs);
    /// Stops it (a drawable going away).
    void stopLoop(LoopHandle& handle, std::uint64_t nowMs);

    /// Once a frame (plus the channel update Miles' timer runs).
    void update(std::uint64_t nowMs);
    /// Stops every sound.
    void stopAll();

    /// How far a map object's loop of this sound carries, 0 if none.
    std::int32_t range(std::uint32_t id) const;
    const SoundEventTable& events() const { return events_; }

    struct Stats {
        int voices = 0;
        int channelsBusy = 0;
        std::int32_t duck = 0x4000;
        std::size_t cachedSamples = 0;
    };
    Stats stats() const;

private:
    enum class State : std::uint8_t { New = 0, Ready = 1, Delayed = 2, Playing = 3, Done = 4 };

    struct Voice {
        std::uint32_t serial = 0;
        std::uint32_t id = 0;
        const SoundEventDef* def = nullptr;
        State state = State::New;
        State afterDelay = State::Ready;
        bool stopped = false; ///< flag 1
        bool started = false; ///< flag 2: its channel is playing for it
        int channel = -1;
        int bonus = 0;        ///< priority added (2 for a player's own sounds)
        RampSet ramps;        ///< the voice's volume, pitch and pan ramps
        std::vector<SamplePtr> loaded; ///< the files this play uses
        int current = -1;              ///< index into `loaded`
        std::vector<int> bag;          ///< what's left to play this time through
        int loops = 0;                 ///< passes made, against the event's `loops`
        std::vector<int> shuffle;      ///< flag 4 with a pause, one file per play
        SamplePtr pending;             ///< the next file, held for after a pause
        std::uint32_t delay = 0;       ///< pause after the current play
        std::uint64_t due = 0;         ///< when a Delayed voice moves on
    };

    struct Channel {
        Voice* owner = nullptr;
        bool claimed = false; ///< flag 0x10
        bool playing = false; ///< flag 1
        std::int32_t priority = 0;
        RampSet own;          ///< the random pitch
        std::int32_t finalVolume = 0; ///< for picking which to take over
        std::uint32_t buffersSeen = 0;
        std::uint32_t sampleRate = 22050;
        std::uint32_t queuedRate = 22050; ///< of the buffer queued after the current one
    };

    struct EventRuntime {
        std::uint64_t frame = 0;   ///< the frame its list was last cleared
        std::list<Voice*> ranked;  ///< this frame's voices of the event, by priority
    };

    Voice* newVoice(std::uint32_t id, std::uint64_t nowMs);
    Voice* find(const LoopHandle& h);
    void setVoiceVolume(Voice& v, int volume, bool glide, std::uint64_t nowMs);
    void setVoicePan(Voice& v, int pan, bool glide, std::uint64_t nowMs);
    void rank(Voice& v, std::uint64_t nowMs);
    void step(Voice& v, std::uint64_t nowMs);
    bool claimChannel(Voice& v, std::uint64_t nowMs);
    int pickChannel() const;
    void endChannel(int channel, std::uint64_t nowMs);
    void releaseChannel(Voice& v, std::uint64_t nowMs);
    void stop(Voice& v, std::uint64_t nowMs);
    void loadFiles(Voice& v);
    int shufflePick(Voice& v);
    SamplePtr restart(Voice& v);
    SamplePtr next(Voice& v);
    bool start(Voice& v, std::uint64_t nowMs);
    void bufferStarted(int channel);
    void ended(int channel, std::uint64_t nowMs);
    void applyChannel(int channel, std::uint64_t nowMs);
    SamplePtr sample(std::uint16_t bagIndex);
    int random(int lo, int hi);
    std::int32_t priorityOf(const Voice& v) const { return v.def->priority + v.bonus; }

    const SoundEventTable& events_;
    AudioBag& bag_;
    Mixer& mixer_;
    bool enabled_ = true;
    RampSet master_;  ///< FXVolume
    RampSet device_;  ///< the device's own level, full
    RampSet duck_;
    std::list<Voice> voices_;
    std::array<Channel, Mixer::kChannels> channels_{};
    std::array<EventRuntime, 1023> runtime_{};
    std::uint64_t frame_ = 0;
    std::uint32_t nextSerial_ = 1;
    std::map<std::uint16_t, SamplePtr> cache_;
    std::size_t cacheBytes_ = 0;
    std::mt19937 rng_{0x4E6F78};

    static constexpr std::size_t kMaxVoices = 200;    ///< the exe's voice pool
    static constexpr std::size_t kCacheBytes = 64u << 20;
};

} // namespace nox::audio
