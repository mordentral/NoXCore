// The game we host's UDP port opened on the router by UPnP (miniupnpc), so people outside can
// join without forwarding it by hand: the router found (SSDP), the port mapped to this machine,
// on a thread of its own (finding the router takes a couple of seconds); the mapping removed
// again when it's dropped. Not GAME.EXE's.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace noxsdl {

class PortMapper {
public:
    enum class State { Working, Mapped, Failed };

    /// Starts mapping `port` (UDP, the same number outside).
    explicit PortMapper(std::uint16_t port);
    ~PortMapper(); ///< the mapping removed (if made)
    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    State state() const noexcept { return state_; }
    /// What happened, for the log ("mapped 18590 to 192.168.1.20, outside 203.0.113.5", or why not).
    std::string detail() const;

    /// Drops a mapper without waiting: its discovery's end and the mapping's removal (seconds of
    /// talking to the router) run on a thread of their own, joined by joinRetired. A mapper made
    /// meanwhile waits for them before it maps, so a late removal can't take its mapping away.
    static void retire(std::unique_ptr<PortMapper> mapper);
    /// Waits for the retired mappers' removals (at quit: a mapping left behind would stay on
    /// the router, with no lease to end it).
    static void joinRetired();

private:
    void run();

    std::uint16_t port_;
    std::atomic<State> state_{State::Working};
    mutable std::mutex mutex_;
    std::string detail_;
    std::string controlUrl_, serviceType_; ///< for the removal
    bool retired_ = false; ///< retire()d (under its lock)
    std::thread thread_;
};

} // namespace noxsdl
