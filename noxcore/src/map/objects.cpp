// Object record header codec (see objects.hpp): the xfer version and the ObjectHeader every
// object type shares. The type-specific remainder is left in ObjectXfer::extra for xfer.cpp.
#include "noxcore/map/objects.hpp"

#include "section_io.hpp"

namespace nox::map {

using namespace detail;

namespace {

constexpr std::uint16_t kMinHeaderVersion = 0x3D; // older headers use a different reader
constexpr std::uint16_t kMaxHeaderVersion = 0x40;
constexpr std::uint16_t kMinXferVersion = 0x28;

// Layout: u16 version, u32 extent, u32 script id, f32 x, f32 y, u8 details flag; when the flag is
// set: u32 flags, u8-length script name, u8 team, u8 inventory count, u16 count + u32 owned
// script ids, u32 status flags, then the pickup handler (v > 0x3E) and decay frames (v > 0x3F).
ObjectHeader readHeader(ByteReader& r)
{
    ObjectHeader h;
    h.version = r.u16();
    if (h.version < kMinHeaderVersion || h.version > kMaxHeaderVersion) {
        r.fail("unsupported object header version " + std::to_string(h.version));
    }
    h.extent = r.u32();
    h.scriptId = r.u32();
    h.x = r.f32();
    h.y = r.f32();
    h.detailsFlag = r.u8();
    if (h.detailsFlag != 0) {
        ObjectHeader::Details d;
        d.flags = r.u32();
        d.scriptName = readString8(r);
        d.team = r.u8();
        d.inventoryCount = r.u8();
        d.ownedScriptIds.resize(r.u16());
        for (auto& id : d.ownedScriptIds) {
            id = r.u32();
        }
        d.statusFlags = r.u32();
        if (h.version > 0x3E) {
            d.onPickup = readScriptHandler(r);
        }
        if (h.version > 0x3F) {
            d.decayFrames = r.i32();
        }
        h.details = std::move(d);
    }
    return h;
}

void writeHeader(ByteWriter& w, const ObjectHeader& h)
{
    if (h.version < kMinHeaderVersion || h.version > kMaxHeaderVersion) {
        throw FormatError("ObjectHeader: unsupported version " + std::to_string(h.version));
    }
    if ((h.detailsFlag != 0) != h.details.has_value()) {
        throw FormatError("ObjectHeader: detailsFlag doesn't match details");
    }
    w.u16(h.version);
    w.u32(h.extent);
    w.u32(h.scriptId);
    w.f32(h.x);
    w.f32(h.y);
    w.u8(h.detailsFlag);
    if (!h.details) {
        return;
    }
    const auto& d = *h.details;
    if ((h.version > 0x3E) != d.onPickup.has_value() || (h.version > 0x3F) != d.decayFrames.has_value()) {
        throw FormatError("ObjectHeader: optional fields don't match the version");
    }
    w.u32(d.flags);
    writeString8(w, d.scriptName);
    w.u8(d.team);
    w.u8(d.inventoryCount);
    w.u16(countOf<std::uint16_t>(d.ownedScriptIds.size(), "owned objects"));
    for (auto id : d.ownedScriptIds) {
        w.u32(id);
    }
    w.u32(d.statusFlags);
    if (d.onPickup) {
        writeScriptHandler(w, *d.onPickup);
    }
    if (d.decayFrames) {
        w.i32(*d.decayFrames);
    }
}

// Recursive: the owner's items are the next `inventoryCount` records, each followed by its own
// items, so `next` advances depth first through the flat record list.
void buildChildren(std::span<const ObjectXfer> objects, std::size_t& next, ObjectTree& owner)
{
    for (std::uint8_t i = 0; i < objects[owner.record].header.inventoryCount(); ++i) {
        if (next >= objects.size()) {
            throw FormatError("ObjectData: inventory count runs past the last object");
        }
        ObjectTree item{next++, {}};
        buildChildren(objects, next, item);
        owner.inventory.push_back(std::move(item));
    }
}

} // namespace

ObjectXfer ObjectXfer::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "object xfer");
    ObjectXfer x;
    x.xferVersion = r.u16();
    if (x.xferVersion < kMinXferVersion) {
        r.fail("unsupported xfer version " + std::to_string(x.xferVersion));
    }
    x.header = readHeader(r);
    auto rest = r.bytes(r.remaining());
    x.extra.assign(rest.begin(), rest.end());
    return x;
}

void ObjectXfer::write(ByteWriter& w) const
{
    w.u16(xferVersion);
    writeHeader(w, header);
    w.bytes(extra);
}

std::vector<std::uint8_t> ObjectXfer::serialize() const
{
    return serialized(*this);
}

ObjectHeader readObjectHeader(ByteReader& r)
{
    return readHeader(r);
}

void writeObjectHeader(ByteWriter& w, const ObjectHeader& header)
{
    writeHeader(w, header);
}

std::size_t ObjectXfer::extraOffset() const
{
    // Measured by encoding the header again, so it always matches what write() produces.
    ByteWriter w;
    w.u16(xferVersion);
    writeHeader(w, header);
    return w.size();
}

std::vector<ObjectTree> buildObjectTree(std::span<const ObjectXfer> objects)
{
    std::vector<ObjectTree> out;
    std::size_t next = 0;
    while (next < objects.size()) {
        ObjectTree top{next++, {}};
        buildChildren(objects, next, top);
        out.push_back(std::move(top));
    }
    return out;
}

} // namespace nox::map
