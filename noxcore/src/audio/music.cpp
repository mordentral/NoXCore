// The music and speech players (MusicPlayer, DialogPlayer); their behaviour is described in music.hpp.
#include "noxcore/audio/music.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/io/file.hpp"

#include <algorithm>
#include <exception>
#include <system_error>

namespace nox::audio {

namespace {

// music\<name>.wav by track number (MSG_MUSIC_EVENT's byte); 0 is silence.
constexpr std::array<std::string_view, MusicPlayer::kTrackCount> kTracks{
    "",      "chap1", "chap2wiz", "chap2con", "chap2war", "chap3",   "chap4",   "chap5",   "chap6",   "chap7",
    "chap8", "chap9", "chapa",    "chapb",    "title",    "town1",   "town2",   "town3",   "sub1",    "sub2",
    "sub3",  "wander1", "wander2", "wander3", "credits",  "shell",   "action1", "action2", "action3", "wander4",
};

/// <dir>\<name>, ".wav" added when the name has no extension.
std::unique_ptr<AudioDecoder> openStream(const std::filesystem::path& gameDir, std::string_view folder,
                                         std::string name)
{
    if (name.find('.') == std::string::npos) {
        name += ".wav";
    }
    // An installed content pack's content\<folder>\<name> wins (the last installed first, as their
    // patches do); one that won't open is passed over for the next, then the game folder's.
    const auto packs = content::contentPackDirs();
    for (auto dir = packs.rbegin(); dir != packs.rend(); ++dir) {
        const auto file = findNoCase(*dir, std::filesystem::path(folder) / name);
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec)) {
            try {
                if (auto d = AudioDecoder::open(readFileBytes(file))) {
                    return d;
                }
            } catch (const std::exception&) {
            }
        }
    }
    try {
        return AudioDecoder::open(readFileBytes(findNoCase(gameDir, std::filesystem::path(folder) / name)));
    } catch (const std::exception&) {
        return nullptr; // missing or unreadable: as if the CD weren't in
    }
}

// A stream's gain: two ramps (0-kFull each) times a 0-100 volume.
float gainOf(std::int32_t a, std::int32_t b, int percent)
{
    return static_cast<float>(a) / RampSet::kFull * static_cast<float>(b) / RampSet::kFull *
           static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f;
}

} // namespace

// ---- music

MusicPlayer::MusicPlayer(Mixer& mixer, std::filesystem::path gameDir, std::uint64_t nowMs)
    : mixer_(mixer)
    , gameDir_(std::move(gameDir))
{
    master_.init(nowMs);
    fade_.init(nowMs); // a full fade takes a second
}

std::string_view MusicPlayer::trackName(int track)
{
    return track >= 0 && track < kTrackCount ? kTracks[static_cast<std::size_t>(track)] : std::string_view{};
}

void MusicPlayer::setVolume(std::int32_t volume, std::uint64_t nowMs)
{
    master_.volume.set(std::clamp(volume, 0, RampSet::kFull));
    master_.update(nowMs);
    applyVolume();
}

void MusicPlayer::setEnabled(bool on)
{
    enabled_ = on;
}

void MusicPlayer::want(Wanted w)
{
    // the volume kept to 0-100; while a briefing holds the music it's what
    // comes back after it, else it's written to the other slot, which becomes the wanted one.
    w.volume = std::clamp(w.volume, 0, 100);
    if (briefing_) {
        held_ = w;
        return;
    }
    const int other = current_ ^ 1;
    slots_[static_cast<std::size_t>(other)] = w;
    current_ = other;
}

MusicPlayer::Wanted MusicPlayer::saved() const
{
    // the wanted track, and how far it has played if it's the one playing.
    Wanted w = slots_[static_cast<std::size_t>(current_)];
    if (streamLive() && playing_ == w.track) {
        w.position = mixer_.streamStatus(Mixer::kMusic).position;
    }
    return w;
}

MusicPlayer::Entry MusicPlayer::current() const
{
    const Wanted w = saved();
    return {w.track, w.volume, static_cast<std::int32_t>(w.position), 0};
}

// Saved tracks are kept per level (menus, game, ...); `index` is within the current level's six.
MusicPlayer::Entry MusicPlayer::stackEntry(int index) const
{
    const auto& w = stack_[static_cast<std::size_t>(std::min(level_, 2))][static_cast<std::size_t>(std::clamp(index, 0, 5))];
    return {w.track, w.volume, static_cast<std::int32_t>(w.position), 0};
}

