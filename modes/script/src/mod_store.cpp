// A mod's own key-value store (mod_store.hpp).
#include "mod_store.hpp"

#include "mod_parts.hpp"
#include "toolbox.hpp"

#include "noxcore/modes/script/manifest.hpp"

#include "noxcore/crypto/crc.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace nox::modes::script {

namespace fs = std::filesystem;

namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'N', 'X', 'S', 'V'};
constexpr std::size_t kHeader = 16;
constexpr char kExtension[] = ".nxv";

std::uint32_t crcOf(std::span<const std::uint8_t> bytes)
{
    return crypto::updateCrc(crypto::kCrcInit, bytes); // (one call: the standard CRC-32)
}

void putU32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::uint32_t getU32(std::span<const std::uint8_t> in, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(in[at + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return v;
}

bool deviceName(std::string_view s)
{
    if (s == "con" || s == "prn" || s == "aux" || s == "nul") {
        return true;
    }
    return s.size() == 4 && (s.starts_with("com") || s.starts_with("lpt")) && s[3] >= '1' && s[3] <= '9';
}

std::string utcStamp()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char s[32];
    std::snprintf(s, sizeof s, "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return s;
}

/// The open stores, by folder (weak: a store goes with its last game).
std::mutex gOpenMutex;
std::unordered_map<std::string, std::weak_ptr<ModStore>>& openStores()
{
    static auto* all = new std::unordered_map<std::string, std::weak_ptr<ModStore>>(); // (never freed: games on other threads at exit)
    return *all;
}

} // namespace

// ------------------------------------------------------------------------- the store

std::shared_ptr<ModStore> ModStore::open(const fs::path& modData, std::string_view id)
{
    const fs::path folder = (modData / std::string(id)).lexically_normal();
    const std::lock_guard lock(gOpenMutex);
    auto& all = openStores();
    std::erase_if(all, [](const auto& e) { return e.second.expired(); });
    if (auto it = all.find(folder.generic_string()); it != all.end()) {
        if (auto s = it->second.lock()) {
            return s;
        }
    }
    auto s = std::make_shared<ModStore>(folder);
    all[folder.generic_string()] = s;
    return s;
}

std::uint64_t ModStore::newOwner() noexcept
{
    static std::atomic<std::uint64_t> next{1};
    return next++;
}

ModStore::ModStore(fs::path folder) : folder_(std::move(folder)) {}

ModStore::~ModStore()
{
    {
        const std::lock_guard lock(m_);
        stop_ = true;
    }
    wake_.notify_all();
    if (writer_.joinable()) {
        writer_.join(); // (it writes the queue out first)
    }
}

std::string ModStore::keyProblem(std::string_view key)
{
    if (key.empty() || key.size() > kMaxKey) {
        return "a key is 1-" + std::to_string(kMaxKey) + " letters";
    }
    int segments = 0;
    for (std::size_t start = 0; start <= key.size();) {
        std::size_t end = key.find('/', start);
        if (end == std::string_view::npos) {
            end = key.size();
        }
        const std::string_view seg = key.substr(start, end - start);
        if (seg.empty() || seg.size() > kMaxSegment) {
            return "each part of a key (between /) is 1-" + std::to_string(kMaxSegment) + " letters";
        }
        if (!std::ranges::all_of(seg, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; })) {
            return "a key is a-z, 0-9, _ and - (lower case), its parts joined by / (nox.store.keyOf makes a part of any text)";
        }
        if (deviceName(seg)) {
            return "\"" + std::string(seg) + "\" can't be a part of a key (a device's name on Windows)";
        }
        if (++segments > kMaxSegments) {
            return "a key has at most " + std::to_string(kMaxSegments) + " parts";
        }
        start = end + 1;
    }
    return {};
}

std::optional<std::string> ModStore::keyOf(std::string_view text)
{
    if (text.empty()) {
        return std::string("-");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('-');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
        if (out.size() > kMaxSegment) {
            return std::nullopt;
        }
    }
    if (deviceName(out)) { // ("con" itself: its first letter as hex, still one to one)
        out = std::string("-") + kHex[static_cast<unsigned char>(out[0]) >> 4] + kHex[out[0] & 15] + out.substr(1);
    }
    return out;
}

