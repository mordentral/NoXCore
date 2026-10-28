// Sound data the game ships and how it's coded:
//   - Audio.bag samples: IMA ADPCM (Microsoft's WAV block layout) or plain PCM (audio_bag.hpp);
//   - music\*.wav: RIFF WAVE, IMA ADPCM (format 0x11), 22050 Hz stereo in 1024-byte blocks;
//   - dialog\*.wav: RIFF WAVE holding MPEG layer 3 (format 0x55; Miles' MP3DEC.ASI decodes them).
// Everything decodes to interleaved signed 16-bit samples.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace nox::audio {

/// Decoded audio: interleaved signed 16-bit samples.
struct Pcm {
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    std::vector<std::int16_t> samples; ///< frames * channels

    std::size_t frames() const { return channels ? samples.size() / channels : 0; }
};

/// Samples per channel in one IMA ADPCM block (the header's sample plus two per data byte).
std::size_t imaAdpcmFramesPerBlock(unsigned channels, unsigned blockAlign);

/// Decodes IMA ADPCM in Microsoft's WAV layout: blocks of `blockAlign` bytes, each starting with
/// a 4-byte header per channel (i16 first sample, u8 step index, u8 reserved), then groups of
/// four bytes (eight samples, low nibble first) for each channel in turn. A short last block is
/// decoded as far as it goes. Appends to `out`.
void decodeImaAdpcm(std::span<const std::uint8_t> data, unsigned channels, unsigned blockAlign,
                    std::vector<std::int16_t>& out);

enum class WavCodec : std::uint8_t {
    Pcm8,     ///< format 1, 8 bits (unsigned)
    Pcm16,    ///< format 1, 16 bits
    ImaAdpcm, ///< format 0x11
    Mp3,      ///< format 0x55
};

/// A RIFF WAVE file's format and where its data lies.
struct WavInfo {
    WavCodec codec = WavCodec::Pcm16;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t blockAlign = 0;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;
};

/// Parses a RIFF WAVE header. nullopt if it isn't one; throws FormatError for a WAVE file whose
/// format can't be played. A data chunk running past the file is cut at the end of the file.
std::optional<WavInfo> parseWav(std::span<const std::uint8_t> file);

/// Decodes a sound file incrementally from memory: RIFF WAVE (PCM, IMA ADPCM or MP3) or a bare
/// MP3 stream. Streams (music, dialog) are read a buffer at a time on the audio thread.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    /// Takes the file's bytes. Throws FormatError if it can't be decoded.
    static std::unique_ptr<AudioDecoder> open(std::vector<std::uint8_t> file);

    virtual std::uint32_t rate() const = 0;
    virtual std::uint16_t channels() const = 0;
    /// Decodes up to `frames` frames into `out` (interleaved); returns how many, 0 at the end.
    virtual std::size_t read(std::int16_t* out, std::size_t frames) = 0;
    /// Continues from `frame` (clamped to the end).
    virtual void seek(std::uint64_t frame) = 0;
    /// The next frame read() returns.
    virtual std::uint64_t position() const = 0;
};

/// The whole of a file decoded at once. Throws FormatError.
Pcm decodeFile(std::vector<std::uint8_t> file);

} // namespace nox::audio
