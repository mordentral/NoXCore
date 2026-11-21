// A thing's own fields for a mode's script: nox.flags (an object's flags, read), nox.setFlag (a few
// of them: SHORT, NO_COLLIDE, NO_PUSH_CHARACTERS, DEAD on what isn't a creature or a player),
// nox.monsterStatus (a creature's status bits, read; its run bits set), nox.cursor (a player's
// cursor in the world, read) and nox.setWallDebris (a WALL type's debris for the map's world alone:
// World::setWallDebris). Fields only: nothing is hooked, and nothing changes while no mode calls
// them. (Internal to the script modes' sources.)
#pragma once

struct lua_State;

namespace nox::modes::script {

/// nox.flags, nox.setFlag, nox.monsterStatus, nox.cursor and nox.setWallDebris set in the `nox`
/// table on top of L's stack.
void addFieldsToolbox(lua_State* L);

} // namespace nox::modes::script
