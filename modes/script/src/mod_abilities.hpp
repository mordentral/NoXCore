// A script mode's abilities (nox.defineAbility, nox.grantAbility...): a player's strip of up to
// four buttons beside the quickbar on a NoX Core client of client level 8 that named ModeUi
// (ui_proto.hpp's kAbilityBar, the whole bar sent whenever it changes; nothing to any other). A
// use comes back as kUseAbility (a slot, a press or release, the cursor's world point); the
// server checks it (granted, the player in play and not busy, its cooldown run out, a held one's
// release only while it's held, at most kUsesPerSecond a second) and asks the mode's
// onModAbility(unit, id, x, y, phase), whose false refuses a press; a press let through starts its
// cooldown here, never the client's. A held ability (target "hold") is released by the client's
// key or button, or ended by the server after its `hold` frames ("end"). Kept by peer: a player
// keeps its abilities through a map change, and they go with it when it leaves. (Internal to the
// script modes' sources.)
#pragma once

#include "noxcore/modes/script/ui_proto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace nox::server {
class GameHost;
} // namespace nox::server

namespace nox::modes::script {

class ModAbilities {
public:
    static constexpr std::size_t kMaxDefs = 64;
    static constexpr int kMaxCooldown = 30 * 600; ///< 10 minutes (a u16's frames)
    static constexpr int kMaxHold = 30 * 60;      ///< a minute
    static constexpr int kUsesPerSecond = 10;     ///< a player's uses (presses and releases) a second at most

    struct Def {
        std::string id, title, icon;
        int cooldown = 0; ///< frames
        uimsg::AbilityTarget target = uimsg::AbilityTarget::Point;
        int hold = 60; ///< a held one's most frames
    };
    struct Slot {
        int def = -1; ///< (none)
        int left = 0; ///< cooldown frames left
    };
    struct Player {
        std::array<Slot, uimsg::kAbilitySlots> slots{};
        int holding = -1; ///< the slot held down (a "hold" ability's press let through); -1 none
        int holdLeft = 0;
        bool dirty = false; ///< its bar to be sent
        std::uint32_t usesFrom = 0;
        int uses = 0;
    };

    /// Whether `peer`'s client is sent a bar (and its uses heard): in the game, not held out,
    /// ModeUi named, client level 8 or above.
    static bool reaches(const server::GameHost& host, std::uint8_t peer);

    /// nox.defineAbility's: a new one, or the one of that id changed (its grants' bars sent again).
    /// False: kMaxDefs are defined.
    bool define(Def d);
    /// The def of `id`; -1: none.
    int defIndex(std::string_view id) const;
    const Def& def(int index) const { return defs_[static_cast<std::size_t>(index)]; }

    /// `def` given to `peer`: its slot (0-3, the first free one; the one it's in already); none
    /// when the strip's full.
    std::optional<int> grant(std::uint8_t peer, int def);
    /// `def` taken from `peer` (a hold of it ended with no word): whether it had it.
    bool revoke(std::uint8_t peer, int def);
    /// `peer`'s slot holding `def`; null: not granted.
    Slot* slotOf(std::uint8_t peer, int def);
    Player* player(std::uint8_t peer);

    /// A use heard from `peer` (frame `frame`), checked: the def it names and whether it's a
    /// release; none when it's refused here (not granted, too many, on cooldown, a release of
    /// nothing held). The caller checks the player and asks the mode.
    struct Asked {
        int slot = 0;
        int def = 0;
        bool release = false;
    };
    std::optional<Asked> heard(std::uint8_t peer, const uimsg::AbilityUse& use, std::uint32_t frame);
    /// A press the mode let through: its cooldown starts (its bar sent), a held one held.
    void pressed(std::uint8_t peer, int slot);
    /// A held one let go (its release heard, or ended): nothing held.
    void letGo(std::uint8_t peer);

    /// Once a frame: the cooldowns and holds counted down; the holds that ran out, or whose
    /// player isn't `inPlay` (dead, watching, gone), (peer, def), let go here, for the mode's
    /// "end".
    std::vector<std::pair<std::uint8_t, int>> step(const std::function<bool(std::uint8_t peer)>& inPlay);
    /// The bars changed sent (to players reaches lets).
    void flush(server::GameHost& host);
    /// `peer` gone: forgotten.
    void left(std::uint8_t peer) { players_.erase(peer); }
    /// The mode's end or reload: everything forgotten.
    void reset() { *this = ModAbilities{}; }
    bool active() const noexcept { return !players_.empty(); }

private:
    std::vector<Def> defs_;
    std::map<std::uint8_t, Player> players_;
};

/// nox.defineAbility, nox.grantAbility, nox.revokeAbility, nox.abilityCooldown and
/// nox.setAbilityCooldown set in the `nox` table on top of L's stack.
void addAbilityToolbox(lua_State* L);

} // namespace nox::modes::script