fs::path ModStore::pathOf(std::string_view key) const
{
    return folder_ / fs::path(std::string(key) + kExtension);
}

void ModStore::indexLocked()
{
    if (indexed_) {
        return;
    }
    indexed_ = true;
    std::error_code ec;
    if (!fs::is_directory(folder_, ec)) {
        return;
    }
    for (auto it = fs::recursive_directory_iterator(folder_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() >= kMaxSegments) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || it->path().extension() != kExtension) {
            continue;
        }
        std::string key = it->path().lexically_relative(folder_).generic_string();
        key.resize(key.size() - (sizeof kExtension - 1));
        if (keyProblem(key).empty()) {
            index_.insert(std::move(key));
        }
    }
}

ModStore::Got ModStore::readLocked(std::string_view key)
{
    Got got;
    const fs::path path = pathOf(key);
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return got; // (Missing)
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0 || static_cast<std::uint64_t>(size) <= kMaxValue + kHeader) {
            bytes.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!in) {
                bytes.clear();
            }
        }
    }
    std::string why;
    if (bytes.size() < kHeader || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        why = "not a store file (or over " + std::to_string(kMaxValue >> 10) + " KB)";
    } else if (bytes[4] != 1 || bytes[5] != 0) {
        why = "a store file of another version";
    } else if (getU32(bytes, 8) != bytes.size() - kHeader) {
        why = "its size is wrong (cut short?)";
    } else if (getU32(bytes, 12) != crcOf(std::span(bytes).subspan(kHeader))) {
        why = "its CRC is wrong (damaged)";
    }
    if (why.empty()) {
        got.state = Got::State::Found;
        got.payload.assign(bytes.begin() + kHeader, bytes.end());
        return got;
    }
    // Set aside, never half-read: it stays for whoever looks, out of the store's way.
    fs::path aside = path;
    aside += ".bad-" + utcStamp();
    std::error_code ec;
    fs::rename(path, aside, ec);
    index_.erase(std::string(key));
    got.state = Got::State::Damaged;
    got.why = std::string(key) + ": " + why + (ec ? " (it couldn't be set aside)" : "; set aside as " + aside.filename().string());
    errors_.push_back("store: " + got.why);
    return got;
}

ModStore::Got ModStore::get(std::string_view key)
{
    const std::lock_guard lock(m_);
    if (const auto it = pending_.find(key); it != pending_.end()) {
        Got got;
        if (it->second.file) {
            got.state = Got::State::Found;
            got.payload.assign(it->second.file->begin() + kHeader, it->second.file->end());
        }
        return got;
    }
    indexLocked();
    if (!index_.contains(key)) {
        return {};
    }
    return readLocked(key); // (under the lock: the writer's renames wait for it)
}

std::string ModStore::put(std::string_view key, std::vector<std::uint8_t> payload)
{
    if (payload.size() > kMaxValue) {
        return "over " + std::to_string(kMaxValue >> 10) + " KB";
    }
    auto file = std::make_shared<std::vector<std::uint8_t>>(kHeader);
    std::copy(kMagic.begin(), kMagic.end(), file->begin());
    (*file)[4] = 1;
    putU32(*file, 8, static_cast<std::uint32_t>(payload.size()));
    putU32(*file, 12, crcOf(payload));
    file->insert(file->end(), payload.begin(), payload.end());
    const std::lock_guard lock(m_);
    indexLocked();
    if (!index_.contains(key) && index_.size() >= kMaxKeys) {
        return "full";
    }
    if (queuedBytes_ + file->size() > kMaxQueued) {
        return "busy";
    }
    const std::uint64_t seq = ++seq_;
    queuedBytes_ += file->size();
    pending_[std::string(key)] = {seq, file};
    queue_.push_back({std::string(key), seq, std::move(file)});
    index_.insert(std::string(key));
    startWriterLocked();
    wake_.notify_one();
    return {};
}

bool ModStore::remove(std::string_view key)
{
    const std::lock_guard lock(m_);
    indexLocked();
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    index_.erase(it);
    const std::uint64_t seq = ++seq_;
    pending_[std::string(key)] = {seq, nullptr};
    queue_.push_back({std::string(key), seq, nullptr});
    startWriterLocked();
    wake_.notify_one();
    return true;
}

