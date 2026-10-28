// LocalBots (see its header): the bot squad ticked on a thread of its own; commands go to it, and its
// status and log come back, under one lock.
#include "local_bots.hpp"

#include <chrono>

namespace noxsdl {

namespace {

std::uint32_t nowMs()
{
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

LocalBots::LocalBots(nox::bot::SquadOptions options, const nox::things::ThingDb& things, const nox::bot::ItemKinds& kinds)
    : squad_(std::move(options), things, kinds), thread_([this] { run(); })
{
}

LocalBots::~LocalBots()
{
    stop_ = true;
    thread_.join();
}

void LocalBots::post(std::function<void(nox::bot::BotSquad&, std::uint32_t)> command)
{
    std::lock_guard lock(mutex_);
    commands_.push_back(std::move(command));
}

std::vector<nox::bot::SquadBotStatus> LocalBots::status()
{
    std::lock_guard lock(mutex_);
    return status_;
}

std::vector<std::string> LocalBots::takeLog()
{
    std::lock_guard lock(mutex_);
    return std::exchange(log_, {});
}

void LocalBots::run()
{
    // Every few ms: the commands, a tick, what a UI shows.
    while (!stop_) {
        std::vector<std::function<void(nox::bot::BotSquad&, std::uint32_t)>> commands;
        {
            std::lock_guard lock(mutex_);
            commands.swap(commands_);
        }
        const std::uint32_t now = nowMs();
        std::vector<std::string> lines;
        for (auto& c : commands) {
            try {
                c(squad_, now);
            } catch (const std::exception& ex) {
                lines.push_back(std::string("error: ") + ex.what());
            }
        }
        try {
            squad_.tick(now);
        } catch (const std::exception& ex) {
            lines.push_back(std::string("error: ") + ex.what());
        }
        auto status = squad_.status();
        for (auto& l : squad_.takeLog()) {
            lines.push_back(std::move(l));
        }
        squad_.takeChat(); // (the game's chat: our own client shows it)
        {
            std::lock_guard lock(mutex_);
            status_ = std::move(status);
            for (auto& l : lines) {
                log_.push_back(std::move(l));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace noxsdl
