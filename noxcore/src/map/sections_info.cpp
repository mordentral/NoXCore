// MapInfo, MapIntro, AmbientData, DebugData, ScriptObject, ScriptData.
// The map's descriptive and script sections (structs in sections.hpp).
#include "section_io.hpp"

namespace nox::map {

using namespace detail;

MapInfo MapInfo::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    MapInfo s;
    // Version 0 stops after the u16. Otherwise ten fixed-size NUL-padded strings (title 64,
    // description 512, version 16, author 64, email 64, author2 128, email2 128, 256, copyright 128,
    // date 32), u32 game flags, then v2 player counts or v3 quest intro/graphic names.
    s.version = r.u16();
    if (s.version > 3) {
        r.fail("unsupported version " + std::to_string(s.version));
    }
    if (s.version >= 1) {
        readFixed(r, s.title);
        readFixed(r, s.description);
        readFixed(r, s.mapVersion);
        readFixed(r, s.author);
        readFixed(r, s.email);
        readFixed(r, s.author2);
        readFixed(r, s.email2);
        readFixed(r, s.unknown);
        readFixed(r, s.copyright);
        readFixed(r, s.date);
        s.gameFlags = r.u32();
        if (s.version == 2) {
            s.minPlayers = r.u8();
            s.maxPlayers = r.u8();
        } else if (s.version == 3) {
            s.questIntro = readString8(r);
            s.questGraphic = readString8(r);
        }
    }
    r.expectEnd();
    return s;
}

void MapInfo::write(ByteWriter& w) const
{
    w.u16(version);
    if (version >= 1) {
        writeFixed(w, title);
        writeFixed(w, description);
        writeFixed(w, mapVersion);
        writeFixed(w, author);
        writeFixed(w, email);
        writeFixed(w, author2);
        writeFixed(w, email2);
        writeFixed(w, unknown);
        writeFixed(w, copyright);
        writeFixed(w, date);
        w.u32(gameFlags);
        if (version == 2) {
            w.u8(minPlayers);
            w.u8(maxPlayers);
        } else if (version == 3) {
            writeString8(w, questIntro);
            writeString8(w, questGraphic);
        }
    }
}

std::vector<std::uint8_t> MapInfo::serialize() const
{
    return serialized(*this);
}

MapIntro MapIntro::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    MapIntro s;
    // u16 version, then an i32-length text. The game reads text only when the length is above
    // 0 (mapRWMapIntro); zero or negative means none.
    s.version = r.u16();
    const std::int32_t n = r.i32();
    if (n > 0) {
        s.text = r.rawString(static_cast<std::uint32_t>(n));
    } else {
        s.emptyLength = n;
    }
    r.expectEnd();
    return s;
}

