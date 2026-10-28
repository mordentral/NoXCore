// The sound-effect engine's voices and channels; the overall scheme is described in sound_engine.hpp.
#include "noxcore/audio/sound_engine.hpp"

#include "noxcore/audio/audio_bag.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cstdlib>

namespace nox::audio {

namespace {

/// A combined volume / pitch / pan, as folds the ramps together.
struct Mix {
    std::int32_t volume = RampSet::kFull;
    std::int32_t pitch = 100;
    std::int32_t pan = RampSet::kCentre;

    // Volumes multiply as fractions of 0x4000, pitches as percentages, pans add their offsets from centre.
    void by(const RampSet& r)
    {
        volume = static_cast<std::int32_t>(
            (static_cast<std::uint32_t>(volume) * static_cast<std::uint32_t>(r.volume.value())) >> 14);
        pitch = pitch * r.pitch.value() / 100;
        if (const std::int32_t offset = r.pan.value() - RampSet::kCentre; offset != 0) {
            pan = std::clamp(pan + offset, 0, RampSet::kFull);
        }
    }
};

constexpr std::uint32_t kShortPause = 0x20; // pauses this short (ms) run files together

} // namespace

SoundEngine::SoundEngine(const SoundEventTable& events, AudioBag& bag, Mixer& mixer, std::uint64_t nowMs)
    : events_(events)
    , bag_(bag)
    , mixer_(mixer)
{
    master_.init(nowMs);
    device_.init(nowMs);
    duck_.init(nowMs);
    for (auto& c : channels_) {
        c.own.init(nowMs);
    }
}

void SoundEngine::setVolume(std::int32_t volume, std::uint64_t nowMs)
{
    master_.volume.set(std::clamp(volume, 0, RampSet::kFull));
    master_.update(nowMs);
}

void SoundEngine::setEnabled(bool on)
{
    enabled_ = on;
}

// A random int in [lo, hi], both ends included.
int SoundEngine::random(int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }
    return nox::util::uniform_int_distribution<int>(lo, hi)(rng_);
}

std::int32_t SoundEngine::range(std::uint32_t id) const
{
    const SoundEventDef* def = events_.find(id);
    return def && def->defined ? def->range : 0;
}

SoundEngine::Stats SoundEngine::stats() const
{
    Stats s;
    for (const auto& v : voices_) {
        s.voices += v.stopped ? 0 : 1;
    }
    for (const auto& c : channels_) {
        s.channelsBusy += (c.claimed || c.playing) ? 1 : 0;
    }
    s.duck = duck_.volume.value();
    s.cachedSamples = cache_.size();
    return s;
}

