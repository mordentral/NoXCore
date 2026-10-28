// Readers shared by the thing.bin parser and the DRAW payload decoder, and the text helpers
// thing_db.cpp and object_class.cpp share.
#pragma once

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace nox::things::detail {

/// Leading and trailing white space off.
using util::trim;

/// Equal without case (thing.bin's keys and class names).
using util::equalsNoCase;

/// A thing's CLASS, SUBCLASS and FLAGS read from its attributes (object_class.cpp), for
/// ThingDb::reindex to keep.
ThingDef::Bits readBits(const ThingDef& thing);

inline std::string readString8(ByteReader& r)
{
    return r.rawString(r.u8());
}

/// i32 video.bag index; -1 is followed by a named image (u8 type, string8 name).
inline ImageRef readImageRef(ByteReader& r)
{
    ImageRef ref;
    ref.index = r.i32();
    if (ref.index == -1) {
        ref.type = r.u8();
        ref.name = readString8(r);
    }
    return ref;
}

// `n` image refs back to back.
inline std::vector<ImageRef> readImageRefs(ByteReader& r, std::size_t n)
{
    std::vector<ImageRef> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(readImageRef(r));
    }
    return out;
}

/// u8 frame count, u8 frame delay, string8 playback kind, then the frames.
inline Animation readAnimation(ByteReader& r)
{
    Animation a;
    const std::uint8_t frames = r.u8();
    a.frameDelay = r.u8();
    a.kind = readString8(r);
    a.playback = animPlayback(a.kind);
    a.frames = readImageRefs(r, frames);
    return a;
}

} // namespace nox::things::detail
