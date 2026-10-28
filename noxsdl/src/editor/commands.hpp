// The map editor's command registry: every action has an id, a label, a menu category, a default
// shortcut the user can rebind, and a function. The menus, the toolbar, the command palette and
// the keyboard all run commands through it.
//
// No SDL here (the tests build it): shortcuts are key names as SDL_GetKeyName gives them ("S",
// "Delete", "F1", "Up"), with Ctrl / Shift / Alt.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noxsdl::editor {

struct Shortcut {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    std::string key; ///< empty: no shortcut

    bool empty() const { return key.empty(); }
    /// "Ctrl+Shift+S"; empty for none.
    std::string text() const;
    /// Reads text() back ("ctrl+s" works too; an empty string is no shortcut). Nothing for text
    /// that isn't a shortcut ("Ctrl+" alone).
    static std::optional<Shortcut> parse(std::string_view text);
    bool operator==(const Shortcut& o) const;
};

struct Command {
    std::string id;       ///< "file.save"
    std::string label;    ///< "Save"
    std::string category; ///< "File" (the menu it's in; the palette shows it)
    Shortcut defaultShortcut;
    std::string help; ///< the tooltip
    std::function<void()> run;
    std::function<bool()> enabled; ///< null: always
    std::function<bool()> checked; ///< null: not a toggle
};

class CommandRegistry {
public:
    void add(Command command);
    const Command* find(std::string_view id) const;
    /// Runs it if it exists and is enabled; returns whether it ran.
    bool run(std::string_view id) const;
    bool enabled(std::string_view id) const;

    /// The shortcut in use: the user's binding, else the default.
    Shortcut shortcut(std::string_view id) const;
    /// The user's binding (an empty shortcut unbinds it).
    void bind(std::string_view id, const Shortcut& shortcut);
    void resetBinding(std::string_view id);
    void resetAll() { overrides_.clear(); }
    const std::map<std::string, Shortcut, std::less<>>& overrides() const { return overrides_; }

    /// The command a key press runs (the first added with that shortcut), or null.
    const Command* match(const Shortcut& pressed) const;
    /// Commands sharing a shortcut, as (first, second) id pairs in registry order.
    std::vector<std::pair<std::string, std::string>> conflicts() const;
    /// The commands whose label, category or id match `query` (every word, in order or not;
    /// prefixes and whole words rank first), best first. An empty query lists them all.
    std::vector<const Command*> search(std::string_view query) const;
    const std::vector<Command>& commands() const { return commands_; }

    /// The user's bindings as lines "id=shortcut" (an unbound command: "id=").
    std::string saveBindings() const;
    /// Reads saveBindings' lines; unknown ids and bad lines are skipped.
    void loadBindings(std::string_view text);

private:
    std::vector<Command> commands_;
    std::map<std::string, Shortcut, std::less<>> overrides_;
};

} // namespace noxsdl::editor
