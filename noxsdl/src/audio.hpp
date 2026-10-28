// noxsdl's sound: an SDL audio device feeding noxcore's mixer, and the game's sound engine,
// music and speech players (noxcore audio/), driven once a frame.
//
//   sounds    the server's MSG_AUDIO_EVENTs and the client's own (World::takeAudio) are held
//             until the render clock reaches the frame they came in, so they line up with the
//             interpolated drawing, as the effects are; the menus' and HUD's sounds play at once;
//   loops     things with an AUDIOLOOP (ambient birds and wind, torches, fires, gears) play while
//             enabled and in range, louder the nearer the listener (our player), panned by where
//             they are across the view;
//   music     the server's music messages, and the menus' tracks (shell, title);
//   speech    NPC dialog and briefings, from dialog\*.wav.
// Volumes are nox.cfg's FXVolume, DialogVolume and MusicVolume (0-0x4000); the Options window's
// three checkboxes switch each part off. With no audio device, or no Audio.bag, it stays silent.
#pragma once

#include "noxcore/audio/audio_bag.hpp"
#include "noxcore/audio/mixer.hpp"
#include "noxcore/audio/music.hpp"
#include "noxcore/audio/sound_engine.hpp"
#include "noxcore/audio/sound_events.hpp"
#include "noxcore/client/world.hpp"
#include "noxcore/config/nox_config.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include <string>
#include <string_view>

namespace noxsdl {

class Audio {
public:
    /// Opens the default playback device and the game's sounds. nullptr (with a log line) if
    /// either is missing: the game then runs silent.
    static std::unique_ptr<Audio> open(const std::filesystem::path& gameDir, const nox::things::ThingDb& things);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    /// FXVolume, DialogVolume and MusicVolume from nox.cfg (0x4000 when absent).
    void setVolumes(const nox::config::Config& config);
    /// The Options window's checkboxes: sound effects, speech, music.
    struct Switches {
        bool effects = true;
        bool dialog = true;
        bool music = true;
    };
    void setSwitches(const Switches& on);
    Switches switches() const;

    /// Snd_PlaySound: a menu or HUD sound by id, `volume` 0-100.
    void play(std::uint32_t id, int volume = 100);
    /// The same by the sound's name ("ShellClick").
    void play(std::string_view name, int volume = 100);
    /// A mod UI's own sound (PCM16 interleaved, `rate` Hz, `channels`), `volume` 0-100 of the
    /// effects' volume: on a stream of its own, a new one cutting the last; none with the effects
    /// off. Read where it is (never copied): `owner` holds it and is kept while it plays.
    void playClip(std::shared_ptr<const void> owner, std::span<const std::int16_t> samples, std::uint32_t rate, std::uint16_t channels,
                  int volume);

    /// A sound or music change from the world, played once `releaseCues` reaches its frame.
    void cue(const nox::client::AudioCue& cue);
    /// Plays the held cues up to game frame `gameTime` (the render clock).
    void releaseCues(double gameTime);
    /// Where the spell-chant cues go (MSG_REPORT_SPELL_START: spell id, its frame).
    std::function<void(std::uint8_t spell, std::uint32_t frame)> onChant;

    /// Snd_SetMusic: track (MusicPlayer::trackName) at `volume` 0-100; 0 stops it.
    void music(int track, int volume = 100);
    int musicTrack() const { return music_.playing(); }
    /// Snd_MusicPush / Snd_MusicPop (Snd_MusicPop): a solo game's host's MusicPushEvent /
    /// MusicPopEvent, at once.
    void musicPush() { music_.push(); }
    void musicPop() { music_.pop(); }
    /// A briefing screen's music and its end: MusicPlayer::briefing.
    void briefingMusic(int track, int volume) { music_.briefing(track, volume); }
    void endBriefingMusic() { music_.endBriefing(); }
    /// A solo save's music state (plr_rwMusic): the current track and this
    /// level's saved ones; a load's.
    nox::audio::MusicPlayer::Entry musicCurrent() const { return music_.current(); }
    int musicStackSize() const { return music_.stackSize(); }
    nox::audio::MusicPlayer::Entry musicStackEntry(int i) const { return music_.stackEntry(i); }
    void setMusicStackEntry(int i, const nox::audio::MusicPlayer::Entry& e) { music_.setStackEntry(i, e); }
    void restoreMusic(const nox::audio::MusicPlayer::Entry& current, int stackSize)
    {
        music_.restore(current);
        music_.setStackSize(stackSize);
    }
    /// Speech (playDialogFile): dialog\<name>.wav; stopDialog fades it out.
    void dialog(std::string name, int volume = 100);
    void stopDialog();
    /// Something is being said.
    bool dialogBusy() const { return dialog_.busy(); }

