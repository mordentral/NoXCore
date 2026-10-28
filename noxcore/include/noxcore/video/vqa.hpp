// Westwood's VQA movies as GAME.EXE plays them (movies\*.VQA: the logos, the intro and the class
// endings), decoded the way the game does, written from our analysis of the format.
//
// Nox's movies are all HiColor VQAs: VQHD version 3, colour mode 4 (16-bit RGB555), 640 x 400,
// 15 fps, blocks of 4 x 2 (the logos) or 4 x 4 (the rest), stereo 16-bit 22050 Hz SND2 sound.
// A file is a FORM WVQA of chunks (big-endian sizes, padded to even):
//   VQHD   the header (0x2A bytes);
//   LINF   LINH {u16 count, u16 flags, ..} and LIND {u16 start, u16 end} per loop;
//   CINF   CINH {u16 count, u16 parts, ..} and CIND {u16 frame, u32 size} per codebook;
//   FINF   each frame's file offset / 2, for seeking: not needed here;
// then per frame, in order (the loader):
//   SN2J   the sound decoder's state for the next SND2: u16 index, i32 sample, the
//          left's then the right's;
//   SND2   Westwood's IMA ADPCM: the left channel's nibbles in the first
//          half of the chunk, the right's in the second, low nibble first;
//   VQFL   a codebook loaded for what follows (its sub-chunks);
//   VQFR   the frame: CBFZ / CBF0 (a whole codebook, LCW-compressed or not), CBPZ / CBP0 (part
//          of the next one), VPRZ / VPTR (the block codes, LCW-compressed or not).
//
// Which codebook a frame draws with: the newest one loaded before the
// frame's chunks, except at a loop's start (LIND, and frame 0): there it's the slot
// its own chunks fill when they hold a VQFL, else the slot filled last (by its own chunks, or
// before them when they fill none).
// A load the exe fails (-4: a sub-chunk the VQFL load doesn't know, CBP pieces past CIND's size) ends
// the movie before that frame.
//
// Drawing: the codebook decompressed (once) into one shared buffer, the
// block codes into another, then the HiColor draw (4 x 2 or 4 x 4) over the
// last frame's picture: skipped blocks keep what was there. Each 16-bit code: type = code >> 13,
//   0  skip (code & 0xFF) blocks;
//   1  the block (code & 0xFF), 2 * ((code >> 8) & 0x1F) + 2 times;
//   2  the block (code & 0xFF), then 2 * ((code >> 8) & 0x1F) + 2 blocks, a byte each;
//   3  the block (code & 0x1FFF);
//   4  the block (code & 0x1FFF), its 0x8000 pixels left as they were;
//   5  the block (code & 0x1FFF), (next byte) times;
//   6  the same as 5, its 0x8000 pixels left as they were;
//   7  nothing.
// After each code, a row of blocks that has been filled exactly moves on to the next row.
#pragma once

#include "noxcore/audio/pcm.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace nox::video {

/// LCW ("format 80") decompression as GAME.EXE's: writes at most dst.size() bytes and
/// returns how many. A first byte of 0 selects the relative form (copies count back from the
/// output position instead of from its start). Reads past the end of `src` stop it.
std::size_t lcwDecompress(std::span<const std::uint8_t> src, std::span<std::uint8_t> dst);

/// VQHD, as the exe keeps it.
struct VqaHeader {
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint16_t frames = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t blockWidth = 0;
    std::uint8_t blockHeight = 0;
    std::uint8_t fps = 0;
    std::uint8_t codebookParts = 0; ///< CBP* chunks to a codebook (0: CIND's sizes)
    std::uint16_t colors = 0;
    std::uint16_t maxBlocks = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t maxVptSize = 0;
    std::uint16_t frequency = 0;
    std::uint8_t channels = 0;
    std::uint8_t bits = 0;
    std::uint8_t colorMode = 0; ///< 4 HiColor
    std::uint32_t largestCodebook = 0;
};

/// The VQHD of a VQA file from its first bytes (VQHD is the FORM's first chunk); throws
/// FormatError without one.
VqaHeader readVqaHeader(std::span<const std::uint8_t> start);

/// The ADPCM state SN2J sets and SND2 carries on: per channel the sample and the step
/// index (0-88; the exe keeps it * 32).
struct WestwoodAdpcmState {
    std::array<std::int32_t, 2> sample{};
    std::array<std::uint16_t, 2> index{};
};

/// SND2 (16-bit): `channels` 1 or 2; appends interleaved samples to `out`: two per
/// byte, low nibble first; in stereo the left channel's bytes are the chunk's first half.
void decodeWestwoodAdpcm(std::span<const std::uint8_t> chunk, unsigned channels, WestwoodAdpcmState& state,
                         std::vector<std::int16_t>& out);