void MapIntro::write(ByteWriter& w) const
{
    w.u16(version);
    if (text.empty()) {
        w.u32(static_cast<std::uint32_t>(emptyLength > 0 ? 0 : emptyLength));
        return;
    }
    w.u32(countOf<std::uint32_t>(text.size(), kName));
    // The game writes the intro one byte per call (it matters for the map CRC).
    writeBytewise(w, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::vector<std::uint8_t> MapIntro::serialize() const
{
    return serialized(*this);
}

AmbientData AmbientData::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    AmbientData s;
    // u16 version, then the r, g, b components as u32.
    s.version = r.u16();
    s.r = r.u32();
    s.g = r.u32();
    s.b = r.u32();
    r.expectEnd();
    return s;
}

void AmbientData::write(ByteWriter& w) const
{
    w.u16(version);
    w.u32(r);
    w.u32(g);
    w.u32(b);
}

std::vector<std::uint8_t> AmbientData::serialize() const
{
    return serialized(*this);
}

namespace {

// DebugData strings: i32 length including the NUL, then the bytes.
std::string readDebugString(ByteReader& r)
{
    const std::int32_t n = r.i32();
    if (n < 1) {
        r.fail("bad debug string length");
    }
    std::string s = r.rawString(static_cast<std::size_t>(n));
    if (s.back() != '\0') {
        r.fail("debug string is not NUL-terminated");
    }
    s.pop_back();
    return s;
}

void writeDebugString(ByteWriter& w, const std::string& s)
{
    w.i32(static_cast<std::int32_t>(s.size() + 1));
    w.rawString(s + '\0');
}

/// ScriptData state is the GLOBAL function's variables (count known only from ScriptObject)
/// followed by the activator block { u16 1; u32 frame; u32 n; n * 20 bytes }. Finds the variable
/// count that makes the block fill the rest exactly, or -1.
long globalsInState(std::span<const std::uint8_t> state)
{
    // Try each possible global count: the activator block is u16 + u32 + u32 + 20 bytes per entry,
    // so 10 + 20 * count bytes must remain after 4 * n bytes of globals.
    for (std::size_t n = 0; 4 * n + 10 <= state.size(); ++n) {
        ByteReader r(state.subspan(4 * n));
        const std::uint16_t version = r.u16();
        r.u32();
        const std::uint64_t count = r.u32();
        if (version == 1 && 4 * n + 10 + 20 * count == state.size()) {
            return static_cast<long>(n);
        }
    }
    return -1;
}

} // namespace

DebugData DebugData::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    DebugData s;
    s.version = r.u16();
    // u16 version, u32 entry count, then key/value string pairs.
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n; ++i) {
        std::string key = readDebugString(r);
        std::string value = readDebugString(r);
        s.entries.emplace_back(std::move(key), std::move(value));
    }
    r.expectEnd();
    return s;
}

void DebugData::write(ByteWriter& w) const
{
    w.u16(version);
    w.u32(countOf<std::uint32_t>(entries.size(), kName));
    for (const auto& [key, value] : entries) {
        writeDebugString(w, key);
        writeDebugString(w, value);
    }
}

std::vector<std::uint8_t> DebugData::serialize() const
{
    return serialized(*this);
}

ScriptObject ScriptObject::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    ScriptObject s;
    // u16 version, then the bytecode with a u32 length.
    s.version = r.u16();
    auto code = r.bytes(r.u32());
    s.code.assign(code.begin(), code.end());
    r.expectEnd();
    return s;
}

void ScriptObject::write(ByteWriter& w) const
{
    w.u16(version);
    w.u32(countOf<std::uint32_t>(code.size(), kName));
    // The game writes the bytecode one byte per call (map CRC).
    writeBytewise(w, code);
}

std::vector<std::uint8_t> ScriptObject::serialize() const
{
    return serialized(*this);
}

ScriptData ScriptData::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    ScriptData s;
    // u16 version, u8 flag, then (when set) the runtime state up to the section end.
    s.version = r.u16();
    s.hasState = r.u8();
    if (s.hasState != 0) {
        auto rest = r.bytes(r.remaining());
        s.state.assign(rest.begin(), rest.end());
    }
    r.expectEnd();
    return s;
}

void ScriptData::write(ByteWriter& w) const
{
    w.u16(version);
    w.u8(hasState);
    if (hasState == 0) {
        return;
    }
    // Globals in one call, then the activator block field by field (server_mapRWScriptData, script_activatorsSave).
    const long globals = globalsInState(state);
    // Couldn't split the state: write it as one block.
    if (globals < 0) {
        w.bytes(state);
        return;
    }
    const std::span<const std::uint8_t> s(state);
    w.bytes(s.first(4 * static_cast<std::size_t>(globals)));
    ByteReader r(s.subspan(4 * static_cast<std::size_t>(globals)));
    w.u16(r.u16());
    w.u32(r.u32());
    const std::uint32_t count = r.u32();
    w.u32(count);
    // Each activator entry is 20 bytes = five u32, written one call each.
    for (std::uint32_t i = 0; i < count * 5; ++i) {
        w.u32(r.u32());
    }
}

std::vector<std::uint8_t> ScriptData::serialize() const
{
    return serialized(*this);
}

} // namespace nox::map
