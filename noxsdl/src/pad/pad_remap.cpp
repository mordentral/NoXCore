// (noxcpp) The Controller screen's changes to the pad's buttons (pad_remap.hpp).
#include "pad/pad_remap.hpp"

#include <algorithm>

namespace noxsdl::pad {

namespace {

/// The rows the pad uses most, first, in this order.
constexpr std::string_view kFirstRows[] = {
    "Action",       "Interact",       "InvokeSlot1",     "InvokeSlot2",      "InvokeSlot3",  "InvokeSlot4",
    "InvokeSlot5",  "Jump",           "FreeCursor",      "TargetNext",       "TargetPrevious", "QuickHealth",
    "QuickMana",    "QuickCurePoison", "PreviousSpellSet", "NextSpellSet",   "SwapWeapons",  "ToggleInventory",
    "ToggleBook",   "ToggleMap",      "ToggleQuitMenu",  "Modifier",
};
constexpr std::string_view kMenuRows[] = {"MenuAccept", "MenuBack", "TabPrevious", "TabNext"};

/// The kinds of binding a button is taken from: a game action's alone, a game action's on the
/// modifier's layer, a menus' action's.
enum class Kind : std::uint8_t { Plain, Layer, Menu };

Kind kindOf(const PadBinding& b)
{
    return menuRow(b.action) ? Kind::Menu : b.withModifier ? Kind::Layer : Kind::Plain;
}

Kind cellKind(std::string_view action, int cell)
{
    return menuRow(action) ? Kind::Menu : cell == 1 ? Kind::Layer : Kind::Plain;
}

/// Never left without a button by a move.
bool essential(std::string_view action)
{
    return action == padActionName(PadAction::Modifier) || action == padActionName(PadAction::MenuAccept) ||
           action == padActionName(PadAction::MenuBack);
}

std::vector<PadButton> buttonsOf(const PadConfig& c, std::string_view action, Kind k)
{
    std::vector<PadButton> out;
    for (const auto& b : c.bindings) {
        if (b.action == action && kindOf(b) == k && std::ranges::find(out, b.button) == out.end()) {
            out.push_back(b.button);
        }
    }
    return out;
}

void setButtons(PadConfig& c, std::string_view action, Kind k, const std::vector<PadButton>& buttons)
{
    std::erase_if(c.bindings, [&](const PadBinding& b) { return b.action == action && kindOf(b) == k; });
    for (const PadButton b : buttons) {
        c.bindings.push_back(PadBinding{std::string(action), b, k == Kind::Layer});
    }
}

void note(std::vector<std::string>& list, std::string_view action)
{
    if (std::ranges::find(list, action) == list.end()) {
        list.emplace_back(action);
    }
}

} // namespace

std::span<const std::string_view> remapRows()
{
    static const std::vector<std::string_view> rows = [] {
        std::vector<std::string_view> v(std::begin(kFirstRows), std::end(kFirstRows));
        for (const std::string_view a : padActionNames()) {
            if (!menuRow(a) && std::ranges::find(v, a) == v.end()) {
                v.push_back(a);
            }
        }
        v.insert(v.end(), std::begin(kMenuRows), std::end(kMenuRows));
        return v;
    }();
    return rows;
}

bool menuRow(std::string_view action)
{
    const auto a = padActionFromName(action);
    return a && menuAction(*a);
}

bool cellUsable(std::string_view action, int cell)
{
    if (cell < 0 || cell > 1 || !knownAction(action)) {
        return false;
    }
    return !(cell == 1 && action == padActionName(PadAction::Modifier));
}

bool bindableButton(PadButton b)
{
    return b != PadButton::Guide && static_cast<std::size_t>(b) < kPadButtonCount;
}

std::optional<PadButton> cellButton(const PadConfig& c, std::string_view action, int cell)
{
    if (!cellUsable(action, cell)) {
        return std::nullopt;
    }
    const Kind k = cellKind(action, cell);
    const auto list = buttonsOf(c, action, k);
    const std::size_t at = k == Kind::Menu ? static_cast<std::size_t>(cell) : 0;
    return at < list.size() ? std::optional<PadButton>(list[at]) : std::nullopt;
}

AssignResult assignCell(PadConfig& c, std::string_view action, int cell, PadButton b)
{
    if (!cellUsable(action, cell) || !bindableButton(b)) {
        return {};
    }
    const Kind k = cellKind(action, cell);
    if (const auto mod = c.modifier(); k == Kind::Layer && mod && *mod == b) {
        return {}; // (the modifier held, then itself: nothing to press)
    }
    const std::optional<PadButton> old = cellButton(c, action, cell);
    AssignResult r;
    r.done = true;
    if (old == b) {
        return r;
    }
    // This row's buttons of the kind, the cell's taking b.
    std::vector<PadButton> mine;
    if (k == Kind::Menu) {
        const auto list = buttonsOf(c, action, k);
        std::optional<PadButton> first = list.empty() ? std::nullopt : std::optional<PadButton>(list[0]);
        std::optional<PadButton> second = list.size() < 2 ? std::nullopt : std::optional<PadButton>(list[1]);
        (cell == 0 ? first : second) = b;
        if (cell == 0 && second == b) {
            second.reset();
        } else if (cell == 1 && first == b) {
            first.reset();
        }
        for (const auto& x : {first, second}) {
            if (x) {
                mine.push_back(*x);
            }
        }
    } else {
        mine.push_back(b);
    }
    // The others of the kind lose it (one that mustn't be left with none takes the old button).
    for (const std::string_view other : padActionNames()) {
        if (other == action) {
            continue;
        }
        auto theirs = buttonsOf(c, other, k);
        if (std::ranges::find(theirs, b) == theirs.end()) {
            continue;
        }
        std::erase(theirs, b);
        note(r.movedFrom, other);
        if (theirs.empty() && essential(other) && old) {
            theirs.push_back(*old);
            note(r.swapped, other);
        }
        setButtons(c, other, k, theirs);
    }
    setButtons(c, action, k, mine);
    // The modifier's button is off its own layer.
    if (const auto mod = c.modifier()) {
        for (const auto& x : c.bindings) {
            if (x.withModifier && x.button == *mod && kindOf(x) == Kind::Layer) {
                note(r.movedFrom, x.action);
            }
        }
        std::erase_if(c.bindings, [&](const PadBinding& x) { return x.withModifier && x.button == *mod && kindOf(x) == Kind::Layer; });
    }
    orderBindings(c);
    return r;
}

bool clearCell(PadConfig& c, std::string_view action, int cell)
{
    if (!cellUsable(action, cell)) {
        return false;
    }
    const Kind k = cellKind(action, cell);
    auto list = buttonsOf(c, action, k);
    if (k == Kind::Menu) {
        if (static_cast<std::size_t>(cell) >= list.size()) {
            return true;
        }
        if (essential(action) && list.size() == 1) {
            return false;
        }
        list.erase(list.begin() + cell);
    } else {
        if (list.empty()) {
            return true;
        }
        if (essential(action)) {
            return false;
        }
        list.clear();
    }
    setButtons(c, action, k, list);
    orderBindings(c);
    return true;
}

ActionButtons actionButtons(const PadConfig& c, std::string_view action)
{
    const PadBinding* layered = nullptr;
    for (const auto& b : c.bindings) {
        if (b.action != action) {
            continue;
        }
        if (!b.withModifier || menuRow(action)) {
            return {std::nullopt, b.button};
        }
        if (!layered) {
            layered = &b;
        }
    }
    if (layered) {
        if (const auto mod = c.modifier()) {
            return {mod, layered->button};
        }
    }
    return {};
}

void CaptureWait::start(std::uint64_t nowMs, std::uint32_t back)
{
    active_ = true;
    back_ = back;
    pressed_ = 0;
    holding_.reset();
    holdFrom_ = nowMs;
}

CaptureWait::Outcome CaptureWait::press(PadButton b, std::uint64_t nowMs)
{
    if (!active_ || !bindableButton(b)) {
        return {};
    }
    pressed_ |= bit(b);
    if (back_ & bit(b)) {
        if (!holding_) {
            holding_ = b; // (binds when it comes up, unless held long enough to cancel)
            holdFrom_ = nowMs;
        }
        return {};
    }
    active_ = false;
    return {Outcome::Bound, b};
}

CaptureWait::Outcome CaptureWait::release(PadButton b, std::uint64_t nowMs)
{
    if (!active_ || !(pressed_ & bit(b)) || holding_ != b) {
        return {};
    }
    active_ = false;
    holding_.reset();
    return nowMs - holdFrom_ >= kCancelMs ? Outcome{Outcome::Cancelled, b} : Outcome{Outcome::Bound, b};
}

CaptureWait::Outcome CaptureWait::tick(std::uint64_t nowMs)
{
    if (!active_ || !holding_ || nowMs - holdFrom_ < kCancelMs) {
        return {};
    }
    const PadButton b = *holding_;
    active_ = false;
    holding_.reset();
    return {Outcome::Cancelled, b};
}

float CaptureWait::cancelShare(std::uint64_t nowMs) const
{
    if (!active_ || !holding_) {
        return 0;
    }
    return std::min(1.0f, static_cast<float>(nowMs - holdFrom_) / static_cast<float>(kCancelMs));
}

std::uint32_t backButtons(const PadConfig& c)
{
    std::uint32_t bits = 0;
    for (const auto& b : c.bindings) {
        if (b.action == padActionName(PadAction::MenuBack)) {
            bits |= 1u << static_cast<unsigned>(b.button);
        }
    }
    return bits;
}

} // namespace noxsdl::pad
