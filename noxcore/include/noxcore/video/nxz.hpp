// NXZ, the compression of video.bag's image segments and of .nxz map files: decoder and encoder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nox::video {

/// Decompresses NXZ data (video.bag segments, .nxz maps): an adaptive Huffman code over 274
/// symbols (bytes, 16 short LZ77 lengths, 8 long-length classes, a table-rebuild symbol and an
/// end-of-chunk symbol) with a 64 KiB window. Follows GAME.EXE's decoder, with its
/// tables. A stream may be several chunks (the big shipped
/// G_*.nxz maps are): each ends with the end symbol, and the next continues with the model and
/// window kept, aligned to the following byte, as the decoder's callers resume it.
///
/// Produces exactly `size` bytes; throws FormatError if the data ends early or is invalid.
std::vector<std::uint8_t> nxzDecompress(std::span<const std::uint8_t> data, std::size_t size);

/// Compresses to an NXZ stream nxzDecompress (and GAME.EXE's decoder) decodes back to `data`.
/// The encoder mirrors the decoder's adaptive model, so any valid schedule of table rebuilds
/// and matches works; this one rebuilds periodically and picks matches by their exact bit cost.
///
/// The stream carries no sizes: a .nxz map file is u32 (the .map's stored size) then this
/// stream of the .map's on-disk (encrypted) bytes; video.bag keeps both sizes
/// in its index.
std::vector<std::uint8_t> nxzCompress(std::span<const std::uint8_t> data);

} // namespace nox::video
