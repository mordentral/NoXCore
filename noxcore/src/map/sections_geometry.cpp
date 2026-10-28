// WallMap, FloorMap, SecretWalls, DestructableWalls, WindowWalls.
// The map's wall and floor grid sections (structs in sections.hpp).
#include "section_io.hpp"

namespace nox::map {

using namespace detail;

namespace {

// List terminators and the FloorMap pair-key flag bits (see FloorTilePair in sections.hpp).
constexpr std::uint8_t kWallEnd = 0xFF;
constexpr std::uint16_t kOddTile = 0x8000;
constexpr std::uint16_t kEvenTile = 0x80;
constexpr std::uint16_t kFloorEnd = 0xFFFF;

// u16 version, then the bounding box as four i32: min x, min y, width, height.
GridHeader readGridHeader(ByteReader& r)
{
    GridHeader h;
    h.version = r.u16();
    h.minX = r.i32();
    h.minY = r.i32();
    h.width = r.i32();
    h.height = r.i32();
    return h;
}

void writeGridHeader(ByteWriter& w, const GridHeader& h)
{
    w.u16(h.version);
    w.i32(h.minX);
    w.i32(h.minY);
    w.i32(h.width);
    w.i32(h.height);
}

// Tile record: u8 image, i16 variant, two u8, then a u8 edge count and 5 bytes per edge
// (u8 floor, i16 variant, u8 edge, u8 direction).
FloorTile readTile(ByteReader& r)
{
    FloorTile t;
    t.image = r.u8();
    t.variant = r.i16();
    t.unknown1 = r.u8();
    t.unknown2 = r.u8();
    const std::uint8_t n = r.u8();
    t.edges.resize(n);
    for (auto& e : t.edges) {
        e.floor = r.u8();
        e.variant = r.i16();
        e.edge = r.u8();
        e.direction = r.u8();
    }
    return t;
}

void writeTile(ByteWriter& w, const FloorTile& t)
{
    w.u8(t.image);
    w.i16(t.variant);
    w.u8(t.unknown1);
    w.u8(t.unknown2);
    w.u8(countOf<std::uint8_t>(t.edges.size(), "FloorMap edges"));
    for (const auto& e : t.edges) {
        w.u8(e.floor);
        w.i16(e.variant);
        w.u8(e.edge);
        w.u8(e.direction);
    }
}

// The wall list sections (DestructableWalls, WindowWalls): u16 version, u16 count, then one
// GridPoint (two i32) per wall.
template <class Section>
Section parseWallList(std::span<const std::uint8_t> data)
{
    ByteReader r(data, Section::kName);
    Section s;
    s.version = r.u16();
    s.walls.resize(r.u16());
    for (auto& p : s.walls) {
        p = readGridPoint(r);
    }
    r.expectEnd();
    return s;
}

template <class Section>
void writeWallList(ByteWriter& w, const Section& s)
{
    w.u16(s.version);
    w.u16(countOf<std::uint16_t>(s.walls.size(), Section::kName));
    for (const auto& p : s.walls) {
        // The game writes each point as one 8-byte call (it matters for the map CRC).
        writeGridPointJoined(w, p);
    }
}

} // namespace

WallMap WallMap::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    WallMap s;
    s.header = readGridHeader(r);
    // 7 bytes per wall: x, y, direction, material, variant, minimap group, unknown. An x of 0xFF ends
    // the list in place of a count.
    for (;;) {
        Wall wall;
        wall.x = r.u8();
        if (wall.x == kWallEnd) {
            break;
        }
        wall.y = r.u8();
        wall.direction = r.u8();
        wall.material = r.u8();
        wall.variant = r.u8();
        wall.minimapGroup = r.u8();
        wall.unknown = r.u8();
        s.walls.push_back(wall);
    }
    r.expectEnd();
    return s;
}

void WallMap::write(ByteWriter& w) const
{
    writeGridHeader(w, header);
    for (const auto& wall : walls) {
        if (wall.x == kWallEnd) {
            throw FormatError("WallMap: wall x of 0xFF would end the list");
        }
        w.u8(wall.x);
        w.u8(wall.y);
        w.u8(wall.direction);
        w.u8(wall.material);
        w.u8(wall.variant);
        w.u8(wall.minimapGroup);
        w.u8(wall.unknown);
    }
    w.u8(kWallEnd);
}

