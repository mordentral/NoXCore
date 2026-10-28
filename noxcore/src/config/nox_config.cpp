// nox.cfg reader and writer (format in nox_config.hpp), with the exe's tables of bindable key
// names and control names.
#include "noxcore/config/nox_config.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace nox::config {

namespace {

// The exe's key table, in the game's order (the input configuration window lists keys by it).
constexpr std::array<KeyName, 121> kKeys{{
    {"KP0", 0x52, "keybind:Kp0"},
    {"KP1", 0x4F, "keybind:Kp1"},
    {"KP2", 0x50, "keybind:Kp2"},
    {"KP3", 0x51, "keybind:Kp3"},
    {"KP4", 0x4B, "keybind:Kp4"},
    {"KP5", 0x4C, "keybind:Kp5"},
    {"KP6", 0x4D, "keybind:Kp6"},
    {"KP7", 0x47, "keybind:Kp7"},
    {"KP8", 0x48, "keybind:Kp8"},
    {"KP9", 0x49, "keybind:Kp9"},
    {"KPDEL", 0x53, "keybind:KpPeriod"},
    {"KPSTAR", 0x37, "keybind:KpStar"},
    {"KPMINUS", 0x4A, "keybind:KpMinus"},
    {"KPPLUS", 0x4E, "keybind:KpPlus"},
    {"KPENTER", 0x9C, "keybind:KpEnter"},
    {"KPSLASH", 0xB5, "keybind:KpSlash"},
    {"ESC", 0x1, "keybind:Esc"},
    {"BACKSPACE", 0xE, "keybind:Back"},
    {"ENTER", 0x1C, "keybind:Enter"},
    {"SPACE", 0x39, "keybind:Space"},
    {"TAB", 0xF, "keybind:Tab"},
    {"F1", 0x3B, "keybind:F1"},
    {"F2", 0x3C, "keybind:F2"},
    {"F3", 0x3D, "keybind:F3"},
    {"F4", 0x3E, "keybind:F4"},
    {"F5", 0x3F, "keybind:F5"},
    {"F6", 0x40, "keybind:F6"},
    {"F7", 0x41, "keybind:F7"},
    {"F8", 0x42, "keybind:F8"},
    {"F9", 0x43, "keybind:F9"},
    {"F10", 0x44, "keybind:F10"},
    {"F11", 0x57, "keybind:F11"},
    {"F12", 0x58, "keybind:F12"},
    {"A", 0x1E, "keybind:A"},
    {"B", 0x30, "keybind:B"},
    {"C", 0x2E, "keybind:C"},
    {"D", 0x20, "keybind:D"},
    {"E", 0x12, "keybind:E"},
    {"F", 0x21, "keybind:F"},
    {"G", 0x22, "keybind:G"},
    {"H", 0x23, "keybind:H"},
    {"I", 0x17, "keybind:I"},
    {"J", 0x24, "keybind:J"},
    {"K", 0x25, "keybind:K"},
    {"L", 0x26, "keybind:L"},
    {"M", 0x32, "keybind:M"},
    {"N", 0x31, "keybind:N"},
    {"O", 0x18, "keybind:O"},
    {"P", 0x19, "keybind:P"},
    {"Q", 0x10, "keybind:Q"},
    {"R", 0x13, "keybind:R"},
    {"S", 0x1F, "keybind:S"},
    {"T", 0x14, "keybind:T"},
    {"U", 0x16, "keybind:U"},
    {"V", 0x2F, "keybind:V"},
    {"W", 0x11, "keybind:W"},
    {"X", 0x2D, "keybind:X"},
    {"Y", 0x15, "keybind:Y"},
    {"Z", 0x2C, "keybind:Z"},
    {"1", 0x2, "keybind:1"},
    {"2", 0x3, "keybind:2"},
    {"3", 0x4, "keybind:3"},
    {"4", 0x5, "keybind:4"},
    {"5", 0x6, "keybind:5"},
    {"6", 0x7, "keybind:6"},
    {"7", 0x8, "keybind:7"},
    {"8", 0x9, "keybind:8"},
    {"9", 0xA, "keybind:9"},
    {"0", 0xB, "keybind:0"},
    {"MINUS", 0xC, "keybind:Minus"},
    {"EQUALS", 0xD, "keybind:Equal"},
    {"LEFT_BRACKET", 0x1A, "keybind:["},
    {"RIGHT_BRACKET", 0x1B, "keybind:]"},
    {"SEMICOLON", 0x27, "keybind:SemiColon"},
    {"APOSTROPHE", 0x28, "keybind:Squote"},
    {"TICK", 0x29, "keybind:Apos"},
    {"BACKSLASH", 0x2B, "keybind:Bslash"},
    {"COMMA", 0x33, "keybind:Comma"},
    {"PERIOD", 0x34, "keybind:Period"},
    {"SLASH", 0x35, "keybind:Slash"},
    {"SYSREQ", 0xB7, "keybind:SysReq"},
    {"CAPS_LOCK", 0x3A, "keybind:Caps"},
    {"NUM", 0x45, "keybind:NumLck"},
    {"SCROLL", 0x46, "keybind:Scroll"},
    {"LEFT_CTRL", 0x1D, "keybind:LCtrl"},
    {"LEFT_ALT", 0x38, "keybind:LAlt"},
    {"LEFT_SHIFT", 0x2A, "keybind:LShift"},
    {"RIGHT_SHIFT", 0x36, "keybind:RShift"},
    {"RIGHT_ALT", 0xB8, "keybind:RAlt"},
    {"RIGHT_CTRL", 0x9D, "keybind:RCtrl"},
    {"UP_ARROW", 0xC8, "keybind:Up"},
    {"DOWN_ARROW", 0xD0, "keybind:Down"},
    {"LEFT_ARROW", 0xCB, "keybind:Left"},
    {"RIGHT_ARROW", 0xCD, "keybind:Right"},
    {"HOME", 0xC7, "keybind:Home"},
    {"END", 0xCF, "keybind:End"},
    {"PAGEUP", 0xC9, "keybind:PgUp"},
    {"PAGEDOWN", 0xD1, "keybind:PgDn"},
    {"INS", 0xD2, "keybind:Ins"},
    {"DEL", 0xD3, "keybind:Del"},
    {"MOUSE_BUTTON_LEFT", 0x10000, "keybind:LeftMouse"},
    {"MOUSE_BUTTON_MID", 0x10001, "keybind:MiddleMouse"},
    {"MOUSE_BUTTON_RIGHT", 0x10002, "keybind:RightMouse"},
    {"MOUSE_WHEEL_UP", 0x10003, "keybind:MouseWheelUp"},
    {"MOUSE_WHEEL_DOWN", 0x10004, "keybind:MouseWheelDown"},
    {"JOYSTICK_BUTTON_1", 0x20000, "keybind:NA"},
    {"JOYSTICK_BUTTON_2", 0x20001, "keybind:NA"},
    {"JOYSTICK_BUTTON_3", 0x20002, "keybind:NA"},
    {"JOYSTICK_BUTTON_4", 0x20003, "keybind:NA"},
    {"JOYSTICK_BUTTON_5", 0x20004, "keybind:NA"},
    {"JOYSTICK_BUTTON_6", 0x20005, "keybind:NA"},
    {"JOYSTICK_BUTTON_7", 0x20006, "keybind:NA"},
    {"JOYSTICK_BUTTON_8", 0x20007, "keybind:NA"},
    {"JOYSTICK_BUTTON_9", 0x20008, "keybind:NA"},
    {"JOYSTICK_BUTTON_10", 0x20009, "keybind:NA"},
    {"JOYSTICK_BUTTON_11", 0x2000A, "keybind:NA"},
    {"JOYSTICK_BUTTON_12", 0x2000B, "keybind:NA"},
    {"JOYSTICK_BUTTON_13", 0x2000C, "keybind:NA"},
}};

// The exe's control table ({name, event, 0} entries there), in the game's order (the input configuration window lists them so).
constexpr std::array<ControlName, 40> kControls{{
    {"MoveForward", 0x2},       {"Action", 0x1},          {"Jump", 0x6},
    {"ToggleInventory", 0x7},   {"ToggleBook", 0xA},      {"ToggleMap", 0x10},
    {"MapZoomOut", 0x22},       {"MapZoomIn", 0x21},      {"InvokeSlot1", 0x1C},
    {"InvokeSlot2", 0x1D},      {"InvokeSlot3", 0x1E},    {"InvokeSlot4", 0x1F},
    {"InvokeSlot5", 0x20},      {"PreviousSpellSet", 0x28}, {"NextSpellSet", 0x27},
    {"SelectSpellSet", 0x29},   {"InvertSpellTarget", 0x31}, {"PlaceTrapBomber", 0x2A},
    {"SwapWeapons", 0x23},      {"QuickHealth", 0x24},    {"QuickMana", 0x25},
    {"QuickCurePoison", 0x26},  {"Chat", 0x8},            {"TeamChat", 0x9},
    {"ToggleConsole", 0xB},     {"ToggleQuitMenu", 0x2C}, {"ToggleServerMenu", 0x2D},
    {"ToggleRank", 0x32},       {"ToggleNetstat", 0x33},  {"ToggleGUI", 0x34},
    {"AutoSave", 0x35},         {"AutoLoad", 0x36},       {"Taunt", 0x2E},
    {"Point", 0x30},            {"Laugh", 0x2F},          {"IncreaseWindowSize", 0xC},
    {"DecreaseWindowSize", 0xD}, {"IncreaseGamma", 0xE},  {"DecreaseGamma", 0xF},
    {"ScreenShot", 0x37},
}};

constexpr std::size_t kMaxNames = 8;                               // per side of a binding
constexpr std::array<std::string_view, 3> kPickupButtons{"Left", "Right", "Middle"};

using util::trim;

// Splits on white space only; "+" and "=" are their own tokens because the game writes them
// with spaces around.
std::vector<std::string_view> tokens(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

// "KEY + KEY = Control + Control": names before the "=" are keys, after it controls.
Binding parseBinding(std::string_view line)
{
    Binding b;
    bool controls = false;
    for (const auto token : tokens(line)) {
        if (token == "+") {
            continue;
        }
        if (token == "=") {
            controls = true;
            continue;
        }
        if (!controls) {
            const KeyName* key = findKey(token);
            if (!key) {
                throw FormatError("nox.cfg: unknown key " + std::string(token));
            }
            b.keys.push_back(key->code);
        } else {
            if (!findControl(token)) {
                throw FormatError("nox.cfg: unknown control " + std::string(token));
            }
            b.controls.emplace_back(token);
        }
    }
    if (b.keys.empty() || b.keys.size() > kMaxNames || b.controls.size() > kMaxNames) {
        throw FormatError("nox.cfg: bad binding: " + std::string(line));
    }
    return b;
}

} // namespace

std::span<const KeyName> keyNames()
{
    return kKeys;
}

std::span<const ControlName> controlNames()
{
    return kControls;
}

const KeyName* findKey(std::string_view name)
{
    const auto it = std::ranges::find(kKeys, name, &KeyName::name);
    return it == kKeys.end() ? nullptr : &*it;
}

const KeyName* findKey(std::uint32_t code)
{
    const auto it = std::ranges::find(kKeys, code, &KeyName::code);
    return it == kKeys.end() ? nullptr : &*it;
}

const ControlName* findControl(std::string_view name)
{
    const auto it = std::ranges::find(kControls, name, &ControlName::name);
    return it == kControls.end() ? nullptr : &*it;
}

std::optional<std::string> Config::setting(std::string_view name) const
{
    for (const auto& [k, v] : settings) {
        if (k == name) {
            return v;
        }
    }
    return std::nullopt;
}

void Config::set(std::string_view name, std::string value)
{
    for (auto& [k, v] : settings) {
        if (k == name) {
            v = std::move(value);
            return;
        }
    }
    settings.emplace_back(std::string(name), std::move(value));
}

std::vector<const Binding*> Config::bindingsFor(std::string_view control) const
{
    std::vector<const Binding*> out;
    for (const auto& b : bindings) {
        if (std::ranges::find(b.controls, control) != b.controls.end()) {
            out.push_back(&b);
        }
    }
    return out;
}

Config parseConfig(std::string_view text)
{
    Config config;
    int part = 0; // 0 settings, 1 bindings, 2 done
    std::size_t pos = 0;
    // Line by line until the second "---"; anything after it is ignored.
    while (pos < text.size() && part < 2) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with("---")) {
            ++part;
            continue;
        }
        if (trim(line).empty() || line.starts_with("#")) {
            continue; // (the game's binding reader stops at a blank line; it's never written)
        }
        const auto eq = line.find('=');
        if (part == 0) {
            if (eq == std::string_view::npos) {
                throw FormatError("nox.cfg: not a setting: " + std::string(line));
            }
            config.settings.emplace_back(std::string(trim(line.substr(0, eq))), std::string(trim(line.substr(eq + 1))));
        // In the bindings part, the MousePickup line (written first) isn't a binding.
        } else if (eq != std::string_view::npos && trim(line.substr(0, eq)) == "MousePickup") {
            const std::string_view button = trim(line.substr(eq + 1));
            if (std::ranges::find(kPickupButtons, button) == kPickupButtons.end()) {
                throw FormatError("nox.cfg: MousePickup " + std::string(button));
            }
            config.mousePickup = std::string(button);
        } else {
            config.bindings.push_back(parseBinding(line));
        }
    }
    if (part < 2) {
        throw FormatError("nox.cfg: a part doesn't end in ---");
    }
    return config;
}

