// Westwood's VQA (HiColor), decoded as the game decodes it (written from our analysis); see vqa.hpp.
#include "noxcore/video/vqa.hpp"

#include "noxcore/audio/ima_adpcm.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/video/channels.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace nox::video {

namespace {

constexpr std::uint16_t kMaxSide = 4096; // a movie's width and height at most (VqaDecoder)
constexpr std::uint32_t kForm = vqaChunkId("FORM");
constexpr std::uint32_t kWvqa = vqaChunkId("WVQA");
constexpr std::uint32_t kVqhd = vqaChunkId("VQHD");
constexpr std::uint32_t kLinf = vqaChunkId("LINF");
constexpr std::uint32_t kLinh = vqaChunkId("LINH");
constexpr std::uint32_t kLind = vqaChunkId("LIND");
constexpr std::uint32_t kCinf = vqaChunkId("CINF");
constexpr std::uint32_t kCinh = vqaChunkId("CINH");
constexpr std::uint32_t kCind = vqaChunkId("CIND");
constexpr std::uint32_t kFinf = vqaChunkId("FINF");
constexpr std::uint32_t kVqfr = vqaChunkId("VQFR");
constexpr std::uint32_t kVqfk = vqaChunkId("VQFK");
constexpr std::uint32_t kVqfl = vqaChunkId("VQFL");
constexpr std::uint32_t kCbf0 = vqaChunkId("CBF0");
constexpr std::uint32_t kCbfz = vqaChunkId("CBFZ");
constexpr std::uint32_t kCbp0 = vqaChunkId("CBP0");
constexpr std::uint32_t kCbpz = vqaChunkId("CBPZ");
constexpr std::uint32_t kCpl0 = vqaChunkId("CPL0");
constexpr std::uint32_t kCplz = vqaChunkId("CPLZ");
constexpr std::uint32_t kVpt0 = vqaChunkId("VPT0");
constexpr std::uint32_t kVptd = vqaChunkId("VPTD");
constexpr std::uint32_t kVptk = vqaChunkId("VPTK");
constexpr std::uint32_t kVptr = vqaChunkId("VPTR");
constexpr std::uint32_t kVptz = vqaChunkId("VPTZ");
constexpr std::uint32_t kVpdz = vqaChunkId("VPDZ");
constexpr std::uint32_t kVpkz = vqaChunkId("VPKZ");
constexpr std::uint32_t kVprz = vqaChunkId("VPRZ");
constexpr std::uint32_t kSnd0 = vqaChunkId("SND0");
constexpr std::uint32_t kSnd2 = vqaChunkId("SND2");
constexpr std::uint32_t kSn2j = vqaChunkId("SN2J");

using util::be32;
using util::le16;
using util::le32;

/// A chunk at `pos` (id, big-endian size, data padded to even); false at the end or if it runs
/// past `end`.
bool chunkAt(const std::vector<std::uint8_t>& f, std::size_t& pos, std::size_t end, std::uint32_t& id,
             std::span<const std::uint8_t>& data)
{
    if (pos + 8 > end) {
        return false;
    }
    id = le32(f.data() + pos);
    const std::uint32_t size = be32(f.data() + pos + 4);
    if (size > end - pos - 8) {
        return false;
    }
    data = std::span<const std::uint8_t>(f.data() + pos + 8, size);
    pos += 8 + ((static_cast<std::size_t>(size) + 1) & ~std::size_t{1});
    return true;
}

/// IMA ADPCM's steps; the exe's tables of deltas (each step's 16 deltas, the classic bitwise
/// sum: step >> 3, plus step >> 2, step >> 1 and step for the nibble's bits) and of steps (the
/// next step: -1, -1, -1, -1, 2, 4, 6, 8, kept to 0-88) are worked out from them (audio/ima_adpcm.hpp).
using audio::kImaIndexStep;
using audio::kImaSteps;

// One 4-bit IMA ADPCM code: updates the channel's sample and step index and returns the sample.
std::int16_t adpcmNibble(unsigned nibble, std::int32_t& sample, std::uint16_t& index)
{
    const std::int32_t step = kImaSteps[std::min<std::size_t>(index, 88)];
    std::int32_t delta = step >> 3;
    if (nibble & 1) {
        delta += step >> 2;
    }
    if (nibble & 2) {
        delta += step >> 1;
    }
    if (nibble & 4) {
        delta += step;
    }
    if (nibble & 8) {
        delta = -delta;
    }
    // Kept to 16 bits and the step index to 0-88, as the exe's tables do.
    sample = std::clamp(sample + delta, -0x8000, 0x7FFF);
    index = static_cast<std::uint16_t>(std::clamp(static_cast<int>(index) + kImaIndexStep[nibble & 7], 0, 88));
    return static_cast<std::int16_t>(sample);
}

/// VQHD's 0x2A bytes as the exe keeps them.
VqaHeader parseVqhd(std::span<const std::uint8_t> d)
{
    if (d.size() != 0x2A) {
        throw FormatError("VQA: VQHD isn't 0x2A bytes");
    }
    VqaHeader out;
    const std::uint8_t* h = d.data();
    out.version = le16(h);
    out.flags = le16(h + 2);
    out.frames = le16(h + 4);
    out.width = le16(h + 6);
    out.height = le16(h + 8);
    out.blockWidth = h[10];
    out.blockHeight = h[11];
    out.fps = h[12];
    out.codebookParts = h[13];
    out.colors = le16(h + 14);
    out.maxBlocks = le16(h + 16);
    out.x = le16(h + 18);
    out.y = le16(h + 20);
    out.maxVptSize = le16(h + 22);
    out.frequency = le16(h + 24);
    out.channels = h[26];
    out.bits = h[27];
    // (bytes 28-31 aren't used here)
    out.colorMode = h[32];
    out.largestCodebook = le32(h + 34);
    return out;
}

/// Throws unless `f` starts as a VQA file does: FORM, its size, WVQA.
void requireWvqa(std::span<const std::uint8_t> f)
{
    if (f.size() < 12 || le32(f.data()) != kForm || le32(f.data() + 8) != kWvqa) {
        throw FormatError("not a VQA file");
    }
}

/// Where the FORM's chunks end: its big-endian size after the 8-byte header, within the file.
std::size_t formEnd(std::span<const std::uint8_t> f)
{
    return std::min<std::size_t>(f.size(), 8 + static_cast<std::size_t>(be32(f.data() + 4)));
}

} // namespace

