// The client's sound events (AudEvent.c): one per sound id (sound_names.hpp), filled from
// thing.bin's AUD and AVNT records with their files resolved against Audio.idx. The table and
// its defaults are priority 1, a 600-unit range, full volume.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::audio {

class AudioBag;

struct SoundEventDef {
    bool defined = false; ///< named by an AUD or AVNT record; only these play
    /// 1 loop, 2 a random file each time (AUD sets just this), 4 play the files as a sequence,
    /// 8 wait a random delay before each play, 0x10 a new voice displaces an old one of about
    /// the same volume when maxInstances is reached (else the new one gives way).
    std::uint32_t flags = 0;
    std::int32_t volume = 0x4000; ///< 0-0x4000 (a percentage x 163)
    std::int32_t priority = 1;    ///< AUD 0-2; a player's own sounds get 2 more
    std::int32_t maxInstances = 0; ///< voices at once, 0 = any number
    std::int32_t loops = 0;        ///< times through the files when looping, 0 = forever
    std::int32_t range = 600;      ///< world units: how far a map object's loop is heard
    std::int32_t pitchMin = 0;     ///< playback rate 100 + a random pick in [min, max] percent
    std::int32_t pitchMax = 0;
    std::uint32_t delayMin = 0;    ///< milliseconds
    std::uint32_t delayMax = 0;
    std::int16_t hearing = 0;      ///< server-side; kept for completeness
    std::vector<std::uint16_t> samples; ///< Audio.idx indices (sorted order), at most 32

    static constexpr std::uint32_t kLoop = 1;
    static constexpr std::uint32_t kRandom = 2;
    static constexpr std::uint32_t kSequence = 4;
    static constexpr std::uint32_t kDelay = 8;
    static constexpr std::uint32_t kReplace = 0x10;
};

struct SoundEventTable {
    std::array<SoundEventDef, 1023> events{};
    /// File names AUD / AVNT records gave that Audio.idx lacks (the game skips them).
    std::vector<std::string> missingSamples;

    const SoundEventDef* find(std::uint32_t id) const { return id < events.size() ? &events[id] : nullptr; }
};

/// Builds the table: AUD records, then AVNT records over them (thing.bin keeps no event in both).
/// Sample names are looked up in `bag`; without one, events get no samples.
SoundEventTable buildSoundEvents(const things::ThingDb& db, const AudioBag* bag);

} // namespace nox::audio
