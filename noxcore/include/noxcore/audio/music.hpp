// Music and NPC speech: the two streams GAME.EXE plays beside the sound effects.
//
// Music. Tracks are numbered (MSG_MUSIC_EVENT's byte; the exe's name
// table) and live in music\<name>.wav. Snd_SetMusic only records the
// wanted track and its volume (0-100); a state machine (run every frame) does the rest:
//   idle      -> start the wanted track, fading in over a second;
//   playing   -> another track wanted, or the stream ended: fade out, close, back to idle (so a
//                track that ends starts over: the music loops);
//   fading    -> closed once silent;
//   paused / fading to pause -> while speech holds the music (a CD install that can't stream
//                both; see DialogPlayer).
// MSG_MUSIC_PUSH_EVENT saves the wanted track and where it is (Snd_MusicPush), POP brings it back from
// that point (Snd_MusicPop): six deep. Track 0 is silence. The music volume (MusicVolume in nox.cfg)
// scales it.
//
// Speech (AudDiag.c): one file at a time from dialog\<name>.wav (MP3 data), wanted by
// playDialogFile (NPC dialogs, shops, quest briefings); a new one, or stop(),
// fades the current one out over half a second. DialogVolume scales it. The exe pauses the music
// for speech only when both stream from the CD (state 1); installed games keep both.
#pragma once

#include "noxcore/audio/mixer.hpp"
#include "noxcore/audio/ramp.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace nox::audio {

class MusicPlayer {
public:
    MusicPlayer(Mixer& mixer, std::filesystem::path gameDir, std::uint64_t nowMs);

    /// Track names: 1 "chap1" ... 29 "wander4"; "" for 0 and out of range.
    static std::string_view trackName(int track);
    static constexpr int kTrackCount = 30;

    /// MusicVolume, 0-0x4000.
    void setVolume(std::int32_t volume, std::uint64_t nowMs);
    /// Options' music checkbox: off fades the music out.
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    /// Snd_SetMusic: the track to play at `volume` (0-100); 0 for none.
    void set(int track, int volume);
    /// MSG_MUSIC_PUSH_EVENT: false (and nothing saved) when six are saved already.
    bool push();
    /// MSG_MUSIC_POP_EVENT: back to the last saved track, where it was.
    void pop();
    /// A briefing screen's music: what plays saved, `track` at `volume` now; until
    /// endBriefing every change (set, pop) is kept for after it instead.
    void briefing(int track, int volume);
    /// The briefing over: the saved music (or what was asked for meanwhile) back.
    void endBriefing();
    /// A game starting: the menus' music saved on a level of its own, the game's own
    /// pushes and pops above it. Three levels at most.
    void pushLevel();
    /// Back to the menus: the saved music resumes where it was.
    void popLevel();
    /// Stops the music.
    void stop() { set(0, 0); }
    /// Speech holding the music, counted.
    void hold(bool on);

    void update(std::uint64_t nowMs);

    /// The 16-byte music state GAME.EXE keeps, as a solo save's Music section holds it: the track,
    /// its volume, how far it has played (ours: frames into the track; the exe's the Miles stream
    /// position) and a word always 0.
    struct Entry {
        std::int32_t track = 0;
        std::int32_t volume = 0;
        std::int32_t position = 0;
        std::int32_t extra = 0;
    };
    /// the wanted track, and how far it has played if it's the one playing.
    Entry current() const;
    /// (g_musicStackIndex) and this level's saved tracks.
    int stackSize() const { return stackSize_; }
    Entry stackEntry(int index) const;
    void setStackEntry(int index, const Entry& e);
    /// the stack's size as read (kept to the six a level holds).
    void setStackSize(int size) { stackSize_ = std::clamp(size, 0, 6); }
    /// with a loaded game's state: wanted (from its position), or held for after a
    /// briefing.
    void restore(const Entry& e) { want({e.track, e.volume, static_cast<std::uint64_t>(std::max(e.position, 0))}); }

    /// The track playing (or fading), 0 if none.
    int playing() const { return playing_; }
    /// Playing or fading.
    bool busy() const { return state_ == State::Playing || state_ == State::FadeOut || state_ == State::FadeToPause; }

private:
    enum class State : std::uint8_t { Idle = 0, Playing = 1, FadeOut = 2, Paused = 3, FadeToPause = 4 };
    struct Wanted {
        int track = 0;
        int volume = 0;
        std::uint64_t position = 0; ///< frames into the track
    };

    bool open(Wanted& w, std::uint64_t nowMs);
    void close();
    void want(Wanted w);
    Wanted saved() const;
    bool streamLive() const;
    void applyVolume();

    Mixer& mixer_;
    std::filesystem::path gameDir_;
    bool enabled_ = true;
    State state_ = State::Idle;
    std::array<Wanted, 2> slots_{}; ///< double-buffered; `current_` is the wanted one
    int current_ = 0;
    int playing_ = 0;
    int playingVolume_ = 0;
    int holds_ = 0;
    std::array<std::array<Wanted, 6>, 3> stack_{}; ///< g_musicStack: six a level
    int stackSize_ = 0;                           ///< g_musicStackIndex
    bool briefing_ = false;                       ///< a briefing holds the music
    Wanted held_;                                 ///< what comes back after the briefing
    int level_ = 0;                               ///< g_musicStackLevel
    std::array<int, 3> levelSizes_{};             ///< each level's size when it was left
    RampSet master_; ///< MusicVolume
    RampSet fade_;   ///< fades in and out over a second
};

class DialogPlayer {
public:
    DialogPlayer(Mixer& mixer, std::filesystem::path gameDir, std::uint64_t nowMs);

    /// DialogVolume, 0-0x4000.
    void setVolume(std::int32_t volume, std::uint64_t nowMs);
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    /// playDialogFile: speak dialog\<name> at `volume` (0-100).
    void play(std::string name, int volume);
    /// fade out whatever is said.
    void stop();
    /// Something said or about to be.
    bool busy() const { return !wanted_.empty() || state_ != State::Idle; }

    void update(std::uint64_t nowMs);

private:
    enum class State : std::uint8_t { Idle = 0, Start = 2, Playing = 3, FadeOut = 4 };

    Mixer& mixer_;
    std::filesystem::path gameDir_;
    bool enabled_ = true;
    State state_ = State::Idle;
    std::string wanted_;
    int wantedVolume_ = 100;
    std::string playingName_;
    int playingVolume_ = 100;
    bool opened_ = false;
    RampSet master_; ///< DialogVolume
    RampSet fade_;   ///< half a second
};

} // namespace nox::audio
