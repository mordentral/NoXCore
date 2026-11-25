// (noxcpp) The game controller's bindings (pad_bindings.hpp): names, the default layout, the
// file's reading and writing, and the router.
#include "pad/pad_bindings.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace noxsdl::pad {

namespace {

namespace cfg = nox::config;

constexpr std::array<std::string_view, kPadButtonCount> kButtonNames{
    "South",  "East",       "West",         "North",       "View",         "Guide",       "Start",       "LeftStick",
    "RightStick", "LB",     "RB",           "DpadUp",      "DpadDown",     "DpadLeft",    "DpadRight",   "Misc",
    "RightPaddle1", "LeftPaddle1", "RightPaddle2", "LeftPaddle2", "Touchpad", "LT",        "RT",
};

/// Other names a file may use for a button (read, never written).
constexpr std::pair<std::string_view, PadButton> kButtonAliases[] = {
    {"L1", PadButton::LeftShoulder}, {"R1", PadButton::RightShoulder}, {"L2", PadButton::LeftTrigger},
    {"R2", PadButton::RightTrigger}, {"L3", PadButton::LeftStick},     {"R3", PadButton::RightStick},
    {"Back", PadButton::View},       {"Select", PadButton::View},
};

constexpr std::array<std::string_view, kPadActionCount> kActionNames{
    "Interact", "TargetNext", "TargetPrevious", "FreeCursor", "Modifier", "MenuAccept", "MenuBack", "TabPrevious", "TabNext",
};

/// The game's controls' names, then the pad's: made once.
const std::vector<std::string_view>& allActionNames()
{
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> v;
        for (const auto& c : cfg::controlNames()) {
            v.push_back(c.name);
        }
        v.insert(v.end(), kActionNames.begin(), kActionNames.end());
        return v;
    }();
    return names;
}

bool sameName(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

/// The file's own header lines: dropped on reading, so they aren't kept as "other lines" and
/// written twice.
constexpr std::string_view kHeader[] = {
    "# NoX Core: the game controller's settings and buttons.",
    "# Buttons by their place: South is A on an Xbox pad, Cross on a PlayStation one, B on a Switch one.",
    "# Action = Button, or Action = Modifier + Button for the modifier's layer; None for no button.",
};

std::optional<float> number(std::string_view v)
{
    float f = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), f);
    if (ec != std::errc{} || end != v.data() + v.size() || !std::isfinite(f)) {
        return std::nullopt;
    }
    return f;
}

std::string numberText(float f)
{
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, f, std::chars_format::fixed, 3);
    std::string s(buf, ec == std::errc{} ? end : buf);
    while (s.size() > 1 && s.back() == '0' && s.find('.') != std::string::npos) {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s;
}

std::optional<bool> flag(std::string_view v)
{
    if (v == "1" || sameName(v, "on") || sameName(v, "true") || sameName(v, "yes")) {
        return true;
    }
    if (v == "0" || sameName(v, "off") || sameName(v, "false") || sameName(v, "no")) {
        return false;
    }
    return std::nullopt;
}

constexpr std::string_view kAimNames[] = {"off", "weak", "normal", "strong"};
constexpr std::string_view kLockNames[] = {"held", "retarget"};

