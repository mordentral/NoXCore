// The modes a process knows (mode.hpp). The process's own (registry()) is filled at start-up by
// the modes built in (modes/: registerBuiltinModes), before any game is hosted; a test may keep a
// registry of its own.
#pragma once

#include "noxcore/modes/mode.hpp"

#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace nox::modes {

class Registry {
public:
    /// Adds a mode. Throws std::invalid_argument for an empty id, an id already there, an opcode
    /// another mode has, or one outside 0xF4-0xF7 (the opcodes the receive switches give modes).
    void add(Mode mode);
    /// Takes a mode out (case aside); whether it was there. Its Mode stays alive (a pointer find
    /// or all gave out stays good), only no longer listed: the menus between games, reading the
    /// mods folder again.
    bool remove(std::string_view id);
    /// Every mode, by its order (then its id).
    std::vector<const Mode*> all() const;
    /// A mode by its id (case aside); null: none.
    const Mode* find(std::string_view id) const;
    /// The mode whose messages use `opcode`; null: none.
    const Mode* byOpcode(std::uint8_t opcode) const;
    /// Whether any mode calls the map ("EXM_1": a maps folder's entry, as given) one of its
    /// generated ones. (Read where map lists are made, never in the frame.)
    bool ownsMap(std::string_view mapName) const;
    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Mode>> modes_; ///< (kept sorted by order, then id)
    std::vector<std::unique_ptr<Mode>> removed_; ///< (remove's: kept for the pointers given out)
};

/// The process's registry.
Registry& registry();

} // namespace nox::modes
