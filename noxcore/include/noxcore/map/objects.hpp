// Map object records (ObjectData) decoded down to the header every xfer shares.
//
// Each record's data is written by the object type's xfer function (DefaultXfer, DoorXfer, ...;
// the THNG XFER attribute, DefaultXfer when absent):
//   u16 xferVersion
//   ObjectHeader                  mapReadWriteObjData
//   type-specific fields          kept undecoded in ObjectXfer::extra for now
#pragma once

#include "noxcore/map/sections.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nox::map {

/// Common object state, header versions 0x3D..0x40.
struct ObjectHeader {
    struct Details {
        std::uint32_t flags = 0; // object flags, masked with 0x11408162 on write
        std::string scriptName;  // at most 255 bytes; empty for none
        std::uint8_t team = 0;
        /// Number of records following this one (after nested inventories) that are its items.
        std::uint8_t inventoryCount = 0;
        std::vector<std::uint32_t> ownedScriptIds; // objects this one owns
        std::uint32_t statusFlags = 0;             // "xStatus", masked with 0x5E
        std::optional<ScriptHandler> onPickup;     // version > 0x3E
        std::optional<std::int32_t> decayFrames;   // version > 0x3F: frames left before decay
        bool operator==(const Details&) const = default;
    };

    std::uint16_t version = 0x40;
    std::uint32_t extent = 0;   // map-unique object number
    std::uint32_t scriptId = 0; // 0 means "assign one on load"
    float x = 0;
    float y = 0;
    std::uint8_t detailsFlag = 1; // non-zero when `details` is present
    std::optional<Details> details;
    bool operator==(const ObjectHeader&) const = default;

    std::uint8_t inventoryCount() const { return details ? details->inventoryCount : 0; }
};

/// One object record's data: xfer version, common header and the type-specific remainder.
struct ObjectXfer {
    std::uint16_t xferVersion = 0x3C;
    ObjectHeader header;
    std::vector<std::uint8_t> extra; // fields after the header, per xfer type

    static ObjectXfer parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;

    /// Where `extra` starts within the serialised record data.
    std::size_t extraOffset() const;
};

/// Reads/writes just the header (the part of ObjectXfer after xferVersion).
ObjectHeader readObjectHeader(ByteReader& r);
void writeObjectHeader(ByteWriter& w, const ObjectHeader& header);

/// An object with its inventory, as indices into a list of records.
struct ObjectTree {
    std::size_t record = 0;
    std::vector<ObjectTree> inventory;
};

/// Nests records by their headers' inventory counts: an owner's items are the records that
/// follow it (each with its own nested items). Throws FormatError if the counts overrun.
std::vector<ObjectTree> buildObjectTree(std::span<const ObjectXfer> objects);

} // namespace nox::map
