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

/// (The one rule for "is it a mod's campaign") The installed campaign mod `id` names (an id from
/// anywhere, a host's message too: it must be exactly a plain mode id, 1-24 of a-z, 0-9 and _, of a
/// mod in `registry` that declares a campaign and has a folder; another case is none); null: none.
/// Every place that asks, the menus, the save folders, hosting, a guest's side, asks this.
const Mode* installedCampaign(const Registry& registry, std::string_view id);
/// The same rule for a mode in hand (the mode lists): a campaign, a folder, a plain id.
bool isInstalledCampaign(const Mode& mode);

/// (The one place that says where) Where campaign mod `mod`'s players' saves go: its own folder's
/// saves\ (kModSavesFolder), made as it's asked for. Empty when it has no folder, declares no
/// campaign, or the folder can't be made or written (a read-only install): the caller then keeps
/// them where it did before. Its player saves go with the mod's folder: deleting or replacing that
/// folder deletes them.
std::filesystem::path campaignSavesFolder(const Mode& mod);

/// (The one place that says which) The game data files a game of `mode` hosted with `options`
/// reads in place of the install's: the mode's own (Mode::data), else, for a mode that runs
/// campaigns, the chosen campaign mod's (its option "campaign": installedCampaign); null: the
/// install's files, as every other game.
std::shared_ptr<const server::ModeData> gameDataFor(const Registry& registry, const Mode& mode, const OptionValues& options);

} // namespace nox::modes
