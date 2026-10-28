// Sound file decoding: RIFF WAVE parsing and the PCM, IMA ADPCM and MP3 decoders behind AudioDecoder.
#include "noxcore/audio/pcm.hpp"

#include "noxcore/audio/ima_adpcm.hpp"
#include "noxcore/io/byte_reader.hpp"

#include <dr_mp3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace nox::audio {

namespace {

// One channel's IMA ADPCM state; next() decodes one 4-bit code to a sample.
struct ImaChannel {
    int predictor = 0;
    int index = 0;

    std::int16_t next(unsigned nibble)
    {
        const int step = kImaSteps[static_cast<std::size_t>(index)];
        // Bits 0-2 add step/4, step/2 and step to a base of step/8; bit 3 is the sign.
        int diff = step >> 3;
        if (nibble & 4) {
            diff += step;
        }
        if (nibble & 2) {
            diff += step >> 1;
        }
        if (nibble & 1) {
            diff += step >> 2;
        }
        predictor = (nibble & 8) ? predictor - diff : predictor + diff;
        predictor = std::clamp(predictor, -32768, 32767);
        index = std::clamp(index + kImaIndexStep[nibble & 15], 0, 88);
        return static_cast<std::int16_t>(predictor);
    }
};

/// One block (possibly cut short) into `out`, interleaved. Returns the frames it gave.
std::size_t decodeBlock(std::span<const std::uint8_t> block, unsigned channels, std::int16_t* out)
{
    if (channels == 0 || block.size() < 4u * channels) {
        return 0;
    }
    std::array<ImaChannel, 2> state{};
    // Each channel's 4-byte header gives the first sample and the step index.
    for (unsigned c = 0; c < channels; ++c) {
        const std::uint8_t* h = block.data() + 4 * c;
        state[c].predictor = static_cast<std::int16_t>(h[0] | (h[1] << 8));
        state[c].index = std::clamp<int>(h[2], 0, 88);
        out[c] = static_cast<std::int16_t>(state[c].predictor);
    }
    // After the headers, groups of 4 bytes per channel in turn, 8 samples each.
    const std::size_t groups = (block.size() - 4u * channels) / (4u * channels);
    const std::uint8_t* p = block.data() + 4 * channels;
    for (std::size_t g = 0; g < groups; ++g) {
        for (unsigned c = 0; c < channels; ++c) {
            // Eight samples of channel c: frames 1 + g*8 .. 8 + g*8.
            std::int16_t* dst = out + (1 + g * 8) * channels + c;
            for (unsigned b = 0; b < 4; ++b) {
                const std::uint8_t byte = *p++;
                dst[(2 * b) * channels] = state[c].next(byte & 15);
                dst[(2 * b + 1) * channels] = state[c].next(byte >> 4);
            }
        }
    }
    return 1 + groups * 8;
}

// 8-bit (unsigned) or 16-bit little-endian PCM, read straight from the file's data chunk.
class PcmDecoder final : public AudioDecoder {
public:
    PcmDecoder(std::vector<std::uint8_t> file, const WavInfo& info)
        : file_(std::move(file))
        , info_(info)
        , bytesPerFrame_(static_cast<std::size_t>(info.channels) * (info.codec == WavCodec::Pcm8 ? 1 : 2))
        , frames_(info.dataSize / bytesPerFrame_)
    {
    }

    std::uint32_t rate() const override { return info_.rate; }
    std::uint16_t channels() const override { return info_.channels; }
    std::uint64_t position() const override { return pos_; }
    void seek(std::uint64_t frame) override { pos_ = std::min<std::uint64_t>(frame, frames_); }

    std::size_t read(std::int16_t* out, std::size_t frames) override
    {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(frames, frames_ - pos_));
        const std::uint8_t* src = file_.data() + info_.dataOffset + pos_ * bytesPerFrame_;
        const std::size_t count = n * info_.channels;
        if (info_.codec == WavCodec::Pcm8) {
            for (std::size_t i = 0; i < count; ++i) {
                // 8-bit WAV samples are unsigned around 128.
                out[i] = static_cast<std::int16_t>((static_cast<int>(src[i]) - 128) << 8);
            }
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                out[i] = static_cast<std::int16_t>(src[2 * i] | (src[2 * i + 1] << 8));
            }
        }
        pos_ += n;
        return n;
    }

private:
    std::vector<std::uint8_t> file_;
    WavInfo info_;
    std::size_t bytesPerFrame_;
    std::uint64_t frames_;
    std::uint64_t pos_ = 0;
};

