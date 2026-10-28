// The standard library's random distributions and shuffle as the Windows build (Microsoft's
// STL) draws them, so a game run from the same seed goes the same way on every system. The
// engines (std::minstd_rand, std::mt19937) are the same everywhere, but how uniform_int_distribution,
// uniform_real_distribution and shuffle turn an engine's numbers into a result is the library's
// own choice: libstdc++ (GCC, Linux) draws other values than Microsoft's from the same engine.
//
// With Microsoft's STL these are the standard ones themselves. Elsewhere they're nox::util::msvc's
// copy of Microsoft's algorithms (its <random> and <algorithm>, the STL shipped with VS 2022
// 17.14: Lemire's method for the integers, P0952R2's generate_canonical for the reals), which
// give the same results as the standard ones on Windows.
#pragma once

#include <algorithm>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>
#include <ranges>
#include <type_traits>
#include <utility>

#include "noxcore/util/x87.hpp"

namespace nox::util {

// Microsoft's STL (https://github.com/microsoft/STL) is Apache-2.0 WITH LLVM-exception, Copyright
// (c) Microsoft Corporation: its notice is in THIRD_PARTY_NOTICES.txt.
namespace msvc {

namespace detail {

/// An engine's result type as Microsoft's library has it: its uint_fast32_t (the result of
/// minstd_rand and mt19937) and unsigned long are 32 bits, where GCC's are 64.
template <class Urng>
using EngineResult = std::conditional_t<((Urng::max)() <= 0xFFFFFFFFull), std::uint32_t, std::uint64_t>;

#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 U128; // (__extension__: GCC's own type, -Wpedantic quiet)
#define NOX_UTIL_HAVE_U128 1
#elif defined(_MSVC_STL_VERSION)
using U128 = std::_Unsigned128; // (Microsoft's library's own, which its <random> uses)
#define NOX_UTIL_HAVE_U128 1
#endif

/// _Rng_from_urng_v2: an engine's numbers as integers in [0, index), by Lemire's method.
template <class Diff, class Urng>
class RngV2 {
public:
    using Ty0 = std::make_unsigned_t<Diff>;
    using Ty1 = EngineResult<Urng>;
    using Udiff = std::conditional_t<(sizeof(Ty1) < sizeof(Ty0)), Ty0, Ty1>;
    static constexpr unsigned int kUdiffBits = sizeof(Udiff) * CHAR_BIT;
#if defined(NOX_UTIL_HAVE_U128)
    using Uprod = std::conditional_t<kUdiffBits <= 16, std::uint32_t, std::conditional_t<kUdiffBits <= 32, std::uint64_t, U128>>;
#else
    static_assert(kUdiffBits <= 32, "nox::util::msvc needs a 128-bit integer for 64-bit draws");
    using Uprod = std::conditional_t<kUdiffBits <= 16, std::uint32_t, std::uint64_t>;
#endif

    explicit RngV2(Urng& urng) noexcept : ref_(urng) {}
    RngV2(const RngV2&) = delete;
    RngV2& operator=(const RngV2&) = delete;

    Diff operator()(Diff index)
    {
        Udiff mask = kBmask;
        unsigned int iterations = 1;
        if constexpr (kBits < kUdiffBits) {
            while (mask < static_cast<Udiff>(index - 1)) {
                mask <<= kBits;
                mask |= kBmask;
                ++iterations;
            }
        }
        Uprod product = randomProduct(index, iterations);
        Udiff rem = static_cast<Udiff>(product) & mask;
        if (rem < index) {
            const auto threshold = (mask - index + 1) % index;
            while (rem < threshold) {
                product = randomProduct(index, iterations);
                rem = static_cast<Udiff>(product) & mask;
            }
        }
        unsigned int generated;
        if constexpr (kBits < kUdiffBits) {
            generated = static_cast<unsigned int>(std::popcount(mask));
        } else {
            generated = kUdiffBits;
        }
        return static_cast<Diff>(product >> generated);
    }

    Udiff allBits()
    {
        Udiff out = bits();
        if constexpr (kBits < kUdiffBits) {
            for (unsigned int n = kBits; n < kUdiffBits; n += kBits) {
                out <<= kBits;
                out |= bits();
            }
        }
        return out;
    }

private:
    Udiff bits()
    {
        constexpr auto urngMin = (Urng::min)();
        for (;;) {
            const Udiff v = static_cast<Udiff>(ref_() - urngMin);
            if (v <= kBmask) {
                return v;
            }
        }
    }