/// A VQA file's frames, decoded one after another. Throws FormatError for a file that isn't a
/// HiColor VQA (colour mode 4, blocks 4 x 2 or 4 x 4).
class VqaDecoder {
public:
    explicit VqaDecoder(std::shared_ptr<const std::vector<std::uint8_t>> file);

    const VqaHeader& header() const { return header_; }
    int frameCount() const { return header_.frames; }
    /// Decodes the next frame; false once there are none.
    bool nextFrame();
    /// The frame last decoded (-1 before the first).
    int frame() const { return frame_; }
    /// The picture: width x height RGB555 pixels (the exe's 16-bit surface at its RGB555 values).
    std::span<const std::uint16_t> pixels() const { return pixels_; }
    /// The picture as RGBA (8 bits a channel, 5 bits widened as v << 3 | v >> 2).
    void toRgba(std::span<std::uint8_t> out) const;

private:
    struct Codebook {
        std::vector<std::uint8_t> data;
        bool compressed = false; ///< CBFZ / CBPZ (type 10); else CBF0 / CBP0 (type 8)
        bool decoded = false;    ///< in the shared buffer (the codebook's flag 4)
    };
    struct Chunk {
        std::uint32_t id = 0;
        std::span<const std::uint8_t> data;
    };

    bool readChunk(std::size_t& pos, std::size_t end, Chunk& out) const;
    /// The VQFL load: false where the exe's load fails (-4).
    bool loadSub(std::span<const std::uint8_t> chunks);
    bool loadPiece(const Chunk& c);
    void codebookDone(bool compressed);
    bool partial(const Chunk& c, bool compressed); ///< CBP0 / CBPZ
    std::uint32_t nextCodebookSize(int frame) const; ///< the first CIND codebook's after `frame`
    bool loopStart(int frame) const;                 ///< frame 0, or a loop's first (LINH's flags 3)
    void draw();

    std::shared_ptr<const std::vector<std::uint8_t>> file_;
    VqaHeader header_;
    std::uint16_t loopCount_ = 0;
    std::uint16_t loopFlags_ = 0;
    std::vector<std::uint16_t> loopStarts_;
    std::vector<std::pair<std::uint16_t, std::uint32_t>> codebookSizes_; ///< CIND
    std::size_t pos_ = 0;
    std::size_t end_ = 0;
    int frame_ = -1;

    std::shared_ptr<Codebook> latest_; ///< the newest codebook loaded
    std::shared_ptr<Codebook> next_;   ///< the ring's next slot
    std::shared_ptr<Codebook> filled_; ///< the slot before `next_`: the one filled last
    std::shared_ptr<Codebook> frameCodebook_;
    bool failed_ = false; ///< a load failed (-4): no more frames
    std::vector<std::uint8_t> partData_; ///< the pieces' bytes gathered
    std::uint32_t partCount_ = 0;        ///< the pieces gathered
    std::uint32_t partExpected_ = 0;
    std::span<const std::uint8_t> vptr_;
    bool vptrCompressed_ = false;
    bool vptrLoaded_ = false;

    std::vector<std::uint8_t> sharedCodebook_; ///< the codebook decompressed, one buffer for all
    std::vector<std::uint8_t> sharedVptr_;     ///< the block codes decompressed
    std::vector<std::uint16_t> pixels_;
};

/// The movie's sound, read from the file's SN2J / SND2 chunks in order (for the mixer's stream).
class VqaAudioDecoder : public audio::AudioDecoder {
public:
    /// Throws FormatError if the file has no VQHD.
    explicit VqaAudioDecoder(std::shared_ptr<const std::vector<std::uint8_t>> file);

    std::uint32_t rate() const override { return rate_; }
    std::uint16_t channels() const override { return channels_; }
    std::size_t read(std::int16_t* out, std::size_t frames) override;
    void seek(std::uint64_t frame) override;
    std::uint64_t position() const override { return position_; }
    /// Whether the file has any sound at all.
    bool hasSound() const { return hasSound_; }

private:
    bool decodeMore();

    std::shared_ptr<const std::vector<std::uint8_t>> file_;
    std::uint32_t rate_ = 22050;
    std::uint16_t channels_ = 2;
    std::size_t start_ = 0;
    std::size_t pos_ = 0;
    std::size_t end_ = 0;
    WestwoodAdpcmState state_;
    std::vector<std::int16_t> pending_;
    std::size_t pendingPos_ = 0;
    std::uint64_t position_ = 0;
    bool hasSound_ = false;
};

/// The FORM chunk ids, as the exe compares them (the four characters little-endian).
constexpr std::uint32_t vqaChunkId(const char (&s)[5])
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24;
}

} // namespace nox::video
