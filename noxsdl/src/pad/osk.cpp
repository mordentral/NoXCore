// (noxcpp) The game controller's on-screen keyboard: layouts, moves, typing (pad/osk.hpp).
#include "pad/osk.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace noxsdl::pad {

namespace {

/// The space-separated parts of a text (views into it: the tables below are static).
std::vector<std::string_view> words(std::string_view s)
{
    std::vector<std::string_view> out;
    while (!s.empty()) {
        const std::size_t sp = s.find(' ');
        if (sp != 0) {
            out.push_back(s.substr(0, sp));
        }
        if (sp == std::string_view::npos) {
            break;
        }
        s.remove_prefix(sp + 1);
    }
    return out;
}

/// A row of Char keys: `normal` and `shifted` space-separated, the first `letters` of them letters
/// (caps lock shifts them).
std::vector<OskKey> charRow(std::string_view normal, std::string_view shifted, std::size_t letters)
{
    const auto n = words(normal);
    const auto s = words(shifted);
    std::vector<OskKey> row;
    row.reserve(n.size());
    for (std::size_t i = 0; i < n.size(); ++i) {
        OskKey k;
        k.normal = n[i];
        k.shifted = i < s.size() && s[i] != n[i] ? s[i] : std::string_view{};
        k.letter = i < letters;
        row.push_back(k);
    }
    return row;
}

OskKey special(OskKeyKind kind, int width)
{
    OskKey k;
    k.kind = kind;
    k.width = width;
    return k;
}

int rowWidth(const std::vector<OskKey>& row)
{
    int w = 0;
    for (const OskKey& k : row) {
        w += k.width;
    }
    return w;
}

} // namespace

OskScript oskScriptFor(std::string_view language)
{
    const std::string_view base = language.substr(0, language.find_first_of("-_"));
    if (base == "ru") {
        return OskScript::Cyrillic;
    }
    if (base == "ko") {
        return OskScript::Hangul;
    }
    return OskScript::Latin;
}

OskLayout oskLayout(OskScript script, std::optional<OskScript> other, bool quick)
{
    OskLayout l;
    l.script = script;
    switch (script) {
    case OskScript::Latin:
        l.rows.push_back(charRow("1 2 3 4 5 6 7 8 9 0 - =", "! @ # $ % ^ & * ( ) _ +", 0));
        l.rows.push_back(charRow("q w e r t y u i o p [ ]", "Q W E R T Y U I O P { }", 10));
        l.rows.push_back(charRow("a s d f g h j k l ; '", "A S D F G H J K L : \"", 9));
        l.rows.push_back(charRow("z x c v b n m , . ? !", "Z X C V B N M < > / \\", 7));
        break;
    case OskScript::Cyrillic:
        l.rows.push_back(charRow("1 2 3 4 5 6 7 8 9 0 - =", "! \" № ; % : ? * ( ) _ +", 0));
        l.rows.push_back(charRow("й ц у к е н г ш щ з х ъ", "Й Ц У К Е Н Г Ш Щ З Х Ъ", 12));
        l.rows.push_back(charRow("ф ы в а п р о л д ж э", "Ф Ы В А П Р О Л Д Ж Э", 11));
        l.rows.push_back(charRow("я ч с м и т ь б ю ё . ,", "Я Ч С М И Т Ь Б Ю Ё . ,", 10));
        break;
    case OskScript::Hangul:
        // Shift gives the tense consonants and ㅒ ㅖ, as on the two-set keyboard.
        l.rows.push_back(charRow("1 2 3 4 5 6 7 8 9 0 - =", "! @ # $ % ^ & * ( ) _ +", 0));
        l.rows.push_back(charRow("ㅂ ㅈ ㄷ ㄱ ㅅ ㅛ ㅕ ㅑ ㅐ ㅔ", "ㅃ ㅉ ㄸ ㄲ ㅆ ㅛ ㅕ ㅑ ㅒ ㅖ", 0));
        l.rows.push_back(charRow("ㅁ ㄴ ㅇ ㄹ ㅎ ㅗ ㅓ ㅏ ㅣ", "ㅁ ㄴ ㅇ ㄹ ㅎ ㅗ ㅓ ㅏ ㅣ", 0));
        l.rows.push_back(charRow("ㅋ ㅌ ㅊ ㅍ ㅠ ㅜ ㅡ , . ? !", "ㅋ ㅌ ㅊ ㅍ ㅠ ㅜ ㅡ < > / \\", 0));
        break;
    }
    // (two keys wide each, Space four: room for their names in every language)
    std::vector<OskKey> bottom;
    bottom.push_back(special(OskKeyKind::Shift, 4));
    if (other) {
        bottom.push_back(special(OskKeyKind::Switch, 4));
    }
    bottom.push_back(special(OskKeyKind::Space, 8));
    bottom.push_back(special(OskKeyKind::Backspace, 4));
    bottom.push_back(special(OskKeyKind::Enter, 4));
    if (quick) {
        bottom.push_back(special(OskKeyKind::Quick, 4));
    }
    bottom.push_back(special(OskKeyKind::Close, 4));
    l.rows.push_back(std::move(bottom));
    return l;
}