    static constexpr unsigned int calcBits()
    {
        unsigned int b = kUdiffBits;
        auto m = static_cast<Udiff>(-1);
        for (; (Urng::max)() - (Urng::min)() < m; m >>= 1) {
            --b;
        }
        return b;
    }

    Uprod randomProduct(Diff index, unsigned int iterations)
    {
        Udiff r = bits();
        if constexpr (kBits < kUdiffBits) {
            while (--iterations > 0) {
                r <<= kBits;
                r |= bits();
            }
        }
        return static_cast<Uprod>(static_cast<Uprod>(r) * static_cast<Uprod>(static_cast<Udiff>(index)));
    }

    Urng& ref_;
    static constexpr unsigned int kBits = calcBits();
    static constexpr Udiff kBmask = static_cast<Udiff>(-1) >> (kUdiffBits - kBits);
};

/// _Rng_from_urng: the older method, which shuffle uses.
template <class Diff, class Urng>
class RngV1 {
public:
    using Ty0 = std::make_unsigned_t<Diff>;
    using Ty1 = EngineResult<Urng>;
    using Udiff = std::conditional_t<(sizeof(Ty1) < sizeof(Ty0)), Ty0, Ty1>;

    explicit RngV1(Urng& urng) : ref_(urng), bits_(CHAR_BIT * sizeof(Udiff)), bmask_(static_cast<Udiff>(-1))
    {
        for (; static_cast<Udiff>((Urng::max)() - (Urng::min)()) < bmask_; bmask_ >>= 1) {
            --bits_;
        }
    }
    RngV1(const RngV1&) = delete;
    RngV1& operator=(const RngV1&) = delete;

    Diff operator()(Diff index)
    {
        for (;;) {
            Udiff ret = 0;
            Udiff mask = 0;
            while (mask < static_cast<Udiff>(index - 1)) {
                ret <<= bits_ - 1; // (two shifts: never a full-width one)
                ret <<= 1;
                ret |= bits();
                mask <<= bits_ - 1;
                mask <<= 1;
                mask |= bmask_;
            }
            if (ret / index < mask / index || mask % index == static_cast<Udiff>(index - 1)) {
                return static_cast<Diff>(ret % index);
            }
        }
    }

private:
    Udiff bits()
    {
        for (;;) {
            const Udiff v = static_cast<Udiff>(ref_() - (Urng::min)());
            if (v <= bmask_) {
                return v;
            }
        }
    }

