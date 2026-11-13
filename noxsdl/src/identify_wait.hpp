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
/// host's features keyed to our level reach us only if it has our query at the connect), or one
/// with a client key realm (our key is ours only if our keyed query is there before the connect).
/// Every other host keeps the join's timing.
inline bool joinWaitsForIdentity(const nox::lobby::ServerEntry* entry)
{
    return entry && entry->extended &&
           ((entry->extended->features & nox::lobby::ServerFeature::NoxCoreOnly) != 0 || entry->extended->minClientLevel != 0 ||
            entry->extended->realm != 0);
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

/// Whether the listed game `entry` is joined as the 1.2b client joins it, with nothing of NoX
/// Core's sent first (LiveSession's `stockShaped`): one the list had from XWIS and never heard
/// from. A GAME.EXE host started from Westwood Online answers no discover ping (its receive takes
/// 0x0C only in a LAN game) and the 1.2b client sends a listed game nothing before its TRY_JOIN; a
/// NoX Core host always answers the ping. Only once its ping has gone unanswered ("No response"):
/// a join waits for a ping still out (PingWait, below), so `checking` is never set here but where
/// that wait gave up at its cap with the ping still out (it can't outlast the browser's own
/// give-up, so only a list that stopped polling): then the game may be a NoX Core host whose
/// answer hasn't come, and is asked as before (the guard kept). A game typed in or from
/// servers.txt / the LAN is asked as before.
inline bool joinStockShaped(const nox::lobby::ServerEntry* entry)
{
    return entry && (entry->sources & nox::lobby::kFromXwis) != 0 && !entry->answered && !entry->checking &&
           !entry->extended;
}

/// Whether joining the listed game `entry` waits for its ping first (the owner's call,
/// 2026-11-13: "wait for the ping, its safer and better"): an XWIS game whose ping is still out
/// ("Checking..."), neither answered nor known as NoX Core yet. How it's joined (stock-shaped or
/// asked first: joinStockShaped) is decided once the ping has answered or given up. Anything else
/// (no ping out, a typed or saved address, a LAN game) is joined at once.
inline bool joinWaitsForPing(const nox::lobby::ServerEntry* entry)
{
    return entry && (entry->sources & nox::lobby::kFromXwis) != 0 && entry->checking && !entry->answered &&
           !entry->extended;
}

/// The join's wait for that ping (the join's dialog up meanwhile, its Cancel the join's): over once
/// the game has answered, its ping has run out ("No response"), it's gone from the list, or the
/// browser's own give-up time (lobby::kPingGiveUpMs) has passed since the wait began, whichever
/// comes first: it never outlasts the list's own "Checking...".
struct PingWait {
    static constexpr std::uint32_t kMaxMs = nox::lobby::kPingGiveUpMs;
    std::uint32_t startedAt = 0;

    enum class Step : std::uint8_t {
        Wait,       ///< the ping still out
        Answered,   ///< the game answered (a NoX Core host, or a LAN-style stock one): asked as now
        NoResponse, ///< the ping ran out: a stock host from Westwood Online, joined stock-shaped
        Gone,       ///< the game left the list (a refresh): joined as the list has it now (by address)
        GaveUp,     ///< kMaxMs passed with the ping still out: joined anyway (joinStockShaped's guard)
    };
    Step step(const nox::lobby::ServerEntry* entry, std::uint32_t nowMs) const
    {
        if (!entry) {
            return Step::Gone;
        }
        if (entry->answered || entry->extended) {
            return Step::Answered;
        }
        if (!entry->checking) {
            return Step::NoResponse;
        }
        return nowMs - startedAt >= kMaxMs ? Step::GaveUp : Step::Wait;
    }
    static const char* name(Step s)
    {
        switch (s) {
        case Step::Wait: return "waiting";
        case Step::Answered: return "answered";
        case Step::NoResponse: return "no response";
        case Step::Gone: return "gone from the list";
        case Step::GaveUp: return "gave up";
        }
        return "?";
    }
};

} // namespace noxsdl
