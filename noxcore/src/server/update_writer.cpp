// Writes a player's MSG_UPDATE_STREAM the way the exe's server packs it (update_writer.hpp).
#include "noxcore/server/update_writer.hpp"

#include "noxcore/server/combat.hpp"

#include <algorithm>
#include <cstdlib>

namespace nox::server {

std::uint8_t directionNibble(std::uint16_t direction)
{
    const auto [dx, dy] = gridDirection(direction & 0xFF);
    int index = dy * 3 + dx + 4;
    if (index > 3) {
        --index;
    }
    return static_cast<std::uint8_t>(index << 4);
}

std::uint8_t serverAlias(const net::AliasTable& aliases, std::uint16_t netCode, std::uint16_t typeId, std::uint32_t frame)
{
    // Probe the client's alias table as the client allocates it: from the net code's low byte,
    // skipping 0 and 0xFF, once round. An expired alias counts as none.
    std::uint32_t start = netCode & 0xFFu;
    if (start == 0xFF || start == 0) {
        start = 1;
    }
    std::uint32_t i = start;
    do {
        const auto& slot = aliases[static_cast<std::uint8_t>(i)];
        if (slot.netCode == netCode && slot.typeId == typeId) {
            return frame <= slot.expireFrame ? static_cast<std::uint8_t>(i) : 0xFF;
        }
        if (++i == 0xFF) {
            i = 1;
        }
    } while (i != start);
    return 0xFF;
}

std::vector<std::uint8_t> writeUpdateStream(const UpdateRecord& self, std::span<const UpdateRecord> others,
                                            const net::AliasTable& aliases, std::uint32_t frame, std::size_t budget,
                                            std::vector<bool>* written)
{
    std::vector<std::uint8_t> out;
    std::vector<bool> taken;
    std::vector<bool> used;
    appendUpdateStream(out, self, others, aliases, frame, budget, written ? *written : taken, used);
    return out;
}

void appendUpdateStream(std::vector<std::uint8_t>& out, const UpdateRecord& self, std::span<const UpdateRecord> others,
                        const net::AliasTable& aliases, std::uint32_t frame, std::size_t budget, std::vector<bool>& written,
                        std::vector<bool>& used)
{
    written.assign(others.size(), false);
    const std::size_t start = out.size();
    out.push_back(0xA4);
    const auto u16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto id = [&](const UpdateRecord& r) {
        const std::uint8_t alias = serverAlias(aliases, r.netCode, r.typeId, frame);
        out.push_back(alias);
        if (alias == 0xFF) {
            u16(r.netCode);
            u16(r.typeId);
        }
    };
    // Simple objects have no animation bytes; bit 7 of anim says a frame byte follows.
    const auto animation = [&](const UpdateRecord& r) {
        if (r.kind == UpdateRecord::Kind::Simple) {
            return;
        }
        out.push_back(r.frame != 0xFF ? static_cast<std::uint8_t>(r.anim | 0x80) : r.anim);
        if (r.frame != 0xFF) {
            out.push_back(r.frame);
        }
        if (r.kind == UpdateRecord::Kind::Player) {
            out.push_back(r.state);
        }
    };
    // The header: the player, at an even position.
    const std::uint16_t hx = self.x & 0xFFFE;
    const std::uint16_t hy = self.y & 0xFFFE;
    id(self);
    u16(hx);
    u16(hy);
    out.push_back(self.frame != 0xFF ? static_cast<std::uint8_t>(self.anim | 0x80) : self.anim);
    if (self.frame != 0xFF) {
        out.push_back(self.frame);
    }
    out.push_back(self.state);
    int lastX = hx;
    int lastY = hy;

    // The rest, nearest first. The game searches windows of growing size (127, 254, ... up to
    // 0xFE0) round the last position written for the first unused record in queue order; a record
    // found only past the first window is written with an absolute position. In one pass: a
    // record's window is the first that holds it (its larger offset / 127, + 1), and the pick the
    // first record in queue order of the smallest window.
    constexpr int kWindow = 0x7F;
    constexpr int kWindows = 0xFE0 / kWindow;
    used.assign(others.size(), false);
    for (;;) {
        std::size_t pick = others.size();
        int best = kWindows + 1;
        for (std::size_t i = 0; i < others.size() && best > 1; ++i) {
            if (used[i]) {
                continue;
            }
            const int far = std::max(std::abs(others[i].x - lastX), std::abs(others[i].y - lastY));
            if (const int window = far / kWindow + 1; window < best) {
                best = window;
                pick = i;
            }
        }
        if (pick == others.size()) {
            break; // (none left, or all over 0xFE0 away: the game leaves those for later)
        }
        const bool absolute = best > 1;
        const UpdateRecord& r = others[pick];
        // The record writer's room check, with the end marker after it.
        const std::size_t need = (absolute ? 10 : 8) + 3;
        if (out.size() - start + need > budget) {
            break;
        }
        if (absolute && (r.x > 6000 || r.y > 6000)) {
            used[pick] = true; // (the writer refuses it)
            continue;
        }
        used[pick] = true;
        written[pick] = true;
        // A 0 byte before the id marks an absolute position.
        if (absolute) {
            out.push_back(0);
        }
        id(r);
        if (absolute) {
            u16(r.x);
            u16(r.y);
        } else {
            // Relative positions are the low bytes' difference; the client reads them back as signed bytes.
            out.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(r.x) - static_cast<std::uint8_t>(lastX)));
            out.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(r.y) - static_cast<std::uint8_t>(lastY)));
        }
        lastX = r.x;
        lastY = r.y;
        animation(r);
    }
    // 00 00 00 ends the stream.
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
}

} // namespace nox::server
