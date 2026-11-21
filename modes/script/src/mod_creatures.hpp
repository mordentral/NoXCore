// A creature's numbers a mode sets where no stock builtin reaches them (internal to the script modes'
// sources): its own monster.bin record in place of its type's (nox.setCreatureProfile, read back by
// nox.creatureProfile), its speed and mass, its immunities, its sound set, an NPC's colours, a spell
// cast at a point as its AI casts one, its spellbook and casting rhythm, and an item its holder can't
// let go of. Each is a field of the object or its AI the engine already has and reads as it always
// does; nothing is asked of the mode, and with no mode using them nothing changes.
#pragma once

struct lua_State;

namespace nox::modes::script {

/// nox.setCreatureProfile, nox.creatureProfile, nox.speed, nox.setSpeed, nox.setMass, nox.immune,
/// nox.setSoundSet, nox.setNpcColors, nox.creatureCast, nox.setCaster and nox.setDroppable set in the
/// `nox` table on top of L's stack.
void addCreaturesToolbox(lua_State* L);

} // namespace nox::modes::script
