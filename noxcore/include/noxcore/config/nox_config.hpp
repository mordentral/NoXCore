// nox.cfg / default.cfg: the game's settings and key bindings, as GAME.EXE reads and writes them.
//
// The game reads nox.cfg, or default.cfg when there's none (and then writes nox.cfg at once).
// The file has two parts, each ending in a line starting "---":
//   settings   "Name = value" lines (VideoMode = 640 480 16, VideoSize = 100, FXVolume = ...)
//   bindings   "MousePickup = Left" (Left, Right or Middle: the button that picks items up), then
//              one binding a line: key names joined by " + " (held together, up to 8), " = ",
//              control names (up to 8, also " + "), read by
//              client_parseConfigHotkeysLine; '#' starts a comment line. A key name the
//              game doesn't know fails the whole file, so only the game's names are ever written.
// The writer (bindings) puts "%s " after every name and "+ " between names,
// "= " between the sides, so a line reads "SPACE = Jump ".
//
// Anything noxcpp's own client adds doesn't belong here: the game would reject it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::config {

/// A key the game can bind (the exe's key table): its name in the file, its code (a DirectInput
/// scan code; mouse buttons 0x10000 + n, joystick buttons 0x20000 + n) and its title's string
/// id for the input configuration window.
struct KeyName {
    std::string_view name;
    std::uint32_t code;
    std::string_view title;
};
inline constexpr std::uint32_t kMouseLeft = 0x10000;
inline constexpr std::uint32_t kMouseMiddle = 0x10001;
inline constexpr std::uint32_t kMouseRight = 0x10002;
inline constexpr std::uint32_t kWheelUp = 0x10003;
inline constexpr std::uint32_t kWheelDown = 0x10004;

/// A control a key can be bound to (the exe's control table): its name in the file and the event
/// number Cli_ProcessControlEvents dispatches on. Its title is the string
/// "bindevent:<name>".
struct ControlName {
    std::string_view name;
    int event;
};

std::span<const KeyName> keyNames();
std::span<const ControlName> controlNames();
/// By name (exact, as the game compares them), or nullptr.
const KeyName* findKey(std::string_view name);
const KeyName* findKey(std::uint32_t code);
const ControlName* findControl(std::string_view name);

struct Binding {
    std::vector<std::uint32_t> keys;     ///< key codes, all held together
    std::vector<std::string> controls;   ///< control names, in order

    bool operator==(const Binding&) const = default;
};

struct Config {
    /// The settings part, in file order, values as written (trimmed).
    std::vector<std::pair<std::string, std::string>> settings;
    std::string mousePickup = "Left";
    std::vector<Binding> bindings;

    std::optional<std::string> setting(std::string_view name) const;
    /// Replaces a setting's value, or adds it at the end.
    void set(std::string_view name, std::string value);
    /// The bindings that fire `control`.
    std::vector<const Binding*> bindingsFor(std::string_view control) const;
};

/// Parses a whole file. Throws FormatError where the game would reject it (an unknown key or
/// control name, more than 8 of either, a part missing its "---").
Config parseConfig(std::string_view text);
/// The file as the game writes it.
std::string writeConfig(const Config& config);

/// nox.cfg in `gameDir`, else default.cfg; nullopt if neither reads. With `fromDefault`, whether
/// it came from default.cfg.
std::optional<Config> loadConfig(const std::filesystem::path& gameDir, bool* fromDefault = nullptr);
/// Writes nox.cfg in `gameDir`. Throws std::system_error.
void saveConfig(const std::filesystem::path& gameDir, const Config& config);

} // namespace nox::config
