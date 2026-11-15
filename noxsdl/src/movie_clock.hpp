// What times a movie (movie_player.cpp), kept apart from SDL so the tests can run it.
#pragma once

#include <cstdint>

namespace noxsdl {

/// Whether a movie's sound can time it: there is some, at a rate above 0. A sound with no rate (a
/// damaged file's header) would make the time heard infinite, and the frame due an int made of it:
/// the movie plays as one without sound, by the clock.
constexpr bool movieSoundTimes(bool hasSound, std::uint32_t rate)
{
    return hasSound && rate > 0;
}

} // namespace noxsdl
