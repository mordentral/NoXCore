// Window files (the game's window\*.wnd): the layout of every menu and in-game panel as a tree
// of windows (a panel and its buttons, lists, sliders, text fields...).
//
// Plain text, read by GAME.EXE's loader like this:
//
//   ENABLEDCOLOR = 230 165 65;        default colours (and FONT) for the windows that follow
//   WINDOW
//     10168 150 40 400 300 USER (Advanced Parent);   id x y width height type, rest ignored
//     STATUS = ENABLED+ABOVE+BORDER;  properties: KEYWORD = value;
//     DATA = 0 100;                   type-specific settings
//     CHILD
//       WINDOW ... END                child windows, colour defaults may change between them
//     END
//   END
//
// Tokens are whitespace-separated (fscanf "%s"); a keyword's "=" is skipped as one token and its
// value runs to the next ';' (leading whitespace dropped, other whitespace turned into
// spaces, at most 255 characters). Unknown keywords inside a window have their value skipped;
// outside, unknown tokens are skipped one at a time. The defaults start each file as black with
// no font, and each window starts from the defaults in force when it's read.
//
// Text and tooltips are string-table ids ("servopts.wnd:Rules"), images are image names in
// video.bag / thing.bin ("UIRadioLit"), fonts are font names; this parser keeps them as named.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nox::gui {

/// STATUS flags: bit n is the nth name of the exe's table.
namespace status {
inline constexpr std::uint32_t Active = 0x1;
inline constexpr std::uint32_t Toggle = 0x2;
inline constexpr std::uint32_t Dragable = 0x4;
inline constexpr std::uint32_t Enabled = 0x8;
inline constexpr std::uint32_t Hidden = 0x10;
inline constexpr std::uint32_t Above = 0x20;
inline constexpr std::uint32_t Below = 0x40;
inline constexpr std::uint32_t Image = 0x80;
inline constexpr std::uint32_t TabStop = 0x100;
inline constexpr std::uint32_t NoInput = 0x200;
inline constexpr std::uint32_t NoFocus = 0x400;
inline constexpr std::uint32_t Destroyed = 0x800;
inline constexpr std::uint32_t Border = 0x1000;
inline constexpr std::uint32_t SmoothText = 0x2000;
inline constexpr std::uint32_t OneLine = 0x4000;
inline constexpr std::uint32_t NoFlush = 0x8000;
} // namespace status

/// STYLE flags (table). The loader also sets the one for the window's type, and
/// 0x2000 for a USER window.
namespace style {
inline constexpr std::uint32_t PushButton = 0x1;
inline constexpr std::uint32_t RadioButton = 0x2;
inline constexpr std::uint32_t CheckBox = 0x4;
inline constexpr std::uint32_t VertSlider = 0x8;
inline constexpr std::uint32_t HorzSlider = 0x10;
inline constexpr std::uint32_t ScrollListBox = 0x20;
inline constexpr std::uint32_t FadeListBox = 0x40;
inline constexpr std::uint32_t EntryField = 0x80;
inline constexpr std::uint32_t MouseTrack = 0x100;
inline constexpr std::uint32_t Animated = 0x200;
inline constexpr std::uint32_t TabStop = 0x400;
inline constexpr std::uint32_t StaticText = 0x800;
inline constexpr std::uint32_t ProgressBar = 0x1000;
inline constexpr std::uint32_t User = 0x2000;
} // namespace style

/// The window types the loader creates. Any other type name creates nothing.
enum class WindowType : std::uint8_t {
    User,
    PushButton,
    RadioButton,
    CheckBox,
    VertSlider,
    HorzSlider,
    ScrollListBox,
    EntryField,
    StaticText,
    ProgressBar,
    Unknown,
};

WindowType windowType(std::string_view name) noexcept;

/// A colour, or TRANSPARENT (the game's 0x80000000).
struct Color {
    std::uint8_t r = 0, g = 0, b = 0;
    bool transparent = false;
    bool operator==(const Color&) const = default;
};

/// DATA per type.
struct SliderData { ///< VERTSLIDER, HORZSLIDER
    std::int32_t min = 0;
    std::int32_t max = 0;
    bool operator==(const SliderData&) const = default;
};
struct ListBoxData { ///< SCROLLLISTBOX
    std::uint16_t lines = 0;      ///< how many lines it holds
    std::uint16_t lineHeight = 0; ///< raised to the font's height when created
    std::int32_t values[5]{};     ///< the rest, as read (meanings not identified yet)
    bool operator==(const ListBoxData&) const = default;
};
struct EntryFieldData { ///< ENTRYFIELD
    std::uint16_t maxLength = 0;      ///< capped at 256 when created
    std::uint16_t width = 0xFFFF;     ///< 0xFFFF when not given
    bool password = false;            ///< any non-zero value
    std::int32_t inputMode = 0;       ///< 1, 2 or 3 (character restrictions; not identified yet)
    bool operator==(const EntryFieldData&) const = default;
};
struct StaticTextData { ///< STATICTEXT
    bool center = false;
    bool glow = false;
    std::string text; ///< string-table id
    bool operator==(const StaticTextData&) const = default;
};
struct RadioButtonData { ///< RADIOBUTTON
    std::int32_t value = 0;
    bool operator==(const RadioButtonData&) const = default;
};
using WindowData =
    std::variant<std::monostate, SliderData, ListBoxData, EntryFieldData, StaticTextData, RadioButtonData>;

struct WindowDef {
    std::int32_t id = 0;
    std::int32_t x = 0, y = 0; ///< relative to the parent
    std::int32_t width = 0, height = 0;
    std::string typeName; ///< as written ("USER", "PUSHBUTTON", ...)
    WindowType type = WindowType::Unknown;
    std::uint32_t status = 0;
    std::uint32_t style = 0; ///< as written (the loader adds the type's bit)
    std::int32_t group = 0;
    Color enabledColor, disabledColor, hiliteColor, selectedColor, textColor, backgroundColor;
    /// Image names; empty for NULL or not given.
    std::string backgroundImage, enabledImage, disabledImage, hiliteImage, selectedImage;
    std::int32_t imageOffsetX = 0, imageOffsetY = 0;
    std::string font;    ///< empty: none given (see usesLargeFont)
    std::string text;    ///< string-table id
    std::string tooltip; ///< string-table id
    WindowData data;
    std::vector<WindowDef> children;

    /// The first window in this tree (itself included) with `id`, or nullptr.
    const WindowDef* find(std::int32_t id) const;
};

/// Parses a window file's text. Throws FormatError if it has no window or ends inside one.
WindowDef parseWindowFile(std::string_view text);

/// Reads and parses a window file.
WindowDef loadWindowFile(const std::filesystem::path& path);

/// Whether a window gets the "large" font: FONT "large" (any case), or no FONT at all while the
/// shell is up ("large" while client_gui_flag is set, else "default"). The
/// menus' screens draw their text large that way, the game's windows small.
bool usesLargeFont(std::string_view font, bool shellUp) noexcept;

} // namespace nox::gui
