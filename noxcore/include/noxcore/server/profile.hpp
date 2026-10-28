// The server's own section timer: where a frame's
// time goes, by section, for a server's benchmark and profile runs. Off unless switched on; then each
// section costs two clock reads. One process-wide record (the server runs on one thread).
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace nox::server {

enum class ProfileSection : std::uint8_t {
    Frame,          ///< Server::step's frames, all of it
    Substep,        ///< Server::step's substeps (the important objects carried on)
    FrameStart,     ///< the frame's start: positions back, the new things' pass
    FrameChecks,    ///< updateServer's: time limit, camper alarm, chat bases, quest
    Packets,        ///< the host's packets read (its hook)
    TickEnd,        ///< the tick's end (the new things' pass, saves, pause)
    UpdateWorld,    ///< Server::updateWorld, all of it
    Effects,        ///< the players' worn effects and cursor objects
    UpdatePlayers,  ///< the update walk: players
    UpdateMonsters, ///< the update walk: monsters
    UpdateObjects,  ///< the update walk: the rest (doors, objects' UPDATEs)
    FallSettle,     ///< the update walk: each one's fall and motion test
    Missiles,
    Motion,         ///< moveObjects
    Contacts,       ///< dispatchContacts
    Buffs,          ///< the enchantments' and poison's countdown
    EndPass,        ///< the motion's end pass, secret walls and decay
    Respawn,
    Spells,
    Abilities,
    Scripts,        ///< the script timers
    Sweep,          ///< the frame's deletions
    HostSubstep,    ///< GameHost::sendSubstep
    HostAfterFrame, ///< GameHost::afterFrame (each client's update)
    HostUpdates,    ///< in it: GameHost::updatePlayer (what each client sees)
    HostSounds,     ///< in it: the sounds each client hears
    HostReports,    ///< in it: the quest's lives and keys reports
    HostPoll,       ///< the transport's poll
    Count
};

inline constexpr std::array<const char*, static_cast<std::size_t>(ProfileSection::Count)> kProfileSectionNames{
    "frame",         "substep",        "  frame start",  "  frame checks", "  packets",       "  tick end",      "updateWorld",    "  effects",  "  update: players", "  update: monsters",
    "  update: rest", "  fall + settle", "  missiles",      "  motion",   "  contacts",        "  buffs",
    "  end pass",    "  respawn",       "  spells",         "  abilities", "  scripts",         "  deletions",
    "host substep",  "host afterFrame", "  client updates", "  client sounds",  "  quest reports",   "host poll"};

struct FrameProfile {
    bool on = false;
    std::array<double, static_cast<std::size_t>(ProfileSection::Count)> seconds{};
    std::array<std::uint64_t, static_cast<std::size_t>(ProfileSection::Count)> calls{};

    /// "update: rest" by the UPDATE function's name ("-" none it runs).
    std::unordered_map<std::string, std::pair<double, std::uint64_t>> byUpdate;

    void reset()
    {
        seconds.fill(0);
        calls.fill(0);
        byUpdate.clear();
    }
    void addUpdate(std::string_view name, double t)
    {
        auto& e = byUpdate[std::string(name.empty() ? std::string_view("-") : name)];
        e.first += t;
        ++e.second;
    }
    void add(ProfileSection s, double t)
    {
        seconds[static_cast<std::size_t>(s)] += t;
        ++calls[static_cast<std::size_t>(s)];
    }
    /// A table: each section's ms a frame (over `frames`), its share of the frames' time, its calls.
    std::string report(std::uint64_t frames) const
    {
        std::string out;
        char line[160];
        const double frameTotal = seconds[static_cast<std::size_t>(ProfileSection::Frame)];
        const double n = frames ? static_cast<double>(frames) : 1.0;
        for (std::size_t i = 0; i < seconds.size(); ++i) {
            if (calls[i] == 0) {
                continue;
            }
            std::snprintf(line, sizeof line, "%-20s %9.4f ms/frame %6.1f%% %10llu calls\n", kProfileSectionNames[i], seconds[i] * 1000.0 / n,
                          frameTotal > 0 ? seconds[i] * 100.0 / frameTotal : 0.0, static_cast<unsigned long long>(calls[i]));
            out += line;
        }
        std::vector<std::pair<std::string, std::pair<double, std::uint64_t>>> named(byUpdate.begin(), byUpdate.end());
        std::ranges::sort(named, [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        if (!named.empty()) {
            out += "update: rest by UPDATE (top 15):\n";
        }
        for (std::size_t i = 0; i < named.size() && i < 15; ++i) {
            std::snprintf(line, sizeof line, "  %-28s %9.4f ms/frame %10llu calls\n", named[i].first.c_str(), named[i].second.first * 1000.0 / n,
                          static_cast<unsigned long long>(named[i].second.second));
            out += line;
        }
        return out;
    }
};

namespace detail {
/// (Made with the program, not at the first look: each look, several an object a frame, is then a
/// plain read.)
inline FrameProfile frameProfileRecord;
} // namespace detail

inline FrameProfile& frameProfile() noexcept
{
    return detail::frameProfileRecord;
}

/// Times its scope into `section` while the profile is on.
class ProfileScope {
public:
    explicit ProfileScope(ProfileSection s) : section_(s), on_(frameProfile().on)
    {
        if (on_) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~ProfileScope()
    {
        if (on_) {
            frameProfile().add(section_, std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count());
        }
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    ProfileSection section_;
    bool on_;
    std::chrono::steady_clock::time_point start_{};
};

} // namespace nox::server
