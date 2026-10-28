// MonsterXfer, NPCXfer and MonsterGeneratorXfer codecs (internal to xfer.cpp).
#pragma once

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/map/xfer.hpp"

namespace nox::map::xfer::monster {

/// Four u8-length names, one per modifier slot (an empty name means the slot is unset): the
/// weapons' and armour's in xfer.cpp, the shops' items here.
Modifiers readModifiers(ByteReader& r);
void writeModifiers(ByteWriter& w, const Modifiers& m);

Monster readMonster(ByteReader& r, std::uint16_t v, const TypeInfo& t);
void writeMonster(ByteWriter& w, const Monster& m, std::uint16_t v, const TypeInfo& t);

Npc readNpc(ByteReader& r, std::uint16_t v);
void writeNpc(ByteWriter& w, const Npc& n, std::uint16_t v);

/// `base` is the offset of the reader/writer's start within the 8-aligned record data.
MonsterGenerator readGenerator(ByteReader& r, std::uint16_t v, std::size_t base);
void writeGenerator(ByteWriter& w, const MonsterGenerator& g, std::uint16_t v, std::size_t base,
                    const RecordDataWriter& nested);

} // namespace nox::map::xfer::monster