// The decoded Audio.bag sample, cached by bag index; nullptr if it can't be decoded or is empty.
SamplePtr SoundEngine::sample(std::uint16_t bagIndex)
{
    if (const auto it = cache_.find(bagIndex); it != cache_.end()) {
        return it->second;
    }
    SamplePtr pcm;
    try {
        pcm = std::make_shared<const Pcm>(bag_.decode(bagIndex));
    } catch (const std::exception&) {
        return nullptr; // a damaged or missing sample: the sound just doesn't play
    }
    if (pcm->frames() == 0) {
        return nullptr;
    }
    // Over budget: let go of samples nothing plays.
    if (cacheBytes_ > kCacheBytes) {
        for (auto it = cache_.begin(); it != cache_.end() && cacheBytes_ > kCacheBytes / 2;) {
            if (it->second.use_count() == 1) {
                cacheBytes_ -= it->second->samples.size() * sizeof(std::int16_t);
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    cacheBytes_ += pcm->samples.size() * sizeof(std::int16_t);
    cache_.emplace(bagIndex, pcm);
    return pcm;
}

// ---- making voices (draw)

// A new voice for event `id` (nullptr if sounds are off, the event is undefined or the pool is full).
// Serials skip 0, which marks a stopped voice.
SoundEngine::Voice* SoundEngine::newVoice(std::uint32_t id, std::uint64_t nowMs)
{
    const SoundEventDef* def = events_.find(id);
    if (!enabled_ || !def || !def->defined) {
        return nullptr;
    }
    if (voices_.size() >= kMaxVoices) {
        voices_.remove_if([](const Voice& v) { return v.stopped; });
        if (voices_.size() >= kMaxVoices) {
            return nullptr;
        }
    }
    Voice& v = voices_.emplace_back();
    v.id = id;
    v.def = def;
    v.ramps.init(nowMs);
    v.serial = nextSerial_++;
    if (nextSerial_ == 0) {
        nextSerial_ = 1;
    }
    return &v;
}

void SoundEngine::setVoiceVolume(Voice& v, int volume, bool glide, std::uint64_t nowMs)
{
    // the event's own volume scaled by 0-100 (x163 / 0x4000).
    const std::uint32_t pct = static_cast<std::uint32_t>(std::clamp(volume, 0, 100));
    const auto level = static_cast<std::int32_t>((static_cast<std::uint32_t>(v.def->volume) * pct * 163) >> 14);
    if (glide) {
        v.ramps.volume.moveTo(level, nowMs);
    } else {
        v.ramps.volume.set(level);
        v.ramps.volume.update(nowMs);
    }
}

void SoundEngine::setVoicePan(Voice& v, int pan, bool glide, std::uint64_t nowMs)
{
    // -50..50 to 0..0x4000.
    const int level = (std::clamp(pan, -50, 50) << 13) / 50 + RampSet::kCentre;
    if (glide) {
        v.ramps.pan.moveTo(level, nowMs);
    } else {
        v.ramps.pan.set(level);
    }
}

void SoundEngine::play(std::uint32_t id, int volume, std::uint64_t nowMs)
{
    if (Voice* v = newVoice(id, nowMs)) {
        setVoiceVolume(*v, volume, false, nowMs);
        step(*v, nowMs);
    }
}

void SoundEngine::playPanned(std::uint32_t id, int volume, int pan, std::uint64_t nowMs)
{
    if (Voice* v = newVoice(id, nowMs)) {
        setVoiceVolume(*v, volume, false, nowMs);
        setVoicePan(*v, pan, false, nowMs);
        step(*v, nowMs);
    }
}

void SoundEngine::playPlayerPanned(std::uint32_t id, int volume, int pan, std::uint64_t nowMs)
{
    if (Voice* v = newVoice(id, nowMs)) {
        setVoiceVolume(*v, volume, false, nowMs);
        setVoicePan(*v, pan, false, nowMs);
        v->bonus = 2;
        step(*v, nowMs);
    }
}

// Finds the voice a loop handle refers to.
SoundEngine::Voice* SoundEngine::find(const LoopHandle& h)
{
    // the handle holds while the voice keeps its serial and event.
    if (h.serial == 0) {
        return nullptr;
    }
    for (auto& v : voices_) {
        if (v.serial == h.serial) {
            return v.id == h.id ? &v : nullptr;
        }
    }
    return nullptr;
}

// Called every frame by whatever owns the loop: starts it, retargets its volume and pan, or stops it.
void SoundEngine::driveLoop(LoopHandle& handle, std::uint32_t id, int volume, int pan, std::uint64_t nowMs)
{
    Voice* v = find(handle);
    if (v && v->id != id) {
        stop(*v, nowMs);
        v = nullptr;
    }
    if (volume <= 0) {
        if (v) {
            stop(*v, nowMs);
        }
        handle = {};
        return;
    }
    if (!v) {
        v = newVoice(id, nowMs);
        if (!v) {
            handle = {};
            return;
        }
        setVoiceVolume(*v, volume, false, nowMs);
        setVoicePan(*v, pan, false, nowMs);
        handle = {v->serial, id};
        return; // it claims a channel in the next update
    }
    setVoicePan(*v, pan, true, nowMs);
    setVoiceVolume(*v, volume, true, nowMs);
}

void SoundEngine::stopLoop(LoopHandle& handle, std::uint64_t nowMs)
{
    if (Voice* v = find(handle)) {
        stop(*v, nowMs);
    }
    handle = {};
}

// ---- the voice's life

void SoundEngine::step(Voice& v, std::uint64_t nowMs)
{
    // Moves a voice on as far as it can now: New claims a channel, Delayed waits for `due`,
    // Done stops. Ready and Playing are handled by update().
    if (!enabled_) {
        v.state = State::Done;
    }
    for (;;) {
        switch (v.state) {
        case State::New:
            if (!claimChannel(v, nowMs)) {
                stop(v, nowMs);
            }
            return;
        case State::Delayed:
            if (nowMs <= v.due) {
                return;
            }
            v.state = v.afterDelay;
            continue;
        case State::Done:
            stop(v, nowMs);
            return;
        default:
            return;
        }
    }
}

int SoundEngine::pickChannel() const
{
    // a free channel; else the lowest priority, preferring among the playing ones
    // the quietest (by a margin of 0x665 of 0x4000) unless one that's only waiting is lower still.
    int playingPick = -1;
    std::int32_t playingPriority = 0x7F;
    std::int32_t playingVolume = 0x7FFFFFFF;
    int waitingPick = -1;
    std::int32_t waitingPriority = 0x7F;
    for (int i = 0; i < Mixer::kChannels; ++i) {
        const Channel& c = channels_[static_cast<std::size_t>(i)];
        if (!c.claimed && !c.playing) {
            return i;
        }
        if (!c.playing) {
            if (c.priority < waitingPriority) {
                waitingPriority = c.priority;
                waitingPick = i;
            }
        } else if (c.priority < playingPriority) {
            playingPick = i;
            playingVolume = c.finalVolume;
            playingPriority = c.priority;
        } else if (c.priority == playingPriority && c.finalVolume < playingVolume &&
                   playingVolume - c.finalVolume > 0x665) {
            playingPick = i;
            playingVolume = c.finalVolume;
        }
    }
    if (waitingPick < 0 || playingPriority < waitingPriority) {
        return playingPick;
    }
    return waitingPick;
}

bool SoundEngine::claimChannel(Voice& v, std::uint64_t nowMs)
{
    // Takes a channel for the voice; false if there is none, or the one picked plays something of
    // higher priority. Sets the channel's random pitch and the event's optional first pause.
    if (v.def->samples.empty()) {
        return false;
    }
    v.loops = 0;
    const std::int32_t priority = priorityOf(v);
    const int index = pickChannel();
    if (index < 0) {
        return false;
    }
    Channel& c = channels_[static_cast<std::size_t>(index)];
    if ((c.claimed || c.playing) && priority < c.priority) {
        return false;
    }
    endChannel(index, nowMs); // whoever had it is done
    if (c.owner && c.owner != &v) {
        c.owner->channel = -1;
    }
    c.priority = priority;
    c.own.volume.set(RampSet::kFull);
    c.own.pitch.set(random(v.def->pitchMin, v.def->pitchMax) + 100);
    c.own.update(nowMs);
    c.claimed = true;
    c.owner = &v;
    v.channel = index;
    v.state = State::Ready;
    if (v.def->flags & SoundEventDef::kDelay) {
        const int pause = random(static_cast<int>(v.def->delayMin), static_cast<int>(v.def->delayMax));
        if (pause > 0x21) {
            v.due = nowMs + static_cast<std::uint64_t>(pause);
            v.afterDelay = State::Ready;
            v.state = State::Delayed;
        }
    }
    return true;
}

void SoundEngine::endChannel(int index, std::uint64_t nowMs)
{
    // stop the channel; its owner hears the end and then the stop.
    Channel& c = channels_[static_cast<std::size_t>(index)];
    if (c.playing) {
        mixer_.stop(index);
        ended(index, nowMs);
    }
    if (c.owner) {
        c.owner->state = State::Done;
    }
}

void SoundEngine::releaseChannel(Voice& v, std::uint64_t nowMs)
{
    // Lets go of the voice's channel, stopping it first if it is playing for this voice.
    if (v.channel < 0) {
        return;
    }
    Channel& c = channels_[static_cast<std::size_t>(v.channel)];
    if (c.owner == &v) {
        if (v.started) {
            endChannel(v.channel, nowMs);
        }
        c.claimed = false;
        c.owner = nullptr;
    }
    v.channel = -1;
}

void SoundEngine::stop(Voice& v, std::uint64_t nowMs)
{
    // Ends the voice for good: channel released, files dropped, serial 0 so loop handles lapse.
    // update() erases stopped voices.
    if (v.stopped) {
        return;
    }
    releaseChannel(v, nowMs);
    v.loaded.clear();
    v.pending.reset();
    v.state = State::Done;
    v.serial = 0;
    v.stopped = true;
}

void SoundEngine::stopAll()
{
    for (auto& v : voices_) {
        stop(v, 0);
    }
    voices_.clear();
}

// ---- which file plays

// Decodes the files this play of the voice uses into v.loaded: one file (the first, or a random
// one), the next in the shuffle when sequenced with a real pause, or all of them in order.
// Without a real pause, files already loaded are kept.
void SoundEngine::loadFiles(Voice& v)
{
    const SoundEventDef& def = *v.def;
    if (!v.loaded.empty()) {
        if (def.delayMin <= kShortPause) {
            return;
        }
        v.loaded.clear();
    }
    auto load = [&](int i) {
        if (i >= 0 && static_cast<std::size_t>(i) < def.samples.size()) {
            if (auto s = sample(def.samples[static_cast<std::size_t>(i)])) {
                v.loaded.push_back(std::move(s));
            }
        }
    };
    const int count = static_cast<int>(def.samples.size());
    if (!(def.flags & SoundEventDef::kSequence)) {
        load((def.flags & SoundEventDef::kRandom) ? random(0, count - 1) : 0);
    } else if (def.delayMin > kShortPause) {
        load(shufflePick(v));
    } else {
        for (int i = 0; i < count; ++i) {
            load(i);
        }
    }
}

// The next file of a sequenced event that pauses between files: in order, or drawn at random
// without repeats until all have played.
int SoundEngine::shufflePick(Voice& v)
{
    if (v.shuffle.empty()) {
        const int count = static_cast<int>(v.def->samples.size());
        for (int i = count - 1; i >= 0; --i) {
            v.shuffle.push_back(i); // n-1 ... 0, taken from the back: 0, 1, 2 ...
        }
    }
    if (!(v.def->flags & SoundEventDef::kRandom)) {
        const int pick = v.shuffle.back();
        v.shuffle.pop_back();
        return pick;
    }
    const int at = random(0, static_cast<int>(v.shuffle.size()) - 1);
    const int pick = v.shuffle[static_cast<std::size_t>(at)];
    v.shuffle.erase(v.shuffle.begin() + at);
    return pick;
}

// Starts a pass over the loaded files and returns the first.
SamplePtr SoundEngine::restart(Voice& v)
{
    v.bag.clear();
    for (int i = 0; i < static_cast<int>(v.loaded.size()); ++i) {
        v.bag.push_back(i);
    }
    v.current = -1;
    return next(v);
}

// The next loaded file of this pass; at its end a looping event starts a new pass (until
// `loops` passes when that's set); nullptr when nothing follows.
SamplePtr SoundEngine::next(Voice& v)
{
    if (!v.bag.empty()) {
        if (!(v.def->flags & SoundEventDef::kRandom)) {
            ++v.current;
            v.bag.pop_back();
        } else {
            const int at = random(0, static_cast<int>(v.bag.size()) - 1);
            v.current = v.bag[static_cast<std::size_t>(at)];
            v.bag.erase(v.bag.begin() + at);
        }
        return v.loaded.at(static_cast<std::size_t>(v.current));
    }
    if (v.def->flags & SoundEventDef::kLoop) {
        if (v.def->loops != 0 && ++v.loops >= v.def->loops) {
            return nullptr;
        }
        return restart(v);
    }
    return nullptr;
}

bool SoundEngine::start(Voice& v, std::uint64_t nowMs)
{
    // Starts the voice's pending file on its channel; false if the channel isn't its own, is
    // already playing, or nothing is pending.
    if (v.channel < 0) {
        return false;
    }
    Channel& c = channels_[static_cast<std::size_t>(v.channel)];
    if (c.owner != &v || c.playing || !v.pending) {
        return false;
    }
    SamplePtr first = std::move(v.pending);
    c.sampleRate = first->rate;
    c.buffersSeen = 0;
    v.state = State::Playing;
    v.started = true;
    applyChannel(v.channel, nowMs);
    mixer_.play(v.channel, std::move(first));
    c.playing = true;
    bufferStarted(v.channel);
    return true;
}

void SoundEngine::bufferStarted(int index)
{
    // Miles asks for the next buffer as it runs out of this one; we
    // decide as soon as a buffer starts, so the mixer has the next one in hand.
    Channel& c = channels_[static_cast<std::size_t>(index)];
    ++c.buffersSeen;
    Voice* v = c.owner;
    if (!v) {
        return;
    }
    SamplePtr following = next(*v);
    if (following) {
        c.queuedRate = following->rate;
    }
    // No pause possible: queue the next file straight behind this one.
    if (v->def->delayMax <= kShortPause) {
        mixer_.queue(index, std::move(following));
        return;
    }
    mixer_.queue(index, nullptr);
    // Otherwise the next file waits for the channel to end, then a random pause (ended()).
    if (!(v->def->flags & SoundEventDef::kDelay) || following || !v->shuffle.empty()) {
        const int pause = random(static_cast<int>(v->def->delayMin), static_cast<int>(v->def->delayMax));
        if (pause <= static_cast<int>(kShortPause)) {
            mixer_.queue(index, std::move(following));
            return;
        }
        v->delay = static_cast<std::uint32_t>(pause);
        v->pending = std::move(following);
    }
}

void SoundEngine::ended(int index, std::uint64_t nowMs)
{
    // The channel's end of stream and the voice's callback.
    Channel& c = channels_[static_cast<std::size_t>(index)];
    c.playing = false;
    c.own.init(nowMs);
    Voice* v = c.owner;
    if (!v) {
        return;
    }
    v->started = false;
    if (v->state == State::Done) {
        return;
    }
    // More to play goes back to Ready (after the pause, if any); else the voice is done (any
    // pause dropped).
    const bool more = v->pending || !v->shuffle.empty();
    if (more && v->delay != 0) {
        v->due = nowMs + v->delay;
        v->afterDelay = State::Ready;
        v->state = State::Delayed;
    } else {
        v->state = more ? State::Ready : State::Done;
    }
    v->delay = 0;
}

void SoundEngine::applyChannel(int index, std::uint64_t nowMs)
{
    // Folds the channel's own ramps, its voice's, the master, the device level and the ducking
    // together and hands the mixer the resulting volume, pan and rate.
    Channel& c = channels_[static_cast<std::size_t>(index)];
    c.own.update(nowMs);
    Mix mix;
    mix.by(c.own);
    if (c.owner) {
        mix.by(c.owner->ramps);
    }
    mix.by(master_);
    mix.by(device_);
    mix.by(duck_);
    c.finalVolume = mix.volume;
    mixer_.setParams(index, static_cast<float>(mix.volume) / RampSet::kFull,
                     static_cast<float>(mix.pan) / RampSet::kFull,
                     static_cast<double>(c.sampleRate) * mix.pitch / 100.0);
}

void SoundEngine::rank(Voice& v, std::uint64_t nowMs)
{
    // keep the event's voices ordered loudest first; past maxInstances the last goes.
    EventRuntime& ev = runtime_[v.id];
    const std::int32_t similar = v.def->volume / 10;
    const std::int32_t volume = v.ramps.volume.value();
    auto at = ev.ranked.begin();
    for (; at != ev.ranked.end(); ++at) {
        const Voice& n = **at;
        const std::int32_t other = n.ramps.volume.value();
        // Within a tenth of the event's volume counts as a tie: with kReplace new voices rank ahead of
        // playing ones (so a playing one is cut first), otherwise behind them.
        if (std::abs(other - volume) < similar) {
            const bool fresh = n.state == State::New;
            if ((v.def->flags & SoundEventDef::kReplace) ? !fresh : fresh) {
                break;
            }
        } else if (other < volume) {
            break;
        }
    }
    ev.ranked.insert(at, &v);
    if (v.def->maxInstances != 0 && static_cast<std::int32_t>(ev.ranked.size()) > v.def->maxInstances) {
        Voice* last = ev.ranked.back();
        ev.ranked.pop_back();
        stop(*last, nowMs);
    }
}

void SoundEngine::update(std::uint64_t nowMs)
{
    // What the mixer did since the last frame: buffers begun, channels run dry.
    for (int i = 0; i < Mixer::kChannels; ++i) {
        Channel& c = channels_[static_cast<std::size_t>(i)];
        if (!c.playing) {
            continue;
        }
        const auto st = mixer_.status(i);
        while (c.buffersSeen < st.buffersStarted) {
            c.sampleRate = c.queuedRate;
            bufferStarted(i);
        }
        if (!st.playing) {
            ended(i, nowMs);
        }
    }

    // Steps 1 and 2 (sound_engine.hpp): each event's voices ranked afresh this frame (the
    // first voice of an event seen this frame clears its list), then every voice stepped.
    master_.update(nowMs);
    ++frame_;
    for (auto& v : voices_) {
        EventRuntime& ev = runtime_[v.id];
        if (ev.frame != frame_) {
            ev.ranked.clear();
            ev.frame = frame_;
        }
        v.ramps.update(nowMs);
        if (v.state != State::Done) {
            rank(v, nowMs);
        }
    }
    for (auto& v : voices_) {
        step(v, nowMs);
    }
    // Step 3: voices that lost their channel stop; the rest add up how loud the sounds are.
    std::int32_t loudness = 0;
    for (auto it = voices_.begin(); it != voices_.end();) {
        Voice& v = *it;
        if (v.channel < 0 || channels_[static_cast<std::size_t>(v.channel)].owner != &v) {
            stop(v, nowMs);
        }
        if (!v.stopped) {
            loudness += (v.def->volume * 0x21) >> 14;  // a full-volume event (0x4000) counts about 33
            ++it;
        } else {
            it = voices_.erase(it);
        }
    }
    // Duck everything down to 100 / loudness of full volume when the total passes 100.
    duck_.volume.moveTo(loudness > 100 ? 0x190000 / loudness : RampSet::kFull, nowMs);
    duck_.update(nowMs);
    // Step 4: voices with a channel load their files and start.
    for (auto it = voices_.begin(); it != voices_.end();) {
        Voice& v = *it;
        if (v.state == State::Ready) {
            loadFiles(v);
            v.pending = restart(v);
            if (!v.pending || !start(v, nowMs)) {
                stop(v, nowMs);
                it = voices_.erase(it);
                continue;
            }
        }
        ++it;
    }

    // The channels take the new levels (Miles' timer).
    for (int i = 0; i < Mixer::kChannels; ++i) {
        if (channels_[static_cast<std::size_t>(i)].playing) {
            applyChannel(i, nowMs);
        }
    }
}

} // namespace nox::audio
