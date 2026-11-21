// The sound effects' table a game plays from (thing.bin's AUD and AVNT sections, their files
// found in Audio.bag): the install's, or, in a game whose mod ships a whole thing.bin, that
// file's (game_data_swap.hpp). GAME.EXE reads AUD and AVNT once, at start-up, from the thing.bin
// in its folder (each game start's read of the types passes over them), so a pack played there
// with its own thing.bin plays that file's sounds. A thing.patch changes types alone (their
// AUDIOLOOP, DAMAGESOUND... lines are the types' own), never these sections: a game with one plays
// the install's table. Needs no SDL.
#pragma once

#include "noxcore/audio/sound_events.hpp"
#include "noxcore/things/data_files.hpp"

#include <span>

namespace nox::audio {
class AudioBag;
}

namespace noxsdl {

class GameSounds {
public:
    /// The install's table, its files looked up in `bag` (none: no files), which must outlive it.
    GameSounds(const nox::things::ThingDb& install, const nox::audio::AudioBag* bag);

    /// The table (the same object throughout: what reads it may hold it).
    const nox::audio::SoundEventTable& table() const { return table_; }
    /// Whether it's a mod's thing.bin's now.
    bool fromMod() const { return fromMod_; }

    /// Whether a game reading `files` (a mod's set; none: the install's) plays from another table
    /// than this one now: a set with a whole thing.bin, or the install's again after one. What
    /// plays from the table must stop before use() makes it anew.
    bool changes(std::span<const nox::things::DataFileBytes> files) const;
    /// The table for a game reading `files`, its thing table `things` (the set's merge, or the
    /// install's): made from `things` when changes() says so; else as it is. Whether it was made.
    bool use(const nox::things::ThingDb& things, std::span<const nox::things::DataFileBytes> files);

private:
    nox::audio::SoundEventTable table_;
    const nox::audio::AudioBag* bag_ = nullptr;
    bool fromMod_ = false;
};

} // namespace noxsdl
