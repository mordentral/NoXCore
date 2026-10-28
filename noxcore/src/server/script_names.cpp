// The script builtins' name tables (script_names.hpp).
#include "noxcore/server/script_names.hpp"

#include "noxcore/net/messages.hpp"
#include "noxcore/util/text.hpp"

#include <array>
#include <string>
#include <unordered_map>

namespace nox::server {

std::string_view effectKindName(EffectKind kind)
{
    switch (kind) {
    case EffectKind::Point: return "point";
    case EffectKind::Ray: return "ray";
    case EffectKind::Burst: return "burst";
    case EffectKind::Quake: return "screen shake";
    case EffectKind::None: break;
    }
    return "none";
}

EffectKind scriptEffectKind(std::uint8_t op)
{
    switch (op) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A:
    case 0x8B: case 0x96: case 0x9A:
        return EffectKind::Point;
    case 0x8C: case 0x8F: case 0x90: case 0x91: case 0x94: case 0x95:
        return EffectKind::Ray;
    case 0x93:
        return EffectKind::Burst;
    case 0x97:
        return EffectKind::Quake;
    // 0x98, the green bolt: sent from four ints never set (an original bug); not sent here.
    default:
        return EffectKind::None;
    }
}

const std::vector<ScriptEffect>& scriptEffects()
{
    static const std::vector<ScriptEffect> list = [] {
        constexpr std::string_view kPrefix = "MSG_FX_";
        std::vector<ScriptEffect> out;
        for (int op = kFirstScriptEffectOp; op <= kLastScriptEffectOp; ++op) {
            std::string_view name = net::opInfo(static_cast<std::uint8_t>(op)).name;
            if (!name.starts_with(kPrefix)) {
                continue;
            }
            name.remove_prefix(kPrefix.size());
            out.push_back({name, static_cast<std::uint8_t>(op), scriptEffectKind(static_cast<std::uint8_t>(op))});
        }
        return out;
    }();
    return list;
}

std::uint8_t scriptEffectOp(std::string_view name)
{
    // (The ops' names are the code's: indexed once, by what follows "MSG_FX_", the first op of a
    // name kept.)
    static const auto kByName = [] {
        constexpr std::string_view kPrefix = "MSG_FX_";
        std::unordered_map<std::string, std::uint8_t, util::NoCaseHash, util::NoCaseEqual> byName;
        for (int op = kFirstScriptEffectOp; op <= kLastScriptEffectOp; ++op) {
            const std::string_view have = net::opInfo(static_cast<std::uint8_t>(op)).name;
            if (have.size() >= kPrefix.size() && util::equalsNoCase(have.substr(0, kPrefix.size()), kPrefix)) {
                byName.try_emplace(std::string(have.substr(kPrefix.size())), static_cast<std::uint8_t>(op));
            }
        }
        return byName;
    }();
    const auto it = kByName.find(name);
    return it == kByName.end() ? 0 : it->second;
}

namespace {

// The table: {name, type} pairs to a null name (the buttons: trade_screens.cpp's dialog window,
// the answer: dialog.cpp's dialogAnswer).
constexpr ScriptDialogType kDialogTypes[] = {
    {"NORMAL", 0, "Replay and Done buttons"},
    {"YESNO", 1, "Yes and No buttons; GetAnswer gives the answer"},
    {"NEXT", 2, "Replay and Next buttons"},
    {"YESNONEXT", 3, "Yes, No and Next buttons; GetAnswer gives 0"},
};

} // namespace

std::span<const ScriptSubclassTable> scriptSubclassTables()
{
    // (thing.bin's tables, in the game's order, each with the class it's for)
    using things::kSubclassTables;
    static const ScriptSubclassTable tables[] = {
        {"armour", kSubclassTables[0]},  {"weapon", kSubclassTables[1]},    {"monster", kSubclassTables[2]},
        {"food", kSubclassTables[3]},    {"other", kSubclassTables[4]},     {"book", kSubclassTables[5]},
        {"missile", kSubclassTables[6]}, {"generator", kSubclassTables[7]}, {"exit", kSubclassTables[8]},
    };
    return tables;
}

std::span<const ScriptDialogType> scriptDialogTypes()
{
    return kDialogTypes;
}

} // namespace nox::server
