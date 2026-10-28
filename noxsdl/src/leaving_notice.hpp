// The notice shown while our leave waits for the server to take it (no SDL here,
// so the tests check it).
//
// Leaving sends OUTGOING_CLIENT and waits up to 20 s for the server's ack before the game is left
// anyway (HeadlessClient::quit, as the exe's client waits after Game_FinishQuit). The exe's window
// stands frozen through that wait; ours keeps drawing, and says what's going on
// (docs/vanilla_deviations.md VD-33):
//   - after the lag window's Exit (the server already silent): at once;
//   - after an ordinary leave: only once it has waited a second, so a quick leave shows nothing.
// The text counts down the seconds left of the 20.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace noxsdl {

struct LeavingNotice {
    static constexpr std::uint32_t kWaitMs = 20'000; ///< HeadlessClient::kQuitAckWaitMs
    static constexpr std::uint32_t kGraceMs = 1'000; ///< an ordinary leave's wait before it shows

    /// The whole seconds left of the wait `waitedMs` into it (rounded up: 20 at the start, 1 in
    /// its last second, 0 once it's over).
    static int secondsLeft(std::uint32_t waitedMs)
    {
        if (waitedMs >= kWaitMs) {
            return 0;
        }
        return static_cast<int>((kWaitMs - waitedMs + 999) / 1000);
    }

    /// The notice `waitedMs` into the wait, or nothing while none shows. `serverSilent`: the leave
    /// is the lag window's Exit.
    static std::optional<std::string> text(std::uint32_t waitedMs, bool serverSilent)
    {
        if (!serverSilent && waitedMs < kGraceMs) {
            return std::nullopt;
        }
        const int left = secondsLeft(waitedMs);
        return std::string("Leaving the game: the server isn't answering.\n\nWaiting up to 20 seconds for it to confirm...\n\n") +
               std::to_string(left) + (left == 1 ? " second left" : " seconds left");
    }
};

} // namespace noxsdl