    Urng& ref_;
    std::size_t bits_;
    Udiff bmask_;
};

struct CanonicalParams {
    bool rxIsPow2 = false;
    int kx = 0;
    std::uint64_t xx = 0;
    float scale = 0.0f;
    int smaxBits = 0;
};

#if defined(NOX_UTIL_HAVE_U128)
/// _Generate_canonical_params.
constexpr CanonicalParams canonicalParams(std::size_t bits, std::uint64_t rm1)
{
    const U128 rx = U128{rm1} + 1;
    const U128 target = U128{1} << bits;
    U128 product = 1;
    CanonicalParams out;
    out.rxIsPow2 = (rx & (rx - 1)) == 0;
    while (product < target) {
        product *= rx;
        ++out.kx;
    }
    out.xx = static_cast<std::uint64_t>(product / target);
    out.scale = 1.0f / static_cast<float>(1ULL << bits);
    --product;
    while (product > 0) {
        ++out.smaxBits;
        product >>= 1;
    }
    return out;
}
#endif

} // namespace detail

/// generate_canonical<Real, digits>: Microsoft's (P0952R2).
template <class Real, class Gen>
Real generateCanonical(Gen& g)
{
#if defined(NOX_UTIL_HAVE_U128)
    constexpr int kMinbits = std::numeric_limits<Real>::digits;
    using ResultUint = std::conditional_t<kMinbits <= 32, std::uint32_t, std::uint64_t>;
    constexpr auto gmin = (Gen::min)();
    constexpr auto gmax = (Gen::max)();
    constexpr auto params = detail::canonicalParams(kMinbits, static_cast<std::uint64_t>(gmax - gmin));
    static_assert(params.kx >= 1);
    if constexpr (params.rxIsPow2) {
        constexpr int discarded = params.smaxBits - kMinbits;
        ResultUint sx = static_cast<ResultUint>((g() - gmin) >> discarded);
        if constexpr (params.kx > 1) {
            constexpr int rxBits = params.smaxBits / params.kx;
            int shift = -discarded;
            for (int i = 1; i < params.kx; ++i) {
                shift += rxBits;
                sx += static_cast<ResultUint>(g() - gmin) << shift;
            }
        }
        return static_cast<Real>(sx) * static_cast<Real>(params.scale);
    } else {
        using Sx = std::conditional_t<params.smaxBits <= 32, std::uint32_t,
                                      std::conditional_t<params.smaxBits <= 64, std::uint64_t, detail::U128>>;
        constexpr auto rx = static_cast<Sx>(Sx{gmax - gmin} + 1u);
        constexpr Sx limit = static_cast<Sx>(params.xx) * (Sx{1} << kMinbits);
        Sx sx;
        do {
            sx = static_cast<Sx>(g() - gmin);
            if constexpr (params.kx == 2) {
                sx += static_cast<Sx>(g() - gmin) * rx;
            } else if constexpr (params.kx > 2) {
                sx += static_cast<Sx>(g() - gmin) * rx;
                Sx factor = rx;
                for (int i = 2; i < params.kx; ++i) {
                    factor *= rx;
                    sx += static_cast<Sx>(g() - gmin) * factor;
                }
            }
        } while (sx >= limit);
        return static_cast<Real>(static_cast<ResultUint>(sx / params.xx)) * static_cast<Real>(params.scale);
    }
#else
    static_assert(sizeof(Gen) == 0, "nox::util::msvc needs a 128-bit integer");
    return Real{};
#endif
}

/// uniform_int_distribution: Microsoft's.
template <class T = int>
class uniform_int_distribution {
public:
    using result_type = T;
    struct param_type {
        using distribution_type = uniform_int_distribution;
        param_type() = default;
        explicit param_type(T a, T b = (std::numeric_limits<T>::max)()) : min(a), max(b) {}
        T a() const { return min; }
        T b() const { return max; }
        friend bool operator==(const param_type&, const param_type&) = default;
        T min = 0;
        T max = (std::numeric_limits<T>::max)();
    };

    uniform_int_distribution() : uniform_int_distribution(0) {}
    explicit uniform_int_distribution(T a, T b = (std::numeric_limits<T>::max)()) : par_(a, b) {}
    explicit uniform_int_distribution(const param_type& p) : par_(p) {}

    T a() const { return par_.min; }
    T b() const { return par_.max; }
    T(min)() const { return par_.min; }
    T(max)() const { return par_.max; }
    param_type param() const { return par_; }
    void param(const param_type& p) { par_ = p; }
    void reset() {}

    template <class Engine>
    T operator()(Engine& eng) const
    {
        return eval(eng, par_.min, par_.max);
    }
    template <class Engine>
    T operator()(Engine& eng, const param_type& p) const
    {
        return eval(eng, p.min, p.max);
    }

private:
    using Uty = std::make_unsigned_t<T>;

    template <class Engine>
    static T eval(Engine& eng, T lo, T hi)
    {
        detail::RngV2<Uty, Engine> gen(eng);
        const Uty umin = adjust(static_cast<Uty>(lo));
        const Uty umax = adjust(static_cast<Uty>(hi));
        Uty uret;
        if (static_cast<Uty>(umax - umin) == static_cast<Uty>(-1)) {
            uret = static_cast<Uty>(gen.allBits());
        } else {
            uret = static_cast<Uty>(gen(static_cast<Uty>(umax - umin + 1)));
        }
        return static_cast<T>(adjust(static_cast<Uty>(uret + umin)));
    }

    static Uty adjust(Uty v)
    {
        if constexpr (std::is_signed_v<T>) {
            constexpr Uty kAdjuster = (static_cast<Uty>(-1) >> 1) + 1; // 2^(N-1)
            return static_cast<Uty>(v ^ kAdjuster);
        } else {
            return v;
        }
    }

