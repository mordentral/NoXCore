// The exe's x87 in its chop mode (rounding towards zero), for the figuring that follows it.
//
// The game's FPU: the CRT's start-up sets the precision to 53 bits (_controlfp(_PC_53,
// _MCW_PC)); WinMain and the main loop (each pass) set the rounding to chop (_controlfp(_RC_CHOP,
// _MCW_RC)); every exception stays masked. Nothing in the game's own code changes either for
// longer than a CRT helper's call (__ftol, floor, ceil, modf: each puts the control word back).
// So, in the game's code:
//   - FLD of a float or a double is exact;
//   - every FADD / FSUB / FMUL / FDIV / FSQRT result on the stack is the exact result chopped to 53
//     bits (the stack's exponent stays the 15-bit one: no overflow or underflow at 53 bits);
//   - every FSTP / FST to a float chops to 24 bits; past the largest float it stops there (the
//     round-towards-zero overflow), never infinity;
//   - FISTP (Math_FloatToInt, Math_DoubleToInt) and __ftol truncate.
//
// Ours: the server's figuring runs with SSE's own rounding set towards zero for its thread
// (RoundingScope, at the server's entry points: Server's map loads and frames, GameHost's tick and
// receive). Then a double operation is exactly the x87's 53-bit chopped result (the same grid), a
// double-to-float conversion its chopped FSTP (the largest float past the floats), and the
// conversions to ints truncate as FISTP and __ftol do. What the x87 has beyond that is only its
// wider exponent on the stack, which no game value comes near (game figures stay far inside a
// double's range and a float's normal range: no intermediate overflows or goes denormal in a
// double where the stack's wouldn't). With the host's "exact original maths" off (VD-37) the scope
// keeps rounding to nearest: the same code, the old last bits.
//
// X87 marks where the exe works on its stack (doubles) and where it stores (f(): a float), so a
// figure keeps the exe's own sequence of operations, stores and constants; the rounding itself is
// the hardware's. This header is the one implementation of the game's float steps: the script
// VM's opcodes (chopAdd...), the builtins and the gameplay maths use it.
#pragma once

#include <cmath>
#include <cstdint>


/// A function the compiler must call, never fold into its caller: one that holds a RoundingScope
/// of its own inside the server's figuring. (Inlined, the caller's own float work, its operands
/// already in registers, could be scheduled between the switch and the switch back: in the
/// other rounding.)
#if defined(_MSC_VER)
#define NOX_NOINLINE __declspec(noinline)
#else
#define NOX_NOINLINE __attribute__((noinline))
#endif

namespace nox::util {

namespace rounding_detail {
// (Out of line, in rounding.cpp: a call the compiler can't see into, so it keeps the figuring
// on its side of the change.)
/// Sets the thread's rounding (towards zero with chop, else to nearest); returns what to give
/// back to restore(), or ~0u when it was so already.
unsigned int set(bool chop) noexcept;
void restore(unsigned int saved) noexcept;
bool chopping() noexcept;
} // namespace rounding_detail

/// The C library's double sine and cosine taken in round-to-nearest whatever the thread's rounding:
/// MSVC's go far wrong (by thousands of millions of units in the last place) in round towards
/// zero. (Its float ones, atan2, hypot, pow and sqrt stay within a unit or two: left as they are.)
/// They stand for the game's FSIN / FCOS, which weren't bit for bit anyway.
double sinNearest(double v) noexcept;
double cosNearest(double v) noexcept;

/// The rules:
///   - The rounding is per thread (MXCSR), and a new thread may start with its creator's (Linux
///     threads inherit it) or with nearest (Windows): every thread the server starts sets its own
///     with a RoundingScope (the game's chop for server figuring, nearest for anything else, as
///     a mode's level builder does).
///   - Never save and restore the rounding around server maths with fegetround / fesetround on
///     x86-64: glibc's fegetround reads the x87's control word, not MXCSR, so under a
///     RoundingScope it answers nearest, and its fesetround then puts MXCSR back to nearest. Use
///     RoundingScope (it saves and restores MXCSR itself).
///   - A scope that switches the rounding inside the server's figuring (to nearest for text, the
///     level generator...) goes at the top of a function of its own marked NOX_NOINLINE, so no
///     figuring of its caller's can be moved into it.
///   - GCC / Clang need no -frounding-math (constant expressions fold to nearest, as the game's
///     own compiler folded its constants); noxcore builds with -ffp-contract=off (no fused
///     multiply-adds), which MSVC never does without /fp:contract or /fp:fast.
/// Sets this thread's rounding for the scope: towards zero (the game's chop mode) with chop,
/// else to nearest (SSE's MXCSR, or the C library's fesetround elsewhere); puts it back as it was
/// at the scope's end. A scope that finds the thread already so changes nothing (a read only), so
/// nested scopes cost next to nothing.
class RoundingScope {
public:
    explicit RoundingScope(bool chop) noexcept : saved_(rounding_detail::set(chop)) {}
    ~RoundingScope()
    {
        if (saved_ != ~0u) {
            rounding_detail::restore(saved_);
        }
    }
    RoundingScope(const RoundingScope&) = delete;
    RoundingScope& operator=(const RoundingScope&) = delete;