/// A settings line read into `s`; false when `name` isn't a setting or its value doesn't read.
bool readSetting(PadSettings& s, std::string_view name, std::string_view value)
{
    if (sameName(name, "DeadZone") || sameName(name, "OuterDeadZone") || sameName(name, "TriggerThreshold") ||
        sameName(name, "Sensitivity") || sameName(name, "RunThreshold")) {
        const auto f = number(value);
        if (!f) {
            return false;
        }
        if (sameName(name, "DeadZone")) {
            s.deadZone = std::clamp(*f, 0.0f, 0.9f);
        } else if (sameName(name, "OuterDeadZone")) {
            s.outerDeadZone = std::clamp(*f, 0.1f, 1.0f);
        } else if (sameName(name, "TriggerThreshold")) {
            s.triggerThreshold = std::clamp(*f, 0.05f, 0.95f);
        } else if (sameName(name, "RunThreshold")) {
            s.runThreshold = std::clamp(*f, kMinRunThreshold, kMaxRunThreshold);
        } else {
            s.sensitivity = std::clamp(*f, 0.25f, 4.0f);
        }
        return true;
    }
    if (sameName(name, "AimAssist")) {
        for (std::size_t i = 0; i < std::size(kAimNames); ++i) {
            if (sameName(value, kAimNames[i])) {
                s.aimAssist = static_cast<AimAssist>(i);
                return true;
            }
        }
        return false;
    }
    if (sameName(name, "LockMode")) {
        for (std::size_t i = 0; i < std::size(kLockNames); ++i) {
            if (sameName(value, kLockNames[i])) {
                s.lockMode = static_cast<LockMode>(i);
                return true;
            }
        }
        return false;
    }
    if (sameName(name, "InvertY") || sameName(name, "Rumble")) {
        const auto b = flag(value);
        if (!b) {
            return false;
        }
        (sameName(name, "InvertY") ? s.invertY : s.rumble) = *b;
        return true;
    }
    return false;
}

bool isSettingName(std::string_view name)
{
    for (const std::string_view n : {"DeadZone", "OuterDeadZone", "TriggerThreshold", "Sensitivity", "RunThreshold", "AimAssist", "LockMode", "InvertY", "Rumble"}) {
        if (sameName(name, n)) {
            return true;
        }
    }
    return false;
}

/// An action's name as the table spells it (the file may differ in case).
std::optional<std::string_view> canonicalAction(std::string_view name)
{
    for (const std::string_view n : allActionNames()) {
        if (sameName(n, name)) {
            return n;
        }
    }
    return std::nullopt;
}

/// The bindings in the actions' order (the game's controls', then the pad's), each action's as
/// they came: one config, one order, however its lines were read.
void sortBindings(PadConfig& c)
{
    const auto& names = allActionNames();
    const auto index = [&](const PadBinding& b) {
        return static_cast<std::size_t>(std::ranges::find(names, std::string_view(b.action)) - names.begin());
    };
    std::ranges::stable_sort(c.bindings, {}, index);
}

} // namespace

std::string_view padButtonName(PadButton b)
{
    const auto i = static_cast<std::size_t>(b);
    return i < kButtonNames.size() ? kButtonNames[i] : std::string_view{};
}

std::optional<PadButton> padButtonFromName(std::string_view name)
{
    for (std::size_t i = 0; i < kButtonNames.size(); ++i) {
        if (sameName(kButtonNames[i], name)) {
            return static_cast<PadButton>(i);
        }
    }
    for (const auto& [alias, b] : kButtonAliases) {
        if (sameName(alias, name)) {
            return b;
        }
    }
    return std::nullopt;
}

std::string_view padActionName(PadAction a)
{
    const auto i = static_cast<std::size_t>(a);
    return i < kActionNames.size() ? kActionNames[i] : std::string_view{};
}

std::optional<PadAction> padActionFromName(std::string_view name)
{
    for (std::size_t i = 0; i < kActionNames.size(); ++i) {
        if (sameName(kActionNames[i], name)) {
            return static_cast<PadAction>(i);
        }
    }
    return std::nullopt;
}

bool menuAction(PadAction a)
{
    return a == PadAction::MenuAccept || a == PadAction::MenuBack || a == PadAction::TabPrevious || a == PadAction::TabNext;
}

std::span<const std::string_view> padActionNames()
{
    return allActionNames();
}

bool knownAction(std::string_view name)
{
    return canonicalAction(name).has_value();
}

std::vector<const PadBinding*> PadConfig::bindingsFor(std::string_view action) const
{
    std::vector<const PadBinding*> out;
    for (const auto& b : bindings) {
        if (b.action == action) {
            out.push_back(&b);
        }
    }
    return out;
}

std::optional<PadButton> PadConfig::modifier() const
{
    for (const auto& b : bindings) {
        if (b.action == padActionName(PadAction::Modifier) && !b.withModifier) {
            return b.button;
        }
    }
    return std::nullopt;
}

