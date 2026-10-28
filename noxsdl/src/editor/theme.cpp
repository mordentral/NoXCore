// The editors' themes by name (theme.hpp).
#include "editor/theme.hpp"

#include "noxcore/util/text.hpp"

#include <string>

namespace noxsdl::editor {

const char* themeKey(Theme theme)
{
    switch (theme) {
    case Theme::Medium: return "medium";
    case Theme::Light: return "light";
    default: return "dark";
    }
}

const char* themeLabel(Theme theme)
{
    switch (theme) {
    case Theme::Medium: return "Medium";
    case Theme::Light: return "Light";
    default: return "Dark";
    }
}

bool themeFromKey(std::string_view key, Theme& out)
{
    const std::string k = nox::util::lowered(key);
    for (const Theme t : {Theme::Dark, Theme::Medium, Theme::Light}) {
        if (k == themeKey(t)) {
            out = t;
            return true;
        }
    }
    return false;
}

} // namespace noxsdl::editor
