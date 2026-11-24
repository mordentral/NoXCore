// The one way NoX Core writes a character file (character_store.hpp).
#include "noxcore/player/character_store.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <system_error>

namespace nox::player::character_store {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBlock = crypto::Blowfish::kBlockSize;
constexpr auto kLabelsId = static_cast<std::uint32_t>(SectionId::NoxCoreLabels);

/// The labels waiting for a file's write, by the file (its path as key: absolute, its letters'
/// case aside on Windows).
struct Waiting {
    std::vector<std::uint8_t> labels;
    bool unwritten = false; ///< (set since the last write that carried them)
};

std::mutex& waitingLock()
{
    static std::mutex m;
    return m;
}

std::map<std::wstring, Waiting>& waiting()
{
    static std::map<std::wstring, Waiting> w;
    return w;
}

std::wstring keyOf(const fs::path& path)
{
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    if (ec) {
        p = path;
    }
    std::wstring key = p.lexically_normal().generic_wstring();
#ifdef _WIN32
    for (wchar_t& c : key) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
#endif
    return key;
}

/// The labels waiting for `path` (copied out), when there are; marked written.
std::optional<std::vector<std::uint8_t>> takeLabels(const fs::path& path)
{
    std::lock_guard lock(waitingLock());
    const auto it = waiting().find(keyOf(path));
    if (it == waiting().end()) {
        return std::nullopt;
    }
    return it->second.labels;
}

void markWritten(const fs::path& path)
{
    std::lock_guard lock(waitingLock());
    if (const auto it = waiting().find(keyOf(path)); it != waiting().end()) {
        it->second.unwritten = false;
    }
}

/// `bytes` as the whole of `path`: written to "<path>.tmp", then put in its place.
bool replaceWith(const fs::path& path, std::span<const std::uint8_t> bytes)
{
    return tryReplaceFileBytes(path, bytes, ".tmp");
}

/// `path`'s bytes as they are on disk; none when it can't be read. (The file itself: a file there
/// as named, so readFileBytes never looks for another of its name in another case.)
std::optional<std::vector<std::uint8_t>> bytesOf(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    try {
        return readFileBytes(path);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void section(ByteWriter& w, std::uint32_t id, std::span<const std::uint8_t> data)
{
    w.u32(id);
    w.align(kBlock);
    w.u64(data.size());
    w.bytes(data);
}

} // namespace

void encipher(std::vector<std::uint8_t>& plain)
{
    crypto::cipherForKey(crypto::FileKey::PlayerFile).encrypt(plain);
}

void write(const fs::path& path, const PlayerFile& file)
{
    std::vector<std::uint8_t> bytes;
    const std::optional<std::vector<std::uint8_t>> labels = takeLabels(path);
    if (labels) {
        PlayerFile with = file;
        putLabels(with, *labels);
        bytes = serializePlayerFile(with);
    } else {
        bytes = serializePlayerFile(file);
    }
    encipher(bytes);
    if (!replaceWith(path, bytes)) {
        throw std::system_error(errno ? errno : EIO, std::generic_category(), "cannot write " + pathUtf8(path));
    }
    if (labels) {
        markWritten(path);
    }
}

bool writeEnciphered(const fs::path& path, std::span<const std::uint8_t> enciphered)
{
    return replaceWith(path, enciphered);
}

bool writePlain(const fs::path& path, std::span<const std::uint8_t> plain)
{
    std::vector<std::uint8_t> bytes(plain.begin(), plain.end());
    bytes.resize((bytes.size() + kBlock - 1) / kBlock * kBlock, 0);
    encipher(bytes);
    return replaceWith(path, bytes);
}

bool appendPlain(const fs::path& path, std::span<const std::uint8_t> plain)
{
    std::optional<std::vector<std::uint8_t>> bytes = bytesOf(path);
    if (!bytes) {
        return false; // (an append's file must be there)
    }
    std::vector<std::uint8_t> more(plain.begin(), plain.end());
    more.resize((more.size() + kBlock - 1) / kBlock * kBlock, 0);
    encipher(more);
    bytes->insert(bytes->end(), more.begin(), more.end());
    return replaceWith(path, *bytes);
}

std::vector<std::uint8_t> sectionBytes(const std::vector<RawSection>& sections, const std::vector<std::uint8_t>* labels)
{
    ByteWriter w;
    for (const RawSection& s : sections) {
        section(w, s.id, s.data);
    }
    if (labels && !labels->empty() && labels->size() <= kMaxLabelBytes) {
        section(w, kLabelsId, *labels);
    }
    w.u32(0);
    w.align(kBlock);
    return std::move(w).take();
}

bool appendSections(const fs::path& path, const std::vector<RawSection>& sections)
{
    const std::optional<std::vector<std::uint8_t>> labels = takeLabels(path);
    if (!appendPlain(path, sectionBytes(sections, labels ? &*labels : nullptr))) {
        return false;
    }
    if (labels) {
        markWritten(path);
    }
    return true;
}

bool copy(const fs::path& from, const fs::path& to)
{
    const std::optional<std::vector<std::uint8_t>> bytes = bytesOf(from);
    return bytes && replaceWith(to, *bytes);
}

std::optional<std::vector<std::uint8_t>> labelsOf(const PlayerFile& file)
{
    for (auto it = file.sections.rbegin(); it != file.sections.rend(); ++it) {
        if (it->id == kLabelsId) {
            if (it->data.size() > kMaxLabelBytes) {
                return std::nullopt;
            }
            return it->data;
        }
    }
    return std::nullopt;
}

void putLabels(PlayerFile& file, const std::vector<std::uint8_t>& labels)
{
    std::erase_if(file.sections, [](const RawSection& s) { return s.id == kLabelsId; });
    if (labels.empty() || labels.size() > kMaxLabelBytes) {
        return;
    }
    file.sections.push_back({kLabelsId, labels});
    // The readers walk the sections until a 0 id: one must follow. Past the last section's data come
    // the zeros to the block, then the whole zero blocks; fewer than an id's 4 bytes of zeros to the
    // block: one zero block more.
    std::size_t end = 0;
    for (const RawSection& s : file.sections) {
        end = (end + 4 + kBlock - 1) / kBlock * kBlock + 8 + s.data.size();
    }
    const std::size_t pad = (kBlock - end % kBlock) % kBlock;
    if (pad < 4 && file.extraZeroBlocks == 0) {
        file.extraZeroBlocks = 1;
    }
}

void setLabels(const fs::path& path, std::vector<std::uint8_t> labels)
{
    std::lock_guard lock(waitingLock());
    Waiting& w = waiting()[keyOf(path)];
    if (w.labels != labels) {
        w.labels = std::move(labels);
        w.unwritten = true;
    }
}

bool labelsWaiting(const fs::path& path)
{
    std::lock_guard lock(waitingLock());
    const auto it = waiting().find(keyOf(path));
    return it != waiting().end() && it->second.unwritten;
}

bool gameEndWrites(std::uint32_t gameFlags)
{
    // (The original game's end writes the client's character only in a network game, not a quest:
    // a quest's is kept by its saves alone. NoX Core's solo and co-op campaign games keep theirs by
    // their saves too.)
    return (gameFlags & 0x2000u) != 0 && (gameFlags & (0x1000u | 0x800u)) == 0;
}

bool flushLabels(const fs::path& path, std::uint32_t gameFlags)
{
    std::optional<std::vector<std::uint8_t>> labels;
    if (gameEndWrites(gameFlags)) {
        std::lock_guard lock(waitingLock());
        const auto it = waiting().find(keyOf(path));
        if (it != waiting().end() && it->second.unwritten) {
            labels = it->second.labels;
        }
    }
    bool wrote = false;
    if (labels) {
        // The file as it is, read whole: written back only when it reads back byte for byte, so its
        // other sections are written exactly as they were.
        if (std::optional<std::vector<std::uint8_t>> bytes = bytesOf(path); bytes && !bytes->empty() && bytes->size() % kBlock == 0) {
            std::vector<std::uint8_t> plain = *bytes;
            crypto::cipherForKey(crypto::FileKey::PlayerFile).decrypt(plain);
            try {
                PlayerFile file = parsePlayerFile(plain);
                if (serializePlayerFile(file) == plain) {
                    putLabels(file, *labels);
                    std::vector<std::uint8_t> out = serializePlayerFile(file);
                    encipher(out);
                    wrote = replaceWith(path, out);
                }
            } catch (const std::exception&) {
            }
        }
    }
    forgetLabels(path);
    return wrote;
}

void forgetLabels(const fs::path& path)
{
    std::lock_guard lock(waitingLock());
    waiting().erase(keyOf(path));
}

void forgetAllLabels()
{
    std::lock_guard lock(waitingLock());
    waiting().clear();
}

} // namespace nox::player::character_store
