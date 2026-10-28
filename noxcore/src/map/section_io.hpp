// Encoding helpers shared by the map section codecs.
#pragma once

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/map/sections.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace nox::map::detail {

inline std::string readString8(ByteReader& r)
{
    return r.rawString(r.u8());
}

// Write-side call boundaries mirror GAME.EXE's cryptfile_ReadWrite calls so that a
// CRC-tracking ByteWriter reproduces the map CRC (see crypto/crc.hpp).

/// u8 length, then the bytes as one call, made even when empty (the game writes names this way).
inline void writeString8(ByteWriter& w, const std::string& s)
{
    if (s.size() > 0xFF) {
        throw FormatError("string too long for a u8 length: " + s.substr(0, 32) + "...");
    }
    w.u8(static_cast<std::uint8_t>(s.size()));
    w.rawString(s);
}

/// Like writeString8, but an empty string is just the zero length: the game writes optional
/// names (modifiers, voice sets) from a pointer that is null when unset.
inline void writeOptString8(ByteWriter& w, const std::string& s)
{
    if (s.empty()) {
        w.u8(0);
    } else {
        writeString8(w, s);
    }
}

inline std::string readString32(ByteReader& r)
{
    const std::uint32_t n = r.u32();
    return r.rawString(n);
}

inline void writeString32(ByteWriter& w, const std::string& s)
{
    w.u32(static_cast<std::uint32_t>(s.size()));
    w.rawString(s);
}

template <std::size_t N>
void readFixed(ByteReader& r, FixedString<N>& s)
{
    auto b = r.bytes(N);
    std::copy(b.begin(), b.end(), s.raw.begin());
}

template <std::size_t N>
void writeFixed(ByteWriter& w, const FixedString<N>& s)
{
    w.rawString({s.raw.data(), N});
}

/// Script handler (reader xferReadScriptHandler): i16 version (<= 1), u32 length (< 1024), name, u32.
inline ScriptHandler readScriptHandler(ByteReader& r)
{
    ScriptHandler h;
    h.version = r.i16();
    if (h.version > 1) {
        r.fail("unsupported script handler version " + std::to_string(h.version));
    }
    const std::uint32_t n = r.u32();
    if (n > 1023) {
        r.fail("script handler name too long");
    }
    h.function = r.rawString(n);
    h.unknown = r.u32();
    return h;
}

/// An empty handler is just the zero length, as GAME.EXE writes it (xferReadScriptHandler). (The map editor,
/// running under game flag 0x600000, sometimes adds a zero-length name call, which is why
/// editor-saved maps carry CRCs we don't reproduce; the game never recomputes them.)
inline void writeScriptHandler(ByteWriter& w, const ScriptHandler& h)
{
    if (h.version > 1 || h.function.size() > 1023) {
        throw FormatError("script handler the game can't load");
    }
    w.i16(h.version);
    w.u32(static_cast<std::uint32_t>(h.function.size()));
    if (!h.function.empty()) {
        w.rawString(h.function);
    }
    w.u32(h.unknown);
}

/// Several fields the game writes in one call.
inline void writeJoined(ByteWriter& w, std::initializer_list<std::uint32_t> values)
{
    std::vector<std::uint8_t> b;
    for (auto v : values) {
        for (int i = 0; i < 4; ++i) {
            b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    w.bytes(b);
}

template <std::size_t N>
void readArray(ByteReader& r, std::array<std::uint8_t, N>& a)
{
    auto b = r.bytes(N);
    std::copy(b.begin(), b.end(), a.begin());
}

/// The array as one call.
template <std::size_t N>
void writeArray(ByteWriter& w, const std::array<std::uint8_t, N>& a)
{
    w.bytes(a);
}

inline std::uint32_t bitsOf(float f)
{
    return std::bit_cast<std::uint32_t>(f);
}

/// Bytes the game writes one call per byte (script and intro blobs).
inline void writeBytewise(ByteWriter& w, std::span<const std::uint8_t> b)
{
    for (auto x : b) {
        w.u8(x);
    }
}

inline GridPoint readGridPoint(ByteReader& r)
{
    GridPoint p;
    p.x = r.i32();
    p.y = r.i32();
    return p;
}

/// Two calls (GroupData wall members).
inline void writeGridPoint(ByteWriter& w, const GridPoint& p)
{
    w.i32(p.x);
    w.i32(p.y);
}

/// One 8-byte call (wall lists).
inline void writeGridPointJoined(ByteWriter& w, const GridPoint& p)
{
    writeJoined(w, {static_cast<std::uint32_t>(p.x), static_cast<std::uint32_t>(p.y)});
}

/// What a section's (or record's) write() writes, as bytes: every serialize().
template <class T>
std::vector<std::uint8_t> serialized(const T& v)
{
    ByteWriter w;
    v.write(w);
    return std::move(w).take();
}

/// An object record's alignment before its size, written below the CRC: the stored bytes while
/// the record still sits at the alignment they fill, else zeros (a clean writer's; an edit
/// elsewhere shifted it). The data starts 8-aligned in the file, so counting from the start of
/// `w` is counting from the file's.
inline void writeRecordPadding(ByteWriter& w, const ObjectRecord& rec)
{
    if (rec.padding.size() == (8 - w.size() % 8) % 8) {
        w.uncountedBytes(rec.padding);
    } else {
        w.align(8);
    }
}

/// Casts a container size to a count field, throwing if it doesn't fit.
template <class T>
T countOf(std::size_t n, std::string_view what)
{
    if (n > std::numeric_limits<T>::max()) {
        throw FormatError(std::string(what) + ": too many entries (" + std::to_string(n) + ")");
    }
    return static_cast<T>(n);
}

} // namespace nox::map::detail