    param_type par_;
};

/// uniform_real_distribution: Microsoft's.
template <class T = double>
class uniform_real_distribution {
public:
    using result_type = T;
    struct param_type {
        using distribution_type = uniform_real_distribution;
        param_type() = default;
        explicit param_type(T a, T b = T{1}) : min(a), max(b) {}
        T a() const { return min; }
        T b() const { return max; }
        friend bool operator==(const param_type&, const param_type&) = default;
        T min = T{0};
        T max = T{1};
    };

    uniform_real_distribution() : uniform_real_distribution(T{0}) {}
    explicit uniform_real_distribution(T a, T b = T{1}) : par_(a, b) {}
    explicit uniform_real_distribution(const param_type& p) : par_(p) {}

    T a() const { return par_.min; }
    T b() const { return par_.max; }
    T(min)() const { return par_.min; }
    T(max)() const { return par_.max; }
    param_type param() const { return par_; }
    void param(const param_type& p) { par_ = p; }
    void reset() {}

    template <class Engine>
    T operator()(Engine& eng) const
    {
        return eval(eng, par_);
    }
    template <class Engine>
    T operator()(Engine& eng, const param_type& p) const
    {
        return eval(eng, p);
    }

private:
    template <class Engine>
    static T eval(Engine& eng, const param_type& p)
    {
        return generateCanonical<T>(eng) * (p.max - p.min) + p.min;
    }

    param_type par_;
};

/// shuffle: Microsoft's.
template <class RanIt, class Urng>
void shuffle(RanIt first, RanIt last, Urng&& g)
{
    using Diff = typename std::iterator_traits<RanIt>::difference_type;
    detail::RngV1<Diff, std::remove_reference_t<Urng>> rng(g);
    if (first == last) {
        return;
    }
    auto target = first;
    Diff targetIndex = 1;
    for (; ++target != last; ++targetIndex) {
        const Diff off = rng(static_cast<Diff>(targetIndex + 1));
        if (off != targetIndex) {
            using std::swap;
            swap(*target, *(first + off));
        }
    }
}

} // namespace msvc

#if defined(_MSVC_STL_VERSION)
// Microsoft's library: its own.
template <class T = int>
using uniform_int_distribution = std::uniform_int_distribution<T>;
template <class T = double>
using uniform_real_distribution = std::uniform_real_distribution<T>;
template <class RanIt, class Urng>
void shuffle(RanIt first, RanIt last, Urng&& g)
{
    std::shuffle(first, last, std::forward<Urng>(g));
}
/// std::ranges::shuffle of a whole range.
template <class Range, class Urng>
void shuffle(Range&& range, Urng&& g)
{
    std::ranges::shuffle(range, std::forward<Urng>(g));
}
#else
template <class T = int>
using uniform_int_distribution = msvc::uniform_int_distribution<T>;
template <class T = double>
using uniform_real_distribution = msvc::uniform_real_distribution<T>;
using msvc::shuffle;
/// std::ranges::shuffle of a whole range (Microsoft's draws the same as its std::shuffle).
template <class Range, class Urng>
void shuffle(Range&& range, Urng&& g)
{
    msvc::shuffle(std::ranges::begin(range), std::ranges::end(range), g);
}
#endif

/// randomFloat: lo + (hi - lo) x (a draw of 0 - 32767 x 3.0518509e-05f, the float
/// 0x38000100), each step on the x87's stack, returned there (ST0: 53 bits, for the caller's own
/// figuring before any store); hi itself, with no draw, when hi - lo is 0. The game's draw is the
/// next entry of its shared table of 4096 numbers of 0 - 32767; ours is the engine's next number
/// (one generate_canonical<float>, as uniform_real_distribution<float> takes) on that table's grid.
template <class Engine>
X87 randomFloat(Engine& eng, float lo, float hi)
{
    const X87 range = X87(hi) - lo;
    if (range == X87(0.0f)) {
        return X87(hi);
    }
    const float u = uniform_real_distribution<float>(0.0f, 1.0f)(eng);
    const int k = (std::min)(static_cast<int>(u * 32768.0f), 32767); // (u x 2^15 is exact)
    return range * (X87::fromDouble(k) * X87(std::bit_cast<float>(0x38000100u))) + lo;
}

} // namespace nox::util
