// A script mode's folder as the host reads it: its mode.json (what the mode is, as the lists see
// it) and where its scripts are.
//
// mode.json (UTF-8):
//   {
//     "api": 1,                          the script API it's written for (kScriptApi)
//     "id": "sample",                    a-z, 0-9 and _, at most 24; the registry's key
//     "name": "Sample", "description": "One line for the list.",
//     "order": 100,                      its place in the list (low first)
//     "players": [1, 16],                the fewest and most players
//     "stockClients": true,              GAME.EXE players can join (false: NoX Core only)
//     "minClientLevel": 5,               the lowest NoX Core client level let in (0, the default:
//                                        any; at most this build's, lobby::kClientLevel)
//     "gameplay": {"teamDamage": false}, the host's team options it pins for the game: any of
//                                        "teamDamage", "autoAssign", "teamPlay" (gameplay flags
//                                        1, 2, 4), each kept as given (none: the host's)
//     "pickup": {"anyClass": true},      players pick up what their class can't use (carried, never
//                                        worn: equipping still asks the class); "duplicates": true
//                                        seconds of what they carry under the weapons-respawn option
//     "levels": "mapFiles",              or "generated"
//     "server": "server/main.luau",      its server script (this is the default)
//     "ui": "ui/main.luau",              its UI script, sent to NoX Core players (none: text only)
//     "uiApi": 1,                        the UI API it's written for (1, 2: images, sounds, keys...,
//                                        or 3: lines, tooltips, labels, HUD windows, drags...;
//                                        script_ui.hpp), sent only to clients that give it
//     "looksLike": "arena",              a stock mode it plays as, its own rules the script's
//                                        ("arena" or "ctf": scripted_mode.hpp)
//     "mapPrefix": "DD",                 its generated maps' (nox.generate): 2-3 of A-Z, no other
//                                        mode's; the maps <prefix>_<slot><TAG>, swept as its own
//     "reference": true,                 a reference for mod authors: the map editor opens it as
//                                        a copy, never in place (the game doesn't read it)
//     "options": [                       its settings, shown under its row in Host Custom:
//       {"key": "rounds", "label": "Rounds", "kind": "number", "min": 1, "max": 9, "default": 3},
//       {"key": "pace", "label": "Pace", "kind": "choice", "choices": ["Calm", "Hectic"], "default": 0},
//       {"key": "fog", "label": "Fog", "kind": "toggle", "default": 0},
//       {"key": "motto", "label": "Motto", "kind": "text"}
//     ]
//   }
#pragma once

#include "noxcore/modes/mode.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nox::modes::script {

/// The script API this build gives (a mode.json's "api"; another's is refused, with its reason).
inline constexpr int kScriptApi = 1;
/// The newest UI API this build gives (a mode.json's "uiApi", 1 to this; script_ui.hpp).
inline constexpr int kUiApi = 4;

struct Manifest {
    ModeInfo info;
    int api = kScriptApi;
    std::string serverScript = "server/main.luau"; ///< (relative to the folder, '/' separated)
    std::string uiScript;                          ///< its UI script (empty: none)
    int uiApi = 1;                                 ///< the UI API its UI is written for
    /// mode.json's "stockClients": false (readManifest's): stock GAME.EXE players are turned away
    /// with a notice (GameHost::Options::stockRefusal). A manifest made in code: never.
    bool refuseStock = false;
    /// mode.json's "minClientLevel": a NoX Core client below it is refused at its join with the
    /// too-old notice (GameHost::Options::minClientLevel, raised to it). 0: any.
    std::uint16_t minClientLevel = 0;
    /// mode.json's "pickup" (GameHost::Options::pickupAnyClass / pickupDuplicates).
    bool pickupAnyClass = false;
    bool pickupDuplicates = false;
    /// mode.json's "gameplay": the gameplay flags it pins (1 team damage, 2 auto-assign, 4 team
    /// play; GameHost::Options::gameplayPinned) and their values (of those bits). 0: none.
    std::uint32_t gameplayPinned = 0;
    std::uint32_t gameplayValues = 0;
    /// The stock mode it plays as (game flag 0x100 Arena, 0x20 CTF; 0: none). The world and every
    /// client have that mode's flags, so the engine and the stock HUD work as in it; its rules
    /// (scoring, flags taken and captured, the limits) are off, the script's instead.
    std::uint32_t looksLike = 0;
};

/// mode.json's text read; the error (the field and why) when it isn't a mode's.
struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string error;
};
ManifestResult readManifest(std::string_view json);

/// Whether `id` may be a mode's or an option's key: 1-32 of a-z, 0-9 and _ (a mode's id: at most 24).
bool validModeId(std::string_view id);

} // namespace nox::modes::script
