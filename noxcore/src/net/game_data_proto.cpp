// MSG_NOXCPP_GAMEDATA (game_data_proto.hpp).
#include "noxcore/net/game_data_proto.hpp"

#include "noxcore/net/messages.hpp"

#include <algorithm>

namespace nox::net::gamedata {

namespace {

void put16(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    put16(out, v);
    put16(out, v >> 16);
}

std::uint32_t get32(std::span<const std::uint8_t> b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

/// {0xF9, sub, u16 length, body}.
std::vector<std::uint8_t> frame(std::uint8_t sub, const std::vector<std::uint8_t>& body)
{
    std::vector<std::uint8_t> out;
    out.reserve(4 + body.size());
    out.push_back(static_cast<std::uint8_t>(Op::NoxcppGameData));
    out.push_back(sub);
    put16(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<things::DataFile> kindOf(std::uint8_t b)
{
    if (b >= things::kDataFileCount) {
        return std::nullopt;
    }
    return static_cast<things::DataFile>(b);
}

} // namespace

std::vector<std::uint8_t> encodeSet(const Set& set)
{
    std::vector<std::uint8_t> body;
    put32(body, set.hash);
    body.push_back(static_cast<std::uint8_t>(set.files.size()));
    for (const SetEntry& f : set.files) {
        body.push_back(static_cast<std::uint8_t>(f.kind));
        put32(body, f.size);
        put32(body, f.crc);
        put32(body, f.merged);
        body.insert(body.end(), f.digest.begin(), f.digest.end());
    }
    return frame(kSet, body);
}

std::vector<std::uint8_t> encodeWant(std::span<const things::DataFile> kinds)
{
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(kinds.size()));
    for (const things::DataFile k : kinds) {
        body.push_back(static_cast<std::uint8_t>(k));
    }
    return frame(kWant, body);
}

std::vector<std::uint8_t> encodeChunk(things::DataFile kind, std::uint32_t offset, std::span<const std::uint8_t> bytes)
{
    std::vector<std::uint8_t> body;
    body.reserve(5 + bytes.size());
    body.push_back(static_cast<std::uint8_t>(kind));
    put32(body, offset);
    body.insert(body.end(), bytes.begin(), bytes.end());
    return frame(kChunk, body);
}

std::optional<Framed> unframe(std::span<const std::uint8_t> m)
{
    if (m.size() < 4 || m[0] != static_cast<std::uint8_t>(Op::NoxcppGameData)) {
        return std::nullopt;
    }
    const std::size_t length = static_cast<std::size_t>(m[2]) | (static_cast<std::size_t>(m[3]) << 8);
    if (m.size() < 4 + length) {
        return std::nullopt;
    }
    return Framed{m[1], m.subspan(4, length)};
}

std::optional<Set> decodeSet(std::span<const std::uint8_t> rest)
{
    if (rest.size() < 5) {
        return std::nullopt;
    }
    Set s;
    s.hash = get32(rest, 0);
    const std::size_t count = rest[4];
    constexpr std::size_t kEntry = 13 + 32; // (kind, size, CRC, merged, SHA-256)
    if (count == 0 || count > things::kDataFileCount || rest.size() != 5 + kEntry * count) {
        return std::nullopt;
    }
    std::size_t total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = 5 + kEntry * i;
        const auto kind = kindOf(rest[at]);
        // (Each table once: a whole file or its override, never both.)
        if (!kind || std::ranges::any_of(s.files, [&](const SetEntry& e) { return things::dataTableOf(e.kind) == things::dataTableOf(*kind); })) {
            return std::nullopt;
        }
        SetEntry e{*kind, get32(rest, at + 1), get32(rest, at + 5), get32(rest, at + 9), {}};
        std::copy_n(rest.begin() + static_cast<std::ptrdiff_t>(at + 13), e.digest.size(), e.digest.begin());
        if (e.size == 0 || e.size > things::dataFileMaxBytes(e.kind)) {
            return std::nullopt;
        }
        total += e.size;
        s.files.push_back(e);
    }
    if (total > things::kMaxDataFilesBytes) {
        return std::nullopt;
    }
    return s;
}

std::optional<std::vector<things::DataFile>> decodeWant(std::span<const std::uint8_t> rest)
{
    if (rest.empty() || rest.size() != 1u + rest[0] || rest[0] > things::kDataFileCount) {
        return std::nullopt;
    }
    std::vector<things::DataFile> out;
    for (std::size_t i = 1; i < rest.size(); ++i) {
        const auto kind = kindOf(rest[i]);
        if (!kind || std::ranges::find(out, *kind) != out.end()) {
            return std::nullopt;
        }
        out.push_back(*kind);
    }
    return out;
}

std::optional<Chunk> decodeChunk(std::span<const std::uint8_t> rest)
{
    if (rest.size() < 6 || rest.size() > 5 + kChunkBytes) {
        return std::nullopt;
    }
    const auto kind = kindOf(rest[0]);
    if (!kind) {
        return std::nullopt;
    }
    return Chunk{*kind, get32(rest, 1), rest.subspan(5)};
}

} // namespace nox::net::gamedata