VqaHeader readVqaHeader(std::span<const std::uint8_t> start)
{
    const std::vector<std::uint8_t> f(start.begin(), start.end());
    requireWvqa(f);
    std::size_t pos = 12;
    std::uint32_t id = 0;
    std::span<const std::uint8_t> d;
    while (chunkAt(f, pos, f.size(), id, d)) {
        if (id == kVqhd) {
            return parseVqhd(d);
        }
    }
    throw FormatError("VQA: no VQHD");
}

std::size_t lcwDecompress(std::span<const std::uint8_t> src, std::span<std::uint8_t> dst)
{
    // Commands until 0x80 or the output is full; each count is cut to the room left.
    std::size_t s = 0;
    // The relative form starts with a 0 byte; offsets then count back from the output position.
    std::size_t d = 0;
    const std::size_t dn = dst.size();
    const bool relative = !src.empty() && src[0] == 0;
    if (relative) {
        s = 1;
    }
    const auto more = [&](std::size_t n) { return s + n <= src.size(); };
    const auto copyBack = [&](std::size_t from, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) { // (REP MOVSB: an overlap repeats the bytes)
            dst[d] = dst[from + i];
            ++d;
        }
    };
    while (d < dn) {
        if (!more(1)) {
            break;
        }
        const unsigned cmd = src[s++];
        const std::size_t room = dn - d;
        if (!(cmd & 0x80)) {
            // 0ccc pppp pppppppp: (c + 3) bytes from `p` back.
            if (!more(1)) {
                break;
            }
            const std::size_t count = std::min<std::size_t>((cmd >> 4) + 3, room);
            const std::size_t offset = (static_cast<std::size_t>(cmd & 0x0F) << 8) | src[s++];
            if (offset > d) {
                break; // (before the output's start: the exe reads whatever lies there)
            }
            copyBack(d - offset, count); // (0 back: the bytes copied onto themselves, left as they were)
        } else if (!(cmd & 0x40)) {
            // 10cccccc: c literal bytes; 0x80 ends it.
            if (cmd == 0x80) {
                break;
            }
            const std::size_t count = std::min<std::size_t>(cmd & 0x3F, room);
            if (!more(count)) {
                break;
            }
            std::memcpy(dst.data() + d, src.data() + s, count);
            s += count;
            d += count;
        } else if (cmd == 0xFE) {
            // u16 count, a byte: a run.
            if (!more(3)) {
                break;
            }
            const std::size_t count = std::min<std::size_t>(le16(src.data() + s), room);
            const std::uint8_t v = src[s + 2];
            s += 3;
            std::memset(dst.data() + d, v, count);
            d += count;
        } else {
            // 11cccccc (c + 3) or 0xFF (u16 count): a copy from u16 `p`, the output's start + p
            // (or `p` back in the relative form).
            std::size_t count = (cmd & 0x3F) + 3;
            if (cmd == 0xFF) {
                if (!more(2)) {
                    break;
                }
                count = le16(src.data() + s);
                s += 2;
            }
            if (!more(2)) {
                break;
            }
            const std::size_t p = le16(src.data() + s);
            s += 2;
            count = std::min(count, room);
            const std::size_t from = relative ? d - p : p;
            // A source outside the output ends the decompression.
            if (relative ? p > d : p >= dn) {
                break;
            }
            for (std::size_t i = 0; i < count; ++i) {
                dst[d] = from + i < dn ? dst[from + i] : 0;
                ++d;
            }
        }
    }
    return d;
}