std::vector<std::uint8_t> WallMap::serialize() const
{
    return serialized(*this);
}

FloorMap FloorMap::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    FloorMap s;
    s.header = readGridHeader(r);
    if (s.header.version != 4) {
        r.fail("unsupported version " + std::to_string(s.header.version));
    }
    for (;;) {
        // Pair key: bits 8..14 x, bits 0..6 y, plus the odd/even presence flags; 0xFFFF ends the list.
        const std::uint16_t key = r.u16();
        if (key == kFloorEnd) {
            break;
        }
        FloorTilePair p;
        p.x = (key >> 8) & 0x7F;
        p.y = key & 0x7F;
        if (key & kOddTile) {
            p.odd = readTile(r);
        }
        if (key & kEvenTile) {
            p.even = readTile(r);
        }
        s.tiles.push_back(std::move(p));
    }
    r.expectEnd();
    return s;
}

void FloorMap::write(ByteWriter& w) const
{
    writeGridHeader(w, header);
    for (const auto& p : tiles) {
        if ((p.x | p.y) & 0x80) {
            throw FormatError("FloorMap: tile pair coordinates must be below 0x80");
        }
        w.u16(static_cast<std::uint16_t>((p.x << 8) | p.y | (p.odd ? kOddTile : 0) | (p.even ? kEvenTile : 0)));
        if (p.odd) {
            writeTile(w, *p.odd);
        }
        if (p.even) {
            writeTile(w, *p.even);
        }
    }
    w.u16(kFloorEnd);
}

std::vector<FloorTilePair> FloorMap::readPairs() const
{
    // (server_mapRWFloorMap's read: each key sets the pair's presence bits and reads the tile
    // over the pair's slot; tileReadOne starts the edge list afresh.)
    std::vector<FloorTilePair> out;
    std::vector<std::int32_t> at(0x80 * 0x80, -1); // pair -> index in out
    for (const auto& p : tiles) {
        if (!p.odd && !p.even) {
            continue;
        }
        auto& i = at[static_cast<std::size_t>(p.x & 0x7F) * 0x80 + (p.y & 0x7F)];
        if (i < 0) {
            i = static_cast<std::int32_t>(out.size());
            out.push_back(p);
            continue;
        }
        auto& q = out[static_cast<std::size_t>(i)];
        if (p.odd) q.odd = p.odd;
        if (p.even) q.even = p.even;
    }
    return out;
}

std::vector<std::uint8_t> FloorMap::serialize() const
{
    return serialized(*this);
}

SecretWalls SecretWalls::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    SecretWalls s;
    s.version = r.u16();
    // u16 count, then 23 bytes per wall: grid point, u32 open wait, u8 flags, state, open delay,
    // u32 last open, u32.
    s.walls.resize(r.u16());
    for (auto& wall : s.walls) {
        wall.pos = readGridPoint(r);
        wall.openWait = r.u32();
        wall.flags = r.u8();
        wall.state = r.u8();
        wall.openDelay = r.u8();
        wall.lastOpen = r.u32();
        wall.unknown = r.u32();
    }
    r.expectEnd();
    return s;
}

void SecretWalls::write(ByteWriter& w) const
{
    w.u16(version);
    w.u16(countOf<std::uint16_t>(walls.size(), kName));
    for (const auto& wall : walls) {
        writeGridPointJoined(w, wall.pos);
        w.u32(wall.openWait);
        w.u8(wall.flags);
        w.u8(wall.state);
        w.u8(wall.openDelay);
        w.u32(wall.lastOpen);
        w.u32(wall.unknown);
    }
}

std::vector<std::uint8_t> SecretWalls::serialize() const
{
    return serialized(*this);
}

DestructableWalls DestructableWalls::parse(std::span<const std::uint8_t> data)
{
    return parseWallList<DestructableWalls>(data);
}

void DestructableWalls::write(ByteWriter& w) const
{
    writeWallList(w, *this);
}

std::vector<std::uint8_t> DestructableWalls::serialize() const
{
    return serialized(*this);
}

WindowWalls WindowWalls::parse(std::span<const std::uint8_t> data)
{
    return parseWallList<WindowWalls>(data);
}

void WindowWalls::write(ByteWriter& w) const
{
    writeWallList(w, *this);
}

std::vector<std::uint8_t> WindowWalls::serialize() const
{
    return serialized(*this);
}

} // namespace nox::map
