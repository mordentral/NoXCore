// Audio.bag / Audio.idx written back (audio_writer.hpp).
#include "noxcore/content/audio_writer.hpp"

#include "noxcore/audio/audio_bag.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include "noxcore/audio/ima_adpcm.hpp"
#include "noxcore/audio/pcm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>

namespace nox::content {

namespace {

/// `pcm` at 22050 Hz (linear between neighbouring frames), as the game's own samples are.
audio::Pcm at22050(const audio::Pcm& pcm)
{
    constexpr std::uint32_t kRate = 22050;
    if (pcm.rate == kRate || pcm.frames() == 0) {
        audio::Pcm out = pcm;
        out.rate = kRate;
        return out;
    }
    audio::Pcm out;
    out.rate = kRate;
    out.channels = pcm.channels;
    const std::size_t frames = pcm.frames();
    const std::size_t outFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::uint64_t(frames) * kRate / pcm.rate));
    out.samples.resize(outFrames * pcm.channels);
    for (std::size_t f = 0; f < outFrames; ++f) {
        const double src = double(f) * double(pcm.rate) / kRate;
        const std::size_t a = std::min(static_cast<std::size_t>(src), frames - 1);
        const std::size_t b = std::min(a + 1, frames - 1);
        const double k = src - double(a);
        for (unsigned c = 0; c < pcm.channels; ++c) {
            const double v = pcm.samples[a * pcm.channels + c] * (1 - k) + pcm.samples[b * pcm.channels + c] * k;
            out.samples[f * pcm.channels + c] = static_cast<std::int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
        }
    }
    return out;
}

/// One channel's IMA ADPCM encoder; its state moves exactly as the decoder's does.
struct ImaEncoder {
    int predictor = 0;
    int index = 0;

    unsigned code(int sample)
    {
        const int step = audio::kImaSteps[static_cast<std::size_t>(index)];
        int diff = sample - predictor;
        unsigned nibble = 0;
        if (diff < 0) {
            nibble = 8;
            diff = -diff;
        }
        int s = step;
        if (diff >= s) {
            nibble |= 4;
            diff -= s;
        }
        s >>= 1;
        if (diff >= s) {
            nibble |= 2;
            diff -= s;
        }
        s >>= 1;
        if (diff >= s) {
            nibble |= 1;
        }
        // (The decoder's step: base step/8, plus step, step/2, step/4 for bits 2, 1, 0.)
        int d = step >> 3;
        if (nibble & 4) d += step;
        if (nibble & 2) d += step >> 1;
        if (nibble & 1) d += step >> 2;
        predictor = std::clamp((nibble & 8) ? predictor - d : predictor + d, -32768, 32767);
        index = std::clamp(index + audio::kImaIndexStep[nibble & 15], 0, 88);
        return nibble;
    }
};

/// `pcm` as IMA ADPCM in the bag's layout (Microsoft's WAV blocks, as decodeImaAdpcm reads them):
/// blocks of `blockAlign` bytes, the last one only as long as it needs.
std::vector<std::uint8_t> encodeImaAdpcm(const audio::Pcm& pcm, unsigned blockAlign)
{
    const unsigned ch = pcm.channels;
    const std::size_t perBlock = audio::imaAdpcmFramesPerBlock(ch, blockAlign);
    const std::size_t frames = pcm.frames();
    std::vector<std::uint8_t> out;
    std::array<ImaEncoder, 2> enc{};
    const auto sample = [&](std::size_t f, unsigned c) -> int { return f < frames ? pcm.samples[f * ch + c] : 0; };
    for (std::size_t first = 0; first < frames; first += perBlock) {
        // Each channel's header: its first sample as it is, and the step index carried on.
        for (unsigned c = 0; c < ch; ++c) {
            enc[c].predictor = sample(first, c);
            const auto s = static_cast<std::uint16_t>(static_cast<std::int16_t>(enc[c].predictor));
            out.push_back(static_cast<std::uint8_t>(s & 0xFF));
            out.push_back(static_cast<std::uint8_t>(s >> 8));
            out.push_back(static_cast<std::uint8_t>(enc[c].index));
            out.push_back(0);
        }
        const std::size_t left = std::min(perBlock, frames - first) - 1;
        const std::size_t groups = (left + 7) / 8;
        for (std::size_t g = 0; g < groups; ++g) {
            for (unsigned c = 0; c < ch; ++c) {
                for (unsigned b = 0; b < 4; ++b) {
                    const std::size_t f = first + 1 + g * 8 + 2 * b;
                    const unsigned lo = enc[c].code(sample(f, c));
                    const unsigned hi = enc[c].code(sample(f + 1, c));
                    out.push_back(static_cast<std::uint8_t>(lo | (hi << 4)));
                }
            }
        }
    }
    return out;
}

} // namespace

