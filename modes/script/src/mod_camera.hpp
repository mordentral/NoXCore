// nox.observe and nox.camera: a player put out of play watching, held out by the mode (its Action
// never brings it in: PlayerControl::heldOut, as a held-out stock player's, with modHeld telling
// the mode's hold from the host's), and its camera placed
// on a point or held on a thing by the mode (PlayerControl::modCamera: the observer's own
// MoveForward and Jump leave it alone, and a later goObserver neither tells it "observation mode"
// nor moves it). The camera is the server's (camX / camY): the update stream is sent about it and
// sounds are heard from it, so no client change is needed. The hold ends with nox.observe(unit,
// false) or nox.backIntoPlay (back into play), nox.camera(unit, nil) (the camera alone: the stock
// observer's controls again, still held out), any path that brings the player out of watching (the
// camera's and the mode's hold out; a held-out stock player's stays the host's), its leaving (the unit goes) and the mode's end (every player's let go: detach). A map's
// change keeps it, as it keeps a watching player watching. (Internal to the script modes' sources.)
#pragma once

#include <cstdint>

struct lua_State;

namespace nox::server {
class GameHost;
struct Fight;
struct ServerObject;
} // namespace nox::server

namespace nox::modes::script {

/// A watching player brought back into play (nox.backIntoPlay, nox.observe(unit, false)): the
/// mode's hold let go, its team, out of watching, its camera its own. Never a held-out stock
/// player's (the caller refuses one).
void bringIntoPlay(server::Fight& fight, server::ServerObject& unit);

/// A player's client ready (its join, or a map's load): one the mode holds watching made unseen and
/// out of reach again (a ready's invulnerability ends an observer's invisibility).
void cameraReady(server::GameHost& host, std::uint8_t peer);

/// The mode's end: every player's hold let go (modCamera cleared, and heldOut where the mode set it:
/// modHeld; a held-out stock player's kept, the host's own), each left watching, free to join.
void releaseCameras(server::GameHost& host);

/// nox.observe and nox.camera set in the `nox` table on top of L's stack.
void addCameraToolbox(lua_State* L);

} // namespace nox::modes::script
