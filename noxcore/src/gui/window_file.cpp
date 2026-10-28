// The window file (.wnd) parser, following the game's loader token by token; the format is in
// window_file.hpp.
#include "noxcore/gui/window_file.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace nox::gui {

namespace {

constexpr std::string_view kSpace = " \t\n\r"; // the loader's strtok delimiters
constexpr std::string_view kLineBreaks = "\t\n\r";

// STATUS and STYLE names in bit order: name n sets bit n (see the status / style namespaces).
constexpr std::array<std::string_view, 16> kStatusNames{
    "ACTIVE", "TOGGLE",  "DRAGABLE",  "ENABLED", "HIDDEN",      "ABOVE",    "BELOW",   "IMAGE",
    "TABSTOP", "NOINPUT", "NOFOCUS", "DESTROYED", "BORDER", "SMOOTH_TEXT", "ONE_LINE", "NO_FLUSH"};
constexpr std::array<std::string_view, 13> kStyleNames{
    "PUSHBUTTON", "RADIOBUTTON", "CHECKBOX", "VERTSLIDER", "HORZSLIDER", "SCROLLLISTBOX", "FADELISTBOX",
    "ENTRYFIELD", "MOUSETRACK",  "ANIMATED", "TABSTOP",    "STATICTEXT", "PROGRESSBAR"};

bool isSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

/// strtok over a copy of a string.
class Tokens {
public:
    explicit Tokens(std::string_view s) : s_(s) {}

    std::optional<std::string_view> next(std::string_view delims)
    {
        while (pos_ < s_.size() && delims.find(s_[pos_]) != std::string_view::npos) {
            ++pos_;
        }
        if (pos_ >= s_.size()) {
            return std::nullopt;
        }
        const std::size_t start = pos_;
        while (pos_ < s_.size() && delims.find(s_[pos_]) == std::string_view::npos) {
            ++pos_;
        }
        const std::string_view t = s_.substr(start, pos_ - start);
        if (pos_ < s_.size()) {
            ++pos_; // strtok overwrites the delimiter that ends the token
        }
        return t;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;
};

/// atoi: optional whitespace and sign, then digits; 0 if there are none (or no token).
std::int32_t toInt(std::optional<std::string_view> t)
{
    if (!t) {
        return 0;
    }
    std::size_t i = 0;
    while (i < t->size() && isSpace((*t)[i])) {
        ++i;
    }
    bool negative = false;
    if (i < t->size() && ((*t)[i] == '-' || (*t)[i] == '+')) {
        negative = (*t)[i] == '-';
        ++i;
    }
    std::int64_t v = 0;
    while (i < t->size() && std::isdigit(static_cast<unsigned char>((*t)[i]))) {
        v = v * 10 + ((*t)[i] - '0');
        v = std::min<std::int64_t>(v, 0x7FFFFFFF);
        ++i;
    }
    return static_cast<std::int32_t>(negative ? -v : v);
}

using util::equalsNoCase;

/// set_bitmask_flags_from_plus_separated_names: "NULL" is none; names are matched
/// case-insensitively, unknown ones ignored.
template <std::size_t N>
std::uint32_t flags(std::string_view value, const std::array<std::string_view, N>& names)
{
    std::uint32_t out = 0;
    if (value.substr(0, 4) == "NULL") {
        return out;
    }
    Tokens t(value);
    while (const auto name = t.next("+")) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (equalsNoCase(*name, names[i])) {
                out |= 1u << i;
                break;
            }
        }
    }
    return out;
}

/// "TRANSPARENT", or three numbers.
Color color(std::string_view value)
{
    Color c;
    if (value == "TRANSPARENT") {
        c.transparent = true;
        return c;
    }
    Tokens t(value);
    c.r = static_cast<std::uint8_t>(toInt(t.next(kSpace)));
    c.g = static_cast<std::uint8_t>(toInt(t.next(kSpace)));
    c.b = static_cast<std::uint8_t>(toInt(t.next(kSpace)));
    return c;
}

/// An image property (friends): NULL is none.
std::string image(std::string_view value)
{
    Tokens t(value);
    const auto name = t.next(kLineBreaks);
    if (!name || *name == "NULL") {
        return {};
    }
    return std::string(*name);
}

struct Defaults {
    Color enabled, disabled, hilite, selected, text, background;
    std::string font;
};

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    WindowDef file()
    {
        // The file's top level: default colours and font up to the first WINDOW, which is the
        // file's root window (anything after it isn't read).
        while (const auto tok = token()) {
            if (*tok == "END") {
                break;
            }
            if (setDefault(*tok)) {
                continue;
            }
            if (*tok == "BACKGROUNDCOLOR") {
                skipToken();
                defaults_.background = color(value());
            } else if (*tok == "FONT") {
                skipToken();
                defaults_.font = value();
            } else if (*tok == "WINDOW") {
                return window();
            }
        }
        throw FormatError("window file: no WINDOW");
    }