AudioIndex AudioIndex::parse(std::span<const std::uint8_t> idx)
{
    ByteReader r(idx, "Audio.idx");
    AudioIndex out;
    out.magic = r.u32();
    out.version = r.u32();
    const std::uint32_t count = r.u32();
    const std::size_t entrySize = out.version == 1 ? 32 : 36;
    if (count > r.remaining() / entrySize) {
        r.fail("implausible sample count " + std::to_string(count));
    }
    out.entries.resize(count);
    for (auto& e : out.entries) {
        e.rawName = r.rawString(16);
        e.name = e.rawName.substr(0, e.rawName.find('\0'));
        e.offset = r.u32();
        e.size = r.u32();
        e.rate = r.u32();
        e.flags = r.u32();
        e.blockAlign = out.version == 1 ? 0 : r.u32();
    }
    if (!r.atEnd()) {
        r.fail("bytes after the last entry");
    }
    return out;
}

std::vector<std::uint8_t> AudioIndex::write() const
{
    ByteWriter w;
    w.u32(magic);
    w.u32(version);
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        std::string name = e.rawName.size() == 16 && e.rawName.substr(0, e.rawName.find('\0')) == e.name ? e.rawName : e.name;
        if (name.size() > 15 && name != e.rawName) {
            throw std::length_error("Audio.idx: a sample name longer than 15 characters: " + e.name);
        }
        name.resize(16, '\0');
        w.rawString(name);
        w.u32(e.offset);
        w.u32(e.size);
        w.u32(e.rate);
        w.u32(e.flags);
        if (version != 1) {
            w.u32(e.blockAlign);
        }
    }
    return std::move(w).take();
}

AudioIndexEntry* AudioIndex::find(std::string_view name)
{
    if (const auto dot = name.rfind('.'); dot != std::string_view::npos) {
        name = name.substr(0, dot);
    }
    for (auto& e : entries) {
        if (util::equalsNoCase(e.name, name)) {
            return &e;
        }
    }
    return nullptr;
}

void writeAudioBag(const std::filesystem::path& bagIn, const AudioIndex& index, const std::vector<AudioReplacement>& replacements,
                   const std::filesystem::path& bagOut, const std::filesystem::path& idxOut)
{
    if (std::error_code ec; std::filesystem::exists(bagOut, ec) && std::filesystem::equivalent(bagIn, bagOut, ec)) {
        throw std::invalid_argument("Audio.bag: never written over the bag it's read from");
    }
    AudioIndex out = index;
    // The old bag's bytes, as they are.
    {
        std::ifstream in(bagIn, std::ios::binary);
        std::ofstream o(bagOut, std::ios::binary | std::ios::trunc);
        if (!in || !o) {
            throw std::runtime_error("Audio.bag: can't open " + (in ? bagOut.string() : bagIn.string()));
        }
        o << in.rdbuf();
        std::uint64_t at = static_cast<std::uint64_t>(o.tellp());
        // Each replacement after them, as the game's own samples are kept: IMA ADPCM at 22050 Hz in
        // 512-byte blocks (1024 stereo), flags 12 / 13. (The game's sample cache holds at most
        // 127 chunks of 8 KiB a sample: longer 16-bit PCM would never play.)
        for (const auto& r : replacements) {
            if (r.pcm.channels < 1 || r.pcm.channels > 2 || r.pcm.rate == 0) {
                throw std::invalid_argument("Audio.bag: " + r.name + " isn't mono or stereo PCM");
            }
            AudioIndexEntry* e = out.find(r.name);
            if (!e) {
                // (No sound event could name a new one: thing.bin's AUD records name the bag's.)
                throw std::invalid_argument("Audio.bag: " + r.name + " isn't one of its samples (a sample is replaced, never added)");
            }
            const unsigned blockAlign = r.pcm.channels == 2 ? 1024u : 512u;
            const std::vector<std::uint8_t> data = encodeImaAdpcm(at22050(r.pcm), blockAlign);
            if (data.size() > kMaxBagSampleBytes) {
                throw std::length_error(std::format("Audio.bag: {} is too long for the game's sample cache ({} bytes encoded, at most {}: about {} s {})", r.name,
                                                    data.size(), kMaxBagSampleBytes, r.pcm.channels == 2 ? 47 : 94, r.pcm.channels == 2 ? "stereo" : "mono"));
            }
            if (at + data.size() > 0xFFFFFFFFull) {
                throw std::length_error("Audio.bag: over 4 GB");
            }
            e->offset = static_cast<std::uint32_t>(at);
            e->size = static_cast<std::uint32_t>(data.size());
            e->rate = 22050;
            e->flags = audio::BagSample::kAdpcm | audio::BagSample::k16Bit | (r.pcm.channels == 2 ? audio::BagSample::kStereo : 0u);
            e->blockAlign = blockAlign;
            o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            at += data.size();
        }
        if (!o) {
            throw std::runtime_error("Audio.bag: writing " + bagOut.string() + " failed");
        }
    }
    writeFileBytes(idxOut, out.write());
}

} // namespace nox::content
