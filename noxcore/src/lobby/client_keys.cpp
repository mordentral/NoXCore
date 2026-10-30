// A NoX Core client's secrets for host realms (client_keys.hpp).
#include "noxcore/lobby/client_keys.hpp"

#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace nox::lobby {

namespace {

constexpr char kHex[] = "0123456789abcdef";

int hexDigit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/// A line's realm and secret; nullopt for anything else (a line that isn't one is left out).
std::optional<ClientSecret> parseLine(std::string_view line)
{
    if (line.size() < 16 + 1 + 64 || line[16] != ' ') {
        return std::nullopt;
    }
    ClientSecret s;
    for (std::size_t i = 0; i < 16; ++i) {
        const int d = hexDigit(line[i]);
        if (d < 0) {
            return std::nullopt;
        }
        s.realm = s.realm << 4 | static_cast<std::uint64_t>(d);
    }
    for (std::size_t i = 0; i < kClientSecretBytes; ++i) {
        const int hi = hexDigit(line[17 + 2 * i]), lo = hexDigit(line[18 + 2 * i]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        s.secret[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    if (s.realm == 0) {
        return std::nullopt;
    }
    return s;
}

std::string lineOf(const ClientSecret& s)
{
    std::string line;
    for (int i = 15; i >= 0; --i) {
        line += kHex[(s.realm >> (4 * i)) & 15];
    }
    line += ' ';
    for (const std::uint8_t b : s.secret) {
        line += kHex[b >> 4];
        line += kHex[b & 15];
    }
    return line;
}

} // namespace

void ClientKeyring::load()
{
    if (loaded_) {
        return;
    }
    loaded_ = true;
    std::ifstream f(file_, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (const auto s = parseLine(line)) {
            secrets_.emplace(s->realm, *s); // (the first line of a realm counts)
        }
    }
}

std::optional<ClientSecret> ClientKeyring::find(std::uint64_t realm)
{
    load();
    const auto it = secrets_.find(realm);
    return it != secrets_.end() ? std::optional(it->second) : std::nullopt;
}

std::optional<ClientSecret> ClientKeyring::secretFor(std::uint64_t realm)
{
    if (realm == 0) {
        return std::nullopt;
    }
    if (const auto kept = find(realm)) {
        return kept;
    }
    ClientSecret s;
    s.realm = realm;
    std::random_device rd;
    for (std::size_t i = 0; i < kClientSecretBytes; i += 4) {
        const std::uint32_t v = rd();
        for (std::size_t j = 0; j < 4; ++j) {
            s.secret[i + j] = static_cast<std::uint8_t>(v >> (8 * j));
        }
    }
    // Added to the file's end (the lines before kept as they are).
    std::error_code ec;
    if (file_.has_parent_path()) {
        std::filesystem::create_directories(file_.parent_path(), ec);
    }
    std::ofstream out(file_, std::ios::binary | std::ios::app);
    out << lineOf(s) << '\n';
    out.flush();
    if (!out) {
        return std::nullopt;
    }
    secrets_.emplace(realm, s);
    return s;
}

} // namespace nox::lobby
