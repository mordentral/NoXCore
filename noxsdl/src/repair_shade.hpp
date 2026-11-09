// Which inventory cells a shop's repair mode darkens, as the game's tray does: every item with
// nothing to mend, at half brightness over its cell, picture and all (the stack's count and the
// charges are drawn over it, unshaded). Something to mend is wear (its health short of its most, a
// most of 0 being indestructible) or a wand short of charges (its most not 0). The server's rule
// (needsRepair) asks a staff subclass of the wand too; the tray doesn't.
#pragma once

#include <cstdint>

namespace noxsdl {

/// Whether the repair mode darkens an item: whole (or indestructible) and, a wand, fully charged.
inline bool repairShaded(std::uint16_t health, std::uint16_t maxHealth, std::uint8_t charges, std::uint8_t maxCharges,
                         bool wand)
{
    const bool charge = wand && charges < maxCharges && maxCharges != 0;
    return (health == maxHealth || maxHealth == 0) && !charge;
}

} // namespace noxsdl
