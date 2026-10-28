// Audio.bag / Audio.idx: the game's sound-sample archive.
//
// Audio.idx (reader): u32 magic "GABA" (not checked), u32 version, u32 count, then
// `count` entries of 36 bytes (32 in version 1, which has no block size):
//   char name[16] (NUL-padded, no extension), u32 offset, u32 size, u32 sampleRate, u32 flags,
//   u32 blockAlign
// flags: 1 stereo, 4 16-bit samples, 8 IMA ADPCM (then blockAlign is its block size).
// The retail file has 1780 entries, nearly all mono 22050 Hz ADPCM in 512-byte blocks. Audio.bag
// holds the data at those offsets. The game sorts the entries by name (case-insensitive) and
// finds samples with a binary search; a sound event's file names lose their extension first.
//
// If the game folder has an "audio" folder, a loose "audio\<name>.wav" replaces the bag's copy
// (read on every load): a modder's override. An installed content pack's content\audio\<name>.wav
// comes before it (content/content_pack.hpp).
#pragma once

#include "noxcore/audio/pcm.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::audio {

struct BagSample {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t rate = 0;
    std::uint32_t flags = 0;
    std::uint32_t blockAlign = 0;

    static constexpr std::uint32_t kStereo = 1;
    static constexpr std::uint32_t k16Bit = 4;
    static constexpr std::uint32_t kAdpcm = 8;
    std::uint16_t channels() const { return (flags & kStereo) ? 2 : 1; }
};

class AudioBag {
public:
    /// Opens Audio.bag and Audio.idx (any case). `overrideDir`, if it exists, is checked for
    /// loose <name>.wav files first. Throws FormatError / std::system_error.
    AudioBag(const std::filesystem::path& bag, const std::filesystem::path& idx,
             std::filesystem::path overrideDir = {});

    /// Opens <gameDir>/Audio.bag, Audio.idx and the "audio" override folder.
    static AudioBag openGameDir(const std::filesystem::path& gameDir);

    std::uint32_t version() const { return version_; }
    /// Entries sorted by name, as the game keeps them; sample indices refer to this order.
    const std::vector<BagSample>& samples() const { return samples_; }

    /// Index of a sample by name (case-insensitive; an extension is ignored), or nullopt.
    std::optional<std::size_t> find(std::string_view name) const;

    /// The sample's raw bytes from the bag.
    std::vector<std::uint8_t> readRaw(std::size_t index);
    /// The sample decoded to 16-bit PCM: the override file if there is one, else the bag's.
    /// Throws FormatError for data it can't decode.
    Pcm decode(std::size_t index);

private:
    std::ifstream bag_;
    std::uint64_t bagSize_ = 0; ///< (readRaw checks a sample against it before making its buffer)
    std::filesystem::path overrideDir_;
    /// The installed content packs' audio files by lower-case name, the last installed pack's first (content_pack.hpp).
    std::unordered_map<std::string, std::vector<std::filesystem::path>> packFiles_;
    std::uint32_t version_ = 0;
    std::vector<BagSample> samples_;
};

/// Decodes one bag sample's raw bytes as its flags say.
Pcm decodeBagSample(const BagSample& sample, std::span<const std::uint8_t> raw);

} // namespace nox::audio
