// WayPoints, Polygons, GroupData, ObjectTOC.
// Also ObjectData. The map's script markers (waypoints, polygons, groups) and the object
// type table and object records (structs in sections.hpp).
#include "section_io.hpp"

namespace nox::map {

using namespace detail;

Waypoints Waypoints::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    Waypoints s;
    s.version = r.u16();
    if (s.version != 4) {
        r.fail("unsupported version " + std::to_string(s.version));
    }
    // u32 count; per waypoint: u32 id, f32 x, f32 y, u8-length name, u32 flags, u8 link count,
    // then 5 bytes per link (u32 target id, u8 flags).
    s.waypoints.resize(r.count(r.u32(), 18));
    for (auto& wp : s.waypoints) {
        wp.id = r.u32();
        wp.x = r.f32();
        wp.y = r.f32();
        wp.name = readString8(r);
        wp.flags = r.u32();
        wp.links.resize(r.u8());
        for (auto& link : wp.links) {
            link.target = r.u32();
            link.flags = r.u8();
        }
    }
    r.expectEnd();
    return s;
}

void Waypoints::write(ByteWriter& w) const
{
    w.u16(version);
    w.u32(countOf<std::uint32_t>(waypoints.size(), kName));
    for (const auto& wp : waypoints) {
        w.u32(wp.id);
        w.f32(wp.x);
        w.f32(wp.y);
        writeString8(w, wp.name);
        w.u32(wp.flags);
        w.u8(countOf<std::uint8_t>(wp.links.size(), "waypoint links"));
        for (const auto& link : wp.links) {
            w.u32(link.target);
            w.u8(link.flags);
        }
    }
}

std::vector<std::uint8_t> Waypoints::serialize() const
{
    return serialized(*this);
}

Polygons Polygons::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    Polygons s;
    s.version = r.u16();
    if (s.version < 3 || s.version > 4) {
        r.fail("unsupported version " + std::to_string(s.version));
    }
    // u32 point count with 12 bytes each (u32 id, f32 x, f32 y), then u32 polygon count; per polygon:
    // name, RGB ambient, u8 minimap group, u16 point-id count + u32 ids, two script handlers,
    // and from v4 a u32 flags.
    s.points.resize(r.count(r.u32(), 12));
    for (auto& p : s.points) {
        p.id = r.u32();
        p.x = r.f32();
        p.y = r.f32();
    }
    s.polygons.resize(r.count(r.u32()));
    for (auto& poly : s.polygons) {
        poly.name = readString8(r);
        for (auto& c : poly.ambientLight) {
            c = r.u8();
        }
        poly.minimapGroup = r.u8();
        poly.points.resize(r.u16());
        for (auto& id : poly.points) {
            id = r.u32();
        }
        poly.playerEnter = readScriptHandler(r);
        poly.monsterEnter = readScriptHandler(r);
        if (s.version >= 4) {
            poly.flags = r.u32();
        }
    }
    r.expectEnd();
    return s;
}

std::vector<std::uint8_t> Polygons::serialize() const
{
    return serialized(*this);
}

void Polygons::write(ByteWriter& w) const
{
    if (version < 3 || version > 4) {
        throw FormatError("Polygons: can only write versions 3 and 4");
    }
    w.u16(version);
    w.u32(countOf<std::uint32_t>(points.size(), kName));
    for (const auto& p : points) {
        w.u32(p.id);
        // x and y go out as one 8-byte call, as the game writes them (map CRC).
        writeJoined(w, {bitsOf(p.x), bitsOf(p.y)});
    }
    w.u32(countOf<std::uint32_t>(polygons.size(), kName));
    for (const auto& poly : polygons) {
        writeString8(w, poly.name);
        writeBytewise(w, poly.ambientLight);
        w.u8(poly.minimapGroup);
        w.u16(countOf<std::uint16_t>(poly.points.size(), "polygon points"));
        std::vector<std::uint8_t> ids; // one call for the whole list
        for (auto id : poly.points) {
            for (int i = 0; i < 4; ++i) {
                ids.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
            }
        }
        w.bytes(ids);
        writeScriptHandler(w, poly.playerEnter);
        writeScriptHandler(w, poly.monsterEnter);
        if (version >= 4) {
            w.u32(poly.flags);
        }
    }
}

