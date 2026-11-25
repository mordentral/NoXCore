// (noxcpp) The game controller's bindings and settings: noxcore_controller.cfg, beside noxcore.cfg
// (nox.cfg can't hold them: the game rejects a file with names it doesn't know), and the router
// that turns a button's press into what it's bound to. No SDL here.
//
// Buttons are named by where they sit, never by what's printed on them: South is A on an Xbox pad,
// Cross on a PlayStation one and B on a Switch one, so one file serves every pad. The file:
//
//     # comments
//     DeadZone = 0.18          (the settings: PadSettings below; DeadZone the left stick's)
//     InvokeSlot1 = West       (an action and its button)
//     QuickCurePoison = LT + DpadUp   (the modifier's layer: held LT, then the button)
//     ToggleConsole = None     (no button)
//
// An action is one of the game's controls (nox.cfg's: InvokeSlot1-5, Action, Jump, QuickHealth...)
// or one the pad alone has (PadAction: Interact, TargetNext, Modifier, the menus').
// An action may have several lines (several buttons); one the file doesn't name keeps its default
// buttons; a line the file has that isn't understood is kept as it is and written back. The first
// name of a "X + Y" pair is the modifier as the file was written; the pair binds Y on the
// modifier's layer whatever X says (the Modifier line says which button that is).
//
// The modifier (LT by default; any button can be; it does nothing else then): while it's held, a
// button's press fires what's bound to "modifier + button" instead of what the button does alone. A button with nothing on the
// modifier's layer does what it does alone even with the modifier held. The layer is taken when a
// button goes down and kept till it comes up, so letting the modifier go first doesn't leave a held
// action stuck, or start the other one.
#pragma once

#include "pad/pad_math.hpp"

#include "noxcore/config/nox_config.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl::pad {

/// A button by its place. The first 21 are SDL_GamepadButton's own numbers (SDL_GAMEPAD_BUTTON_SOUTH
/// to SDL_GAMEPAD_BUTTON_TOUCHPAD); the triggers are SDL's axes, read as buttons.
enum class PadButton : std::uint8_t {
    South,
    East,
    West,
    North,
    View, ///< Back / Select / Share / Minus
    Guide,
    Start,
    LeftStick, ///< L3
    RightStick, ///< R3
    LeftShoulder, ///< LB / L1 / L
    RightShoulder, ///< RB / R1 / R
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc, ///< Share / Capture / the microphone button
    RightPaddle1,
    LeftPaddle1,
    RightPaddle2,
    LeftPaddle2,
    Touchpad,
    LeftTrigger, ///< LT / L2 / ZL
    RightTrigger, ///< RT / R2 / ZR
    Count,
};
inline constexpr std::size_t kPadButtonCount = static_cast<std::size_t>(PadButton::Count);
/// How many of PadButton are SDL_GamepadButton's numbers (the rest are the triggers).
inline constexpr std::size_t kSdlButtonCount = static_cast<std::size_t>(PadButton::LeftTrigger);

/// A button's name in the file ("South", "LB", "LT", "DpadUp"...).
std::string_view padButtonName(PadButton b);
/// The button a file's name stands for (as padButtonName writes it; also L1 / R1 / L2 / R2 / L3 /
/// R3, Back and Select), any case; nullopt for none.
std::optional<PadButton> padButtonFromName(std::string_view name);

/// The pad's own actions, beside the game's controls.
enum class PadAction : std::uint8_t {
    Interact,       ///< pick up, talk, trade, use, open what's in front of the player
    TargetNext,     ///< the next target
    TargetPrevious, ///< the one before
    Modifier,       ///< held: the second layer
    MenuAccept,     ///< the menus' A
    MenuBack,       ///< the menus' B / Start: Escape
    TabPrevious,    ///< a window's tab before
    TabNext,        ///< and after
    Count,
};
inline constexpr std::size_t kPadActionCount = static_cast<std::size_t>(PadAction::Count);
std::string_view padActionName(PadAction a);
std::optional<PadAction> padActionFromName(std::string_view name);
/// Whether a pad action belongs to the menus (MenuAccept, MenuBack, the tabs) rather than the game.
bool menuAction(PadAction a);