// IMA ADPCM WAVs, decoded a block at a time into block_ as read() needs them.
class AdpcmDecoder final : public AudioDecoder {
public:
    AdpcmDecoder(std::vector<std::uint8_t> file, const WavInfo& info)
        : file_(std::move(file))
        , info_(info)
        , perBlock_(imaAdpcmFramesPerBlock(info.channels, info.blockAlign))
        , block_(perBlock_ * info.channels)
    {
    }

    std::uint32_t rate() const override { return info_.rate; }
    std::uint16_t channels() const override { return info_.channels; }
    std::uint64_t position() const override { return pos_; }

    void seek(std::uint64_t frame) override
    {
        // Seeks by decoding the block holding `frame` and skipping to it within the block.
        nextBlock_ = static_cast<std::size_t>(frame / perBlock_);
        loaded_ = 0;
        used_ = 0;
        pos_ = static_cast<std::uint64_t>(nextBlock_) * perBlock_;
        if (load()) {
            used_ = std::min<std::size_t>(static_cast<std::size_t>(frame % perBlock_), loaded_);
            pos_ += used_;
        }
    }

    std::size_t read(std::int16_t* out, std::size_t frames) override
    {
        std::size_t done = 0;
        while (done < frames) {
            if (used_ == loaded_ && !load()) {
                break;
            }
            const std::size_t n = std::min(frames - done, loaded_ - used_);
            std::memcpy(out + done * info_.channels, block_.data() + used_ * info_.channels,
                        n * info_.channels * sizeof(std::int16_t));
            used_ += n;
            done += n;
        }
        pos_ += done;
        return done;
    }

private:
    // Decodes the next block; false past the data's end.
    bool load()
    {
        const std::size_t start = nextBlock_ * info_.blockAlign;
        if (start >= info_.dataSize) {
            return false;
        }
        const std::size_t size = std::min<std::size_t>(info_.blockAlign, info_.dataSize - start);
        ++nextBlock_;
        loaded_ = decodeBlock(std::span(file_).subspan(info_.dataOffset + start, size), info_.channels, block_.data());
        used_ = 0;
        return loaded_ != 0;
    }

    std::vector<std::uint8_t> file_;
    WavInfo info_;
    std::size_t perBlock_;
    std::vector<std::int16_t> block_;
    std::size_t nextBlock_ = 0;
    std::size_t loaded_ = 0;
    std::size_t used_ = 0;
    std::uint64_t pos_ = 0;
};

// MP3, inside a WAV data chunk or as a bare stream, through dr_mp3.
class Mp3Decoder final : public AudioDecoder {
public:
    Mp3Decoder(std::vector<std::uint8_t> file, std::size_t offset, std::size_t size)
        : file_(std::move(file))
    {
        if (!drmp3_init_memory(&mp3_, file_.data() + offset, size, nullptr)) {
            throw FormatError("not an MP3 stream");
        }
        open_ = true;
        if (mp3_.channels == 0 || mp3_.channels > 2 || mp3_.sampleRate == 0) {
            throw FormatError("unsupported MP3 stream (" + std::to_string(mp3_.channels) + " channels)");
        }
    }
    ~Mp3Decoder() override
    {
        if (open_) {
            drmp3_uninit(&mp3_);
        }
    }
    Mp3Decoder(const Mp3Decoder&) = delete;
    Mp3Decoder& operator=(const Mp3Decoder&) = delete;

    std::uint32_t rate() const override { return mp3_.sampleRate; }
    std::uint16_t channels() const override { return static_cast<std::uint16_t>(mp3_.channels); }
    std::uint64_t position() const override { return pos_; }

    void seek(std::uint64_t frame) override
    {
        if (drmp3_seek_to_pcm_frame(&mp3_, frame)) {
            pos_ = frame;
        }
    }

    std::size_t read(std::int16_t* out, std::size_t frames) override
    {
        const auto n = static_cast<std::size_t>(drmp3_read_pcm_frames_s16(&mp3_, frames, out));
        pos_ += n;
        return n;
    }

private:
    std::vector<std::uint8_t> file_; // dr_mp3 reads from it; never resized once open
    drmp3 mp3_{};
    bool open_ = false;
    std::uint64_t pos_ = 0;
};

} // namespace

std::size_t imaAdpcmFramesPerBlock(unsigned channels, unsigned blockAlign)
{
    if (channels == 0 || blockAlign < 4u * channels) {
        return 0;
    }
    // The header's sample, then two samples per remaining byte per channel.
    return (blockAlign - 4u * channels) * 2u / channels + 1u;
}