std::vector<std::string> ModStore::list(std::string_view prefix, std::size_t max)
{
    const std::lock_guard lock(m_);
    indexLocked();
    std::vector<std::string> out;
    for (auto it = index_.lower_bound(prefix); it != index_.end() && it->starts_with(prefix) && out.size() < max; ++it) {
        out.push_back(*it);
    }
    return out;
}

bool ModStore::lock(std::string_view key, std::uint64_t owner)
{
    const std::lock_guard lock(m_);
    const auto [it, made] = locks_.try_emplace(std::string(key), owner);
    return made || it->second == owner;
}

void ModStore::unlock(std::string_view key, std::uint64_t owner)
{
    const std::lock_guard lock(m_);
    if (const auto it = locks_.find(key); it != locks_.end() && it->second == owner) {
        locks_.erase(it);
    }
}

void ModStore::unlockAll(std::uint64_t owner)
{
    const std::lock_guard lock(m_);
    std::erase_if(locks_, [&](const auto& e) { return e.second == owner; });
}

void ModStore::flush()
{
    std::unique_lock lock(m_);
    drained_.wait(lock, [&] { return queue_.empty() && !busyWriting_; });
}

std::vector<std::string> ModStore::takeErrors()
{
    const std::lock_guard lock(m_);
    return std::exchange(errors_, {});
}

void ModStore::startWriterLocked()
{
    if (!writer_.joinable()) {
        writer_ = std::thread([this] { writerLoop(); });
    }
}

void ModStore::writerLoop()
{
    std::unique_lock lock(m_);
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
            return; // (stopping, the queue written)
        }
        Write w = std::move(queue_.front());
        queue_.pop_front();
        busyWriting_ = true;
        const fs::path path = pathOf(w.key);
        std::string failed;
        lock.unlock();
        // The file's bytes beside it first (no lock: a get may read the old one meanwhile).
        fs::path tmp = path;
        tmp += ".tmp";
        std::error_code ec;
        if (w.file) {
            fs::create_directories(path.parent_path(), ec);
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(w.file->data()), static_cast<std::streamsize>(w.file->size()));
            out.flush();
            if (!out) {
                failed = "can't write " + tmp.string();
            }
        }
        lock.lock();
        // Its new name (or its removal) under the lock: a get reads it whole, before or after.
        if (failed.empty()) {
            if (w.file) {
                fs::rename(tmp, path, ec);
                if (ec) {
                    failed = "can't replace " + path.string() + ": " + ec.message();
                }
            } else {
                fs::remove(path, ec);
            }
        }
        if (!failed.empty()) {
            fs::remove(tmp, ec);
            errors_.push_back("store: " + w.key + ": " + failed);
        }
        if (const auto it = pending_.find(w.key); it != pending_.end() && it->second.seq == w.seq) {
            pending_.erase(it);
        }
        if (w.file) {
            queuedBytes_ -= std::min(queuedBytes_, w.file->size());
        }
        busyWriting_ = false;
        if (queue_.empty()) {
            drained_.notify_all();
        }
    }
}

// ------------------------------------------------------------------------- nox.store

