// Bots the game we host runs (Server Settings' Bots tab): noxcore's BotSquad, its bots joined to
// our server over the loopback as any client, on a thread of their own so their thinking and
// sockets don't take from our frames or the server's.
// Commands go in, a snapshot and the log come out.
#pragma once

#include "noxcore/bot/bot_squad.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace noxsdl {

class LocalBots {
public:
    /// `things` and `kinds` must outlive it.
    LocalBots(nox::bot::SquadOptions options, const nox::things::ThingDb& things, const nox::bot::ItemKinds& kinds);
    ~LocalBots(); ///< the thread stopped, the bots gone with the squad
    LocalBots(const LocalBots&) = delete;
    LocalBots& operator=(const LocalBots&) = delete;

    /// Run on the bots' thread before their next tick (with its clock).
    void post(std::function<void(nox::bot::BotSquad&, std::uint32_t nowMs)> command);
    std::vector<nox::bot::SquadBotStatus> status();
    std::vector<std::string> takeLog();

private:
    void run();

    nox::bot::BotSquad squad_;
    std::mutex mutex_;
    std::vector<std::function<void(nox::bot::BotSquad&, std::uint32_t)>> commands_;
    std::vector<nox::bot::SquadBotStatus> status_;
    std::vector<std::string> log_;
    std::atomic<bool> stop_{false};
    std::thread thread_; ///< (last: started once the rest is made)
};

} // namespace noxsdl
