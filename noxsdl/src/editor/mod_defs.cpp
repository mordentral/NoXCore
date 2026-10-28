// The mod editor's declarations (mod_defs.hpp).
#include "editor/mod_defs.hpp"

#include "noxcore/modes/script/builtin_scripts.hpp"

#include <algorithm>
#include <utility>

namespace noxsdl::editor {

namespace {

std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool identifier(std::string_view s)
{
    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) {
        return false;
    }
    for (const char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<ModDef> parseModDefs(std::string_view source)
{
    std::vector<ModDef> out;
    std::string section;
    std::string doc;
    bool inNox = false;
    for (std::size_t at = 0; at < source.size();) {
        const std::size_t end = std::min(source.find('\n', at), source.size());
        const std::string_view line = trimmed(source.substr(at, end - at));
        at = end + 1;
        if (line.starts_with("--- @hook ")) {
            const std::string_view rest = line.substr(10);
            const std::size_t bar = rest.find(" | ");
            ModDef d;
            d.kind = ModDef::Kind::Hook;
            d.section = "Hooks";
            d.signature = std::string(trimmed(rest.substr(0, bar)));
            d.name = d.signature.substr(0, d.signature.find('('));
            d.doc = bar == std::string_view::npos ? std::string() : std::string(trimmed(rest.substr(bar + 3)));
            out.push_back(std::move(d));
            continue;
        }
        if (line.starts_with("--- # ")) {
            section = std::string(line.substr(6));
            doc.clear();
            continue;
        }
        if (line.starts_with("--- ")) {
            doc = std::string(line.substr(4));
            continue;
        }
        if (line.starts_with("--") || line.empty()) {
            continue;
        }
        if (line.starts_with("declare nox:")) {
            inNox = true;
            doc.clear();
            continue;
        }
        if (inNox && line.starts_with("}")) {
            inNox = false;
            continue;
        }
        if (line.starts_with("declare function ")) {
            const std::string_view rest = line.substr(17);
            const std::size_t paren = rest.find('(');
            ModDef d;
            d.kind = ModDef::Kind::Global;
            d.section = "Globals";
            d.name = std::string(rest.substr(0, paren));
            d.signature = paren == std::string_view::npos ? std::string() : std::string(rest.substr(paren));
            d.doc = std::exchange(doc, {});
            out.push_back(std::move(d));
            continue;
        }
        if (inNox) {
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos || !identifier(trimmed(line.substr(0, colon)))) {
                continue;
            }
            std::string_view sig = trimmed(line.substr(colon + 1));
            if (sig.ends_with(",")) {
                sig.remove_suffix(1);
            }
            ModDef d;
            d.section = section;
            d.name = std::string(trimmed(line.substr(0, colon)));
            d.signature = std::string(sig);
            d.doc = std::exchange(doc, {});
            out.push_back(std::move(d));
            continue;
        }
        doc.clear();
    }
    return out;
}

std::string_view modDefSource(ModHalf half)
{
    return nox::modes::script::builtinScript("defs", half == ModHalf::Server ? "defs/server.d.luau" : "defs/ui.d.luau").value_or("");
}

const std::vector<ModDef>& modDefs(ModHalf half)
{
    static const std::vector<ModDef> server = parseModDefs(modDefSource(ModHalf::Server));
    static const std::vector<ModDef> ui = parseModDefs(modDefSource(ModHalf::Ui));
    return half == ModHalf::Server ? server : ui;
}

} // namespace noxsdl::editor