namespace {

ModStore* storeOf(lua_State* L)
{
    Parts& parts = toolbox::partsOf(L);
    if (!parts.store) {
        const ServerContext& ctx = toolbox::contextOf(L);
        if (ctx.modData.empty()) {
            luaL_error(L, "this host keeps no store");
        }
        parts.store = ModStore::open(ctx.modData, toolbox::manifestOf(L).info.id);
    }
    for (std::string& e : parts.store->takeErrors()) {
        toolbox::report(L, std::move(e));
    }
    return parts.store.get();
}

std::string_view keyArg(lua_State* L, int idx)
{
    size_t n = 0;
    const char* s = luaL_checklstring(L, idx, &n);
    const std::string_view key(s, n);
    if (const std::string why = ModStore::keyProblem(key); !why.empty()) {
        luaL_argerror(L, idx, why.c_str());
    }
    return key;
}

int storeGet(lua_State* L)
{
    const std::string_view key = keyArg(L, 1);
    const ModStore::Got got = storeOf(L)->get(key);
    switch (got.state) {
    case ModStore::Got::State::Missing: lua_pushnil(L); return 1;
    case ModStore::Got::State::Damaged:
        lua_pushnil(L);
        lua_pushstring(L, "damaged");
        return 2;
    case ModStore::Got::State::Found: break;
    }
    if (!decodeValue(L, got.payload, ModStore::kLimits)) {
        lua_pushnil(L);
        lua_pushstring(L, "damaged");
        toolbox::report(L, "store: " + std::string(key) + ": its value can't be read");
        return 2;
    }
    return 1;
}

int storePut(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.store.put");
    const std::string_view key = keyArg(L, 1);
    luaL_checkany(L, 2);
    std::vector<std::uint8_t> payload;
    std::string error;
    if (lua_isnil(L, 2)) {
        error = "nil isn't a value to keep (nox.store.remove takes a key away)";
    } else if (encodeValue(L, 2, payload, error, ModStore::kLimits)) {
        error = storeOf(L)->put(key, std::move(payload));
    }
    if (!error.empty()) {
        lua_pushboolean(L, 0);
        lua_pushlstring(L, error.data(), error.size());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int storeRemove(lua_State* L)
{
    toolbox::refuseInsideAsked(L, "nox.store.remove");
    lua_pushboolean(L, storeOf(L)->remove(keyArg(L, 1)));
    return 1;
}

int storeList(lua_State* L)
{
    size_t n = 0;
    const char* s = luaL_optlstring(L, 1, "", &n);
    const auto keys = storeOf(L)->list(std::string_view(s, n));
    lua_createtable(L, static_cast<int>(keys.size()), 0);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        lua_pushlstring(L, keys[i].data(), keys[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int storeLock(lua_State* L)
{
    const std::string_view key = keyArg(L, 1);
    lua_pushboolean(L, storeOf(L)->lock(key, toolbox::partsOf(L).storeOwner));
    return 1;
}

int storeUnlock(lua_State* L)
{
    const std::string_view key = keyArg(L, 1);
    storeOf(L)->unlock(key, toolbox::partsOf(L).storeOwner);
    return 0;
}

int storeKeyOf(lua_State* L)
{
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    const auto seg = ModStore::keyOf(std::string_view(s, n));
    luaL_argcheck(L, seg.has_value(), 1, "too long for a key's part (64 letters made)");
    lua_pushlstring(L, seg->data(), seg->size());
    return 1;
}

} // namespace

void pushStoreTable(lua_State* L)
{
    static constexpr std::pair<const char*, lua_CFunction> kStore[] = {
        {"get", storeGet},   {"put", storePut},       {"remove", storeRemove}, {"list", storeList},
        {"lock", storeLock}, {"unlock", storeUnlock}, {"keyOf", storeKeyOf},
    };
    lua_createtable(L, 0, static_cast<int>(std::size(kStore)));
    for (const auto& [name, fn] : kStore) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    lua_setreadonly(L, -1, 1);
}

std::uint64_t clientKeyRealm(const fs::path& modData, std::string_view id, std::string& error)
{
    const fs::path file = (modData / (std::string(id) + ".realm")).lexically_normal();
    const auto read = [&]() -> std::uint64_t {
        std::ifstream f(file, std::ios::binary);
        std::string text;
        std::getline(f, text);
        if (text.size() < 16) {
            return 0;
        }
        std::uint64_t v = 0;
        for (const char c : std::string_view(text).substr(0, 16)) {
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) {
                return 0;
            }
            v = v << 4 | static_cast<std::uint64_t>(d);
        }
        return v;
    };
    std::error_code ec;
    fs::create_directories(modData, ec);
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (fs::exists(file, ec)) {
            if (const std::uint64_t v = read()) {
                return v;
            }
            // (Being written by another game this moment, or damaged: a short wait, then again.)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::random_device rd;
        std::uint64_t v = 0;
        while (v == 0) {
            v = static_cast<std::uint64_t>(rd()) << 32 | rd();
        }
        char text[18];
        std::snprintf(text, sizeof text, "%016llx\n", static_cast<unsigned long long>(v));
        // Exclusive: only one game makes it ("x": fails when it's there already).
        if (std::FILE* f = std::fopen(file.string().c_str(), "wbx")) {
            const bool ok = std::fwrite(text, 1, 17, f) == 17;
            std::fclose(f);
            if (ok) {
                return v;
            }
        }
    }
    error = file.string() + ": the client key realm can't be made or read (a damaged file is set aside by hand)";
    return 0;
}

} // namespace nox::modes::script