PadConfig defaultPadConfig()
{
    using B = PadButton;
    PadConfig c;
    const auto bind = [&c](std::string_view action, B button, bool withModifier = false) {
        c.bindings.push_back(PadBinding{std::string(action), button, withModifier});
    };
    bind("Action", B::RightTrigger);
    bind("Interact", B::South);
    bind("InvokeSlot1", B::West);
    bind("InvokeSlot2", B::North);
    bind("InvokeSlot3", B::East);
    bind("InvokeSlot4", B::RightShoulder);
    bind("InvokeSlot5", B::LeftShoulder);
    bind("Jump", B::LeftStick);
    bind("FreeCursor", B::RightStick);
    bind("QuickHealth", B::DpadUp);
    bind("QuickMana", B::DpadDown);
    bind("PreviousSpellSet", B::DpadLeft);
    bind("NextSpellSet", B::DpadRight);
    bind("ToggleInventory", B::View);
    bind("ToggleQuitMenu", B::Start);
    bind("Modifier", B::LeftTrigger);
    bind("QuickCurePoison", B::DpadUp, true);
    bind("SwapWeapons", B::DpadDown, true);
    bind("ToggleBook", B::View, true);
    bind("ToggleMap", B::Start, true);
    bind("Chat", B::DpadRight, true); // (the on-screen keyboard opens with the line)
    bind("TeamChat", B::DpadLeft, true);
    bind("ToggleConsole", B::RightStick, true);
    bind("MenuAccept", B::South);
    bind("MenuBack", B::East);
    bind("MenuBack", B::Start);
    bind("TabPrevious", B::LeftShoulder);
    bind("TabNext", B::RightShoulder);
    sortBindings(c);
    return c;
}

PadConfig parsePadConfig(std::string_view text)
{
    PadConfig c = defaultPadConfig();
    std::vector<std::string_view> named; // the actions the text has lines for (their defaults gone)
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view raw = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        std::string_view line = raw;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::string_view t = trimmed(line);
        if (t.empty() || std::ranges::find(kHeader, t) != std::end(kHeader)) {
            continue;
        }
        const auto keep = [&] { c.otherLines.emplace_back(line); };
        const std::size_t eq = t.find('=');
        if (t.front() == '#' || t.front() == ';' || eq == std::string_view::npos) {
            keep();
            continue;
        }
        const std::string_view name = trimmed(t.substr(0, eq));
        const std::string_view value = trimmed(t.substr(eq + 1));
        if (isSettingName(name)) {
            if (!readSetting(c.settings, name, value)) {
                keep();
            }
            continue;
        }
        const auto action = canonicalAction(name);
        if (!action) {
            keep();
            continue;
        }
        // The value: None, a button, or "modifier + button".
        std::optional<PadBinding> binding;
        bool none = false;
        if (sameName(value, "None")) {
            none = true;
        } else if (const std::size_t plus = value.find('+'); plus != std::string_view::npos) {
            const auto first = padButtonFromName(trimmed(value.substr(0, plus)));
            const auto second = padButtonFromName(trimmed(value.substr(plus + 1)));
            if (first && second) {
                binding = PadBinding{std::string(*action), *second, true};
            }
        } else if (const auto b = padButtonFromName(value)) {
            binding = PadBinding{std::string(*action), *b, false};
        }
        if (!binding && !none) {
            keep();
            continue;
        }
        if (std::ranges::find(named, *action) == named.end()) {
            named.push_back(*action);
            std::erase_if(c.bindings, [&](const PadBinding& b) { return b.action == *action; });
        }
        if (binding) {
            c.bindings.push_back(std::move(*binding));
        }
    }
    sortBindings(c);
    return c;
}