/// A set of pad actions, a bit each.
struct PadActionSet {
    std::uint32_t bits = 0;
    bool has(PadAction a) const { return (bits >> static_cast<unsigned>(a)) & 1u; }
    void add(PadAction a) { bits |= 1u << static_cast<unsigned>(a); }
    bool empty() const { return bits == 0; }
    bool operator==(const PadActionSet&) const = default;
};

/// Aim assist's strength (the Controller screen's; AimAssist = off / weak / normal / strong).
enum class AimAssist : std::uint8_t { Off, Weak, Normal, Strong };
/// A target locked while the attack or slot button's held (held), or picked again as the aim
/// turns (retarget). LockMode = held / retarget.
enum class LockMode : std::uint8_t { WhileHeld, Retarget };

/// The buttons' names shown (the prompts, the Controller screen): the connected pad's (auto), or
/// always one family's. Prompts = auto / xbox / playstation / switch.
enum class PromptStyle : std::uint8_t { Auto, Xbox, PlayStation, Switch };

/// RunThreshold's range: the left stick's tilt (past the dead zone) where its cursor point
/// reaches the server's run distance.
inline constexpr float kMinRunThreshold = 0.3f;
inline constexpr float kMaxRunThreshold = 0.95f;
/// The inner dead zones' range (as the file is read).
inline constexpr float kMaxDeadZone = 0.9f;

/// The file's settings.
struct PadSettings {
    float deadZone = 0.18f;         ///< DeadZone: the left stick's inner dead zone (0-0.9)
    float rightDeadZone = 0.18f;    ///< RightDeadZone: the right stick's (0-0.9)
    float outerDeadZone = 0.95f;    ///< OuterDeadZone: full tilt from here (above DeadZone, to 1)
    float triggerThreshold = 0.35f; ///< TriggerThreshold: a trigger's press (0.05-0.95)
    float runThreshold = kDefaultRunThreshold; ///< RunThreshold: the left stick's tilt that runs (0.3-0.95)
    AimAssist aimAssist = AimAssist::Normal;
    LockMode lockMode = LockMode::WhileHeld;
    bool invertY = false; ///< InvertY: the right stick's up and down swapped
    bool rumble = false;  ///< Rumble: off by default
    PromptStyle prompts = PromptStyle::Auto; ///< Prompts: the buttons' names shown

    bool operator==(const PadSettings&) const = default;
};

/// One action's button: on the modifier's layer or not. `action` is a game control's name or a
/// PadAction's.
struct PadBinding {
    std::string action;
    PadButton button = PadButton::South;
    bool withModifier = false;

    bool operator==(const PadBinding&) const = default;
};

/// noxcore_controller.cfg, read.
struct PadConfig {
    PadSettings settings;
    std::vector<PadBinding> bindings; ///< in the actions' order (padActionNames), each action's as given
    std::vector<std::string> otherLines; ///< lines not understood (comments too), written back as they were

    bool operator==(const PadConfig&) const = default;
    /// The bindings of `action`, in order.
    std::vector<const PadBinding*> bindingsFor(std::string_view action) const;
    /// The modifier's button (Modifier's first binding), if there's one.
    std::optional<PadButton> modifier() const;
};

/// Every action a pad can be bound to, as the file names them: the game's controls in their
/// table's order, then the pad's own.
std::span<const std::string_view> padActionNames();
/// Whether `name` is an action (a game control or a pad action).
bool knownAction(std::string_view name);

/// The defaults: the settings, and the bindings by the default layout (left stick moves, RT
/// attacks, A interacts, X Y B RB LB the five slots, L3 jumps, R3 nothing alone, the D-pad's up
/// and down the potions, its left and right the spell sets, View the inventory, Start the game's
/// menu; with LT held: Up cure poison, Down swap weapons, View the spellbook, Start the map, Right
/// chat, Left team chat, R3 the console; in the
/// menus A accepts, B and Start go back, LB / RB the tabs).
PadConfig defaultPadConfig();

