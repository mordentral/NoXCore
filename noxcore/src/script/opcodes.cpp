// NoxScript opcode and builtin names, a disassembler and a code checker (tools and diagnostics;
// the interpreter itself is vm.cpp).
#include "noxcore/script/opcodes.hpp"

#include <array>
#include <bit>
#include <cstdio>

namespace nox::script {

namespace {

// Indexed by opcode (Op).
constexpr std::array<std::string_view, kOpCount> kOpNames{
    "LoadVarInt",    "LoadVarFloat",   "LoadVarPtr",     "LoadVarString",  "PushInt",       "PushFloat",
    "PushString",    "IntAdd",         "FloatAdd",       "IntSub",         "FloatSub",      "IntMul",
    "FloatMul",      "IntDiv",         "FloatDiv",       "IntMod",         "IntAnd",        "IntOr",
    "IntXor",        "Jump",           "JumpIf",         "JumpIfNot",      "StoreInt",      "StoreFloat",
    "StoreString",   "StoreIntMul",    "StoreFloatMul",  "StoreIntDiv",    "StoreFloatDiv", "StoreIntAdd",
    "StoreFloatAdd", "StoreStringAdd", "StoreIntSub",    "StoreFloatSub",  "StoreIntMod",   "IntEq",
    "FloatEq",       "StringEq",       "IntLsh",         "IntRsh",         "IntLt",         "FloatLt",
    "StringLt",      "IntGt",          "FloatGt",        "StringGt",       "IntLte",        "FloatLte",
    "StringLte",     "IntGte",         "FloatGte",       "StringGte",      "IntNeq",        "FloatNeq",
    "StringNeq",     "BoolAnd",        "BoolOr",         "StoreIntLsh",    "StoreIntRsh",   "StoreIntAnd",
    "StoreIntOr",    "StoreIntXor",    "BoolNot",        "IntNot",         "IntNeg",        "FloatNeg",
    "IndexInt",      "IndexFloat",     "IndexPtr",       "CallBuiltin",    "CallScript",    "Return0",
    "Return",        "StringAdd",
};

// noxworld-dev/noxscript ns/asm builtins.go; checked against the exe's table where it
// names the function (SetQuestStatus 119, WideScreen 137, SetRoamFlag 153, JournalDelete 158,
// RetreatLevel 160, GiveXp 172, IsTalking 177, MakeFriendly..BecomeEnemy 180-183, SetHalberd 186,
// IsTrading 193).
// Indexed by builtin id (CallBuiltin's operand).
constexpr std::array<std::string_view, kBuiltinCount> kBuiltinNames{
    "Wall", "WallOpen", "WallGroupOpen", "WallClose", "WallGroupClose", "WallToggle", "WallGroupToggle",
    "WallBreak", "WallGroupBreak", "SecondTimer", "FrameTimer", "Move", "GroupMove", "LookAtDirection",
    "GroupLookAtDirection", "ObjectOn", "ObjectGroupOn", "WaypointOn", "WaypointGroupOn", "ObjectOff",
    "ObjectGroupOff", "WaypointOff", "WaypointGroupOff", "ObjectToggle", "ObjectGroupToggle",
    "WaypointToggle", "WaypointGroupToggle", "Delete", "GroupDelete", "Wander", "GroupWander", "Unused1f",
    "Unused20", "GoBackHome", "AudioEvent", "Print", "PrintToAll", "Chat", "StopScript", "UnlockDoor",
    "LockDoor", "IsObjectOn", "IsWaypointOn", "IsLocked", "RandomFloat", "Random", "SecondTimerWithArg",
    "FrameTimerWithArg", "IntToString", "FloatToString", "CreateObject", "Damage", "GroupDamage",
    "CreateMover", "GroupCreateMover", "AwardSpell", "GroupAwardSpell", "Enchant", "GroupEnchant",
    "GetHost", "Object", "GetObjectX", "GetWaypointX", "GetObjectY", "GetWaypointY", "GetObjectZ",
    "GetDirection", "MoveObject", "MoveWaypoint", "Raise", "LookWithAngle", "PushObjectTo", "PushObject",
    "GetLastItem", "GetPreviousItem", "HasItem", "GetHolder", "Pickup", "Drop", "HasClass", "Unused50",
    "HasEnchant", "EnchantOff", "CurrentHealth", "MaxHealth", "RestoreHealth", "Distance", "IsVisibleTo",
    "Unused58", "Unused59", "Unused5a", "Unused5b", "Unused5c", "Unused5d", "Unused5e",
    "GetCharacterData", "LookAtObject", "Walk", "GroupWalk", "CancelTimer", "Effect", "SetOwner",
    "GroupSetOwner", "SetOwners", "GroupSetOwners", "IsOwnedBy", "GroupIsOwnedBy", "IsOwnedByAny",
    "GroupIsOwnedByAny", "ClearOwner", "Waypoint", "WaypointGroup", "ObjectGroup", "WallGroup",
    "ChatTimerSeconds", "ChatTimer", "Unused74", "DestroyChat", "DestroyEveryChat", "SetQuestStatus",
    "SetQuestStatusFloat", "GetQuestStatus", "GetQuestStatusFloat", "ResetQuestStatus", "IsTrigger",
    "IsCaller", "SetDialog", "CancelDialog", "StoryPic", "TellStory", "StartDialog",
    "CastSpellObjectObject", "CastSpellObjectLocation", "CastSpellLocationObject",
    "CastSpellLocationLocation", "UnBlind", "Blind", "WideScreen", "GetElevatorStatus", "CreatureGuard",
    "CreatureGroupGuard", "CreatureHunt", "CreatureGroupHunt", "CreatureIdle", "CreatureGroupIdle",
    "CreatureFollow", "CreatureGroupFollow", "AggressionLevel", "GroupAggressionLevel", "HitLocation",
    "GroupHitLocation", "HitFarLocation", "GroupHitFarLocation", "SetRoamFlag", "GroupSetRoamFlag",
    "Attack", "GroupAttack", "JournalEntry", "JournalDelete", "JournalEdit", "RetreatLevel",
    "GroupRetreatLevel", "ResumeLevel", "GroupResumeLevel", "RunAway", "GroupRunAway", "PauseObject",
    "GroupPauseObject", "IsAttackedBy", "GetGold", "ChangeGold", "GetAnswer", "GiveXp", "HasSubclass",
    "AutoSave", "Music", "StartupScreen", "IsTalking", "GetTrigger", "GetCaller", "MakeFriendly",
    "MakeEnemy", "BecomePet", "BecomeEnemy", "IsFriendly", "IsPet", "SetHalberd", "DeathScreen",
    "Frozen", "NoWallSound", "SetCallback", "DeleteObjectTimer", "TrapSpells", "IsTrading",
    "ClearMessages", "SetShopkeeperText", "ImmediateUnBlind", "IsSummoned", "ZombieStayDown",
    "ZombieGroupStayDown", "RaiseZombie", "RaiseZombieGroup", "MusicPushEvent", "MusicPopEvent",
    "MusicEvent", "IsGameBall", "IsCrown", "EndGame", "ImmediateBlind", "ChangeScore", "GetScore",
};

// A string as a C-style literal for the disassembly.
std::string quoted(std::string_view s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

} // namespace

std::optional<std::string_view> opName(std::uint32_t op)
{
    if (op >= kOpCount) {
        return std::nullopt;
    }
    return kOpNames[op];
}

std::uint32_t operandCount(std::uint32_t op)
{
    switch (static_cast<Op>(op)) {
    case Op::LoadVarInt:
    case Op::LoadVarFloat:
    case Op::LoadVarPtr:
    case Op::LoadVarString:
        return 2;
    case Op::PushInt:
    case Op::PushFloat:
    case Op::PushString:
    case Op::Jump:
    case Op::JumpIf:
    case Op::JumpIfNot:
    case Op::CallBuiltin:
    case Op::CallScript:
        return 1;
    default:
        return 0;
    }
}

std::string builtinName(std::uint32_t id)
{
    return id < kBuiltinCount ? std::string(kBuiltinNames[id]) : "Builtin" + std::to_string(id);
}

std::string disassemble(const ScriptFile& file, std::size_t function)
{
    const Function& fn = file.functions.at(function);
    std::string out;
    std::size_t at = 0;
    char line[64];
    // One instruction a line: its word index, name and operands.
    while (at < fn.code.size()) {
        const std::uint32_t op = fn.code[at];
        const std::uint32_t n = operandCount(op);
        std::snprintf(line, sizeof line, "%6zu  ", at);
        out += line;
        if (n > 0 && at + n >= fn.code.size()) {
            out += "(cut short)\n";
            break;
        }
        const auto name = opName(op);
        out += name ? std::string(*name) : "Op" + std::to_string(op);
        const auto arg = [&](std::size_t i) { return fn.code[at + 1 + i]; };
        switch (static_cast<Op>(op)) {
        case Op::LoadVarInt:
        case Op::LoadVarFloat:
        case Op::LoadVarPtr:
        case Op::LoadVarString: {
            // LoadVar's operands: the global flag, then the symbol index (negative: an object variable).
            const auto var = static_cast<std::int32_t>(arg(1));
            out += arg(0) ? " global " : var < 0 ? " object " : " local ";
            out += std::to_string(var < 0 && !arg(0) ? -var : var);
            break;
        }
        case Op::PushFloat:
            std::snprintf(line, sizeof line, " %g", static_cast<double>(std::bit_cast<float>(arg(0))));
            out += line;
            break;
        case Op::PushString:
            out += " " + (arg(0) < file.strings.size() ? quoted(file.strings[arg(0)]) : "#" + std::to_string(arg(0)));
            break;
        case Op::CallBuiltin:
            out += " " + builtinName(arg(0));
            break;
        case Op::CallScript:
            out += " " + (arg(0) < file.functions.size() ? file.functions[arg(0)].name : "#" + std::to_string(arg(0)));
            break;
        default:
            if (n == 1) {
                out += " " + std::to_string(static_cast<std::int32_t>(arg(0)));
            }
            break;
        }
        out += '\n';
        at += 1 + n;
    }
    return out;
}

std::optional<std::string> check(const ScriptFile& file, std::size_t function)
{
    const Function& fn = file.functions.at(function);
    const auto where = [&](std::size_t at, const std::string& what) {
        return fn.name + " at " + std::to_string(at) + ": " + what;
    };
    std::size_t at = 0;
    // Walks the instructions as the disassembler does, checking each operand against the file.
    while (at < fn.code.size()) {
        const std::uint32_t op = fn.code[at];
        if (op >= kOpCount) {
            return where(at, "unknown opcode " + std::to_string(op));
        }
        const std::uint32_t n = operandCount(op);
        if (at + n >= fn.code.size() && n > 0) {
            return where(at, "cut short");
        }
        const auto arg = [&](std::size_t i) { return fn.code[at + 1 + i]; };
        switch (static_cast<Op>(op)) {
        case Op::LoadVarInt:
        case Op::LoadVarFloat:
        case Op::LoadVarPtr:
        case Op::LoadVarString: {
            const auto var = static_cast<std::int32_t>(arg(1));
            const Function* owner = arg(0) ? (file.functions.size() > 1 ? &file.functions[1] : nullptr)
                                    : var < 0 ? &file.functions[0]
                                              : &fn;
            const auto index = static_cast<std::size_t>(var < 0 && !arg(0) ? -static_cast<std::int64_t>(var) : var);
            if (!owner || index >= owner->symbolSizes.size()) {
                return where(at, "no variable " + std::to_string(var));
            }
            break;
        }
        case Op::PushString:
            if (arg(0) >= file.strings.size()) {
                return where(at, "no string " + std::to_string(arg(0)));
            }
            break;
        case Op::Jump:
        case Op::JumpIf:
        case Op::JumpIfNot:
            if (arg(0) >= fn.code.size()) {
                return where(at, "a jump to " + std::to_string(arg(0)) + ", outside the function");
            }
            break;
        case Op::CallBuiltin:
            if (arg(0) >= kBuiltinCount) {
                return where(at, "no builtin " + std::to_string(arg(0)));
            }
            break;
        case Op::CallScript:
            if (arg(0) >= file.functions.size()) {
                return where(at, "no function " + std::to_string(arg(0)));
            }
            break;
        default:
            break;
        }
        at += 1 + n;
    }
    return std::nullopt;
}

} // namespace nox::script