void decodeWestwoodAdpcm(std::span<const std::uint8_t> chunk, unsigned channels, WestwoodAdpcmState& state,
                         std::vector<std::int16_t>& out)
{
    // SND2 (16-bit): mono, two samples a byte; stereo, the left channel from the chunk's first
    // half, the right from its second (source + size / 2, output + 2), each n samples.
    // Stereo: n bytes in all give n samples per channel (two nibbles a byte from each half).
    if (channels == 2) {
        const std::size_t n = chunk.size();
        const std::size_t half = n / 2;
        const std::size_t base = out.size();
        out.resize(base + 2 * n);
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const std::uint8_t* src = chunk.data() + ch * half;
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint8_t b = src[i / 2];
                const unsigned nibble = (i & 1) ? b >> 4 : b & 0x0F;
                out[base + 2 * i + ch] = adpcmNibble(nibble, state.sample[ch], state.index[ch]);
            }
        }
        return;
    }
    out.reserve(out.size() + chunk.size() * 2);
    for (const std::uint8_t b : chunk) {
        out.push_back(adpcmNibble(b & 0x0F, state.sample[0], state.index[0]));
        out.push_back(adpcmNibble(b >> 4, state.sample[0], state.index[0]));
    }
}

VqaDecoder::VqaDecoder(std::shared_ptr<const std::vector<std::uint8_t>> file)
    : file_(std::move(file))
{
    const auto& f = *file_;
    requireWvqa(f);
    // The chunks before the frames (VQHD, LINF, CINF, ...) up to FINF.
    end_ = formEnd(f);
    std::size_t pos = 12;
    bool haveHeader = false;
    for (;;) {
        std::uint32_t id = 0;
        std::span<const std::uint8_t> d;
        if (!chunkAt(f, pos, end_, id, d)) {
            throw FormatError("VQA: no FINF before the frames");
        }
        if (id == kVqhd) {
            header_ = parseVqhd(d);
            haveHeader = true;
        // Loop and codebook tables: walk their sub-chunks (LINH / LIND, CINH / CIND).
        } else if (id == kLinf || id == kCinf) {
            std::size_t q = static_cast<std::size_t>(d.data() - f.data());
            const std::size_t qe = q + d.size();
            std::uint32_t sid = 0;
            std::span<const std::uint8_t> sd;
            while (chunkAt(f, q, qe, sid, sd)) {
                if (sid == kLinh && sd.size() >= 4) {
                    loopCount_ = le16(sd.data());
                    loopFlags_ = le16(sd.data() + 2);
                } else if (sid == kLind) {
                    for (std::size_t i = 0; i + 4 <= sd.size() && loopStarts_.size() < loopCount_; i += 4) {
                        loopStarts_.push_back(le16(sd.data() + i));
                    }
                } else if (sid == kCind) {
                    // CIND entries are 6 bytes: u16 frame, u32 codebook size.
                    for (std::size_t i = 0; i + 6 <= sd.size(); i += 6) {
                        codebookSizes_.emplace_back(le16(sd.data() + i), le32(sd.data() + i + 2));
                    }
                }
            }
        } else if (id == kFinf) {
            break;
        }
    }
    if (!haveHeader) {
        throw FormatError("VQA: no VQHD");
    }
    const VqaHeader& h = header_;
    // (colour mode 1 is accepted too; the draw is the HiColor one either way)
    if (h.colorMode != 4 && h.colorMode != 1) {
        throw FormatError("VQA: not a HiColor movie (colour mode " + std::to_string(h.colorMode) + ")");
    }
    if (h.blockWidth != 4 || (h.blockHeight != 2 && h.blockHeight != 4) || h.width == 0 || h.height == 0 ||
        h.width % h.blockWidth != 0 || h.height % h.blockHeight != 0) {
        throw FormatError("VQA: unsupported block size");
    }
    // (The game's movies are 640 x 400: a header saying 65532 x 36860 would have the picture take
    // gigabytes. The game plays them into its 640 x 480 screen, so anything far bigger is no movie.)
    if (h.width > kMaxSide || h.height > kMaxSide) {
        throw FormatError("VQA: " + std::to_string(h.width) + " x " + std::to_string(h.height) + " is too big for a movie");
    }
    pos_ = pos;
    // The codebook buffer (maxBlocks x block + 4, word-aligned, twice for 16
    // bits) and the block codes' (a word a block + 4).
    std::size_t cbSize = (static_cast<std::size_t>(h.maxBlocks) * h.blockWidth * h.blockHeight + 4) & ~std::size_t{3};
    cbSize *= 2;
    if (h.largestCodebook > cbSize) {
        throw FormatError("VQA: codebook larger than its buffer");
    }
    sharedCodebook_.assign(cbSize, 0);
    const std::size_t blocks = static_cast<std::size_t>(h.width / h.blockWidth) * (h.height / h.blockHeight);
    sharedVptr_.assign((blocks * 2 + 4) & ~std::size_t{3}, 0);
    // VQA_Test clears the surface first (memset 0): skipped blocks of the first frame are black.
    pixels_.assign(static_cast<std::size_t>(h.width) * h.height, 0);
    next_ = std::make_shared<Codebook>();
    latest_ = next_;
    filled_ = std::make_shared<Codebook>(); // (the ring's last slot, before the first: empty)
}