std::string formatPadConfig(const PadConfig& c)
{
    std::string out;
    for (const std::string_view h : kHeader) {
        out.append(h).append("\n");
    }
    const PadSettings& s = c.settings;
    out += "DeadZone = " + numberText(s.deadZone) + "\n";
    out += "OuterDeadZone = " + numberText(s.outerDeadZone) + "\n";
    out += "TriggerThreshold = " + numberText(s.triggerThreshold) + "\n";
    out += "Sensitivity = " + numberText(s.sensitivity) + "\n";
    out += "RunThreshold = " + numberText(s.runThreshold) + "\n";
    out += "AimAssist = " + std::string(kAimNames[static_cast<std::size_t>(s.aimAssist)]) + "\n";
    out += "LockMode = " + std::string(kLockNames[static_cast<std::size_t>(s.lockMode)]) + "\n";
    out += std::string("InvertY = ") + (s.invertY ? "1" : "0") + "\n";
    out += std::string("Rumble = ") + (s.rumble ? "1" : "0") + "\n";
    const std::string modifier(padButtonName(c.modifier().value_or(PadButton::LeftTrigger)));
    for (const std::string_view action : allActionNames()) {
        const auto mine = c.bindingsFor(action);
        if (mine.empty()) {
            out.append(action).append(" = None\n");
            continue;
        }
        for (const PadBinding* b : mine) {
            out.append(action).append(" = ");
            if (b->withModifier) {
                out.append(modifier).append(" + ");
            }
            out.append(padButtonName(b->button)).append("\n");
        }
    }
    for (const auto& line : c.otherLines) {
        out.append(line).append("\n");
    }
    return out;
}

void PadRouter::configure(const PadConfig& config)
{
    layers_ = {};
    menu_ = {};
    latched_.fill(kNone);
    modifier_ = config.modifier();
    gameBindings_.clear();
    for (const auto& b : config.bindings) {
        const auto i = static_cast<std::size_t>(b.button);
        if (i >= kPadButtonCount) {
            continue;
        }
        if (const auto a = padActionFromName(b.action)) {
            if (menuAction(*a)) {
                menu_[i].add(*a);
            } else if (*a != PadAction::Modifier) {
                layers_[i][b.withModifier ? 1 : 0].actions.add(*a);
            }
            continue;
        }
        if (!cfg::findControl(b.action) || (modifier_ && b.button == *modifier_ && !b.withModifier)) {
            continue; // (not a control; or on the modifier, which does nothing else)
        }
        layers_[i][b.withModifier ? 1 : 0].game = true;
        gameBindings_.push_back(cfg::Binding{{padCode(b.button, b.withModifier)}, {b.action}});
    }
}

PadRouter::Edge PadRouter::press(PadButton b, bool modifierHeld)
{
    const auto i = static_cast<std::size_t>(b);
    if (i >= kPadButtonCount) {
        return {};
    }
    Edge e;
    if (modifier_ && b == *modifier_) {
        e.isModifier = true;
        e.actions.add(PadAction::Modifier);
        latched_[i] = 0;
        return e;
    }
    // The modifier's layer when it's held and has something for this button; else the button's own.
    const std::uint8_t layer = modifierHeld && modifier_ && layers_[i][1].any() ? 1 : 0;
    latched_[i] = layer;
    const Layer& l = layers_[i][layer];
    e.code = l.game ? padCode(b, layer == 1) : 0;
    e.actions = l.actions;
    e.modifierLayer = layer == 1;
    return e;
}

PadRouter::Edge PadRouter::release(PadButton b)
{
    const auto i = static_cast<std::size_t>(b);
    if (i >= kPadButtonCount || latched_[i] == kNone) {
        return {};
    }
    const std::uint8_t layer = std::exchange(latched_[i], kNone);
    Edge e;
    if (modifier_ && b == *modifier_) {
        e.isModifier = true;
        e.actions.add(PadAction::Modifier);
        return e;
    }
    const Layer& l = layers_[i][layer];
    e.code = l.game ? padCode(b, layer == 1) : 0;
    e.actions = l.actions;
    e.modifierLayer = layer == 1;
    return e;
}

void PadRouter::dropAll()
{
    latched_.fill(kNone);
}

bool PadRouter::held(PadAction a) const
{
    if (a == PadAction::Modifier) {
        return modifier_ && pressed(*modifier_);
    }
    for (std::size_t i = 0; i < kPadButtonCount; ++i) {
        if (latched_[i] != kNone && layers_[i][latched_[i]].actions.has(a)) {
            return true;
        }
    }
    return false;
}

} // namespace noxsdl::pad
