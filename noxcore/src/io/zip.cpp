// Stored zip archives (zip.hpp).
#include "noxcore/io/zip.hpp"

#include "noxcore/crypto/crc.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace nox::io {

namespace {

constexpr std::uint32_t kLocalSig = 0x04034B50;
constexpr std::uint32_t kCentralSig = 0x02014B50;
constexpr std::uint32_t kEndSig = 0x06054B50;
constexpr std::uint16_t kUtf8Flag = 0x0800;  ///< general purpose bit 11: the name is UTF-8
constexpr std::uint16_t kVersion = 10;       ///< 1.0: stored entries, folders
constexpr std::uint32_t kDosFolder = 0x10;   ///< an external attribute: MS-DOS's directory bit

void u16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void u32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint16_t flagsFor(std::string_view name)
{
    return std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) >= 0x80; }) ? kUtf8Flag : 0;
}

bool goodName(std::string_view name)
{
    if (name.empty() || name.size() > 0xFFFF || name.front() == '/' || name.find('\\') != std::string_view::npos ||
        name.find(':') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    for (std::size_t at = 0; at < name.size();) {
        const std::size_t slash = std::min(name.find('/', at), name.size());
        const std::string_view part = name.substr(at, slash - at);
        if (part == ".." || part == "." || (part.empty() && slash < name.size())) {
            return false;
        }
        at = slash + 1;
    }
    return true;
}

struct Reader {
    std::span<const std::uint8_t> in;
    bool ok(std::size_t at, std::size_t n) const { return at <= in.size() && n <= in.size() - at; }
    std::uint16_t r16(std::size_t at) const { return static_cast<std::uint16_t>(in[at] | in[at + 1] << 8); }
    std::uint32_t r32(std::size_t at) const
    {
        return in[at] | in[at + 1] << 8 | in[at + 2] << 16 | static_cast<std::uint32_t>(in[at + 3]) << 24;
    }
};

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept
{
    return crypto::updateCrc(crypto::kCrcInit, data); // (one call: the standard CRC-32)
}

void ZipWriter::add(std::string_view name, std::span<const std::uint8_t> data, std::uint16_t dosTime, std::uint16_t dosDate)
{
    if (!goodName(name)) {
        throw std::invalid_argument("zip: \"" + std::string(name) + "\" isn't a relative '/' path");
    }
    if (std::ranges::any_of(central_, [&](const Central& c) { return c.name == name; })) {
        throw std::invalid_argument("zip: \"" + std::string(name) + "\" is there twice");
    }
    const bool folder = name.back() == '/';
    if (folder && !data.empty()) {
        throw std::invalid_argument("zip: a folder's entry has no data");
    }
    const std::uint64_t end = body_.size() + 30 + name.size() + data.size();
    if (central_.size() >= 0xFFFF || data.size() > std::numeric_limits<std::uint32_t>::max() || end > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("zip: past the format's limits (no zip64 here)");
    }
    Central c;
    c.name = std::string(name);
    c.crc = data.empty() ? 0 : crc32(data);
    c.size = static_cast<std::uint32_t>(data.size());
    c.offset = static_cast<std::uint32_t>(body_.size());
    c.time = dosTime;
    c.date = dosDate;
    u32(body_, kLocalSig);
    u16(body_, kVersion);
    u16(body_, flagsFor(name));
    u16(body_, 0); // (stored)
    u16(body_, c.time);
    u16(body_, c.date);
    u32(body_, c.crc);
    u32(body_, c.size); // (compressed: the same, stored)
    u32(body_, c.size);
    u16(body_, static_cast<std::uint16_t>(name.size()));
    u16(body_, 0); // (no extra field)
    body_.insert(body_.end(), name.begin(), name.end());
    body_.insert(body_.end(), data.begin(), data.end());
    central_.push_back(std::move(c));
}

std::vector<std::uint8_t> ZipWriter::finish() const
{
    std::vector<std::uint8_t> out = body_;
    const auto start = static_cast<std::uint32_t>(out.size());
    for (const Central& c : central_) {
        u32(out, kCentralSig);
        u16(out, 20); // (made by: 2.0, MS-DOS's attributes)
        u16(out, kVersion);
        u16(out, flagsFor(c.name));
        u16(out, 0);
        u16(out, c.time);
        u16(out, c.date);
        u32(out, c.crc);
        u32(out, c.size);
        u32(out, c.size);
        u16(out, static_cast<std::uint16_t>(c.name.size()));
        u16(out, 0); // (extra)
        u16(out, 0); // (comment)
        u16(out, 0); // (disk)
        u16(out, 0); // (internal attributes)
        u32(out, c.name.back() == '/' ? kDosFolder : 0);
        u32(out, c.offset);
        out.insert(out.end(), c.name.begin(), c.name.end());
    }
    const auto size = static_cast<std::uint32_t>(out.size() - start);
    u32(out, kEndSig);
    u16(out, 0);
    u16(out, 0);
    u16(out, static_cast<std::uint16_t>(central_.size()));
    u16(out, static_cast<std::uint16_t>(central_.size()));
    u32(out, size);
    u32(out, start);
    u16(out, 0); // (no comment)
    return out;
}

std::optional<std::vector<ZipEntry>> readStoredZip(std::span<const std::uint8_t> bytes, std::string& error)
{
    const Reader r{bytes};
    // The end record: the last one, its comment at most 64 KB.
    std::size_t end = std::string::npos;
    if (bytes.size() >= 22) {
        const std::size_t lowest = bytes.size() > 22 + 0xFFFF ? bytes.size() - 22 - 0xFFFF : 0;
        for (std::size_t at = bytes.size() - 22;; --at) {
            if (r.r32(at) == kEndSig) {
                end = at;
                break;
            }
            if (at == lowest) break;
        }
    }
    if (end == std::string::npos) {
        error = "not a zip (no end record)";
        return std::nullopt;
    }
    const std::uint16_t count = r.r16(end + 10);
    const std::uint32_t cdSize = r.r32(end + 12);
    const std::uint32_t cdAt = r.r32(end + 16);
    if (!r.ok(cdAt, cdSize) || cdAt + static_cast<std::size_t>(cdSize) > end) {
        error = "the central directory is outside the file";
        return std::nullopt;
    }
    std::vector<ZipEntry> out;
    std::size_t at = cdAt;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (!r.ok(at, 46) || r.r32(at) != kCentralSig) {
            error = "a central directory entry is cut short";
            return std::nullopt;
        }
        const std::uint16_t method = r.r16(at + 10);
        const std::uint32_t crc = r.r32(at + 16);
        const std::uint32_t csize = r.r32(at + 20);
        const std::uint32_t usize = r.r32(at + 24);
        const std::size_t nameLen = r.r16(at + 28);
        const std::size_t skip = static_cast<std::size_t>(r.r16(at + 30)) + r.r16(at + 32);
        const std::uint32_t local = r.r32(at + 42);
        if (!r.ok(at + 46, nameLen + skip)) {
            error = "a central directory entry is cut short";
            return std::nullopt;
        }
        ZipEntry e;
        e.name.assign(reinterpret_cast<const char*>(bytes.data() + at + 46), nameLen);
        at += 46 + nameLen + skip;
        if (method != 0 || csize != usize) {
            error = e.name + " is compressed (only stored entries are read)";
            return std::nullopt;
        }
        if (!r.ok(local, 30) || r.r32(local) != kLocalSig) {
            error = e.name + ": its local header is missing";
            return std::nullopt;
        }
        const std::size_t dataAt = local + 30 + static_cast<std::size_t>(r.r16(local + 26)) + r.r16(local + 28);
        if (!r.ok(dataAt, usize)) {
            error = e.name + ": its data is outside the file";
            return std::nullopt;
        }
        e.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataAt), bytes.begin() + static_cast<std::ptrdiff_t>(dataAt + usize));
        if ((e.data.empty() ? 0 : crc32(e.data)) != crc) {
            error = e.name + ": its CRC doesn't match";
            return std::nullopt;
        }
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace nox::io
