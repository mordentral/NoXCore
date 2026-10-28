// The server's frame and substep counter (clock.hpp).
#include "noxcore/server/clock.hpp"

#include <algorithm>

namespace nox::server {

SimClock::SimClock(int multiplier) noexcept
    : multiplier_(std::clamp(multiplier, 1, kMaxMultiplier))
    , nextMultiplier_(multiplier_)
{
}

void SimClock::setMultiplier(int multiplier) noexcept
{
    nextMultiplier_ = std::clamp(multiplier, 1, kMaxMultiplier);
    if (substep_ == 0) {
        multiplier_ = nextMultiplier_;
    }
}

int SimClock::advance(bool countFrame) noexcept
{
    // A new frame starts: a pending multiplier change applies only here, so a frame keeps the
    // substep count it began with.
    if (substep_ == 0) {
        multiplier_ = nextMultiplier_;
        if (countFrame) {
            ++frame_;
        }
    }
    const int ran = substep_;
    // Wrap back to substep 0 after the last substep of the frame.
    substep_ = substep_ + 1 >= multiplier_ ? 0 : substep_ + 1;
    return ran;
}

} // namespace nox::server
