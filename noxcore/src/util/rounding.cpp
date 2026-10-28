// util::RoundingScope's switch (x87.hpp): the thread's rounding, out of line.
#include "noxcore/util/x87.hpp"

#include <cfenv>
#include <cmath>

#if defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#define NOX_ROUNDING_SSE 1
#endif

namespace nox::util::rounding_detail {

unsigned int set(bool chop) noexcept
{
#ifdef NOX_ROUNDING_SSE
    const unsigned int now = _mm_getcsr();
    const unsigned int want = (now & ~0x6000u) | (chop ? 0x6000u : 0u); // (MXCSR's RC: 11 towards zero)
    if (want == now) {
        return ~0u;
    }
    _mm_setcsr(want);
    return now;
#else
    const int now = std::fegetround();
    const int want = chop ? FE_TOWARDZERO : FE_TONEAREST;
    if (want == now) {
        return ~0u;
    }
    std::fesetround(want);
    return static_cast<unsigned int>(now);
#endif
}

void restore(unsigned int saved) noexcept
{
#ifdef NOX_ROUNDING_SSE
    _mm_setcsr(saved);
#else
    std::fesetround(static_cast<int>(saved));
#endif
}

bool chopping() noexcept
{
#ifdef NOX_ROUNDING_SSE
    return (_mm_getcsr() & 0x6000u) == 0x6000u;
#else
    return std::fegetround() == FE_TOWARDZERO;
#endif
}

} // namespace nox::util::rounding_detail

namespace nox::util {

double sinNearest(double v) noexcept
{
    const RoundingScope nearest(false);
    return std::sin(v);
}

double cosNearest(double v) noexcept
{
    const RoundingScope nearest(false);
    return std::cos(v);
}

} // namespace nox::util