private:
    /// fscanf("%s"): the next whitespace-separated token, or nullopt at the end.
    std::optional<std::string> token()
    {
        while (pos_ < s_.size() && isSpace(s_[pos_])) {
            ++pos_;
        }
        if (pos_ >= s_.size()) {
            return std::nullopt;
        }
        const std::size_t start = pos_;
        while (pos_ < s_.size() && !isSpace(s_[pos_])) {
            ++pos_;
        }
        lastToken_ = std::string(s_.substr(start, pos_ - start));
        return lastToken_;
    }

    void skipToken() { token(); } // fscanf("%*s"): the "="

    /// up to the next ';' (dropped), leading whitespace skipped, the rest of the
    /// whitespace turned into spaces; at most 255 characters.
    std::string value()
    {
        std::string out;
        bool leading = true;
        while (pos_ < s_.size() && out.size() < 255) {
            const char c = s_[pos_++];
            if (isSpace(c)) {
                if (!leading) {
                    out.push_back(' ');
                }
                continue;
            }
            leading = false;
            if (c == ';') {
                return out;
            }
            out.push_back(c);
        }
        return out;
    }

    /// The colour defaults that can change between windows.
    bool setDefault(std::string_view tok)
    {
        Color* target = tok == "ENABLEDCOLOR"    ? &defaults_.enabled
                        : tok == "DISABLEDCOLOR" ? &defaults_.disabled
                        : tok == "HILITECOLOR"   ? &defaults_.hilite
                        : tok == "SELECTEDCOLOR" ? &defaults_.selected
                        : tok == "TEXTCOLOR"     ? &defaults_.text
                                                 : nullptr;
        if (!target) {
            return false;
        }
        skipToken();
        *target = color(value());
        return true;
    }

    WindowDef window()
    {
        // One WINDOW ... END: starts from the defaults in force, reads its header line
        // ("id x y width height type ...;"), then properties, DATA and CHILD blocks until END.
        WindowDef w;
        w.enabledColor = defaults_.enabled;
        w.disabledColor = defaults_.disabled;
        w.hiliteColor = defaults_.hilite;
        w.selectedColor = defaults_.selected;
        w.textColor = defaults_.text;
        w.backgroundColor = defaults_.background;
        w.font = defaults_.font;
        {
            const std::string header = value();
            Tokens t(header);
            w.id = toInt(t.next(kSpace));
            w.x = toInt(t.next(kSpace));
            w.y = toInt(t.next(kSpace));
            w.width = toInt(t.next(kSpace));
            w.height = toInt(t.next(kSpace));
            if (const auto type = t.next(kSpace)) {
                w.typeName = std::string(type->substr(0, 63));
            }
            w.type = windowType(w.typeName);
        }
        while (true) {
            auto tok = token();
            if (!tok) {
                // At the end of the file fscanf fails and leaves the last token in the buffer:
                // when that was the END of the children, the window ends too. sanchlp.wnd and
                // lsanchlp.wnd lack their last END and load that way.
                if (lastToken_ != "END") {
                    throw FormatError("window file: ends inside window " + std::to_string(w.id));
                }
                tok = lastToken_;
            }
            if (property(w, *tok)) {
                continue;
            }
            if (*tok == "DATA") {
                skipToken();
                data(w, value());
            } else if (*tok == "END") {
                return w;
            } else if (*tok == "CHILD") {
                children(w);
            } else {
                value(); // unknown: its value is skipped
            }
        }
    }

    void children(WindowDef& parent)
    {
        // windows until END; colour defaults may change in between.
        while (const auto tok = token()) {
            if (*tok == "END") {
                return;
            }
            if (setDefault(*tok)) {
                continue;
            }
            if (*tok == "WINDOW") {
                parent.children.push_back(window());
            }
        }
    }

    /// The property table at parseWindowFuncs.
    bool property(WindowDef& w, std::string_view tok)
    {
        using Setter = void (*)(WindowDef&, std::string_view);
        struct Entry {
            std::string_view name;
            Setter set;
        };
        static constexpr Entry kProperties[] = {
            {"STATUS", [](WindowDef& d, std::string_view v) { d.status = flags(v, kStatusNames); }},
            {"STYLE", [](WindowDef& d, std::string_view v) { d.style = flags(v, kStyleNames); }},
            {"GROUP", [](WindowDef& d, std::string_view v) { d.group = toInt(Tokens(v).next(kLineBreaks)); }},
            {"BACKGROUNDCOLOR", [](WindowDef& d, std::string_view v) { d.backgroundColor = color(v); }},
            {"BACKGROUNDIMAGE", [](WindowDef& d, std::string_view v) { d.backgroundImage = image(v); }},
            {"ENABLEDCOLOR", [](WindowDef& d, std::string_view v) { d.enabledColor = color(v); }},
            {"ENABLEDIMAGE", [](WindowDef& d, std::string_view v) { d.enabledImage = image(v); }},
            {"DISABLEDCOLOR", [](WindowDef& d, std::string_view v) { d.disabledColor = color(v); }},
            {"DISABLEDIMAGE", [](WindowDef& d, std::string_view v) { d.disabledImage = image(v); }},
            {"HILITECOLOR", [](WindowDef& d, std::string_view v) { d.hiliteColor = color(v); }},
            {"HILITEIMAGE", [](WindowDef& d, std::string_view v) { d.hiliteImage = image(v); }},
            {"SELECTEDCOLOR", [](WindowDef& d, std::string_view v) { d.selectedColor = color(v); }},
            {"SELECTEDIMAGE", [](WindowDef& d, std::string_view v) { d.selectedImage = image(v); }},
            {"IMAGEOFFSET",
             [](WindowDef& d, std::string_view v) {
                 Tokens t(v);
                 d.imageOffsetX = toInt(t.next(kSpace));
                 d.imageOffsetY = toInt(t.next(kSpace));
             }},
            {"TEXTCOLOR", [](WindowDef& d, std::string_view v) { d.textColor = color(v); }},
            {"TEXT", [](WindowDef& d, std::string_view v) { d.text = std::string(v); }},
            {"FONT", [](WindowDef& d, std::string_view v) { d.font = std::string(v); }},
            {"TOOLTIP", [](WindowDef& d, std::string_view v) { d.tooltip = std::string(v); }},
        };
        // A known keyword: skip its "=", read its value up to the ";" and hand it to the setter.
        for (const auto& p : kProperties) {
            if (p.name == tok) {
                skipToken();
                p.set(w, value());
                return true;
            }
        }
        return false;
    }

    /// DATA by type (w.type: its header's type name, windowType).
    static void data(WindowDef& w, std::string_view v)
    {
        // Fields not given read as 0 (atoi of nothing), except the entry field's optional ones.
        Tokens t(v);
        const auto next = [&] { return toInt(t.next(kSpace)); };
        switch (w.type) {
        case WindowType::VertSlider:
        case WindowType::HorzSlider: {
            SliderData d;
            d.min = next();
            d.max = next();
            w.data = d;
            break;
        }
        case WindowType::ScrollListBox: {
            ListBoxData d;
            d.lines = static_cast<std::uint16_t>(next());
            d.lineHeight = static_cast<std::uint16_t>(next());
            for (auto& x : d.values) {
                x = next();
            }
            w.data = d;
            break;
        }
        case WindowType::EntryField: {
            EntryFieldData d;
            d.maxLength = static_cast<std::uint16_t>(next());
            if (const auto width = t.next(kSpace)) {
                d.width = static_cast<std::uint16_t>(toInt(width));
            }
            if (const auto password = t.next(kSpace)) {
                d.password = toInt(password) != 0;
            }
            if (const auto mode = t.next(kSpace)) {
                d.inputMode = toInt(mode);
            }
            w.data = d;
            break;
        }
        case WindowType::StaticText: {
            StaticTextData d;
            d.center = next() != 0;
            d.glow = next() != 0;
            if (const auto text = t.next(kSpace)) {
                d.text = std::string(*text);
            }
            w.data = d;
            break;
        }
        case WindowType::RadioButton:
            w.data = RadioButtonData{next()};
            break;
        default:
            break;
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::string lastToken_; ///< what fscanf's buffer holds
    Defaults defaults_; // black, no font
};

} // namespace