GroupData GroupData::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    GroupData s;
    s.version = r.u16();
    if (s.version != 3) {
        r.fail("unsupported version " + std::to_string(s.version));
    }
    // u32 count; per group: name (with its NUL inside the u8 length), u8 kind, i32 id, i32 member
    // count, then members as i32 ids or, for wall groups, grid points.
    s.groups.resize(r.count(r.u32()));
    for (auto& g : s.groups) {
        g.name = readString8(r);
        // The game's reader (mapRWGroupData) copies the length's bytes and never terminates
        // them, so a name stored without its NUL loads: the stored bytes are the name.
        if (!g.name.empty() && g.name.back() == '\0') {
            g.name.pop_back();
        } else {
            g.terminated = false;
        }
        const std::uint8_t kind = r.u8();
        if (kind > static_cast<std::uint8_t>(Group::Kind::Unknown3)) {
            r.fail("unknown group kind " + std::to_string(kind));
        }
        g.kind = static_cast<Group::Kind>(kind);
        g.id = r.i32();
        const std::int32_t n = r.i32();
        if (n < 0) {
            r.fail("negative group size");
        }
        for (std::int32_t i = 0; i < n; ++i) {
            if (g.kind == Group::Kind::Walls) {
                g.walls.push_back(readGridPoint(r));
            } else {
                g.members.push_back(r.i32());
            }
        }
    }
    r.expectEnd();
    return s;
}

void GroupData::write(ByteWriter& w) const
{
    w.u16(version);
    w.u32(countOf<std::uint32_t>(groups.size(), kName));
    for (const auto& g : groups) {
        writeString8(w, g.terminated ? g.name + '\0' : g.name);
        w.u8(static_cast<std::uint8_t>(g.kind));
        w.i32(g.id);
        if (g.kind == Group::Kind::Walls) {
            w.i32(countOf<std::int32_t>(g.walls.size(), kName));
            for (const auto& p : g.walls) {
                writeGridPoint(w, p);
            }
        } else {
            w.i32(countOf<std::int32_t>(g.members.size(), kName));
            for (auto id : g.members) {
                w.i32(id);
            }
        }
    }
}

std::vector<std::uint8_t> GroupData::serialize() const
{
    return serialized(*this);
}

ObjectTOC ObjectTOC::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    ObjectTOC s;
    s.version = r.u16();
    // u16 count; per entry: u16 id, u8-length type name.
    s.entries.resize(r.u16());
    for (auto& e : s.entries) {
        e.id = r.u16();
        e.name = readString8(r);
    }
    r.expectEnd();
    return s;
}

ObjectData ObjectData::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, kName);
    ObjectData s;
    s.version = r.u16();
    if (s.version > 1) {
        r.fail("unsupported version " + std::to_string(s.version));
    }
    // Records until a toc id of 0; see ObjectData in sections.hpp for the layout.
    for (;;) {
        ObjectRecord rec;
        rec.tocId = r.u16();
        if (rec.tocId == 0) {
            break;
        }
        const std::size_t padStart = r.offset();
        // Padding is counted from the start of the section data, which the file keeps 8-aligned.
        r.align(8);
        rec.padding.assign(data.begin() + static_cast<std::ptrdiff_t>(padStart),
                           data.begin() + static_cast<std::ptrdiff_t>(r.offset()));
        rec.size = r.u32();
        rec.sizeHi = r.u32();
        auto body = r.bytes(rec.size);
        rec.data.assign(body.begin(), body.end());
        s.records.push_back(std::move(rec));
    }
    r.expectEnd();
    return s;
}

void ObjectData::write(ByteWriter& w) const
{
    w.u16(version);
    for (const auto& rec : records) {
        if (rec.tocId == 0 || rec.size != rec.data.size()) {
            throw FormatError("ObjectData: record with id 0 or a size that doesn't match its data");
        }
        w.u16(rec.tocId);
        // Padding and size are written below the CRC, not through ReadWrite.
        writeRecordPadding(w, rec);
        w.uncountedU32(rec.size);
        w.uncountedU32(rec.sizeHi);
        w.bytes(rec.data);
    }
    w.u16(0);
}

std::vector<std::uint8_t> ObjectData::serialize() const
{
    return serialized(*this);
}

void ObjectTOC::write(ByteWriter& w) const
{
    w.u16(version);
    w.u16(countOf<std::uint16_t>(entries.size(), kName));
    for (const auto& e : entries) {
        w.u16(e.id);
        writeString8(w, e.name);
    }
}

std::vector<std::uint8_t> ObjectTOC::serialize() const
{
    return serialized(*this);
}

} // namespace nox::map