void decodeImaAdpcm(std::span<const std::uint8_t> data, unsigned channels, unsigned blockAlign,
                    std::vector<std::int16_t>& out)
{
    const std::size_t perBlock = imaAdpcmFramesPerBlock(channels, blockAlign);
    if (perBlock == 0) {
        throw FormatError("bad IMA ADPCM block size " + std::to_string(blockAlign));
    }
    const std::size_t blocks = (data.size() + blockAlign - 1) / blockAlign;
    const std::size_t base = out.size();
    // Room for every block's frames; a short last block's (one byte gives at most two samples,
    // plus the header's) counted by its own size, not the block size: an Audio.idx's u32 block size
    // over a few bytes of data would otherwise ask for gigabytes.
    const std::size_t full = data.size() / blockAlign;
    const std::size_t rest = data.size() % blockAlign;
    const std::size_t room = full * perBlock + (rest ? std::min<std::size_t>(perBlock, rest * 2 + 1) : 0);
    out.resize(base + room * channels);
    std::size_t frames = 0;
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t start = b * blockAlign;
        const std::size_t size = std::min<std::size_t>(blockAlign, data.size() - start);
        frames += decodeBlock(data.subspan(start, size), channels, out.data() + base + frames * channels);
    }
    out.resize(base + frames * channels);
}

std::optional<WavInfo> parseWav(std::span<const std::uint8_t> file)
{
    // RIFF WAVE: "RIFF", u32 size, "WAVE", then chunks of { char[4] id, u32 size, data }.
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
        return std::nullopt;
    }
    ByteReader r(file, "WAVE");
    r.skip(12);
    std::optional<WavInfo> info;
    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    // Walk the chunks until "data" (fmt must come first); unknown chunks are skipped.
    while (r.remaining() >= 8) {
        const std::string id = r.rawString(4);
        const std::uint32_t size = r.u32();
        if (id == "fmt ") {
            if (size < 16 || size > r.remaining()) {
                r.fail("short fmt chunk");
            }
            ByteReader f(r.bytes(size), "WAVE fmt");
            info.emplace();
            format = f.u16();
            info->channels = f.u16();
            info->rate = f.u32();
            f.skip(4); // bytes per second
            info->blockAlign = f.u16();
            bits = f.u16();
        } else if (id == "data") {
            if (!info) {
                r.fail("data before fmt");
            }
            info->dataOffset = r.offset();
            info->dataSize = std::min<std::size_t>(size, r.remaining());  // cut to what the file holds
            break;
        } else {
            r.skip(std::min<std::size_t>(size, r.remaining()));
        }
        if ((size & 1) && r.remaining() > 0) {
            r.skip(1); // chunks are padded to even sizes
        }
    }
    if (!info || info->dataOffset == 0) {
        r.fail("no fmt or data chunk");
    }
    if (info->channels == 0 || info->channels > 2 || info->rate == 0) {
        r.fail("unsupported channel count or rate");
    }
    switch (format) {
    case 1:  // WAVE_FORMAT_PCM
        if (bits == 8) {
            info->codec = WavCodec::Pcm8;
        } else if (bits == 16) {
            info->codec = WavCodec::Pcm16;
        } else {
            r.fail("unsupported PCM sample size " + std::to_string(bits));
        }
        break;
    case 0x11:  // WAVE_FORMAT_IMA_ADPCM
        info->codec = WavCodec::ImaAdpcm;
        if (imaAdpcmFramesPerBlock(info->channels, info->blockAlign) == 0) {
            r.fail("bad IMA ADPCM block size");
        }
        break;
    case 0x55:  // WAVE_FORMAT_MPEGLAYER3
        info->codec = WavCodec::Mp3;
        break;
    default:
        r.fail("unsupported WAVE format 0x" + std::to_string(format));
    }
    return info;
}

std::unique_ptr<AudioDecoder> AudioDecoder::open(std::vector<std::uint8_t> file)
{
    const auto info = parseWav(file);
    if (!info) {
        // Not a WAV: try the bytes as a bare MP3 stream.
        const std::size_t size = file.size();
        return std::make_unique<Mp3Decoder>(std::move(file), 0, size);
    }
    switch (info->codec) {
    case WavCodec::ImaAdpcm:
        return std::make_unique<AdpcmDecoder>(std::move(file), *info);
    case WavCodec::Mp3:
        return std::make_unique<Mp3Decoder>(std::move(file), info->dataOffset, info->dataSize);
    default:
        return std::make_unique<PcmDecoder>(std::move(file), *info);
    }
}

Pcm decodeFile(std::vector<std::uint8_t> file)
{
    auto decoder = AudioDecoder::open(std::move(file));
    Pcm pcm;
    pcm.rate = decoder->rate();
    pcm.channels = decoder->channels();
    std::vector<std::int16_t> chunk(4096 * pcm.channels);
    for (;;) {
        const std::size_t n = decoder->read(chunk.data(), 4096);
        if (n == 0) {
            break;
        }
        pcm.samples.insert(pcm.samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n * pcm.channels));
    }
    return pcm;
}

} // namespace nox::audio
