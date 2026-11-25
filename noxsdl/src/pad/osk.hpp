// (noxcpp) The game controller's on-screen keyboard: its layouts, the moves between its keys and
// what its keys and the pad's buttons type (pad/osk_game.cpp puts it on the screen and sends what it
// types on as the keyboard's typing). No SDL here.
//
//   layouts     by the interface's language: Latin (English and the rest), Cyrillic (ЙЦУКЕН) for
//               Russian, Korean's two-set (Dubeolsik) jamo composed into syllables (pad/hangul.hpp).
//               Each has the digits row (shifted: punctuation), three rows of letters and
//               punctuation, and a row of Shift, the other script (Latin from Cyrillic or Korean, and
//               back), Space, Delete, Enter, Quick chat (in a game's chat) and Close.
//   moving      the D-pad / stick: left and right along a row, round to its other end; up and down
//               to the key of the next row nearest across (rows are centred, of different lengths),
//               round from the top row to the bottom one.
//   the pad     A types the key, X deletes, Y a space, LT held shifts (the Shift key: the next key
//               only), L3 caps lock (letters only), Start enters, B closes, LB / RB the quick-chat
//               list beside the keys (up / down choose, A sends).
#pragma once

#include "pad/hangul.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noxsdl::pad {

enum class OskScript : std::uint8_t { Latin, Cyrillic, Hangul };
/// The layout for an interface language ("ru": Cyrillic, "ko": Hangul, the rest Latin).
OskScript oskScriptFor(std::string_view language);

enum class OskKeyKind : std::uint8_t { Char, Shift, Switch, Space, Backspace, Enter, Quick, Close };

struct OskKey {
    OskKeyKind kind = OskKeyKind::Char;
    std::string_view normal;  ///< what it types (UTF-8), Char keys
    std::string_view shifted; ///< with Shift (empty: the same)
    bool letter = false;      ///< caps lock shifts it
    int width = 2;            ///< in half keys
};

struct OskLayout {
    OskScript script = OskScript::Latin;
    std::vector<std::vector<OskKey>> rows;
};

/// A script's layout; `other`: the script its Switch key goes to (none: no Switch key); `quick`: a
/// Quick chat key.
OskLayout oskLayout(OskScript script, std::optional<OskScript> other, bool quick);

struct OskPos {
    int row = 0;
    int col = 0;
    bool operator==(const OskPos&) const = default;
};
enum class OskDir : std::uint8_t { Up, Down, Left, Right };

/// The widest row, in half keys.
int oskWidth(const OskLayout& layout);
/// A key's span across the keyboard in half keys from its left edge, [first, end) (rows centred).
std::pair<int, int> oskKeySpan(const OskLayout& layout, OskPos at);
/// The key one step `dir` from `at`, round the ends.
OskPos oskMove(const OskLayout& layout, OskPos at, OskDir dir);
/// The first key of a kind (the special row's), if the layout has one.
std::optional<OskPos> oskFind(const OskLayout& layout, OskKeyKind kind);

/// Where the keyboard's typing goes (the field, the chat line, the console: osk_game.cpp).
class OskSink {
public:
    virtual ~OskSink() = default;
    virtual void text(std::string_view utf8) = 0;
    virtual void backspace() = 0;
    virtual void enter() = 0;
    /// B or the Close key: the keyboard closing (the caller says what else goes).
    virtual void close() = 0;
    /// A quick-chat line chosen (its index).
    virtual void quick(std::size_t index) = 0;
};

/// What the pad asks of the keyboard.
enum class OskInput : std::uint8_t {
    Up, Down, Left, Right,
    Press,     ///< A: the key, or the quick-chat line
    Backspace, ///< X
    Space,     ///< Y
    Enter,     ///< Start
    Close,     ///< B
    Pane,      ///< LB / RB: the keys or the quick-chat list
    CapsLock,  ///< L3
};

class Osk {
public:
    /// Open on `native`'s layout (with a Switch key to Latin when it isn't Latin), and a quick-chat
    /// list of `quickCount` lines (0: none).
    void open(OskScript native, std::size_t quickCount);
    void close();
    bool isOpen() const { return open_; }

    /// The pad's input; `shiftHeld`: LT is held.
    void input(OskInput in, bool shiftHeld, OskSink& sink);
    /// The syllable being composed typed (Korean), e.g. before the keyboard closes.
    void flush(OskSink& sink);
    /// The syllable being composed dropped.
    void dropComposing() { hangul_.clear(); }

    const OskLayout& layout() const { return layout_; }
    OskPos pos() const { return pos_; }
    const OskKey& key(OskPos at) const;
    OskScript script() const { return layout_.script; }
    OskScript native() const { return native_; }
    bool quickPane() const { return quickPane_; }
    std::size_t quickCount() const { return quickCount_; }
    std::size_t quickIndex() const { return quickIndex_; }
    bool capsLock() const { return caps_; }
    bool shiftOnce() const { return shiftOnce_; }
    /// What a Char key types now (its shifted text with Shift / LT, letters with caps lock).
    std::string_view typed(const OskKey& k, bool shiftHeld) const;
    /// The syllable being composed (UTF-8; empty when none).
    std::string preedit() const { return hangul_.preedit(); }

private:
    void press(const OskKey& k, bool shiftHeld, OskSink& sink);
    void layoutTo(OskScript script);

    bool open_ = false;
    OskScript native_ = OskScript::Latin;
    OskLayout layout_;
    OskPos pos_;
    bool caps_ = false;
    bool shiftOnce_ = false;
    bool quickPane_ = false;
    std::size_t quickCount_ = 0;
    std::size_t quickIndex_ = 0;
    HangulComposer hangul_;
};

} // namespace noxsdl::pad
