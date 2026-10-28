// Reader for MSG_UPDATE_STREAM; the byte layout is described in update_stream.hpp.
#include "noxcore/net/update_stream.hpp"

#include "noxcore/io/byte_reader.hpp"

#include <string>

namespace nox::net {

namespace {

// Thing class bits the entry layout depends on, the coordinate range the client accepts, and
// how many frames an entry's alias lasts.
constexpr std::uint32_t kClassPlayer = 0x4;
constexpr std::uint32_t kClassComplex = 0x200000;
constexpr std::int32_t kMaxCoord = 6000;
constexpr std::uint32_t kEntryAliasLifetime = 60;

/// Bounds-checked cursor; 1.2b reads past the datagram here, we throw instead.
class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> d)
        : d_(d)
    {
    }

    std::size_t pos = 0;

    std::uint8_t at(std::size_t off) const
    {
        if (off >= d_.size()) {
            throw FormatError("update stream: truncated at offset " + std::to_string(off));
        }
        return d_[off];
    }
    std::uint8_t u8() { return at(pos++); }
    std::uint16_t u16()
    {
        const auto v = static_cast<std::uint16_t>(at(pos) | (at(pos + 1) << 8));
        pos += 2;
        return v;
    }
    bool atEnd() const { return pos >= d_.size(); }

private:
    std::span<const std::uint8_t> d_;
};

/// Reads an object id whose first byte is `first`, assigning an alias for the long form.
void readId(Cursor& c, std::uint8_t first, StreamObject& o, AliasTable& aliases, std::uint32_t frame,
            std::uint32_t expire, UpdateStream& out)
{
    if (first != AliasTable::kNone) {
        o.alias = first;
        o.netCode = aliases[first].netCode;
        o.typeId = aliases[first].typeId;
        return;
    }
    o.longForm = true;
    o.netCode = c.u16();
    o.typeId = c.u16();
    o.alias = aliases.allocate(o.netCode, o.typeId, frame);
    if (o.alias != AliasTable::kNone) {
        aliases.set(o.alias, {o.netCode, o.typeId, expire});
        out.newAliases.push_back({o.alias, o.netCode, o.typeId, expire});
    }
}

/// The anim byte, and the explicit frame byte that follows it when its bit 7 says so.
void readAnim(Cursor& c, StreamObject& o)
{
    o.hasAnim = true;
    o.anim = c.u8();
    if (o.anim & 0x80) {
        o.frame = c.u8();
    }
}

} // namespace

std::uint8_t AliasTable::allocate(std::uint16_t netCode, std::uint16_t typeId, std::uint32_t frame) const
{
    // Probe from the slot the net code's low byte names, skipping 0 and 0xFF, wrapping round
    // once.
    std::uint32_t start = netCode & 0xFF;
    if (start == 0 || start == 0xFF) {
        start = 1;
    }
    std::uint32_t i = start;
    do {
        const Slot& s = slots_[i];
        if ((s.netCode == netCode && s.typeId == typeId) || s.expireFrame < frame) {
            return static_cast<std::uint8_t>(i);
        }
        if (++i == 0xFF) {
            i = 1;
        }
    } while (i != start);
    return kNone;
}

UpdateStream readUpdateStream(std::span<const std::uint8_t> data, AliasTable& aliases, std::uint32_t frame,
                              const DrawableClassLookup& classOf)
{
    Cursor c(data);
    if (c.u8() != static_cast<std::uint8_t>(Op::UpdateStream)) {
        throw FormatError("update stream: wrong opcode");
    }
    UpdateStream out;

    // Header.
    StreamObject& h = out.header;
    readId(c, c.u8(), h, aliases, frame, 0xFFFFFFFF, out);
    h.absolute = true;
    h.x = c.u16();
    h.y = c.u16();
    readAnim(c, h);
    h.state = c.u8();

    // Entries, relative to the previous object.
    std::int32_t x = h.x;
    std::int32_t y = h.y;
    while (!c.atEnd()) {
        StreamObject e;
        std::uint8_t first = c.u8();
        // A leading 0 marks an absolute position; 00 00 00 ends the stream.
        if (first == 0) {
            first = c.u8();
            if (first == 0 && c.at(c.pos) == 0) {
                ++c.pos; // 00 00 00 ends the stream
                break;
            }
            e.absolute = true;
        }
        readId(c, first, e, aliases, frame, frame + kEntryAliasLifetime, out);
        if (e.absolute) {
            x = static_cast<std::int16_t>(c.u16());
            y = static_cast<std::int16_t>(c.u16());
        } else {
            x += static_cast<std::int8_t>(c.u8());
            y += static_cast<std::int8_t>(c.u8());
        }
        e.x = x;
        e.y = y;
        const bool inRange = x >= 0 && x <= kMaxCoord && y >= 0 && y <= kMaxCoord;
        // The class lookup is skipped for out-of-range positions, which abort the same way.
        const std::optional<std::uint32_t> cls = inRange ? classOf(e.netCode, e.typeId) : std::nullopt;
        if (!cls) {
            out.entries.push_back(e);
            out.aborted = true;
            break;
        }
        // Only complex objects carry anim bytes, and only players a state byte.
        if (*cls & kClassComplex) {
            readAnim(c, e);
            if (*cls & kClassPlayer) {
                e.state = c.u8();
            }
        }
        out.entries.push_back(e);
    }
    out.size = c.pos;
    return out;
}

} // namespace nox::net
