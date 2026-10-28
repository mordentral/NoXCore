// Audio.bag / Audio.idx written back (audio/audio_bag reads them): the index as the file stores it
// (its entries in file order: the game sorts them itself), and a new bag with the old one's bytes
// copied as they are and every replaced or added sample appended at its end as 16-bit PCM, so no
// other sample moves. With nothing replaced the two files written are the originals, byte for byte.
#pragma once

#include "noxcore/audio/pcm.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nox::content {

struct AudioIndexEntry {
    std::string name; ///< up to 15 characters (char[16], NUL-padded)
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t rate = 0;
    std::uint32_t flags = 0;
    std::uint32_t blockAlign = 0;
    std::string rawName; ///< the 16 name bytes as stored (kept for an unchanged entry)
};

struct AudioIndex {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::vector<AudioIndexEntry> entries; ///< file order

    static AudioIndex parse(std::span<const std::uint8_t> idx);
    std::vector<std::uint8_t> write() const;
    /// The entry of a sample (name compared without case, an extension ignored), or nullptr.
    AudioIndexEntry* find(std::string_view name);
};

struct AudioReplacement {
    std::string name; ///< one of the bag's samples (none is added: no sound event could name it)
    audio::Pcm pcm;
};

/// The longest sample the game's sample cache can load (127 chunks of 8 KiB).
inline constexpr std::size_t kMaxBagSampleBytes = 127u * 8192u;

/// Writes `bagOut` and `idxOut`: `bagIn`'s bytes, then each replacement as the game keeps its own
/// samples (IMA ADPCM at 22050 Hz, 512-byte blocks, 1024 stereo); the index's entries for them
/// pointing there. Throws for a name the bag hasn't, or a sound too long for the game's sample
/// cache (kMaxBagSampleBytes encoded: about 94 s mono, 47 s stereo).
void writeAudioBag(const std::filesystem::path& bagIn, const AudioIndex& index, const std::vector<AudioReplacement>& replacements,
                   const std::filesystem::path& bagOut, const std::filesystem::path& idxOut);

} // namespace nox::content