/// Reads the file's text over the defaults (an action the text names gets the text's buttons in
/// place of its defaults); never fails: what isn't understood is kept in otherLines.
PadConfig parsePadConfig(std::string_view text);
/// The file's text: a short header, the settings, every action's lines (None for an action with
/// no button), then otherLines. parsePadConfig gives back the same config.
std::string formatPadConfig(const PadConfig& config);
/// The bindings in the actions' order (padActionNames), each action's kept as they are: the order
/// parsePadConfig gives them in (so an edited config compares equal to its file read back).
void orderBindings(PadConfig& config);

/// The game's code for a pad button on a layer, as InputBindings matches it with the keys' and
/// mouse buttons' (0x40000 up: no name in nox.cfg, so it never reaches that file).
inline constexpr std::uint32_t kPadCodeBase = 0x40000;
constexpr std::uint32_t padCode(PadButton b, bool modifierLayer)
{
    return kPadCodeBase | (modifierLayer ? 0x100u : 0u) | static_cast<std::uint32_t>(b);
}
constexpr bool isPadCode(std::uint32_t code)
{
    return (code & ~0x1FFu) == kPadCodeBase;
}

/// What a pad button does, from a PadConfig: the game's controls go to InputBindings under pad codes
/// (gameBindings), the pad's own actions are told to the caller. press() takes the button's layer
/// (the modifier held or not) and keeps it till release() (see the file's header above).
class PadRouter {
public:
    PadRouter() { configure(defaultPadConfig()); }
    explicit PadRouter(const PadConfig& config) { configure(config); }

    /// Builds the tables from `config`; forgets what's held.
    void configure(const PadConfig& config);
    /// The game controls' bindings under pad codes, one key each (for InputBindings::setPadBindings).
    const std::vector<nox::config::Binding>& gameBindings() const { return gameBindings_; }
    std::optional<PadButton> modifier() const { return modifier_; }

    /// A press in a game, or its release.
    struct Edge {
        std::uint32_t code = 0;   ///< the pad code for InputBindings (0: none, no game control on it)
        PadActionSet actions;     ///< the pad's own (game) actions on it
        bool modifierLayer = false; ///< the layer it was taken on
        bool isModifier = false;  ///< the button is the modifier itself
    };
    /// `b` went down in a game, with the modifier held or not: its layer chosen and kept.
    Edge press(PadButton b, bool modifierHeld);
    /// `b` came up: what its press gave (nothing if its press didn't come here, or was dropped).
    Edge release(PadButton b);
    /// Forgets every press (InputBindings::releaseAll's moments): their releases give nothing.
    void dropAll();
    /// Whether a press giving `a` is held (pad actions).
    bool held(PadAction a) const;
    /// Whether `b` is held as this router saw it (its press here, not dropped).
    bool pressed(PadButton b) const { return latched_[static_cast<std::size_t>(b)] != kNone; }

    /// The menus' actions on `b` (MenuAccept, MenuBack, the tabs): no layer, nothing kept.
    PadActionSet menuActions(PadButton b) const { return menu_[static_cast<std::size_t>(b)]; }

private:
    static constexpr std::uint8_t kNone = 0xFF;
    struct Layer {
        bool game = false;    ///< a game control is bound here
        PadActionSet actions; ///< pad actions (the game's)
        bool any() const { return game || !actions.empty(); }
    };
    std::array<std::array<Layer, 2>, kPadButtonCount> layers_{};
    std::array<PadActionSet, kPadButtonCount> menu_{};
    std::array<std::uint8_t, kPadButtonCount> latched_{}; ///< the layer a held press took, kNone: none
    std::optional<PadButton> modifier_;
    std::vector<nox::config::Binding> gameBindings_;
};

} // namespace noxsdl::pad
