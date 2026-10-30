// A NoX Core client's secrets, one per host realm (a mode's "clientKeys": lobby/extended.hpp's
// ClientSecret), kept in one text file in the game's folder beside noxcore.cfg:
//
//   <realm: 16 hex digits> <secret: 64 hex digits>      one line each
//
// A secret is made (random) the first time the client meets a realm and kept for good: the host
// keeps only the key made from it, the same for this PC on every join. Nothing is read before the
// first call, and nothing is written unless a realm is met.
#pragma once

#include "noxcore/lobby/extended.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>

namespace nox::lobby {

class ClientKeyring {
public:
    explicit ClientKeyring(std::filesystem::path file) : file_(std::move(file)) {}
    /// The secret for `realm` (0: none), made and written at the first call for it; nullopt when
    /// realm is 0 or the file can't be written (a secret not kept would be a new PC each join).
    std::optional<ClientSecret> secretFor(std::uint64_t realm);
    /// The secret kept for `realm`, without making one.
    std::optional<ClientSecret> find(std::uint64_t realm);

private:
    void load();
    std::filesystem::path file_;
    bool loaded_ = false;
    std::map<std::uint64_t, ClientSecret> secrets_;
};

} // namespace nox::lobby
