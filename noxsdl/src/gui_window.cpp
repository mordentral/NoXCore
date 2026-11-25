// The window system's windows (gui.hpp): built from a .wnd definition, shown and hidden, placed,
// telling their parent. Apart from the drawing (gui.cpp), so the pad's navigation tests build
// windows without a renderer.
#include "gui.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl::gui {

namespace st = nox::gui::status;

namespace {

// client_gui_flag (setShellUp / shellUp).
bool shellFlag = false;

} // namespace

bool typeInto(Window& field, std::string_view utf8)
{
    bool typed = false;
    for (const char16_t c : nox::strings::fromUtf8(utf8)) {
        // Printable characters only, one fewer than the field's limit as the game's edit box
        // (its last place is the terminator: 8 for the login's 9, 9 for the nick's 10).
        if (c >= 0x20 && field.entry.size() + 1 < field.entryMax) {
            field.entry.push_back(c);
            typed = true;
        }
    }
    return typed;
}

bool eraseLast(Window& field)
{
    if (field.entry.empty()) {
        return false;
    }
    field.entry.pop_back();
    return true;
}

int fontLine(const Window& w)
{
    return w.largeFont ? static_cast<int>(std::lround(16.0f * w.largeScale)) : 10;
}

int listLineHeight(const Window& w)
{
    const auto* d = std::get_if<nox::gui::ListBoxData>(&w.data);
    return std::max(fontLine(w), d ? static_cast<int>(d->lineHeight) : 0);
}

void setShellUp(bool up)
{
    shellFlag = up;
}

bool shellUp()
{
    return shellFlag;
}

void setDialogFonts(Window& dialog, bool large)
{
    for (const std::int32_t id : {4005, 4004}) {
        if (Window* w = dialog.find(id)) {
            w->largeFont = large;
        }
    }
}

Window::Window(const nox::gui::WindowDef& def, Window* parentWindow, const Resources& res)
    : Window(def, parentWindow, res.strings)
{
}

// Copies a .wnd definition, resolving its text and tooltip ids through the string table.
Window::Window(const nox::gui::WindowDef& def, Window* parentWindow, const nox::strings::StringTable* strings)
    : id(def.id), rect{def.x, def.y, def.width, def.height}, type(def.type), status(def.status), style(def.style),
      group(def.group), enabledColor(def.enabledColor), disabledColor(def.disabledColor),
      hiliteColor(def.hiliteColor), selectedColor(def.selectedColor), textColor(def.textColor),
      backgroundColor(def.backgroundColor), backgroundImage(def.backgroundImage), enabledImage(def.enabledImage),
      disabledImage(def.disabledImage), hiliteImage(def.hiliteImage), selectedImage(def.selectedImage),
      imageOffset{def.imageOffsetX, def.imageOffsetY}, data(def.data), parent(parentWindow)
{
    // The font: "large", else the default ("default", "small"); none at all is the large one
    // while the shell is up.
    largeFont = nox::gui::usesLargeFont(def.font, shellUp());
    const auto resolve = [&](const std::string& textId) -> std::string {
        if (textId.empty() || !strings) {
            return textId;
        }
        const auto t = strings->text(textId);
        return t ? nox::strings::toUtf8(*t) : std::string{};
    };
    text = resolve(def.text);
    tooltip = resolve(def.tooltip);
    if (auto* st = std::get_if<nox::gui::StaticTextData>(&data)) {
        text = resolve(st->text);
    }
    if (const auto* rb = std::get_if<nox::gui::RadioButtonData>(&data)) {
        radioValue = rb->value;
    }
    if (const auto* sl = std::get_if<nox::gui::SliderData>(&data)) {
        sliderValue = sl->min;
    }
    if (const auto* ef = std::get_if<nox::gui::EntryFieldData>(&data)) {
        entryMax = ef->maxLength == 0 ? 256 : std::min<std::size_t>(ef->maxLength, 256);  // 0 in the file: the 256 limit
    }
}

void Window::show(bool visibleNow)
{
    if (visibleNow) {
        status &= ~st::Hidden;
    } else {
        status |= st::Hidden;
    }
}

SDL_Point Window::screenPos() const
{
    SDL_Point p{rect.x, rect.y};
    for (const Window* w = parent; w; w = w->parent) {
        p.x += w->rect.x;
        p.y += w->rect.y;
    }
    return p;
}

Window* Window::find(std::int32_t wanted)
{
    if (id == wanted) {
        return this;
    }
    for (auto& c : children) {
        if (Window* hit = c->find(wanted)) {
            return hit;
        }
    }
    return nullptr;
}

void Window::notify(int message)
{
    if (parent && parent->proc) {
        parent->proc(*parent, message, *this);
    }
}

// Builds a window and its children from a definition, depth first.
std::unique_ptr<Window> buildWindowTree(const nox::gui::WindowDef& def, Window* parent,
                                        const nox::strings::StringTable* strings, const Proc& proc)
{
    auto w = std::make_unique<Window>(def, parent, strings);
    if (def.type == nox::gui::WindowType::User) {
        w->proc = proc; // the file's procedure for its USER windows
    }
    for (const auto& c : def.children) {
        w->children.push_back(buildWindowTree(c, w.get(), strings, proc));
    }
    return w;
}

} // namespace noxsdl::gui
