// MSG_NOXCPP_GAMEDATA (0xF9): a mod's own game data files (things/data_files.hpp,
// server/mode_data.hpp), between a game whose mod ships them and its NoX Core clients. Framed as
// every noxcpp message: {0xF9, u8 sub, u16 the rest's length, the rest}; identified clients only
// (a stock client is never sent one: the transport refuses it).
//
//   1 kSet   (S->C, once, at the join, before its first USE_MAP): u32 the set's hash, u8 count,
//            then per file {u8 kind (DataFile), u32 size, u32 CRC-32, u32 merged}: the client's
//            files of the set (thing, modifier and gamedata, whole or override; the server keeps
//            monster's and soundset's to itself); "merged", an override's check of the table it
//            gives over the host's install (0 for a whole file), which the client's own merge must
//            give. The client loads the map only once it has them all.
//   2 kWant  (C->S): u8 count, then count kinds: the files it has no copy of (by CRC).
//   3 kChunk (S->C): u8 kind, u32 offset, then up to kChunkBytes of the file, in order.
//
// A client below lobby::kGameDataLevel never joins such a game (its join is refused with the
// too-old notice), nor does a stock client (held out with the NoX Core-only notice).
#pragma once

#include "noxcore/things/data_files.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nox::net::gamedata {

inline constexpr std::uint8_t kSet = 1;
inline constexpr std::uint8_t kWant = 2;
inline constexpr std::uint8_t kChunk = 3;

/// A chunk's most bytes of file (its message under a reliable packet's 1024: net::kMaxReliablePayload).
inline constexpr std::size_t kChunkBytes = 960;

struct SetEntry {
    things::DataFile kind = things::DataFile::Things;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    std::uint32_t merged = 0; ///< (an override's: the merged table's check; 0 a whole file)
    bool operator==(const SetEntry&) const = default;
};
struct Set {
    std::uint32_t hash = 0;
    std::vector<SetEntry> files;
    bool operator==(const Set&) const = default;
};

std::vector<std::uint8_t> encodeSet(const Set& set);
std::vector<std::uint8_t> encodeWant(std::span<const things::DataFile> kinds);
std::vector<std::uint8_t> encodeChunk(things::DataFile kind, std::uint32_t offset, std::span<const std::uint8_t> bytes);

/// A framed message's sub and its rest (`m` from its 0xF9 on); none: not one, or cut short.
struct Framed {
    std::uint8_t sub = 0;
    std::span<const std::uint8_t> rest;
};
std::optional<Framed> unframe(std::span<const std::uint8_t> m);

/// A kSet's rest read and checked: at most kDataFileCount files, each a known kind once, of a
/// size from 1 to its cap, together at most kMaxDataFilesBytes; none: it isn't one.
std::optional<Set> decodeSet(std::span<const std::uint8_t> rest);
/// A kWant's kinds (known ones, each once); none: it isn't one.
std::optional<std::vector<things::DataFile>> decodeWant(std::span<const std::uint8_t> rest);
struct Chunk {
    things::DataFile kind = things::DataFile::Things;
    std::uint32_t offset = 0;
    std::span<const std::uint8_t> bytes;
};
/// A kChunk's parts (a known kind, 1 to kChunkBytes bytes); none: it isn't one.
std::optional<Chunk> decodeChunk(std::span<const std::uint8_t> rest);

} // namespace nox::net::gamedata