bool VqaDecoder::readChunk(std::size_t& pos, std::size_t end, Chunk& out) const
{
    return chunkAt(*file_, pos, end, out.id, out.data);
}

bool VqaDecoder::loopStart(int frame) const
{
    // Frame 0, or a loop's first frame when LINH's flags have 3.
    if (frame == 0) {
        return true;
    }
    if (loopCount_ != 0 && (loopFlags_ & 3) != 0) {
        return std::ranges::find(loopStarts_, static_cast<std::uint16_t>(frame)) != loopStarts_.end();
    }
    return false;
}

std::uint32_t VqaDecoder::nextCodebookSize(int frame) const
{
    // The size of the first CIND codebook after `frame`.
    for (const auto& [f, size] : codebookSizes_) {
        if (frame < static_cast<int>(f)) {
            return size;
        }
    }
    return 0;
}

void VqaDecoder::codebookDone(bool compressed)
{
    // The ring's slot filled: the newest codebook, and on to the next slot.
    next_->compressed = compressed;
    next_->decoded = false;
    latest_ = next_;
    filled_ = next_;
    next_ = std::make_shared<Codebook>();
}

bool VqaDecoder::partial(const Chunk& c, bool compressed)
{
    // CBP0 / CBPZ: the pieces gathered in the next slot until there are
    // CINH's count of them, or (none given) CIND's size for the next codebook. The count
    // and the size gathered are kept apart: a whole codebook (CBF*) zeroes
    // the count only, and the expected size is looked up when nothing has been gathered.
    const unsigned parts = header_.codebookParts;
    if (partData_.empty()) {
        // (the frame being loaded, one after the frame last drawn here)
        partExpected_ = parts == 0 ? nextCodebookSize(frame_ + 1) : 0;
    }
    partData_.insert(partData_.end(), c.data.begin(), c.data.end());
    ++partCount_;
    if (parts == 0) {
        if (partData_.size() != partExpected_) {
            return partData_.size() < partExpected_; // (past it: -4, the load fails)
        }
    } else if (partCount_ != parts) {
        return true;
    }
    next_->data = std::move(partData_);
    partData_.clear();
    partCount_ = 0;
    codebookDone(compressed);
    return true;
}

