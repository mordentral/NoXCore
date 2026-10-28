// Which Blowfish key each Nox file type uses, and the exe's table the keys come from.
#pragma once

#include "noxcore/crypto/blowfish.hpp"

#include <filesystem>
#include <optional>

namespace nox::crypto {

/// Indices into GAME.EXE's Blowfish key table for the files that use one.
enum class FileKey : int {
    SoundSetBin = 5,
    ThingBin = 7,
    GameDataBin = 8,
    ModifierBin = 13,
    Map = 19,
    MonsterBin = 23,
    PlayerFile = 27,
};

/// Highest valid key index. Key i is the 56 bytes at offset 28*i of the 896-byte table.
inline constexpr int kMaxKeyIndex = 30;

/// Key used for a game file, chosen by file name (case-insensitive), or nullopt if the
/// file isn't one of Nox's encrypted formats.
std::optional<FileKey> keyForFile(const std::filesystem::path& path);

/// Cipher for key index 0..kMaxKeyIndex. Throws std::out_of_range otherwise.
Blowfish cipherForKey(int index);

inline Blowfish cipherForKey(FileKey key)
{
    return cipherForKey(static_cast<int>(key));
}

} // namespace nox::crypto
