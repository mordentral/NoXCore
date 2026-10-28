// The map editor's command registry.
#include "editor/commands.hpp"

#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace noxsdl::editor {

namespace {

using nox::util::equalsNoCase;
using nox::util::lowered;

std::string trim(std::string_view s)
{
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) {
        return {};
    }
    const auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

/// How well one query word matches the text (0: not at all).
int wordScore(const std::string& text, const std::string& word)
{
    const auto at = text.find(word);
    if (at == std::string::npos) {
        // In order with gaps ("svas" for "save as"): weakest.
        std::size_t i = 0;
        for (const char c : text) {
            if (i < word.size() && c == word[i]) {
                ++i;
            }
        }
        return i == word.size() ? 1 : 0;
    }
    if (at == 0) {
        return 4; // a prefix of the whole text
    }
    const char before = text[at - 1];
    return (before == ' ' || before == '.' || before == ':') ? 3 : 2; // a word's start, else inside one
}

} // namespace

std::string Shortcut::text() const
{
    if (key.empty()) {
        return {};
    }
    std::string s;
    if (ctrl) s += "Ctrl+";
    if (shift) s += "Shift+";
    if (alt) s += "Alt+";
    return s + key;
}

std::optional<Shortcut> Shortcut::parse(std::string_view text)
{
    Shortcut s;
    std::string rest = trim(text);
    if (rest.empty()) {
        return s;
    }
    for (;;) {
        // A modifier is a word followed by '+' (a lone "+" key stays a key).
        const auto plus = rest.find('+', 1);
        if (plus == std::string::npos) {
            break;
        }
        const std::string mod = trim(std::string_view(rest).substr(0, plus));
        if (equalsNoCase(mod, "ctrl") || equalsNoCase(mod, "control")) {
            s.ctrl = true;
        } else if (equalsNoCase(mod, "shift")) {
            s.shift = true;
        } else if (equalsNoCase(mod, "alt")) {
            s.alt = true;
        } else {
            return std::nullopt;
        }
        rest = trim(std::string_view(rest).substr(plus + 1));
    }
    if (rest.empty()) {
        return std::nullopt;
    }
    // Key names as SDL gives them: single letters upper case, named keys capitalised.
    if (rest.size() == 1) {
        rest[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(rest[0])));
    }
    s.key = rest;
    return s;
}

bool Shortcut::operator==(const Shortcut& o) const
{
    return ctrl == o.ctrl && shift == o.shift && alt == o.alt && equalsNoCase(key, o.key);
}

void CommandRegistry::add(Command command)
{
    const auto it = std::ranges::find(commands_, command.id, &Command::id);
    if (it != commands_.end()) {
        *it = std::move(command);
    } else {
        commands_.push_back(std::move(command));
    }
}

const Command* CommandRegistry::find(std::string_view id) const
{
    const auto it = std::ranges::find(commands_, id, &Command::id);
    return it == commands_.end() ? nullptr : &*it;
}

bool CommandRegistry::enabled(std::string_view id) const
{
    const Command* c = find(id);
    return c && c->run && (!c->enabled || c->enabled());
}

bool CommandRegistry::run(std::string_view id) const
{
    if (!enabled(id)) {
        return false;
    }
    find(id)->run();
    return true;
}

Shortcut CommandRegistry::shortcut(std::string_view id) const
{
    if (const auto it = overrides_.find(id); it != overrides_.end()) {
        return it->second;
    }
    const Command* c = find(id);
    return c ? c->defaultShortcut : Shortcut{};
}

void CommandRegistry::bind(std::string_view id, const Shortcut& shortcut)
{
    const Command* c = find(id);
    if (!c) {
        return;
    }
    if (shortcut == c->defaultShortcut && shortcut.empty() == c->defaultShortcut.empty()) {
        overrides_.erase(std::string(id));
    } else {
        overrides_[std::string(id)] = shortcut;
    }
}

void CommandRegistry::resetBinding(std::string_view id)
{
    if (const auto it = overrides_.find(id); it != overrides_.end()) {
        overrides_.erase(it);
    }
}

const Command* CommandRegistry::match(const Shortcut& pressed) const
{
    if (pressed.empty()) {
        return nullptr;
    }
    for (const Command& c : commands_) {
        if (const Shortcut s = shortcut(c.id); !s.empty() && s == pressed) {
            return &c;
        }
    }
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> CommandRegistry::conflicts() const
{
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i < commands_.size(); ++i) {
        const Shortcut a = shortcut(commands_[i].id);
        if (a.empty()) {
            continue;
        }
        for (std::size_t j = i + 1; j < commands_.size(); ++j) {
            if (shortcut(commands_[j].id) == a) {
                out.emplace_back(commands_[i].id, commands_[j].id);
            }
        }
    }
    return out;
}

std::vector<const Command*> CommandRegistry::search(std::string_view query) const
{
    std::vector<std::string> words;
    {
        std::istringstream in(lowered(query));
        for (std::string w; in >> w;) {
            words.push_back(w);
        }
    }
    std::vector<std::pair<int, const Command*>> scored;
    for (const Command& c : commands_) {
        if (!c.run) {
            continue;
        }
        const std::string label = lowered(c.label);
        const std::string all = lowered(c.category + ": " + c.label + " " + c.id);
        int score = 0;
        bool every = true;
        for (const auto& w : words) {
            const int s = std::max(wordScore(label, w) * 2, wordScore(all, w));
            if (s == 0) {
                every = false;
                break;
            }
            score += s;
        }
        if (every) {
            scored.emplace_back(score, &c);
        }
    }
    std::ranges::stable_sort(scored, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const Command*> out;
    out.reserve(scored.size());
    for (const auto& [s, c] : scored) {
        out.push_back(c);
    }
    return out;
}

std::string CommandRegistry::saveBindings() const
{
    std::string out;
    for (const auto& [id, s] : overrides_) {
        out += id + "=" + s.text() + "\n";
    }
    return out;
}

void CommandRegistry::loadBindings(std::string_view text)
{
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string id = trim(std::string_view(line).substr(0, eq));
        const auto s = Shortcut::parse(std::string_view(line).substr(eq + 1));
        if (!s || !find(id)) {
            continue;
        }
        overrides_[id] = *s;
    }
}

} // namespace noxsdl::editor
