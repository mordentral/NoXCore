// The sound effects' table a game plays from (game_sounds.hpp).
#include "game_sounds.hpp"

#include <algorithm>

namespace noxsdl {

namespace {

bool wholeThingBin(std::span<const nox::things::DataFileBytes> files)
{
    return std::ranges::any_of(files, [](const nox::things::DataFileBytes& f) { return f.kind == nox::things::DataFile::Things; });
}

} // namespace

GameSounds::GameSounds(const nox::things::ThingDb& install, const nox::audio::AudioBag* bag)
    : table_(nox::audio::buildSoundEvents(install, bag))
    , bag_(bag)
{
}

bool GameSounds::changes(std::span<const nox::things::DataFileBytes> files) const
{
    // (Two mods' whole files may differ: each game with one has its table made.)
    return wholeThingBin(files) || fromMod_;
}

bool GameSounds::use(const nox::things::ThingDb& things, std::span<const nox::things::DataFileBytes> files)
{
    if (!changes(files)) {
        return false;
    }
    table_ = nox::audio::buildSoundEvents(things, bag_); // (every event afresh, as at start-up from that file)
    fromMod_ = wholeThingBin(files);
    return true;
}

} // namespace noxsdl
