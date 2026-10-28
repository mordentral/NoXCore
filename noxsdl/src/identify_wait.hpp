// The join's wait for a noxcpp host's answer to our extended query (live_session.cpp), when the host
// must know us before we join: our content packs add object types (a host with added types lets in
// only a game whose query it has), or the game is NoX Core only (a host holds out and puts out a
// client it takes for a stock one). The query is resent every kResendMs till the answer comes,
// kWaitMs at most; then the join. No SDL here: the tests use it.
#pragma once

#include "noxcore/lobby/browser.hpp"
#include "noxcore/lobby/extended.hpp"

#include <cstdint>

namespace noxsdl {

struct IdentifyWait {
    static constexpr std::uint32_t kWaitMs = 3000;
    static constexpr std::uint32_t kResendMs = 250;
    std::uint32_t firstAt = 0;
    std::uint32_t askedAt = 0;

    /// The query just sent (its first copies).
    void begin(std::uint32_t nowMs) { firstAt = askedAt = nowMs; }
    enum class Step : std::uint8_t { Join, Resend, Wait };
    /// What now: join (answered, or waited long enough), resend the query, or wait.
    Step step(std::uint32_t nowMs, bool answered)
    {
        if (answered || nowMs - firstAt >= kWaitMs) {
            return Step::Join;
        }
        if (nowMs - askedAt >= kResendMs) {
            askedAt = nowMs;
            return Step::Resend;
        }
        return Step::Wait;
    }
};

/// Whether joining the listed game `entry` waits for the answer: a NoX Core-only game's
/// (ServerFeature::NoxCoreOnly in its extended reply), or one with a lowest client level (its
/// host's features keyed to our level reach us only if it has our query at the connect). Every
/// other host keeps the join's timing.
inline bool joinWaitsForIdentity(const nox::lobby::ServerEntry* entry)
{
    return entry && entry->extended &&
           ((entry->extended->features & nox::lobby::ServerFeature::NoxCoreOnly) != 0 || entry->extended->minClientLevel != 0);
}

/// Whether a join waits for the host's answer before it begins (live_session.cpp): our content
/// packs add object types (`addsTypes`), or the listed game wants to know us first
/// (`identifyFirst`: joinWaitsForIdentity); never at a host the game list saw as stock
/// (`knownStock`). Our client level alone never makes a join wait: a stock host never answers,
/// and the list can't tell an XWIS GAME.EXE game is one (it answers no ping), so every such
/// join would wait the whole kWaitMs. The query still goes out ahead of the join, as ModeUi's
/// always has; a host with a lowest level refuses a lower client at its connect, and the reply
/// that comes after says why (client_level_gate.hpp).
inline bool joinWaits(bool addsTypes, bool identifyFirst, bool knownStock)
{
    return (addsTypes || identifyFirst) && !knownStock;
}

} // namespace noxsdl
