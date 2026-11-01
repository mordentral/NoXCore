// The noxcpp server query and reply datagrams; the byte layout is in extended.hpp.
#include "noxcore/lobby/extended.hpp"

#include "noxcore/crypto/sha256.hpp"

#include <algorithm>
#include <string_view>

namespace nox::lobby {

namespace {

constexpr std::string_view kQueryMagic = "NXSD";
constexpr std::string_view kReplyMagic = "NXSV";

// 00 00, the opcode, the 4-byte magic, u16 version, u32 token (13 bytes, little-endian).
void header(std::vector<std::uint8_t>& out, std::string_view magic, std::uint16_t version, std::uint32_t token)
{
    out = {0, 0, kExtendedOp};
    out.insert(out.end(), magic.begin(), magic.end());
    out.push_back(static_cast<std::uint8_t>(version));
    out.push_back(static_cast<std::uint8_t>(version >> 8));
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(token >> (8 * i)));
    }
}

/// The version and token after `magic`, if the datagram starts so; `at` then points past them.
std::optional<std::pair<std::uint16_t, std::uint32_t>> readHeader(std::span<const std::uint8_t> d, std::string_view magic,
                                                                  std::size_t& at)
{
    if (d.size() < 13 || d[0] != 0 || d[1] != 0 || d[2] != kExtendedOp ||
        !std::equal(magic.begin(), magic.end(), d.begin() + 3)) {
        return std::nullopt;
    }
    const auto version = static_cast<std::uint16_t>(d[7] | d[8] << 8);
    const std::uint32_t token = d[9] | d[10] << 8 | d[11] << 16 | static_cast<std::uint32_t>(d[12]) << 24;
    at = 13;
    return std::pair{version, token};
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

std::uint16_t get16(std::span<const std::uint8_t> d, std::size_t at)
{
    return static_cast<std::uint16_t>(d[at] | d[at + 1] << 8);
}

std::uint32_t get32(std::span<const std::uint8_t> d, std::size_t at)
{
    return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
}

std::uint64_t get64(std::span<const std::uint8_t> d, std::size_t at)
{
    return get32(d, at) | static_cast<std::uint64_t>(get32(d, at + 4)) << 32;
}

} // namespace

std::vector<std::uint8_t> encodeExtendedQuery(std::uint32_t token, std::uint32_t clientFeatures, std::uint64_t typesHash,
                                              std::uint16_t clientLevel, const std::optional<ClientSecret>& secret)
{
    std::vector<std::uint8_t> out;
    if (clientFeatures == 0 && typesHash == 0 && clientLevel == 0 && !secret) {
        header(out, kQueryMagic, kExtendedVersion, token); // (version 1's, byte for byte)
        return out;
    }
    const std::uint16_t version = secret             ? kExtendedRealmVersion
                                  : clientLevel != 0 ? kExtendedLevelVersion
                                  : typesHash != 0   ? kExtendedTypesVersion
                                                     : 2;
    header(out, kQueryMagic, version, token);
    put32(out, clientFeatures);
    if (version >= kExtendedTypesVersion) {
        put64(out, typesHash); // (0 in a version 4 one without added types)
    }
    if (version >= kExtendedLevelVersion) {
        put16(out, clientLevel);
    }
    if (version >= kExtendedRealmVersion) {
        put64(out, secret->realm);
        out.insert(out.end(), secret->secret.begin(), secret->secret.end());
    }
    return out;
}

std::optional<ClientSecret> extendedQuerySecret(std::span<const std::uint8_t> datagram)
{
    std::size_t at = 0;
    const auto h = readHeader(datagram, kQueryMagic, at);
    if (!h || h->first < kExtendedRealmVersion) {
        return std::nullopt;
    }
    at += 4 + 8 + 2;
    if (datagram.size() < at + 8 + kClientSecretBytes) {
        return std::nullopt; // (a version 5 query without its tail: garbled)
    }
    ClientSecret s;
    s.realm = get64(datagram, at);
    std::copy_n(datagram.begin() + static_cast<std::ptrdiff_t>(at + 8), kClientSecretBytes, s.secret.begin());
    if (s.realm == 0) {
        return std::nullopt; // (no realm's)
    }
    return s;
}

std::optional<std::uint32_t> decodeExtendedQuery(std::span<const std::uint8_t> datagram)
{
    std::size_t at = 0;
    const auto h = readHeader(datagram, kQueryMagic, at);
    return h ? std::optional(h->second) : std::nullopt;
}

std::optional<std::uint16_t> extendedQueryVersion(std::span<const std::uint8_t> datagram)
{
    std::size_t at = 0;
    const auto h = readHeader(datagram, kQueryMagic, at);
    return h ? std::optional(h->first) : std::nullopt;
}

std::optional<std::uint32_t> extendedQueryClientFeatures(std::span<const std::uint8_t> datagram)
{
    std::size_t at = 0;
    const auto h = readHeader(datagram, kQueryMagic, at);
    if (!h) {
        return std::nullopt;
    }
    if (h->first < 2 || datagram.size() < at + 4) {
        return 0u;
    }
    return get32(datagram, at);
}

std::optional<std::uint64_t> extendedQueryTypesHash(std::span<const std::uint8_t> datagram)
{
    std::size_t at = 0;
    const auto h = readHeader(datagram, kQueryMagic, at);
    if (!h) {
        return std::nullopt;
    }
    if (h->first < kExtendedTypesVersion) {
        return 0u;
    }
    if (datagram.size() < at + 4 + 8) {
        return std::nullopt; // (a version 3 query without its tail: garbled)
    }
    return get64(datagram, at + 4);
}

