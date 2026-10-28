// The names the map script's builtins take as strings and look up in tables of their own, kept in
// one place so the script host and the editor's script tools (script/arg_values.hpp: autocomplete,
// Function Info's values and the lint) read the same lists:
//   - Effect's effect names: the MSG_FX_ messages (net/messages.hpp's names, "MSG_FX_" left off)
//     that ScriptHost::effect sends, each with how it is sent (a point, a ray, a burst, a quake);
//   - HasClass's CLASS names (by bit) and HasSubclass's SUBCLASS tables, searched in order;
//   - SetDialog's dialog types.
// (The enchantments' names are enchant.hpp's kEnchantNames; the spells' are things/spell_ids.hpp's.)
#pragma once

#include "noxcore/things/object_class.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace nox::server {

/// How Effect sends an effect message (none: Effect sends nothing for it).
enum class EffectKind : std::uint8_t {
    None,
    Point, ///< at (x1, y1) (netSendPointFx)
    Ray,   ///< from (x1, y1) to (x2, y2), whole units (netSendRayFx)
    Burst, ///< a spark burst at (x1, y1), x2 its size (netSparkExplosionFx)
    Quake, ///< a screen shake round (x1, y1), x2 its strength (earthquakeSend)
};
std::string_view effectKindName(EffectKind kind);

/// The range of messages Effect looks a name up in ("MSG_FX_" + name, any case).
inline constexpr std::uint8_t kFirstScriptEffectOp = 0x7E;
inline constexpr std::uint8_t kLastScriptEffectOp = 0x9A;
/// How Effect sends message `op` (None for one it doesn't: SUMMON, SHIELD, ... and GREEN_BOLT,
/// whose coordinates the original never sets, OB-055).
EffectKind scriptEffectKind(std::uint8_t op);

struct ScriptEffect {
    std::string_view name; ///< as a script writes it ("SMOKE_BLAST")
    std::uint8_t op = 0;
    EffectKind kind = EffectKind::None;
};
/// Every name Effect looks up (the messages kFirstScriptEffectOp-kLastScriptEffectOp), in op
/// order, those it sends nothing for too (kind None).
const std::vector<ScriptEffect>& scriptEffects();
/// The message Effect sends for `name`: the first in the range named "MSG_FX_" + name (any
/// case), 0 for none. (By an index of the ops' names, made once.)
std::uint8_t scriptEffectOp(std::string_view name);

/// HasClass's table: the CLASS names by bit (thing.bin's), compared exactly (strcmp).
inline constexpr const auto& kScriptClassNames = things::kClassNames;

/// One of HasSubclass's tables: the SUBCLASS names by bit of one class's subclasses (thing.bin's).
struct ScriptSubclassTable {
    std::string_view what; ///< "armour", "weapon"... (the class whose subclasses they are)
    std::span<const std::string_view> names;
};
/// HasSubclass's tables in the order it searches them (the first naming the name wins).
std::span<const ScriptSubclassTable> scriptSubclassTables();

/// SetDialog's dialog types, by name (case counts; an unknown name is NORMAL's 0).
struct ScriptDialogType {
    std::string_view name;
    std::uint8_t type = 0;
    std::string_view note; ///< the buttons the dialog window shows
};
std::span<const ScriptDialogType> scriptDialogTypes();

} // namespace nox::server