    /// Whether this thread rounds towards zero now.
    static bool chopping() noexcept { return rounding_detail::chopping(); }

private:
    unsigned int saved_;
};
/// FSTP to a float: the stack's value (53 bits: a double holds it exactly) converted in the
/// thread's rounding; in the server's chop mode towards zero, past the largest float the largest
/// float.
inline float chopToFloat(double v) { return static_cast<float>(v); }

/// Math_FloatToInt: towards zero (FISTP; past an int's range or a NaN, the integer indefinite,
/// 0x80000000).
constexpr int chop(float v)
{
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) {
        return static_cast<int>(0x80000000u);
    }
    return static_cast<int>(v);
}

/// A value on the game's x87 stack: a double, each operation the hardware's in the thread's
/// rounding. Built from a float or a double (an FLD), it is stored back to a float only by f()
/// (FSTP): there is no implicit conversion, so the exe's stores stay where they are. Comparisons
/// are the stack's (FCOMP on the 53-bit value; a NaN compares false here, unlike FCOMP's flags:
/// the VM's own compares keep those).
struct X87 {
    double v = 0.0;

    constexpr X87() = default;
    constexpr X87(float f) : v(f) {}                   // FLD float
    static constexpr X87 fromDouble(double d) { X87 x; x.v = d; return x; } // FLD double (exact)

    /// FSTP float.
    float f() const { return chopToFloat(v); }
    /// FSTP double: the stack's value as it is (53 bits).
    constexpr double d() const { return v; }

    friend X87 operator+(X87 a, X87 b) { return fromDouble(a.v + b.v); }
    friend X87 operator-(X87 a) { return fromDouble(-a.v); } // FCHS
    friend X87 operator-(X87 a, X87 b) { return fromDouble(a.v - b.v); }
    friend X87 operator*(X87 a, X87 b) { return fromDouble(a.v * b.v); }
    friend X87 operator/(X87 a, X87 b) { return fromDouble(a.v / b.v); }
    X87& operator+=(X87 b) { return *this = *this + b; }
    X87& operator-=(X87 b) { return *this = *this - b; }
    X87& operator*=(X87 b) { return *this = *this * b; }
    X87& operator/=(X87 b) { return *this = *this / b; }

    friend constexpr bool operator<(X87 a, X87 b) { return a.v < b.v; }
    friend constexpr bool operator<=(X87 a, X87 b) { return a.v <= b.v; }
    friend constexpr bool operator>(X87 a, X87 b) { return a.v > b.v; }
    friend constexpr bool operator>=(X87 a, X87 b) { return a.v >= b.v; }
    friend constexpr bool operator==(X87 a, X87 b) { return a.v == b.v; }
    friend constexpr bool operator!=(X87 a, X87 b) { return a.v != b.v; }
};

/// FSQRT (sqrtsd keeps the thread's rounding).
inline X87 sqrt(X87 x) { return X87::fromDouble(std::sqrt(x.v)); }
/// FABS.
inline X87 fabs(X87 x) { return X87::fromDouble(std::fabs(x.v)); }

/// Math_FloatToInt of the value stored to a float (its float argument): FSTP then FISTP.
inline int floatToInt(X87 x) { return chop(x.f()); }

// One float operation stored (FLD, FADD / FSUB / FMUL / FDIV, FSTP float). The script VM's
// arithmetic opcodes and the game's single-step figures share these.
inline float chopAdd(float a, float b) { return (X87(a) + b).f(); }
inline float chopSub(float a, float b) { return (X87(a) - b).f(); }
inline float chopMul(float a, float b) { return (X87(a) * b).f(); }
inline float chopDiv(float a, float b) { return (X87(a) / b).f(); }

/// utilNormalizeVector: the length's root on the stack, stored as a double then as a float
/// (the exe's double-to-float helper), each part divided by it and stored. No test of a zero
/// length: (0, 0) gives NaNs, as the game's.
inline void normalizeVector(float& x, float& y)
{
    const float len = sqrt(X87(x) * x + X87(y) * y).f();
    x = (X87(x) / len).f();
    y = (X87(y) / len).f();
}

/// __ftol: the stack's value truncated to 64 bits, the low 32 kept (no float store between).
inline int ftol(X87 x)
{
    if (!(std::fabs(x.v) < 9223372036854775808.0)) {
        return 0; // (FISTP's 64-bit integer indefinite, 0x8000000000000000: its low half)
    }
    // (The conversion truncates whatever the rounding.)
    return static_cast<int>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(x.v))));
}

} // namespace nox::util
