// The scripts' quest variables (quest_status.hpp), as GAME.EXE's journalQuestSet,
// journalQuestSetBool, scriptGetJournal, its two getters and the reset keep them.
#include "noxcore/server/quest_status.hpp"

#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cstring>

namespace nox::server {

namespace {

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

/// _stricmp(a, b) == 0 (ASCII letters folded).
using util::equalsNoCase;

/// _strnicmp(a, b, n) == 0: the first n letters alike, or both ending at the same place before.
bool prefixEqualNoCase(std::string_view a, std::string_view b, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        const char x = i < a.size() ? a[i] : '\0';
        const char y = i < b.size() ? b[i] : '\0';
        if (lower(x) != lower(y)) {
            return false;
        }
        if (x == '\0') {
            return true;
        }
    }
    return true;
}

/// strstr(text + from, what) found, and what it found runs to the text's end (strlen equal).
bool firstFindEndsText(std::string_view text, std::size_t from, std::string_view what)
{
    if (from > text.size()) {
        return false; // (the exe reads past the name's end there)
    }
    const std::size_t at = text.find(what, from);
    return at != std::string_view::npos && text.size() - at == what.size();
}

} // namespace

std::string questKey(const World& world, std::string_view name)
{
    // (strchr for ':'; else the map's name, ':' and the name: scriptGetJournal / its reset's twin)
    if (name.find(':') != std::string_view::npos) {
        return std::string(name);
    }
    return world.mapName + ":" + std::string(name);
}

World::QuestVariable* findQuestVariable(World& world, std::string_view name)
{
    const std::string key = questKey(world, name);
    for (World::QuestVariable& q : world.questVariables) {
        if (equalsNoCase(q.name, key)) {
            return &q;
        }
    }
    return nullptr;
}

void setQuestVariable(World& world, std::string_view name, std::uint32_t type, std::uint32_t value)
{
    if (World::QuestVariable* q = findQuestVariable(world, name)) {
        q->value = value; // (its type kept)
        return;
    }
    world.questVariables.insert(world.questVariables.begin(), {questKey(world, name), type, value});
}

std::uint32_t questVariableValue(World& world, std::string_view name)
{
    const World::QuestVariable* q = findQuestVariable(world, name);
    return q ? q->value : 0u;
}

void resetQuestVariables(World& world, std::string_view name)
{
    auto& list = world.questVariables;
    const std::string key = questKey(world, name);
    const std::size_t star = key.find('*');
    if (star == std::string::npos) {
        // The variable of that name, if there's one.
        if (World::QuestVariable* q = findQuestVariable(world, name)) {
            list.erase(list.begin() + (q - list.data()));
        }
        return;
    }
    const auto drop = [&](auto matches) { std::erase_if(list, [&](const World::QuestVariable& q) { return matches(q.name); }); };
    if (key == "*:*") {
        list.clear();
    } else if (star == key.size() - 1) {
        // "Prefix*": the names starting with Prefix, case ignored.
        drop([&](const std::string& n) { return prefixEqualNoCase(n, key, key.size() - 1); });
    } else if (star == 0) {
        // "*Suffix": Suffix's first appearance (case kept) runs to the name's end.
        const std::string_view suffix = std::string_view(key).substr(1);
        drop([&](const std::string& n) { return firstFindEndsText(n, 0, suffix); });
    } else {
        // Taken as "Scope:*Suffix": the letters up to the first ':' alike (case ignored), then,
        // from two past that ':', the first appearance of what follows ":?" runs to the end.
        const std::size_t colon = key.find(':');
        const std::string_view suffix = colon + 2 <= key.size() ? std::string_view(key).substr(colon + 2) : std::string_view();
        drop([&](const std::string& n) { return prefixEqualNoCase(n, key, colon + 1) && firstFindEndsText(n, colon + 2, suffix); });
    }
}

} // namespace nox::server
