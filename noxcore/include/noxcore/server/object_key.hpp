// The kinds of number that name an object on the server, each its own type so one can't be taken
// for another (map numbers and clients' codes were once looked up as keys, and keys kept across a
// renumbering): what can name a thing, and how it's looked up.
//
//  - ObjectKey: our key, ServerObject::extent (World::byExtent). For a dynamic thing also its net
//    code. A load gives a record whose number is taken a fresh one (World::placeRecords).
//  - MapNumber: the number a map, save or .plr record gives a thing (the exe's Extent,
//    ServerObject::fileExtent; World::byMapNumber), and what files name things by: a mover's load,
//    an object group's members, a lift's or pad's partner. Not unique, and not a key.
//  - NetCode: a code a client sends for a thing (the message decoders; World::byPacketCode,
//    World::byNetCode): a dynamic thing's key, or a map object's map number with bit 15.
//  - GroupMember: one of a map group's members: a map number, a waypoint id or a group id by the
//    group's kind.
//  - ObjectRef: a stored link to another thing (its key), held in a field that outlives the frame
//    (an owner, a holder, a target...). Every such field is listed with its rule for a map change in
//    object_links.hpp, which the map change carries them all through (carryLinks), and a test checks
//    the lists against the headers: a new link field can't be left out.
//
// MapNumber, NetCode and GroupMember convert to nothing implicitly, so passing one where a key is
// expected (byExtent, an ObjectRef) doesn't compile; their raw values are explicit (number(), code(),
// raw()), and so is taking one as a key (asExtent()), each such crossing a decision of its own. All of
// them are a 32-bit number inside: no cost at run time.
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <vector>

namespace nox::server {

/// Our key for an object (ServerObject::extent; World::byExtent's): a plain number, as every
/// extent in the server is. What's kept as a link to another object is an ObjectRef.
using ObjectKey = std::uint32_t;

/// A number a map, save or .plr record gives a thing (the exe's Extent): what files and scripts name
/// things by. 0 names none.
class MapNumber {
public:
    constexpr MapNumber() noexcept = default;
    constexpr explicit MapNumber(std::uint32_t number) noexcept : number_(number) {}
    /// The number itself (what a file holds, what's written or sent).
    constexpr std::uint32_t number() const noexcept { return number_; }
    /// The number taken as our key as it is: the extent the record's thing had at its load unless
    /// it was numbered afresh. Only where that's meant (World::byMapNumber's first candidate, a
    /// record's thing given its number as its key); a lookup by map number is byMapNumber.
    constexpr ObjectKey asExtent() const noexcept { return number_; }
    constexpr explicit operator bool() const noexcept { return number_ != 0; }
    friend constexpr bool operator==(MapNumber, MapNumber) noexcept = default;
    friend constexpr auto operator<=>(MapNumber, MapNumber) noexcept = default;

private:
    std::uint32_t number_ = 0;
};

/// A client's code for a thing (packetDynamicUnitCode's input): a dynamic thing's net code (our
/// key), or a map object's map number with bit 15.
class NetCode {
public:
    constexpr NetCode() noexcept = default;
    constexpr explicit NetCode(std::uint32_t code) noexcept : code_(code) {}
    /// The code itself (as the message has it).
    constexpr std::uint32_t code() const noexcept { return code_; }
    /// Bit 15: a map object's code, its map number below.
    constexpr bool mapObject() const noexcept { return (code_ & 0x8000u) != 0; }
    /// A map object's map number (the code's low 15 bits).
    constexpr MapNumber mapNumber() const noexcept { return MapNumber{code_ & 0x7FFFu}; }
    /// The code taken as our key as it is (our net code is the extent), no bit-15 test: only where
    /// a decoder looks a code up that way and checks what it found (a holder, the thing's own
    /// code). A lookup as the exe makes it is byPacketCode or byNetCode.
    constexpr ObjectKey asExtent() const noexcept { return code_; }
    friend constexpr bool operator==(NetCode, NetCode) noexcept = default;

private:
    std::uint32_t code_ = 0;
};

/// One of a map group's members (the file's i32): an object group's are map numbers, a waypoint
/// group's waypoint ids, a group of groups' group ids. Which one is the reader's choice.
class GroupMember {
public:
    constexpr GroupMember() noexcept = default;
    constexpr explicit GroupMember(std::int32_t value) noexcept : value_(value) {}
    /// As the file has it (the save writes it back).
    constexpr std::int32_t raw() const noexcept { return value_; }
    /// An object group's member: a thing's map number (World::byMapNumber).
    constexpr MapNumber mapNumber() const noexcept { return MapNumber{static_cast<std::uint32_t>(value_)}; }
    /// A waypoint group's member: a waypoint's id.
    constexpr std::int32_t waypointId() const noexcept { return value_; }
    /// A group of groups' member: a group's id.
    constexpr std::int32_t groupId() const noexcept { return value_; }
    friend constexpr bool operator==(GroupMember, GroupMember) noexcept = default;

private:
    std::int32_t value_ = 0;
};

/// A stored link to another object by its key (0: none). It reads as the key it holds and takes a
/// key by assignment; made from a key only explicitly, so a map number or a client's code can't
/// become one by accident (neither converts to a key). Each field of this type is listed, with what a
/// map change does to it, in object_links.hpp.
class ObjectRef {
public:
    constexpr ObjectRef() noexcept = default;
    constexpr explicit ObjectRef(ObjectKey key) noexcept : key_(key) {}
    constexpr ObjectRef& operator=(ObjectKey key) noexcept
    {
        key_ = key;
        return *this;
    }
    constexpr operator ObjectKey() const noexcept { return key_; }
    constexpr ObjectKey key() const noexcept { return key_; }

private:
    ObjectKey key_ = 0;
};

static_assert(sizeof(MapNumber) == 4 && sizeof(NetCode) == 4 && sizeof(GroupMember) == 4 && sizeof(ObjectRef) == 4);

/// A list of links copied as keys (to walk while the list itself changes: an inventory emptied).
inline std::vector<ObjectKey> keysOf(const std::vector<ObjectRef>& refs) { return {refs.begin(), refs.end()}; }

} // namespace nox::server

template <>
struct std::hash<nox::server::MapNumber> {
    std::size_t operator()(nox::server::MapNumber n) const noexcept { return std::hash<std::uint32_t>{}(n.number()); }
};