// The type names the loader knows, matched exactly (upper case).
WindowType windowType(std::string_view name) noexcept
{
    struct Entry {
        std::string_view name;
        WindowType type;
    };
    static constexpr Entry kTypes[] = {
        {"USER", WindowType::User},
        {"PUSHBUTTON", WindowType::PushButton},
        {"RADIOBUTTON", WindowType::RadioButton},
        {"CHECKBOX", WindowType::CheckBox},
        {"VERTSLIDER", WindowType::VertSlider},
        {"HORZSLIDER", WindowType::HorzSlider},
        {"SCROLLLISTBOX", WindowType::ScrollListBox},
        {"ENTRYFIELD", WindowType::EntryField},
        {"STATICTEXT", WindowType::StaticText},
        {"PROGRESSBAR", WindowType::ProgressBar},
    };
    for (const auto& e : kTypes) {
        if (e.name == name) {
            return e.type;
        }
    }
    return WindowType::Unknown;
}

const WindowDef* WindowDef::find(std::int32_t wanted) const
{
    if (id == wanted) {
        return this;
    }
    for (const auto& c : children) {
        if (const auto* hit = c.find(wanted)) {
            return hit;
        }
    }
    return nullptr;
}

WindowDef parseWindowFile(std::string_view text)
{
    return Parser(text).file();
}

WindowDef loadWindowFile(const std::filesystem::path& path)
{
    const auto bytes = readFileBytes(path);
    return parseWindowFile(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool usesLargeFont(std::string_view font, bool shellUp) noexcept
{
    if (font.empty()) {
        return shellUp;
    }
    // (guiFontPtrByName compares the names without case.)
    constexpr std::string_view kLarge = "large";
    return font.size() == kLarge.size() && std::equal(font.begin(), font.end(), kLarge.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
           });
}

} // namespace nox::gui
