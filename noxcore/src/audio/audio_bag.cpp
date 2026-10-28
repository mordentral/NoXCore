// Audio.bag / Audio.idx reading and sample decoding (layout in audio_bag.hpp), with loose .wav
// files in the game's audio folder taking the place of bag samples of the same name.
#include "noxcore/audio/audio_bag.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace nox::audio {

namespace {

// strcmp-style case-insensitive compare: the samples are kept sorted by it for binary search.
using util::compareNoCase;

} // namespace

AudioBag::AudioBag(const std::filesystem::path& bag, const std::filesystem::path& idx,
                   std::filesystem::path overrideDir)
    : bag_(bag, std::ios::binary)
    , overrideDir_(std::move(overrideDir))
{
    if (!bag_) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory), bag.string());
    }
    bag_.seekg(0, std::ios::end);
    bagSize_ = static_cast<std::uint64_t>(std::max<std::streamoff>(0, bag_.tellg()));
    bag_.seekg(0);
    const auto data = readFileBytes(idx);
    ByteReader r(data, "Audio.idx");
    r.u32(); // "GABA"; the game doesn't check it
    version_ = r.u32();
    const std::uint32_t count = r.u32();
    // Version 1 entries lack the trailing u32 block alignment.
    const std::size_t entrySize = version_ == 1 ? 32 : 36;
    if (count > r.remaining() / entrySize) {
        r.fail("implausible sample count " + std::to_string(count));
    }
    samples_.resize(count);
    for (auto& s : samples_) {
        // Each entry: char[16] name (NUL-padded), u32 offset, size, rate, flags[, blockAlign].
        const std::string name = r.rawString(16);
        s.name = name.substr(0, name.find('\0'));
        s.offset = r.u32();
        s.size = r.u32();
        s.rate = r.u32();
        s.flags = r.u32();
        s.blockAlign = version_ == 1 ? 0 : r.u32();
    }
    std::stable_sort(samples_.begin(), samples_.end(),
                     [](const BagSample& a, const BagSample& b) { return compareNoCase(a.name, b.name) < 0; });
    std::error_code ec;
    if (!overrideDir_.empty() && !std::filesystem::is_directory(overrideDir_, ec)) {
        overrideDir_.clear();
    }
    // The installed content packs' audio files, as they stand when the bag opens (none: none):
    // each name's, the last installed pack's first (as their patches win), found once here.
    const auto dirs = content::contentPackAudioDirs();
    for (auto dir = dirs.rbegin(); dir != dirs.rend(); ++dir) {
        for (std::filesystem::directory_iterator it(*dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && util::equalsNoCase(it->path().extension().string(), ".wav")) {
                packFiles_[util::lowered(it->path().stem().string())].push_back(it->path());
            }
        }
        ec.clear();
    }
}

AudioBag AudioBag::openGameDir(const std::filesystem::path& gameDir)
{
    return AudioBag(findNoCase(gameDir, "Audio.bag"), findNoCase(gameDir, "Audio.idx"),
                    findNoCase(gameDir, "audio"));
}

std::optional<std::size_t> AudioBag::find(std::string_view name) const
{
    if (const auto dot = name.rfind('.'); dot != std::string_view::npos) {
        name = name.substr(0, dot);
    }
    const auto it = std::lower_bound(samples_.begin(), samples_.end(), name,
                                     [](const BagSample& s, std::string_view n) { return compareNoCase(s.name, n) < 0; });
    if (it != samples_.end() && compareNoCase(it->name, name) == 0) {
        return static_cast<std::size_t>(it - samples_.begin());
    }
    return std::nullopt;
}

std::vector<std::uint8_t> AudioBag::readRaw(std::size_t index)
{
    const BagSample& s = samples_.at(index);
    // (Checked before the buffer is made: Audio.idx's u32 size may say gigabytes.)
    if (std::uint64_t{s.offset} + s.size > bagSize_) {
        throw FormatError("Audio.bag: sample " + s.name + " runs past the end of the file");
    }
    std::vector<std::uint8_t> out(s.size);
    bag_.clear();
    bag_.seekg(s.offset);
    bag_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!bag_) {
        throw FormatError("Audio.bag: sample " + s.name + " runs past the end of the file");
    }
    return out;
}

Pcm AudioBag::decode(std::size_t index)
{
    const BagSample& s = samples_.at(index);
    // A content pack's <name>.wav wins, then the game folder's audio folder's, over the bag's own
    // sample; one that won't decode is passed over for the next (the exe logs it and plays the
    // bag's).
    if (!packFiles_.empty()) {
        if (const auto it = packFiles_.find(util::lowered(s.name)); it != packFiles_.end()) {
            for (const auto& file : it->second) {
                try {
                    return decodeFile(readFileBytes(file));
                } catch (const std::exception&) {
                }
            }
        }
    }
    if (!overrideDir_.empty()) {
        const auto file = findNoCase(overrideDir_, s.name + ".wav");
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec)) {
            try {
                return decodeFile(readFileBytes(file));
            } catch (const std::exception&) {
            }
        }
    }
    const auto raw = readRaw(index);
    return decodeBagSample(s, raw);
}

Pcm decodeBagSample(const BagSample& sample, std::span<const std::uint8_t> raw)
{
    Pcm pcm;
    pcm.rate = sample.rate;
    pcm.channels = sample.channels();
    if (sample.flags & BagSample::kAdpcm) {
        decodeImaAdpcm(raw, pcm.channels, sample.blockAlign, pcm.samples);
    } else if (sample.flags & BagSample::k16Bit) {
        pcm.samples.resize(raw.size() / 2);
        for (std::size_t i = 0; i < pcm.samples.size(); ++i) {
            pcm.samples[i] = static_cast<std::int16_t>(raw[2 * i] | (raw[2 * i + 1] << 8));
        }
    } else {
        pcm.samples.resize(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            pcm.samples[i] = static_cast<std::int16_t>((static_cast<int>(raw[i]) - 128) << 8);
        }
    }
    // Drops a trailing half frame of a stereo sample.
    pcm.samples.resize(pcm.frames() * pcm.channels);
    return pcm;
}

} // namespace nox::audio