std::optional<std::uint16_t> extendedQueryClientLevel(std::span<const std::uint8_t> datagram)
{
    std::size_t at = 0;
    const auto h = readHeader(datagram, kQueryMagic, at);
    if (!h) {
        return std::nullopt;
    }
    if (h->first < kExtendedLevelVersion) {
        return std::uint16_t{0};
    }
    if (datagram.size() < at + 4 + 8 + 2) {
        return std::nullopt; // (a version 4 query without its tail: garbled)
    }
    return get16(datagram, at + 4 + 8);
}

std::string clientKeyOf(const ClientSecret& secret)
{
    constexpr std::string_view kTag = "noxcore-client-key";
    crypto::Sha256 h;
    h.update(std::span(reinterpret_cast<const std::uint8_t*>(kTag.data()), kTag.size()));
    std::vector<std::uint8_t> realm;
    put64(realm, secret.realm);
    h.update(realm);
    h.update(secret.secret);
    const auto digest = h.finish();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string key;
    for (std::size_t i = 0; i < 16; ++i) {
        key += kHex[digest[i] >> 4];
        key += kHex[digest[i] & 15];
    }
    return key;
}

std::vector<std::uint8_t> encodeExtendedReply(std::uint32_t token, const ExtendedServer& server)
{
    std::vector<std::uint8_t> out;
    const bool types = server.typesHash != 0;
    const bool modeName = !server.modeName.empty();
    const bool realm = server.realm != 0 || modeName;
    const bool levels = server.minClientLevel != 0 || server.clientLevel != 0 || realm;
    const std::uint16_t version = modeName ? std::max(server.version, kExtendedModeNameVersion)
                                  : realm  ? std::max(server.version, kExtendedRealmVersion)
                                  : levels ? std::max(server.version, kExtendedLevelVersion)
                                  : types  ? std::max(server.version, kExtendedTypesVersion)
                                           : server.version;
    header(out, kReplyMagic, version, token);
    // Then u8 max substeps a frame and u32 feature bits.
    out.push_back(server.maxMultiplier);
    put32(out, server.features);
    if (types || levels) { // (version 3's tail: a version 4 one without added types has 0 and none)
        put64(out, server.typesHash);
        // At most 255 bytes: cut after the last whole pack id that fits (else inside one, never
        // inside a UTF-8 character).
        const std::string_view packs = server.typesPacks;
        std::size_t n = std::min<std::size_t>(packs.size(), 255);
        if (n < packs.size()) {
            if (const auto sep = packs.substr(0, n).rfind(", "); sep != std::string_view::npos && sep > 0) {
                n = sep;
            } else {
                while (n > 0 && (static_cast<unsigned char>(packs[n]) & 0xC0) == 0x80) {
                    --n;
                }
            }
        }
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), server.typesPacks.begin(), server.typesPacks.begin() + static_cast<std::ptrdiff_t>(n));
    }
    if (levels) {
        put16(out, server.minClientLevel);
        put16(out, server.clientLevel);
    }
    if (realm) { // (version 5's tail: a version 6 one without a realm has 0)
        put64(out, server.realm);
    }
    if (modeName) {
        // At most kModeNameBytes: cut between UTF-8 characters, never inside one.
        const std::string_view name = server.modeName;
        std::size_t n = std::min(name.size(), kModeNameBytes);
        if (n < name.size()) {
            while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) {
                --n;
            }
        }
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), name.begin(), name.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return out;
}

std::optional<ExtendedReply> decodeExtendedReply(std::span<const std::uint8_t> datagram)
{
    std::size_t at = 0;
    const auto h = readHeader(datagram, kReplyMagic, at);
    if (!h || datagram.size() < at + 5) {
        return std::nullopt;
    }
    ExtendedReply r;
    r.token = h->second;
    r.server.version = h->first;
    r.server.maxMultiplier = datagram[at];
    r.server.features = get32(datagram, at + 1);
    at += 5;
    if (h->first >= kExtendedTypesVersion) {
        // u64 the hash, u8 n, n bytes: checked against the datagram before each read.
        if (datagram.size() < at + 9) {
            return std::nullopt;
        }
        r.server.typesHash = get64(datagram, at);
        const std::size_t n = datagram[at + 8];
        at += 9;
        if (datagram.size() < at + n) {
            return std::nullopt;
        }
        r.server.typesPacks.assign(reinterpret_cast<const char*>(datagram.data() + at), n);
        at += n;
    }
    if (h->first >= kExtendedLevelVersion) {
        // u16 the lowest client level let in, u16 the server's own.
        if (datagram.size() < at + 4) {
            return std::nullopt;
        }
        r.server.minClientLevel = get16(datagram, at);
        r.server.clientLevel = get16(datagram, at + 2);
        at += 4;
    }
    if (h->first >= kExtendedRealmVersion) {
        // u64 the host's client key realm.
        if (datagram.size() < at + 8) {
            return std::nullopt;
        }
        r.server.realm = get64(datagram, at);
        at += 8;
    }
    if (h->first >= kExtendedModeNameVersion) {
        // u8 n, n bytes: the mode's name (more than kModeNameBytes taken as sent).
        if (datagram.size() < at + 1 || datagram.size() < at + 1 + datagram[at]) {
            return std::nullopt;
        }
        const std::size_t n = datagram[at];
        r.server.modeName.assign(reinterpret_cast<const char*>(datagram.data() + at + 1), n);
    }
    return r;
}

} // namespace nox::lobby