bool VqaDecoder::loadPiece(const Chunk& c)
{
    switch (c.id) {
    case kCbf0:
    case kCbfz:
        // A whole codebook into the next slot (the pieces' count
        // zeroed).
        next_->data.assign(c.data.begin(), c.data.end());
        partCount_ = 0;
        codebookDone(c.id == kCbfz);
        return true;
    case kCbp0:
    case kCbpz:
        return partial(c, c.id == kCbpz);
    case kVptr:
    case kVptk:
    case kVptd:
    case kVpt0:
        // As they are (flag 0x10 off); only VPTR draws with the HiColor block codes.
        vptr_ = c.data;
        vptrCompressed_ = false;
        vptrLoaded_ = c.id == kVptr;
        return true;
    case kVprz:
    case kVptz:
    case kVpkz:
    case kVpdz:
        // LCW-compressed (flag 0x10); VPRZ draws with the HiColor codes (flag 0x20).
        vptr_ = c.data;
        vptrCompressed_ = true;
        vptrLoaded_ = c.id == kVprz;
        return true;
    case kCpl0:
    case kCplz:
        return true; // an 8-bit movie's palette, none in HiColor
    default:
        return false; // not a frame's piece (sound among them): the load fails (-4)
    }
}

bool VqaDecoder::loadSub(std::span<const std::uint8_t> chunks)
{
    // A VQFR / VQFL's sub-chunks; any other id (sound among them) fails the load (-4).
    std::size_t q = static_cast<std::size_t>(chunks.data() - file_->data());
    const std::size_t qe = q + chunks.size();
    Chunk c;
    while (readChunk(q, qe, c)) {
        if (!loadPiece(c)) {
            return false;
        }
    }
    return true;
}

bool VqaDecoder::nextFrame()
{
    const int n = frame_ + 1;
    if (n >= header_.frames || failed_) {
        return false;
    }
    // At a loop's start the frame takes the slot its own chunks fill.
    if (loopStart(n)) {
        latest_ = next_;
    }
    frameCodebook_ = latest_;
    vptrLoaded_ = false;
    vptr_ = {};
    bool sawLoader = false;
    bool done = false;
    while (!done) {
        Chunk c;
        if (!readChunk(pos_, end_, c)) {
            break;
        }
        bool ok = true;
        switch (c.id) {
        case kVqfl:
            ok = loadSub(c.data);
            sawLoader = true; // (the loader's own flag)
            break;
        case kVqfr:
        case kVqfk:
            ok = loadSub(c.data);
            done = true;
            break;
        case kCbf0:
        case kCbfz:
        case kCbp0:
        case kCbpz:
        case kCpl0:
        case kCplz:
            ok = loadPiece(c);
            break;
        case kVptr:
        case kVptk:
        case kVptd:
        case kVpt0:
        case kVprz:
        case kVptz:
        case kVpkz:
        case kVpdz:
            ok = loadPiece(c); // (a frame's block codes at the top level end its loading too)
            done = true;
            break;
        default:
            break; // SN2J, SND*: the sound's (VqaAudioDecoder); anything else is skipped.
        }
        if (!ok) {
            // A load that fails (-4) ends the loading: that frame isn't
            // drawn and the movie ends.
            failed_ = true;
            return false;
        }
    }
    if (!done) {
        return false;
    }
    if (loopStart(n) && !sawLoader) {
        // The loader's end: the newest is the slot filled last, whether or not this
        // frame's chunks filled one (none: the codebook before, not the empty slot).
        latest_ = filled_;
        frameCodebook_ = latest_;
    }
    frame_ = n;
    draw();
    return true;
}

