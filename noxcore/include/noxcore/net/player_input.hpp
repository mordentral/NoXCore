// MSG_PLAYER_INPUT (0x3F) entries: the client's control events.
//
// After `u8 size`, the message holds `size` bytes of entries. Each entry is the control code as
// a u32 (the server reads only the low byte) followed by its payload, whose size comes from the
// exe's control table at ctrlevent_code_infos ({name, hasData, size} per code; read by
// SrvNet_DecodePlayerInputEntries). The server appends the decoded batch to the
// player's 128-entry control buffer, dropping it whole if it would overflow.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace nox::net {

/// The 1.2b control codes (names from the exe's table; there are no gamma codes in 1.2b).
enum class ControlCode : std::uint8_t {
    Null = 0,
    Orientation = 1,
    MoveForward = 2,
    MoveBackward = 3,
    MoveLeft = 4,
    MoveRight = 5,
    Action = 6,
    Jump = 7,
    Chat = 8,
    TeamChat = 9,
    ReadSpellbook = 10,
    ToggleConsole = 11,
    IncreaseWindowSize = 12,
    DecreaseWindowSize = 13,
    Quit = 14,
    QuitMenu = 15,
    ReadMap = 16,
    Inventory = 17,
    SpellGestureUp = 18,
    SpellGestureDown = 19,
    SpellGestureLeft = 20,
    SpellGestureRight = 21,
    SpellGestureUpperRight = 22,
    SpellGestureUpperLeft = 23,
    SpellGestureLowerRight = 24,
    SpellGestureLowerLeft = 25,
    SpellPatternEnd = 26,
    CastQueuedSpell = 27,
    CastMostRecentSpell = 28,
    CastSpell1 = 29,
    CastSpell2 = 30,
    CastSpell3 = 31,
    CastSpell4 = 32,
    CastSpell5 = 33,
    MapZoomIn = 34,
    MapZoomOut = 35,
    NextWeapon = 36,
    QuickHealthPotion = 37,
    QuickManaPotion = 38,
    QuickCurePoisonPotion = 39,
    NextSpellSet = 40,
    PreviousSpellSet = 41,
    SelectSpellSet = 42,
    BuildTrap = 43,
    ServerOptions = 44,
    Taunt = 45,
    Laugh = 46,
    Point = 47,
    InvertSpellTarget = 48,
    ToggleRank = 49,
    ToggleNetstat = 50,
    ToggleGUI = 51,
    AutoSave = 52,
    AutoLoad = 53,
    ScreenShot = 54,
};

inline constexpr std::uint8_t kControlCodeCount = 55;

/// The exe's name without the "CC_" prefix, or "" for codes past the table.
std::string_view controlName(ControlCode code) noexcept;

/// Payload bytes after the 4-byte code: 1 for Orientation, the moves and SpellPatternEnd; 4 for
/// the two spell casts; 0 otherwise.
std::size_t controlPayloadSize(ControlCode code) noexcept;

struct ControlEntry {
    ControlCode code = ControlCode::Null;
    std::uint32_t data = 0; ///< the payload, zero-extended (orientation, spell id, ...)

    bool operator==(const ControlEntry&) const = default;
};

/// The entry bytes (the part after `u8 size`). Throws FormatError for codes past the table or if
/// they don't fit in the u8 size.
std::vector<std::uint8_t> encodeControls(std::span<const ControlEntry> entries);

/// Parses the entry bytes. Throws FormatError for unknown codes or a truncated entry.
std::vector<ControlEntry> decodeControls(std::span<const std::uint8_t> entries);
/// The same into `out` (emptied first: the caller's, kept to save its allocations).
void decodeControls(std::span<const std::uint8_t> entries, std::vector<ControlEntry>& out);

} // namespace nox::net
