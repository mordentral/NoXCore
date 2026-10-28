// The scripts' quest variables (SetQuestStatus, SetQuestStatusFloat, GetQuestStatus,
// GetQuestStatusFloat, ResetQuestStatus; a solo .plr's GameData keeps them): World::questVariables,
// one list for the whole game, kept from map to map, newest first.
//
// GAME.EXE (journalQuestSet, journalQuestSetBool, scriptGetJournal and the reset's walk): a name
// with a ':' is the variable's whole name; one without is scoped to the map, "<map>:<name>" (the
// map's name as the map load set it: World::mapName). Names are found ignoring case. A variable
// keeps the type it was made with (0 by SetQuestStatus, 1 by SetQuestStatusFloat) whichever sets it
// later; its value is a word either reads.
#pragma once

#include "noxcore/server/world.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace nox::server {

/// The variable's whole name: `name` itself if it has a ':', else "<map>:<name>".
std::string questKey(const World& world, std::string_view name);
/// The variable of that name (scoped as questKey; case ignored), or null.
World::QuestVariable* findQuestVariable(World& world, std::string_view name);
/// journalQuestSet (type 0) / journalQuestSetBool (type 1): its value set, or a new one of that
/// type at the list's head.
void setQuestVariable(World& world, std::string_view name, std::uint32_t type, std::uint32_t value);
/// Its value (0, the float 0, when there's none).
std::uint32_t questVariableValue(World& world, std::string_view name);
/// ResetQuestStatus: the variable gone, or with a '*' in the scoped name every one matching:
/// "*:*" all; "Prefix*" (a '*' last) those starting so (case ignored); "*Suffix" (a '*' first) those
/// whose name, from Suffix's first appearance (case kept), ends there; any other '*' the
/// "Scope:*Suffix" form: those starting with what's up to the first ':' (case ignored) whose name,
/// two letters past that ':', has Suffix (what follows the letter after the ':') at its end, by its
/// first appearance (an original bug: the '*' is taken to stand right after the ':').
void resetQuestVariables(World& world, std::string_view name);

} // namespace nox::server