void VqaDecoder::draw()
{
    if (!vptrLoaded_ || !frameCodebook_) {
        return; // (the other block-code chunks draw with none in HiColor files)
    }
    // The codebook into the shared buffer once; the codes, compressed, each frame.
    Codebook& cbk = *frameCodebook_;
    std::span<const std::uint8_t> cb;
    std::vector<std::uint8_t> raw;
    // A compressed codebook is decompressed into the shared buffer once and reused by later frames;
    // a plain one is copied, padded with zeros to the buffer's size.
    if (cbk.compressed) {
        if (!cbk.decoded) {
            lcwDecompress(cbk.data, sharedCodebook_);
            cbk.decoded = true;
        }
        cb = sharedCodebook_;
    } else {
        raw = cbk.data;
        raw.resize(std::max(raw.size(), sharedCodebook_.size()), 0);
        cb = raw;
    }
    std::span<const std::uint8_t> codes = vptr_;
    if (vptrCompressed_) {
        lcwDecompress(vptr_, sharedVptr_);
        codes = sharedVptr_;
    }

    // The HiColor draw: blocks of 4 x 2 or 4 x 4.
    const int bh = header_.blockHeight;
    const std::size_t pitch = header_.width;
    const std::size_t blocksX = header_.width / 4;
    // `pos` is the pixel offset of the next block; rows of blocks run from rowStart to rowEnd.
    const std::size_t entry = 4 * static_cast<std::size_t>(bh); // pixels in a codebook entry
    const std::size_t total = pixels_.size();
    const std::size_t frameEnd = (header_.height / static_cast<std::size_t>(bh)) * bh * pitch;
    std::size_t rowStart = 0;
    std::size_t rowEnd = blocksX * 4;
    std::size_t pos = 0;
    std::size_t ip = 0;
    // Reads past the codes give 0 (and still advance, so the loop ends).
    const auto byte = [&]() -> unsigned { return ip < codes.size() ? codes[ip++] : (++ip, 0u); };
    const auto cbPixel = [&](std::size_t index, std::size_t k) -> std::uint16_t {
        const std::size_t b = (index * entry + k) * 2;
        return b + 1 < cb.size() ? static_cast<std::uint16_t>(cb[b] | cb[b + 1] << 8) : 0;
    };
    // Draws one codebook entry at pixel offset `at`; `transparent` keeps pixels whose entry is 0x8000.
    const auto block = [&](std::size_t at, std::size_t index, bool transparent) {
        for (int r = 0; r < bh; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                const std::uint16_t v = cbPixel(index, static_cast<std::size_t>(r) * 4 + c);
                const std::size_t p = at + static_cast<std::size_t>(r) * pitch + c;
                if ((!transparent || v != 0x8000) && p < total) {
                    pixels_[p] = v;
                }
            }
        }
    };
    // One 16-bit code at a time until the frame is covered or the codes run out (see vqa.hpp).
    while (pos < frameEnd && ip + 1 < codes.size()) {
        const unsigned code = codes[ip] | codes[ip + 1] << 8;
        ip += 2;
        // The run length used by types 1 and 2.
        const unsigned count = 2 * ((code >> 8) & 0x1F) + 2;
        switch (code >> 13) {  // type: the top 3 bits
        case 0:
            pos += (code & 0xFF) * 4;
            break;
        case 1:
            for (unsigned i = 0; i < count; ++i, pos += 4) {
                block(pos, code & 0xFF, false);
            }
            break;
        case 2:
            block(pos, code & 0xFF, false);
            pos += 4;
            for (unsigned i = 0; i < count; ++i, pos += 4) {
                block(pos, byte(), false);
            }
            break;
        case 3:
            block(pos, code & 0x1FFF, false);
            pos += 4;
            break;
        case 4:
            block(pos, code & 0x1FFF, true);
            pos += 4;
            break;
        case 5:
        case 6: {
            const unsigned n = byte();
            for (unsigned i = 0; i < n; ++i, pos += 4) {
                block(pos, code & 0x1FFF, (code >> 13) == 6);
            }
            break;
        }
        default:
            break;
        }
        // A row of blocks filled exactly: on to the first block of the next row.
        if (pos == rowEnd) {
            rowStart += pitch * static_cast<std::size_t>(bh);
            pos = rowStart;
            rowEnd = rowStart + blocksX * 4;
        }
    }
}

// RGB555 to RGBA, red in bits 10-14, green 5-9, blue 0-4.
void VqaDecoder::toRgba(std::span<std::uint8_t> out) const
{
    const std::size_t n = std::min(pixels_.size(), out.size() / 4);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint16_t c = pixels_[i];
        out[i * 4 + 0] = expand5((c >> 10) & 31);
        out[i * 4 + 1] = expand5((c >> 5) & 31);
        out[i * 4 + 2] = expand5(c & 31);
        out[i * 4 + 3] = 0xFF;
    }
}