void MusicPlayer::setStackEntry(int index, const Entry& e)
{
    stack_[static_cast<std::size_t>(std::min(level_, 2))][static_cast<std::size_t>(std::clamp(index, 0, 5))] =
        {e.track, e.volume, static_cast<std::uint64_t>(std::max(e.position, 0))};
}

void MusicPlayer::set(int track, int volume)
{
    want({track, volume, 0});
}

bool MusicPlayer::push()
{
    // Six saved a level: a seventh push is refused.
    if (stackSize_ > 5) {
        stackSize_ = 6;
        return false;
    }
    stack_[static_cast<std::size_t>(std::min(level_, 2))][static_cast<std::size_t>(stackSize_++)] = saved();
    return true;
}

void MusicPlayer::pop()
{
    if (stackSize_ < 1) {
        stackSize_ = 0;
        return;
    }
    want(stack_[static_cast<std::size_t>(std::min(level_, 2))][static_cast<std::size_t>(--stackSize_)]);
}

void MusicPlayer::briefing(int track, int volume)
{
    // what plays saved, the briefing's track at once, then held.
    held_ = saved();
    set(track, volume);
    briefing_ = true;
}

void MusicPlayer::endBriefing()
{
    // The briefing's saved music (or what was asked for during it) becomes the wanted track.
    briefing_ = false;
    want(held_);
}

void MusicPlayer::pushLevel()
{
    // Three levels at most; a fourth push is dropped (the level stays at 3).
    if (level_ > 2) {
        level_ = 3;
        return;
    }
    push();
    levelSizes_[static_cast<std::size_t>(level_)] = stackSize_;
    stackSize_ = 0;
    ++level_;
}

void MusicPlayer::popLevel()
{
    if (level_ < 1) {
        level_ = 0;
        return;
    }
    --level_;
    stackSize_ = levelSizes_[static_cast<std::size_t>(level_)];
    pop();
}

void MusicPlayer::hold(bool on)
{
    holds_ = std::max(0, holds_ + (on ? 1 : -1));
}

bool MusicPlayer::streamLive() const
{
    const auto st = mixer_.streamStatus(Mixer::kMusic);
    return st.open && !st.ended;
}

bool MusicPlayer::open(Wanted& w, std::uint64_t nowMs)
{
    // Opens the track's file at its saved position and fades in from silence; false if it can't be
    // opened. The saved position is used up.
    close();
    const std::string_view name = trackName(w.track);
    if (name.empty()) {
        return false; // (silence: no file to read)
    }
    auto decoder = openStream(gameDir_, "music", std::string(name));
    if (!decoder) {
        return false;
    }
    decoder->seek(w.position);
    fade_.volume.set(0);
    fade_.update(nowMs);
    fade_.volume.moveTo(RampSet::kFull, nowMs);
    playing_ = w.track;
    playingVolume_ = w.volume;
    w.position = 0;
    mixer_.playStream(Mixer::kMusic, std::move(decoder));
    applyVolume();
    return true;
}

void MusicPlayer::close()
{
    // Stops the music stream; nothing is playing after.
    mixer_.stopStream(Mixer::kMusic);
    playing_ = 0;
}

void MusicPlayer::applyVolume()
{
    // fade x track volume x MusicVolume (x the device's level, full).
    mixer_.setStreamGain(Mixer::kMusic, gainOf(fade_.volume.value(), master_.volume.value(), playingVolume_));
}

