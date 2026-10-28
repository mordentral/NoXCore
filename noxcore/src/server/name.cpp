// The process-wide table behind Name: interned strings that compare by pointer.
#include "noxcore/server/name.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <ostream>

namespace nox::server {

namespace {

// 64-bit FNV-1a over the text's bytes.
constexpr std::size_t hashText(std::string_view s) noexcept
{
    std::uint64_t h = 1469598103934665603ull; // (FNV-1a)
    for (const char c : s) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
}

/// The process's names: chains of entries by hash, read without a lock (each chain's head is
/// published with release after its entry is whole), added to under one. Never freed, so a
/// Name stays good to the process's end (the table itself is never destroyed).
class Table {
public:
    Table()
    {
        // Pre-intern the names listed in NOX_SERVER_NAMES so each gets its fixed NameId (1-based in
        // list order); names interned later get NameId::None.
        static constexpr std::string_view kKnown[] = {
#define NOX_NAME_TEXT(n) #n,
            NOX_SERVER_NAMES(NOX_NAME_TEXT)
#undef NOX_NAME_TEXT
        };
        std::scoped_lock lock(mutex_);
        for (std::size_t i = 0; i < std::size(kKnown); ++i) {
            add(kKnown[i], hashText(kKnown[i]), static_cast<NameId>(i + 1));
        }
    }

    const NameEntry* find(std::string_view text, std::size_t hash) const noexcept
    {
        // kBuckets is a power of two, so the low bits of the hash pick the chain.
        for (const NameEntry* e = heads_[hash & (kBuckets - 1)].load(std::memory_order_acquire); e; e = e->next) {
            if (e->hash == hash && e->text == text) {
                return e;
            }
        }
        return nullptr;
    }

    const NameEntry* intern(std::string_view text)
    {
        const std::size_t hash = hashText(text);
        if (const NameEntry* e = find(text, hash)) {
            return e;
        }
        // Not found without the lock: look again under it before adding, as another thread may have
        // just added the same text.
        std::scoped_lock lock(mutex_);
        if (const NameEntry* e = find(text, hash)) {
            return e; // (another thread made it meanwhile)
        }
        return add(text, hash, NameId::None);
    }

private:
    static constexpr std::size_t kBuckets = std::size_t{1} << 12;

    /// (Under the lock.)
    const NameEntry* add(std::string_view text, std::size_t hash, NameId id)
    {
        NameEntry& e = entries_.emplace_back();
        e.text = std::string(text);
        e.hash = hash;
        e.id = id;
        e.traits = text.ends_with("FlameCleanse") ? NameTrait::FlameCleanse : std::uint8_t{0};
        // Publish the entry at the head of its chain only once it is filled in, so lock-free readers
        // never see a half-built entry.
        auto& head = heads_[hash & (kBuckets - 1)];
        e.next = head.load(std::memory_order_relaxed);
        head.store(&e, std::memory_order_release);
        return &e;
    }

    std::array<std::atomic<const NameEntry*>, kBuckets> heads_{};
    mutable std::mutex mutex_;
    std::deque<NameEntry> entries_; ///< (a deque: entries never move as it grows)
};

Table& table()
{
    static Table* const t = new Table; // (never destroyed: names outlive every other static)
    return *t;
}

} // namespace

const NameEntry* Name::intern(std::string_view text)
{
    return text.empty() ? nullptr : table().intern(text);
}

const std::string& Name::emptyString() noexcept
{
    static const std::string* const empty = new std::string;
    return *empty;
}

std::ostream& operator<<(std::ostream& os, const Name& n)
{
    return os << n.view();
}

} // namespace nox::server