    /// Where loops are heard from: the listener (our player), and the view's centre and half
    /// width in world units for the pan.
    struct Listener {
        float x = 0;
        float y = 0;
        float viewCentreX = 0;
        float halfViewWidth = 320;
    };
    /// Each frame: one call per thing with a loop (`key` names it across frames; `enabled`
    /// false or out of range silences it), between beginLoops and endLoops. Loops not named
    /// this frame stop (their thing went away).
    void beginLoops(const Listener& listener);
    void loop(std::uint64_t key, std::uint32_t soundId, float x, float y, bool enabled);
    /// A loop heard everywhere alike (the weather's wind), at `volume` 0-100 (0: silent), centred.
    void loopAt(std::uint64_t key, std::uint32_t soundId, int volume);
    void endLoops();
    /// Stops every sound effect and loop, and drops held cues.
    void stopEffects();
    /// A game starting: the menus' music is saved and stops.
    void enterGame();
    /// Back to the menus: the game's sounds stop and the menus' music resumes.
    void leaveGame();

    /// A movie about to play: every sound effect and the speech stopped, the music
    /// held (it fades and pauses).
    void beginMovie();
    /// The music and speech have gone quiet: the exe waits for that before the movie starts.
    bool quietForMovie() const;
    /// The movie's sound, on a stream of its own (the exe's VQA player has its own DirectSound).
    void playMovieSound(std::unique_ptr<nox::audio::AudioDecoder> decoder);
    struct MovieSound {
        bool open = false;
        bool ended = false;
        std::uint64_t heard = 0; ///< frames played so far
    };
    MovieSound movieSound() const;
    /// The movie over: its sound stopped, the music let go.
    void endMovie();

    /// Once a frame.
    void update();

    /// Debugging (--audio-capture): records what's played, written to path (16-bit WAV) when
    /// the Audio goes. At most ten minutes.
    void capture(std::filesystem::path path);
    /// Debugging (--audio-log): logs every sound asked for.
    void setLog(bool on) { log_ = on; }

    nox::audio::SoundEngine& engine() { return engine_; }
    const nox::audio::SoundEventTable& events() const { return events_; }

private:
    Audio(SDL_AudioStream* stream, std::uint32_t rate, const std::filesystem::path& gameDir,
          const nox::things::ThingDb& things, nox::audio::AudioBag bag);
    static void SDLCALL feed(void* user, SDL_AudioStream* stream, int additional, int total);
    static std::uint64_t now() { return SDL_GetTicks(); }

    SDL_AudioStream* stream_ = nullptr;
    nox::audio::Mixer mixer_;
    nox::audio::AudioBag bag_;
    nox::audio::SoundEventTable events_;
    nox::audio::SoundEngine engine_;
    nox::audio::MusicPlayer music_;
    nox::audio::DialogPlayer dialog_;
    Switches on_;
    std::deque<nox::client::AudioCue> cues_;
    struct Loop {
        nox::audio::SoundEngine::LoopHandle handle;
        bool seen = false;
        // This frame's ask (loop()), driven or silenced in endLoops().
        std::uint32_t soundId = 0;
        int volume = 0;
        int pan = 0;
    };
    std::map<std::uint64_t, Loop> loops_;
    /// Of one looping sound, only this many sources play at once (the loudest, those playing
    /// kept first): a field of identical things (FlagWar's 169 rotating spikes) otherwise has
    /// dozens of the same loop take each other's channels, each restart replaying its start.
    static constexpr std::size_t kLoopsPerSound = 3;
    Listener listener_;
    std::uint32_t rate_ = 48000;
    std::int32_t fxVolume_ = 0x4000; ///< (FXVolume: a mod UI's sounds')
    bool log_ = false;
    bool holdingForMovie_ = false;
    std::filesystem::path capturePath_;
    std::mutex captureMutex_;
    std::vector<std::int16_t> captured_;
};

} // namespace noxsdl
