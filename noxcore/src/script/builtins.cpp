// The builtins' source-language signatures (builtins.hpp). Argument counts and results are the
// server host's table's; the types are each builtin's pops in GAME.EXE's builtins table (int,
// float or string id), with waypoints, groups and walls as `object` and script function indices
// as `function`, checked against every shipped map's calls.
#include "noxcore/script/builtins.hpp"

#include "noxcore/script/opcodes.hpp"

#include <array>
#include <map>

namespace nox::script {

namespace {

constexpr Type V = Type::Void;
constexpr Type I = Type::Int;
constexpr Type F = Type::Float;
constexpr Type S = Type::String;
constexpr Type O = Type::Object;
constexpr Type Fn = Type::Function;

// Indexed by builtin id.
const std::array<BuiltinSignature, kBuiltinCount> kTable{{    {"Wall", O, {I, I}}, // 0
    {"WallOpen", V, {O}}, // 1
    {"WallGroupOpen", V, {O}}, // 2
    {"WallClose", V, {O}}, // 3
    {"WallGroupClose", V, {O}}, // 4
    {"WallToggle", V, {O}}, // 5
    {"WallGroupToggle", V, {O}}, // 6
    {"WallBreak", V, {O}}, // 7
    {"WallGroupBreak", V, {O}}, // 8
    {"SecondTimer", I, {I, Fn}}, // 9
    {"FrameTimer", I, {I, Fn}}, // 10
    {"Move", V, {I, O}}, // 11
    {"GroupMove", V, {O, O}}, // 12
    {"LookAtDirection", V, {I, I}}, // 13
    {"GroupLookAtDirection", V, {O, I}}, // 14
    {"ObjectOn", V, {I}}, // 15
    {"ObjectGroupOn", V, {O}}, // 16
    {"WaypointOn", V, {O}}, // 17
    {"WaypointGroupOn", V, {O}}, // 18
    {"ObjectOff", V, {I}}, // 19
    {"ObjectGroupOff", V, {O}}, // 20
    {"WaypointOff", V, {O}}, // 21
    {"WaypointGroupOff", V, {O}}, // 22
    {"ObjectToggle", V, {I}}, // 23
    {"ObjectGroupToggle", V, {O}}, // 24
    {"WaypointToggle", V, {O}}, // 25
    {"WaypointGroupToggle", V, {O}}, // 26
    {"Delete", V, {I}}, // 27
    {"GroupDelete", V, {O}}, // 28
    {"Wander", V, {I}}, // 29
    {"GroupWander", V, {O}}, // 30
    {"Unused1f", V, {I}}, // 31
    {"Unused20", V, {I}}, // 32
    {"GoBackHome", V, {I}}, // 33
    {"AudioEvent", V, {S, O}}, // 34
    {"Print", V, {S}}, // 35
    {"PrintToAll", V, {S}}, // 36
    {"Chat", V, {I, S}}, // 37
    {"StopScript", V, {}}, // 38
    {"UnlockDoor", V, {I}}, // 39
    {"LockDoor", V, {I}}, // 40
    {"IsObjectOn", I, {I}}, // 41
    {"IsWaypointOn", I, {O}}, // 42
    {"IsLocked", I, {I}}, // 43
    {"RandomFloat", F, {F, F}}, // 44
    {"Random", I, {I, I}}, // 45
    {"SecondTimerWithArg", I, {I, I, Fn}}, // 46
    {"FrameTimerWithArg", I, {I, I, Fn}}, // 47
    {"IntToString", S, {I}}, // 48
    {"FloatToString", S, {F}}, // 49
    {"CreateObject", I, {S, O}}, // 50
    {"Damage", V, {I, I, I, I}}, // 51
    {"GroupDamage", V, {O, I, I, I}}, // 52
    {"CreateMover", I, {I, O, F}}, // 53
    {"GroupCreateMover", V, {O, O, F}}, // 54
    {"AwardSpell", I, {I, S}}, // 55
    {"GroupAwardSpell", V, {O, S}}, // 56
    {"Enchant", V, {I, S, F}}, // 57
    {"GroupEnchant", V, {O, S, F}}, // 58
    {"GetHost", I, {}}, // 59
    {"Object", I, {S}}, // 60
    {"GetObjectX", F, {I}}, // 61
    {"GetWaypointX", F, {O}}, // 62
    {"GetObjectY", F, {I}}, // 63
    {"GetWaypointY", F, {O}}, // 64
    {"GetObjectZ", F, {I}}, // 65
    {"GetDirection", I, {I}}, // 66
    {"MoveObject", V, {I, F, F}}, // 67
    {"MoveWaypoint", V, {O, F, F}}, // 68
    {"Raise", V, {I, F}}, // 69
    {"LookWithAngle", V, {I, I}}, // 70
    {"PushObjectTo", V, {I, F, F}}, // 71
    {"PushObject", V, {I, F, F, F}}, // 72
    {"GetLastItem", I, {I}}, // 73
    {"GetPreviousItem", I, {I}}, // 74
    {"HasItem", I, {I, I}}, // 75
    {"GetHolder", I, {I}}, // 76
    {"Pickup", I, {I, I}}, // 77
    {"Drop", I, {I, I}}, // 78
    {"HasClass", I, {I, S}}, // 79
    {"Unused50", V, {}}, // 80
    {"HasEnchant", I, {I, S}}, // 81
    {"EnchantOff", V, {I, S}}, // 82
    {"CurrentHealth", I, {I}}, // 83
    {"MaxHealth", I, {I}}, // 84
    {"RestoreHealth", V, {I, I}}, // 85
    {"Distance", F, {F, F, F, F}}, // 86
    {"IsVisibleTo", I, {I, I}}, // 87
    {"Unused58", V, {I, I}}, // 88
    {"Unused59", V, {I, I}}, // 89
    {"Unused5a", V, {I, I}}, // 90
    {"Unused5b", V, {I, I}}, // 91
    {"Unused5c", V, {I, I}}, // 92
    {"Unused5d", V, {I, I}}, // 93
    {"Unused5e", I, {S}}, // 94
    {"GetCharacterData", I, {I}}, // 95
    {"LookAtObject", V, {I, I}}, // 96
    {"Walk", V, {I, F, F}}, // 97
    {"GroupWalk", V, {O, F, F}}, // 98
    {"CancelTimer", I, {I}}, // 99
    {"Effect", V, {S, F, F, F, F}}, // 100
    {"SetOwner", V, {I, I}}, // 101
    {"GroupSetOwner", V, {I, O}}, // 102
    {"SetOwners", V, {O, I}}, // 103
    {"GroupSetOwners", V, {O, O}}, // 104
    {"IsOwnedBy", I, {I, I}}, // 105
    {"GroupIsOwnedBy", I, {I, O}}, // 106
    {"IsOwnedByAny", I, {O, I}}, // 107
    {"GroupIsOwnedByAny", I, {O, O}}, // 108
    {"ClearOwner", V, {I}}, // 109
    {"Waypoint", O, {S}}, // 110
    {"WaypointGroup", O, {S}}, // 111
    {"ObjectGroup", O, {S}}, // 112
    {"WallGroup", O, {S}}, // 113
    {"ChatTimerSeconds", V, {I, S, I}}, // 114
    {"ChatTimer", V, {I, S, I}}, // 115
    {"Unused74", V, {I, I}}, // 116
    {"DestroyChat", V, {I}}, // 117
    {"DestroyEveryChat", V, {}}, // 118
    {"SetQuestStatus", V, {I, S}}, // 119
    {"SetQuestStatusFloat", V, {F, S}}, // 120
    {"GetQuestStatus", I, {S}}, // 121
    {"GetQuestStatusFloat", F, {S}}, // 122
    {"ResetQuestStatus", V, {S}}, // 123
    {"IsTrigger", I, {I}}, // 124
    {"IsCaller", I, {I}}, // 125
    {"SetDialog", V, {I, S, Fn, Fn}}, // 126
    {"CancelDialog", V, {I}}, // 127
    {"StoryPic", V, {I, S}}, // 128
    {"TellStory", V, {S, S}}, // 129
    {"StartDialog", V, {I, I}}, // 130
    {"CastSpellObjectObject", V, {S, I, I}}, // 131
    {"CastSpellObjectLocation", V, {S, I, F, F}}, // 132
    {"CastSpellLocationObject", V, {S, F, F, I}}, // 133
    {"CastSpellLocationLocation", V, {S, F, F, F, F}}, // 134
    {"UnBlind", V, {}}, // 135
    {"Blind", V, {}}, // 136
    {"WideScreen", V, {I}}, // 137
    {"GetElevatorStatus", I, {I}}, // 138
    {"CreatureGuard", V, {I, F, F, F, F, F}}, // 139
    {"CreatureGroupGuard", V, {O, F, F, F, F, F}}, // 140
    {"CreatureHunt", V, {I}}, // 141
    {"CreatureGroupHunt", V, {O}}, // 142
    {"CreatureIdle", V, {I}}, // 143
    {"CreatureGroupIdle", V, {O}}, // 144
    {"CreatureFollow", V, {I, I}}, // 145
    {"CreatureGroupFollow", V, {O, I}}, // 146
    {"AggressionLevel", V, {I, F}}, // 147
    {"GroupAggressionLevel", V, {O, F}}, // 148
    {"HitLocation", V, {I, F, F}}, // 149
    {"GroupHitLocation", V, {O, F, F}}, // 150
    {"HitFarLocation", V, {I, F, F}}, // 151
    {"GroupHitFarLocation", V, {O, F, F}}, // 152
    {"SetRoamFlag", V, {I, I}}, // 153
    {"GroupSetRoamFlag", V, {O, I}}, // 154
    {"Attack", V, {I, I}}, // 155
    {"GroupAttack", V, {O, I}}, // 156
    {"JournalEntry", V, {I, S, I}}, // 157
    {"JournalDelete", V, {I, S}}, // 158
    {"JournalEdit", V, {I, S, I}}, // 159
    {"RetreatLevel", V, {I, F}}, // 160
    {"GroupRetreatLevel", V, {O, F}}, // 161
    {"ResumeLevel", V, {I, F}}, // 162
    {"GroupResumeLevel", V, {O, F}}, // 163
    {"RunAway", V, {I, I, I}}, // 164
    {"GroupRunAway", V, {O, I, I}}, // 165
    {"PauseObject", V, {I, I}}, // 166
    {"GroupPauseObject", V, {O, I}}, // 167
    {"IsAttackedBy", I, {I, I}}, // 168
    {"GetGold", I, {I}}, // 169
    {"ChangeGold", V, {I, I}}, // 170
    {"GetAnswer", I, {I}}, // 171
    {"GiveXp", V, {I, F}}, // 172
    {"HasSubclass", I, {I, S}}, // 173
    {"AutoSave", V, {}}, // 174
    {"Music", V, {I, I}}, // 175
    {"StartupScreen", V, {I}}, // 176
    {"IsTalking", I, {}}, // 177
    {"GetTrigger", I, {}}, // 178
    {"GetCaller", I, {}}, // 179
    {"MakeFriendly", V, {I}}, // 180
    {"MakeEnemy", V, {I}}, // 181
    {"BecomePet", V, {I}}, // 182
    {"BecomeEnemy", V, {I}}, // 183
    {"IsFriendly", I, {I}}, // 184 (was Unknownb8: the subclass bit MakeFriendly sets)
    {"IsPet", I, {I}}, // 185 (was Unknownb9: the subclass bit BecomePet sets)
    {"SetHalberd", V, {I}}, // 186
    {"DeathScreen", V, {I}}, // 187
    {"Frozen", V, {I, I}}, // 188
    {"NoWallSound", V, {I}}, // 189
    {"SetCallback", V, {I, I, Fn}}, // 190
    {"DeleteObjectTimer", V, {I, I}}, // 191
    {"TrapSpells", V, {I, S, S, S}}, // 192
    {"IsTrading", I, {}}, // 193
    {"ClearMessages", V, {I}}, // 194
    {"SetShopkeeperText", V, {I, S}}, // 195
    {"ImmediateUnBlind", V, {}}, // 196 (was Unknownc4: ImmediateBlind undone, the game unpaused)
    {"IsSummoned", I, {I}}, // 197
    {"ZombieStayDown", V, {I}}, // 198
    {"ZombieGroupStayDown", V, {O}}, // 199
    {"RaiseZombie", V, {I}}, // 200
    {"RaiseZombieGroup", V, {O}}, // 201
    {"MusicPushEvent", V, {}}, // 202
    {"MusicPopEvent", V, {}}, // 203
    {"MusicEvent", V, {}}, // 204
    {"IsGameBall", I, {I}}, // 205
    {"IsCrown", I, {I}}, // 206
    {"EndGame", V, {I}}, // 207
    {"ImmediateBlind", V, {}}, // 208
    {"ChangeScore", V, {I, I}}, // 209
    {"GetScore", I, {I}}, // 210
}};

} // namespace

std::string_view typeName(Type t)
{
    switch (t) {
    case Type::Void: return "void";
    case Type::Int: return "int";
    case Type::Float: return "float";
    case Type::String: return "string";
    case Type::Object: return "object";
    case Type::Function: return "function";
    }
    return "?";
}

const BuiltinSignature& builtinSignature(std::uint32_t id)
{
    return kTable.at(id);
}

namespace {

// The builtins NoX Core named (from what each does in GAME.EXE) that the community tools call
// UnknownXX: the old spelling stays accepted.
struct Renamed {
    std::uint32_t id;
    std::string_view oldName;
};
constexpr std::array<Renamed, 3> kRenamed{{{184, "Unknownb8"}, {185, "Unknownb9"}, {196, "Unknownc4"}}};

} // namespace

std::optional<std::uint32_t> findBuiltin(std::string_view name)
{
    static const std::map<std::string_view, std::uint32_t, std::less<>> ids = [] {
        std::map<std::string_view, std::uint32_t, std::less<>> m;
        for (std::uint32_t i = 0; i < kBuiltinCount; ++i) {
            m.emplace(kTable[i].name, i);
        }
        for (const Renamed& r : kRenamed) {
            m.emplace(r.oldName, r.id);
        }
        return m;
    }();
    const auto it = ids.find(name);
    return it == ids.end() ? std::nullopt : std::optional<std::uint32_t>(it->second);
}

std::optional<std::string_view> builtinAlias(std::uint32_t id)
{
    for (const Renamed& r : kRenamed) {
        if (r.id == id) {
            return r.oldName;
        }
    }
    return std::nullopt;
}

bool isNewBuiltinName(std::string_view name)
{
    for (const Renamed& r : kRenamed) {
        if (kTable[r.id].name == name) {
            return true;
        }
    }
    return false;
}

std::string builtinPrototypes()
{
    std::string out;
    for (const auto& b : kTable) {
        out += std::string(typeName(b.returns)) + " " + std::string(b.name) + "(";
        for (std::size_t i = 0; i < b.args.size(); ++i) {
            out += (i ? ", " : "") + std::string(typeName(b.args[i]));
        }
        out += ");\n";
    }
    return out;
}

} // namespace nox::script