int oskWidth(const OskLayout& layout)
{
    int w = 0;
    for (const auto& row : layout.rows) {
        w = std::max(w, rowWidth(row));
    }
    return w;
}

std::pair<int, int> oskKeySpan(const OskLayout& layout, OskPos at)
{
    const auto& row = layout.rows[static_cast<std::size_t>(at.row)];
    int x = (oskWidth(layout) - rowWidth(row)) / 2;
    for (int i = 0; i < at.col; ++i) {
        x += row[static_cast<std::size_t>(i)].width;
    }
    return {x, x + row[static_cast<std::size_t>(at.col)].width};
}

OskPos oskMove(const OskLayout& layout, OskPos at, OskDir dir)
{
    const int rows = static_cast<int>(layout.rows.size());
    if (rows == 0) {
        return at;
    }
    const auto count = [&](int r) { return static_cast<int>(layout.rows[static_cast<std::size_t>(r)].size()); };
    switch (dir) {
    case OskDir::Left:
        return {at.row, (at.col + count(at.row) - 1) % count(at.row)};
    case OskDir::Right:
        return {at.row, (at.col + 1) % count(at.row)};
    case OskDir::Up:
    case OskDir::Down: {
        const int row = (at.row + (dir == OskDir::Down ? 1 : rows - 1)) % rows;
        const auto [a0, a1] = oskKeySpan(layout, at);
        const int mid = a0 + a1; // (doubled: whole numbers)
        int best = 0;
        int bestD = std::numeric_limits<int>::max();
        for (int c = 0; c < count(row); ++c) {
            const auto [b0, b1] = oskKeySpan(layout, {row, c});
            const int d = std::abs(b0 + b1 - mid);
            if (d < bestD) { // (a tie: the one on the left)
                bestD = d;
                best = c;
            }
        }
        return {row, best};
    }
    }
    return at;
}

std::optional<OskPos> oskFind(const OskLayout& layout, OskKeyKind kind)
{
    for (std::size_t r = 0; r < layout.rows.size(); ++r) {
        for (std::size_t c = 0; c < layout.rows[r].size(); ++c) {
            if (layout.rows[r][c].kind == kind) {
                return OskPos{static_cast<int>(r), static_cast<int>(c)};
            }
        }
    }
    return std::nullopt;
}

void Osk::open(OskScript native, std::size_t quickCount)
{
    open_ = true;
    native_ = native;
    quickCount_ = quickCount;
    quickIndex_ = 0;
    quickPane_ = false;
    caps_ = false;
    shiftOnce_ = false;
    hangul_.clear();
    layoutTo(native);
    pos_ = {1, 0}; // (the first letter)
}

void Osk::close()
{
    open_ = false;
    hangul_.clear();
    quickPane_ = false;
}

const OskKey& Osk::key(OskPos at) const
{
    return layout_.rows[static_cast<std::size_t>(at.row)][static_cast<std::size_t>(at.col)];
}

