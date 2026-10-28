// Encoding and decoding of the control entries inside MSG_PLAYER_INPUT (see player_input.hpp).
#include "noxcore/net/player_input.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"

#include <array>
#include <string>

namespace nox::net {

namespace {

constexpr std::array<std::string_view, kControlCodeCount> kNames = {
    "Null", "Orientation", "MoveForward", "MoveBackward", "MoveLeft", "MoveRight", "Action", "Jump", "Chat",
    "TeamChat", "ReadSpellbook", "ToggleConsole", "IncreaseWindowSize", "DecreaseWindowSize", "Quit", "QuitMenu",
    "ReadMap", "Inventory", "SpellGestureUp", "SpellGestureDown", "SpellGestureLeft", "SpellGestureRight",
    "SpellGestureUpperRight", "SpellGestureUpperLeft", "SpellGestureLowerRight", "SpellGestureLowerLeft",
    "SpellPatternEnd", "CastQueuedSpell", "CastMostRecentSpell", "CastSpell1", "CastSpell2", "CastSpell3",
    "CastSpell4", "CastSpell5", "MapZoomIn", "MapZoomOut", "NextWeapon", "QuickHealthPotion", "QuickManaPotion",
    "QuickCurePoisonPotion", "NextSpellSet", "PreviousSpellSet", "SelectSpellSet", "BuildTrap", "ServerOptions",
    "Taunt", "Laugh", "Point", "InvertSpellTarget", "ToggleRank", "ToggleNetstat", "ToggleGUI", "AutoSave",
    "AutoLoad", "ScreenShot",
};

bool known(ControlCode code)
{
    return static_cast<std::uint8_t>(code) < kControlCodeCount;
}

} // namespace

std::string_view controlName(ControlCode code) noexcept
{
    return known(code) ? kNames[static_cast<std::uint8_t>(code)] : std::string_view{};
}

std::size_t controlPayloadSize(ControlCode code) noexcept
{
    switch (code) {
    case ControlCode::Orientation:
    case ControlCode::MoveForward:
    case ControlCode::MoveBackward:
    case ControlCode::MoveLeft:
    case ControlCode::MoveRight:
    case ControlCode::SpellPatternEnd:
        return 1;
    case ControlCode::CastQueuedSpell:
    case ControlCode::CastMostRecentSpell:
        return 4;
    default:
        return 0;
    }
}

std::vector<std::uint8_t> encodeControls(std::span<const ControlEntry> entries)
{
    ByteWriter w;
    for (const auto& e : entries) {
        if (!known(e.code)) {
            throw FormatError("player input: unknown control code " + std::to_string(static_cast<int>(e.code)));
        }
        // Each entry starts with the control code as a u32, then its payload (controlPayloadSize).
        w.u32(static_cast<std::uint8_t>(e.code));
        switch (controlPayloadSize(e.code)) {
        case 1:
            w.u8(static_cast<std::uint8_t>(e.data));
            break;
        case 4:
            w.u32(e.data);
            break;
        default:
            break;
        }
    }
    // The message's length byte is a u8, so all entries must fit in 255 bytes.
    if (w.size() > 0xFF) {
        throw FormatError("player input: entries don't fit in one message");
    }
    return std::move(w).take();
}

std::vector<ControlEntry> decodeControls(std::span<const std::uint8_t> entries)
{
    std::vector<ControlEntry> out;
    decodeControls(entries, out);
    return out;
}

void decodeControls(std::span<const std::uint8_t> entries, std::vector<ControlEntry>& out)
{
    ByteReader r(entries, "player input");
    out.clear();
    while (!r.atEnd()) {
        ControlEntry e;
        // Only the low byte of the u32 code is used.
        e.code = static_cast<ControlCode>(r.u32() & 0xFF);
        if (!known(e.code)) {
            r.fail("unknown control code " + std::to_string(static_cast<int>(e.code)));
        }
        switch (controlPayloadSize(e.code)) {
        case 1:
            e.data = r.u8();
            break;
        case 4:
            e.data = r.u32();
            break;
        default:
            break;
        }
        out.push_back(e);
    }
}

} // namespace nox::net