VqaAudioDecoder::VqaAudioDecoder(std::shared_ptr<const std::vector<std::uint8_t>> file)
    : file_(std::move(file))
{
    const auto& f = *file_;
    requireWvqa(f);
    end_ = formEnd(f);
    std::size_t pos = 12;
    bool header = false;
    for (;;) {
        std::uint32_t id = 0;
        std::span<const std::uint8_t> d;
        if (!chunkAt(f, pos, end_, id, d)) {
            break;
        }
        // VQHD: +24 the sample rate, +26 channels, +27 bits; flags & 1 says the movie has sound (16-bit only).
        if (id == kVqhd && d.size() == 0x2A) {
            rate_ = le16(d.data() + 24);
            channels_ = d[26] == 2 ? 2 : 1;
            hasSound_ = (le16(d.data() + 2) & 1) != 0 && rate_ != 0 && d[27] == 16;
            header = true;
        } else if (id == kFinf) {
            break;
        }
    }
    if (!header) {
        throw FormatError("VQA: no VQHD");
    }
    if (rate_ == 0) {
        rate_ = 22050;
    }
    start_ = pos_ = pos;
}

// Decodes the next sound chunk into pending_; false at the end of the file.
bool VqaAudioDecoder::decodeMore()
{
    // The loader's sound chunks in file order: SN2J sets the state (u16 index * 32,
    // i32 sample per channel), SND2 decodes on from it, SND0 is plain 16-bit PCM.
    const auto& f = *file_;
    while (pos_ < end_) {
        std::uint32_t id = 0;
        std::span<const std::uint8_t> d;
        if (!chunkAt(f, pos_, end_, id, d)) {
            pos_ = end_;
            return false;
        }
        // SN2J: per channel u16 index * 32 then i32 sample (6 bytes mono, 12 stereo).
        if (id == kSn2j && d.size() <= 12 && d.size() >= 6) {
            state_.index[0] = static_cast<std::uint16_t>(std::min(le16(d.data()) >> 5, 88));
            state_.sample[0] = static_cast<std::int32_t>(le32(d.data() + 2));
            if (channels_ == 2 && d.size() >= 12) {
                state_.index[1] = static_cast<std::uint16_t>(std::min(le16(d.data() + 6) >> 5, 88));
                state_.sample[1] = static_cast<std::int32_t>(le32(d.data() + 8));
            }
        } else if (id == kSnd2) {
            decodeWestwoodAdpcm(d, channels_, state_, pending_);
            return true;
        } else if (id == kSnd0) {
            for (std::size_t i = 0; i + 1 < d.size(); i += 2) {
                pending_.push_back(static_cast<std::int16_t>(le16(d.data() + i)));
            }
            return true;
        }
    }
    return false;
}

// Copies decoded frames out, decoding more chunks as pending_ runs dry; returns the frames written.
std::size_t VqaAudioDecoder::read(std::int16_t* out, std::size_t frames)
{
    std::size_t done = 0;
    while (done < frames) {
        if (pendingPos_ >= pending_.size()) {
            pending_.clear();
            pendingPos_ = 0;
            if (!decodeMore()) {
                break;
            }
            continue;
        }
        const std::size_t have = (pending_.size() - pendingPos_) / channels_;
        const std::size_t n = std::min(have, frames - done);
        std::copy_n(pending_.data() + pendingPos_, n * channels_, out + done * channels_);
        pendingPos_ += n * channels_;
        done += n;
        if (n == 0) {
            pendingPos_ = pending_.size();
        }
    }
    position_ += done;
    return done;
}

// Seeks by decoding from the first sound chunk again up to `frame` (ADPCM can't be entered midway).
void VqaAudioDecoder::seek(std::uint64_t frame)
{
    pos_ = start_;
    state_ = {};
    pending_.clear();
    pendingPos_ = 0;
    position_ = 0;
    std::vector<std::int16_t> skip(4096 * channels_);
    while (position_ < frame) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(4096, frame - position_));
        if (read(skip.data(), want) == 0) {
            break;
        }
    }
}

} // namespace nox::video