void Osk::layoutTo(OskScript script)
{
    std::optional<OskScript> other;
    if (script != native_) {
        other = native_;
    } else if (native_ != OskScript::Latin) {
        other = OskScript::Latin;
    }
    layout_ = oskLayout(script, other, quickCount_ > 0);
    pos_.row = std::clamp(pos_.row, 0, static_cast<int>(layout_.rows.size()) - 1);
    pos_.col = std::clamp(pos_.col, 0, static_cast<int>(layout_.rows[static_cast<std::size_t>(pos_.row)].size()) - 1);
}

std::string_view Osk::typed(const OskKey& k, bool shiftHeld) const
{
    bool shift = shiftHeld || shiftOnce_;
    if (k.letter && caps_) {
        shift = !shift;
    }
    return shift && !k.shifted.empty() ? k.shifted : k.normal;
}

void Osk::flush(OskSink& sink)
{
    const std::string done = hangul_.flush();
    if (!done.empty()) {
        sink.text(done);
    }
}

void Osk::press(const OskKey& k, bool shiftHeld, OskSink& sink)
{
    switch (k.kind) {
    case OskKeyKind::Char: {
        const std::string_view t = typed(k, shiftHeld);
        shiftOnce_ = false;
        const char32_t c = firstCodePoint(t);
        if (layout_.script == OskScript::Hangul && HangulComposer::isJamo(c)) {
            const std::string done = hangul_.input(c);
            if (!done.empty()) {
                sink.text(done);
            }
            return;
        }
        flush(sink);
        sink.text(t);
        return;
    }
    case OskKeyKind::Shift:
        shiftOnce_ = !shiftOnce_;
        return;
    case OskKeyKind::Switch:
        flush(sink);
        layoutTo(layout_.script == native_ ? OskScript::Latin : native_);
        if (const auto at = oskFind(layout_, OskKeyKind::Switch)) {
            pos_ = *at;
        }
        return;
    case OskKeyKind::Space:
        input(OskInput::Space, shiftHeld, sink);
        return;
    case OskKeyKind::Backspace:
        input(OskInput::Backspace, shiftHeld, sink);
        return;
    case OskKeyKind::Enter:
        input(OskInput::Enter, shiftHeld, sink);
        return;
    case OskKeyKind::Quick:
        input(OskInput::Pane, shiftHeld, sink);
        return;
    case OskKeyKind::Close:
        input(OskInput::Close, shiftHeld, sink);
        return;
    }
}

void Osk::input(OskInput in, bool shiftHeld, OskSink& sink)
{
    if (!open_) {
        return;
    }
    switch (in) {
    case OskInput::Up:
    case OskInput::Down:
    case OskInput::Left:
    case OskInput::Right:
        if (quickPane_) {
            if (in == OskInput::Up) {
                quickIndex_ = (quickIndex_ + quickCount_ - 1) % quickCount_;
            } else if (in == OskInput::Down) {
                quickIndex_ = (quickIndex_ + 1) % quickCount_;
            } else {
                quickPane_ = false; // (left or right: back to the keys)
            }
            return;
        }
        pos_ = oskMove(layout_, pos_, in == OskInput::Up     ? OskDir::Up
                                      : in == OskInput::Down ? OskDir::Down
                                      : in == OskInput::Left ? OskDir::Left
                                                             : OskDir::Right);
        return;
    case OskInput::Press:
        if (quickPane_) {
            sink.quick(quickIndex_);
            return;
        }
        press(key(pos_), shiftHeld, sink);
        return;
    case OskInput::Backspace:
        if (!hangul_.backspace()) {
            sink.backspace();
        }
        return;
    case OskInput::Space:
        flush(sink);
        shiftOnce_ = false;
        sink.text(" ");
        return;
    case OskInput::Enter:
        flush(sink);
        sink.enter();
        return;
    case OskInput::Close:
        sink.close();
        return;
    case OskInput::Pane:
        if (quickCount_ > 0) {
            quickPane_ = !quickPane_;
        }
        return;
    case OskInput::CapsLock:
        caps_ = !caps_;
        return;
    }
}

} // namespace noxsdl::pad