std::string writeConfig(const Config& config)
{
    std::string out;
    for (const auto& [k, v] : config.settings) {
        out += k + " = " + v + "\n";
    }
    out += "---\n";
    out += "MousePickup = " + config.mousePickup + "\n";
    for (const auto& b : config.bindings) {
        // Every name is followed by a space and names are joined by "+ ", as the game writes it.
        for (std::size_t i = 0; i < b.keys.size(); ++i) {
            if (const KeyName* key = findKey(b.keys[i])) {
                out += std::string(key->name) + " ";
            }
            if (i + 1 != b.keys.size()) {
                out += "+ ";
            }
        }
        out += "= ";
        for (std::size_t i = 0; i < b.controls.size(); ++i) {
            out += b.controls[i] + " ";
            if (i + 1 != b.controls.size()) {
                out += "+ ";
            }
        }
        out += "\n";
    }
    out += "---\n";
    return out;
}

std::optional<Config> loadConfig(const std::filesystem::path& gameDir, bool* fromDefault)
{
    for (const char* name : {"nox.cfg", "default.cfg"}) {
        try {
            const auto bytes = readFileBytes(gameDir / name);
            Config config = parseConfig({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            if (fromDefault) {
                *fromDefault = std::string_view(name) == "default.cfg";
            }
            return config;
        } catch (const std::exception&) {
            // As the game: a missing or rejected nox.cfg falls back to default.cfg.
        }
    }
    return std::nullopt;
}

void saveConfig(const std::filesystem::path& gameDir, const Config& config)
{
    const std::string text = writeConfig(config);
    writeFileBytes(gameDir / "nox.cfg", {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

} // namespace nox::config
