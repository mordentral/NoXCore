// nox.store: a mod's own key-value store on the host, under
// <ServerContext::modData>/<mod id>/, one file a key (<key>.nxv, its segments as folders):
//
//   "NXSV", u16 version (1), u16 0, u32 the payload's size, u32 its CRC-32, the payload
//   (value_codec's bytes, at most 256 KB), little-endian
//
// Written whole on the store's own writer thread, in order (<file>.tmp, then renamed over it), so
// a put never stalls the frame; a get or list of a key whose write is still queued answers from the
// queue. A bad file (its mark, size, CRC or value wrong) is set aside (<file>.bad-<UTC time>),
// never half-read. The keys are read once (one walk of the folder, at the first call) and kept.
// One store a folder a process, shared by every game of the mod the process runs (a pool runs
// several); lock() keeps a key to one game. Nothing is made, read or started until the first call.
// (Internal to the script modes' sources.)
#pragma once

#include "value_codec.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct lua_State;

namespace nox::modes::script {

class ModStore {
public:
    static constexpr std::size_t kMaxValue = 256u << 10;  ///< a value's encoded bytes
    static constexpr std::size_t kMaxQueued = 32u << 20;  ///< bytes waiting for the writer
    static constexpr std::size_t kMaxKeys = 10'000;
    static constexpr std::size_t kMaxList = 1000;         ///< keys a list gives
    static constexpr std::size_t kMaxKey = 160;
    static constexpr std::size_t kMaxSegment = 64;
    static constexpr int kMaxSegments = 8;
    /// The values' limits (value_codec's bytes, bigger than the UI's).
    static constexpr CodecLimits kLimits{.depth = 16, .pairs = 8192, .bytes = kMaxValue};

    /// The store of mod `id` in `modData` (`modData`/`id`/): the one this process has open, or a
    /// new one.
    static std::shared_ptr<ModStore> open(const std::filesystem::path& modData, std::string_view id);
    /// A token for one game's locks.
    static std::uint64_t newOwner() noexcept;

    explicit ModStore(std::filesystem::path folder);
    ~ModStore(); ///< (the queue written first)
    ModStore(const ModStore&) = delete;
    ModStore& operator=(const ModStore&) = delete;

    /// Empty: `key` is a key; else why not.
    static std::string keyProblem(std::string_view key);
    /// Any text as one key segment: a-z and 0-9 kept, every other byte '-' and two hex digits ("Bob"
    /// "-42ob"; empty text "-"). None: longer than a segment.
    static std::optional<std::string> keyOf(std::string_view text);

    struct Got {
        enum class State : std::uint8_t { Missing, Found, Damaged } state = State::Missing;
        std::vector<std::uint8_t> payload; ///< (Found) value_codec's bytes
        std::string why;                   ///< (Damaged) what was wrong, where it went
    };
    Got get(std::string_view key);
    /// Empty: queued; else why not ("full", "busy").
    std::string put(std::string_view key, std::vector<std::uint8_t> payload);
    /// Whether there was one.
    bool remove(std::string_view key);
    /// The keys starting with `prefix`, sorted, at most `max`.
    std::vector<std::string> list(std::string_view prefix, std::size_t max = kMaxList);

    /// True: `owner` holds `key` now (or did already); false: another does.
    bool lock(std::string_view key, std::uint64_t owner);
    void unlock(std::string_view key, std::uint64_t owner);
    void unlockAll(std::uint64_t owner);

    /// Every queued write done (a game's end).
    void flush();
    /// The writer's failures since the last call (for the script's errors).
    std::vector<std::string> takeErrors();

    const std::filesystem::path& folder() const noexcept { return folder_; }

private:
    struct Write {
        std::string key;
        std::uint64_t seq = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> file; ///< (null: a removal)
    };
    struct Pending {
        std::uint64_t seq = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> file;
    };
    std::filesystem::path pathOf(std::string_view key) const;
    void indexLocked();
    Got readLocked(std::string_view key);
    void startWriterLocked();
    void writerLoop();

    std::filesystem::path folder_;
    std::mutex m_;
    std::condition_variable wake_, drained_;
    std::deque<Write> queue_;
    std::map<std::string, Pending, std::less<>> pending_; ///< (the latest queued write of a key)
    std::size_t queuedBytes_ = 0;
    std::uint64_t seq_ = 0;
    bool busyWriting_ = false;
    bool stop_ = false;
    std::thread writer_;
    bool indexed_ = false;
    std::set<std::string, std::less<>> index_;
    std::map<std::string, std::uint64_t, std::less<>> locks_;
    std::vector<std::string> errors_;
};

/// The toolbox's nox.store table pushed (its functions).
void pushStoreTable(lua_State* L);

/// (mode.json's "clientKeys") A mod's client key realm on this host: `<modData>/<id>.realm`, 16 hex
/// digits, made at the first call (random, never 0) and kept for good. Made exclusive: of games
/// starting at once (a pool's) one writes it and every one reads that one. 0 when it can't be made
/// or read (`error` why).
std::uint64_t clientKeyRealm(const std::filesystem::path& modData, std::string_view id, std::string& error);

} // namespace nox::modes::script