void MusicPlayer::update(std::uint64_t nowMs)
{
    // The music's state machine (see music.hpp), run every frame; one step per call.
    Wanted& wanted = slots_[static_cast<std::size_t>(current_)];
    switch (state_) {
    // Idle: a hold while enabled goes straight to Paused; otherwise open the wanted track at full volume.
    // A track that won't open is forgotten so it isn't retried every frame.
    case State::Idle:
        if (holds_ != 0 && enabled_) {
            state_ = State::Paused;
            break;
        }
        if (wanted.track == 0 || !enabled_) {
            break;
        }
        fade_.volume.set(RampSet::kFull);
        if (open(wanted, nowMs)) {
            state_ = State::Playing;
        } else {
            wanted.track = 0;
        }
        break;
    // Playing: keep going while it is still the wanted track and the stream hasn't ended;
    // a hold fades to a pause, anything else (new track, disabled, ended) fades out.
    case State::Playing:
        if (enabled_ && wanted.track == playing_ && streamLive()) {
            if (holds_ != 0) {
                state_ = State::FadeToPause;
                fade_.volume.moveTo(0, nowMs);
            }
            break;
        }
        state_ = State::FadeOut;
        fade_.volume.moveTo(0, nowMs);
        break;
    // FadeOut: wait for silence (or the stream ending by itself), then close and go back to Idle.
    case State::FadeOut:
        if (streamLive() && fade_.volume.value() != 0) {
            break;
        }
        close();
        state_ = State::Idle;
        break;
    // Paused: stay while held; on release resume the same track if it is still wanted, else close.
    case State::Paused:
        if (holds_ != 0 && enabled_) {
            break;
        }
        if (enabled_ && wanted.track == playing_ && streamLive()) {
            fade_.volume.moveTo(RampSet::kFull, nowMs);
            mixer_.pauseStream(Mixer::kMusic, false);
            state_ = State::Playing;
            break;
        }
        close();
        state_ = State::Idle;
        break;
    // FadeToPause: once silent, pause the stream (kept open, resumable); a stream that ended meanwhile is closed.
    case State::FadeToPause:
        if (streamLive()) {
            if (fade_.volume.value() != 0) {
                break;
            }
            mixer_.pauseStream(Mixer::kMusic, true);
            state_ = State::Paused;
            break;
        }
        close();
        state_ = State::Idle;
        break;
    }
    // Advance both volume ramps and push the combined gain to the mixer every frame.
    master_.update(nowMs);
    fade_.update(nowMs);
    applyVolume();
}

// ---- speech

DialogPlayer::DialogPlayer(Mixer& mixer, std::filesystem::path gameDir, std::uint64_t nowMs)
    : mixer_(mixer)
    , gameDir_(std::move(gameDir))
{
    master_.init(nowMs);
    fade_.init(nowMs);
    fade_.volume.setRate(500, RampSet::kFull); // WorkerHurt
}

void DialogPlayer::setVolume(std::int32_t volume, std::uint64_t nowMs)
{
    master_.volume.set(std::clamp(volume, 0, RampSet::kFull));
    master_.update(nowMs);
}

void DialogPlayer::setEnabled(bool on)
{
    enabled_ = on;
}

void DialogPlayer::play(std::string name, int volume)
{
    if (!enabled_ || name.empty()) {
        return;
    }
    wanted_ = std::move(name);
    wantedVolume_ = std::min(volume, 100);
}

void DialogPlayer::stop()
{
    wanted_.clear();
    playingName_.clear();
}

void DialogPlayer::update(std::uint64_t nowMs)
{
    // Speech's state machine, run every frame: open the file paused, start it at full volume,
    // play until it ends or something else is wanted, then fade out and close.
    const auto st = mixer_.streamStatus(Mixer::kDialog);
    const bool live = opened_ && st.open && !st.ended;
    switch (state_) {
    case State::Idle:
        if (wanted_.empty() || !enabled_) {
            break;
        }
        if (auto decoder = openStream(gameDir_, "dialog", wanted_)) {
            mixer_.playStream(Mixer::kDialog, std::move(decoder), true);
            opened_ = true;
            state_ = State::Start;
        } else {
            wanted_.clear();
        }
        break;
    case State::Start:
        fade_.volume.set(RampSet::kFull);
        fade_.update(nowMs);
        playingName_ = wanted_;
        playingVolume_ = wantedVolume_;
        mixer_.setStreamGain(Mixer::kDialog,
                             gainOf(fade_.volume.value(), master_.volume.value(), playingVolume_));
        mixer_.pauseStream(Mixer::kDialog, false);
        state_ = State::Playing;
        break;
    case State::Playing:
        if (!enabled_ || playingName_.empty() || wanted_ != playingName_ || !live) {
            state_ = State::FadeOut;
            fade_.volume.moveTo(0, nowMs);
        }
        break;
    case State::FadeOut:
        if (live && fade_.volume.value() != 0) {
            break;
        }
        mixer_.stopStream(Mixer::kDialog);
        opened_ = false;
        state_ = State::Idle;
        if (playingName_ == wanted_) {
            wanted_.clear(); // said; a different one wanted meanwhile starts next
        }
        playingName_.clear();
        break;
    }
    master_.update(nowMs);
    fade_.update(nowMs);
    if (opened_) {
        mixer_.setStreamGain(Mixer::kDialog, gainOf(fade_.volume.value(), master_.volume.value(), playingVolume_));
    }
}

} // namespace nox::audio
