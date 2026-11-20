// mode.json (manifest.hpp).
#include "noxcore/modes/script/manifest.hpp"

#include "json.hpp"
#include "noxcore/lobby/extended.hpp"

#include <algorithm>
#include <cmath>

namespace nox::modes::script {

namespace {

struct Bad {
    std::string why;
};

const json::Value* field(const json::Value& o, std::string_view key)
{
    return o.find(key);
}

std::string text(const json::Value& o, std::string_view key, std::string fallback, std::size_t maxLength)
{
    const json::Value* v = field(o, key);
    if (!v) {
        return fallback;
    }
    if (!v->isString()) {
        throw Bad{"\"" + std::string(key) + "\" must be text"};
    }
    if (v->string.size() > maxLength) {
        throw Bad{"\"" + std::string(key) + "\" is longer than " + std::to_string(maxLength)};
    }
    return v->string;
}

int whole(const json::Value& o, std::string_view key, int fallback, int lo, int hi)
{
    const json::Value* v = field(o, key);
    if (!v) {
        return fallback;
    }
    if (!v->isNumber() || v->number != std::floor(v->number) || v->number < lo || v->number > hi) {
        throw Bad{"\"" + std::string(key) + "\" must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi)};
    }
    return static_cast<int>(v->number);
}

bool flag(const json::Value& o, std::string_view key, bool fallback)
{
    const json::Value* v = field(o, key);
    if (!v) {
        return fallback;
    }
    if (!v->isBool()) {
        throw Bad{"\"" + std::string(key) + "\" must be true or false"};
    }
    return v->boolean;
}

OptionSpec optionOf(const json::Value& o, std::size_t index)
{
    if (!o.isObject()) {
        throw Bad{"options[" + std::to_string(index) + "] must be an object"};
    }
    OptionSpec s;
    s.key = text(o, "key", "", 32);
    if (!validModeId(s.key)) {
        throw Bad{"options[" + std::to_string(index) + "].key must be 1-32 of a-z, 0-9 and _"};
    }
    s.label = text(o, "label", s.key, 48);
    const std::string kind = text(o, "kind", "toggle", 16);
    if (kind == "choice") {
        s.kind = OptionSpec::Kind::Choice;
        const json::Value* c = field(o, "choices");
        if (!c || !c->isArray() || c->array->empty() || c->array->size() > 16) {
            throw Bad{"option \"" + s.key + "\": a choice needs 1-16 \"choices\""};
        }
        for (const json::Value& v : *c->array) {
            if (!v.isString() || v.string.empty() || v.string.size() > 32) {
                throw Bad{"option \"" + s.key + "\": each choice is text, 1-32 letters"};
            }
            s.choices.push_back(v.string);
        }
        s.defaultValue = whole(o, "default", 0, 0, static_cast<int>(s.choices.size()) - 1);
    } else if (kind == "toggle") {
        s.kind = OptionSpec::Kind::Toggle;
        s.defaultValue = whole(o, "default", 0, 0, 1);
    } else if (kind == "number") {
        s.kind = OptionSpec::Kind::Number;
        s.min = whole(o, "min", 0, -1'000'000, 1'000'000);
        s.max = whole(o, "max", 100, s.min, 1'000'000);
        s.defaultValue = whole(o, "default", s.min, s.min, s.max);
    } else if (kind == "text") {
        s.kind = OptionSpec::Kind::Text;
    } else {
        throw Bad{"option \"" + s.key + "\": \"kind\" is choice, toggle, number or text"};
    }
    return s;
}

/// Whether `name` may be a campaign map's: 1-31 of A-Z, a-z, 0-9, '_' and '-' (a maps folder's
/// name, as an exit names it).
bool validMapName(std::string_view name)
{
    return !name.empty() && name.size() <= 31 && std::ranges::all_of(name, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

bool sameNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               const auto low = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch; };
               return low(x) == low(y);
           });
}

/// mode.json's "campaign": {"title", "firstMap", "classes", "maps"}.
CampaignInfo campaignOf(const json::Value& c, const std::string& modeName)
{
    if (!c.isObject()) {
        throw Bad{"\"campaign\" must be an object of \"title\", \"firstMap\", \"classes\" and \"maps\""};
    }
    for (const auto& [key, v] : *c.object) {
        (void)v;
        if (key != "title" && key != "firstMap" && key != "classes" && key != "maps") {
            throw Bad{"\"campaign\": \"" + key + "\" isn't one (\"title\", \"firstMap\", \"classes\" or \"maps\")"};
        }
    }
    CampaignInfo k;
    k.title = text(c, "title", modeName, 32);
    if (k.title.empty()) {
        throw Bad{"\"campaign\": \"title\" is 1-32 letters"};
    }
    k.firstMap = text(c, "firstMap", "", 31);
    if (!validMapName(k.firstMap)) {
        throw Bad{"\"campaign\": \"firstMap\" is a map's name (1-31 of letters, digits, _ and -)"};
    }
    const json::Value* classes = field(c, "classes");
    if (!classes || !classes->isArray() || classes->array->empty() || classes->array->size() > 3) {
        throw Bad{"\"campaign\": \"classes\" lists 1-3 of \"warrior\", \"wizard\" and \"conjurer\""};
    }
    for (const json::Value& v : *classes->array) {
        const std::uint8_t bit = !v.isString()              ? 0
                                 : v.string == "warrior"   ? 1
                                 : v.string == "wizard"    ? 2
                                 : v.string == "conjurer"  ? 4
                                                           : 0;
        if (bit == 0 || (k.classes & bit)) {
            throw Bad{"\"campaign\": \"classes\" lists 1-3 of \"warrior\", \"wizard\" and \"conjurer\", each once"};
        }
        k.classes |= bit;
    }
    if (const json::Value* maps = field(c, "maps")) {
        if (!maps->isArray() || maps->array->size() > 256) {
            throw Bad{"\"campaign\": \"maps\" is a list of at most 256 maps' names"};
        }
        for (const json::Value& v : *maps->array) {
            if (!v.isString() || !validMapName(v.string)) {
                throw Bad{"\"campaign\": each of \"maps\" is a map's name (1-31 of letters, digits, _ and -)"};
            }
            if (std::ranges::any_of(k.maps, [&](const std::string& n) { return sameNoCase(n, v.string); })) {
                throw Bad{"\"campaign\": the map " + v.string + " is in \"maps\" twice"};
            }
            k.maps.push_back(v.string);
        }
        if (!k.maps.empty() && std::ranges::none_of(k.maps, [&](const std::string& n) { return sameNoCase(n, k.firstMap); })) {
            throw Bad{"\"campaign\": \"firstMap\" " + k.firstMap + " isn't one of its \"maps\""};
        }
    }
    return k;
}

} // namespace

bool validModeId(std::string_view id)
{
    return !id.empty() && id.size() <= 32 &&
           std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

ManifestResult readManifest(std::string_view text_)
{
    ManifestResult out;
    try {
        const json::Value doc = json::parse(text_);
        if (!doc.isObject()) {
            throw Bad{"mode.json must hold one object"};
        }
        Manifest m;
        m.api = whole(doc, "api", 0, 0, 1000);
        if (m.api != kScriptApi) {
            throw Bad{"written for script API " + std::to_string(m.api) + "; this build runs API " + std::to_string(kScriptApi)};
        }
        m.info.id = text(doc, "id", "", 24);
        if (!validModeId(m.info.id) || m.info.id.size() > 24) {
            throw Bad{"\"id\" must be 1-24 of a-z, 0-9 and _"};
        }
        m.info.name = text(doc, "name", m.info.id, 32);
        m.info.description = text(doc, "description", "", 120);
        m.info.order = whole(doc, "order", 1000, -100'000, 100'000);
        if (const json::Value* p = field(doc, "players")) {
            if (!p->isArray() || p->array->size() != 2 || !(*p->array)[0].isNumber() || !(*p->array)[1].isNumber()) {
                throw Bad{"\"players\" must be [fewest, most]"};
            }
            const double fewest = (*p->array)[0].number, most = (*p->array)[1].number;
            // (Checked as numbers before the casts: a huge one cast to int is undefined.)
            if (!(fewest >= 1 && most <= 32 && fewest <= most) || fewest != std::floor(fewest) || most != std::floor(most)) {
                throw Bad{"\"players\" must be 1-32 (whole numbers), the fewest first"};
            }
            m.info.minPlayers = static_cast<int>(fewest);
            m.info.maxPlayers = static_cast<int>(most);
        }
        m.info.stockClients = flag(doc, "stockClients", true);
        m.refuseStock = !m.info.stockClients;
        // (Above this build's level: no client it knows of has it.)
        m.minClientLevel = static_cast<std::uint16_t>(whole(doc, "minClientLevel", 0, 0, 1000));
        if (m.minClientLevel > lobby::kClientLevel) {
            throw Bad{"asks for client level " + std::to_string(m.minClientLevel) + "; this build's clients are level " +
                      std::to_string(lobby::kClientLevel)};
        }
        m.clientKeys = flag(doc, "clientKeys", false);
        m.noMapRespawns = !flag(doc, "mapRespawns", true);
        m.noMapStartInvulnerable = !flag(doc, "mapStartInvulnerable", true);
        const std::string levels = text(doc, "levels", "mapFiles", 16);
        if (levels != "mapFiles" && levels != "generated") {
            throw Bad{"\"levels\" is \"mapFiles\" or \"generated\""};
        }
        m.info.levels = levels == "generated" ? ModeInfo::Levels::Generated : ModeInfo::Levels::MapFiles;
        const auto inside = [](const std::string& p) {
            return !p.empty() && p.find("..") == std::string::npos && p.front() != '/' && p.find(':') == std::string::npos &&
                   p.find('\\') == std::string::npos;
        };
        m.serverScript = text(doc, "server", m.serverScript, 128);
        if (!inside(m.serverScript)) {
            throw Bad{"\"server\" must be a path inside the mode's folder, '/' separated"};
        }
        m.uiScript = text(doc, "ui", "", 128);
        if (!m.uiScript.empty() && !inside(m.uiScript)) {
            throw Bad{"\"ui\" must be a path inside the mode's folder, '/' separated"};
        }
        m.uiApi = whole(doc, "uiApi", 1, 1, 1000);
        if (m.uiApi > kUiApi) {
            throw Bad{"written for UI API " + std::to_string(m.uiApi) + "; this build gives UI API 1 to " + std::to_string(kUiApi)};
        }
        m.info.mapPrefix = text(doc, "mapPrefix", "", 3);
        if (!m.info.mapPrefix.empty() &&
            (m.info.mapPrefix.size() < 2 || !std::ranges::all_of(m.info.mapPrefix, [](char c) { return c >= 'A' && c <= 'Z'; }))) {
            throw Bad{"\"mapPrefix\" is 2-3 of A-Z (its generated maps' names start with it)"};
        }
        const std::string looks = text(doc, "looksLike", "", 16);
        if (looks == "arena") {
            m.looksLike = 0x100;
        } else if (looks == "ctf") {
            m.looksLike = 0x20;
        } else if (!looks.empty()) {
            throw Bad{"\"looksLike\" is \"arena\" or \"ctf\""};
        }
        if (const json::Value* g = field(doc, "gameplay")) {
            if (!g->isObject()) {
                throw Bad{"\"gameplay\" must be an object of \"teamDamage\", \"autoAssign\" and \"teamPlay\" (true or false)"};
            }
            for (const auto& [key, v] : *g->object) {
                const std::uint32_t bit = key == "teamDamage" ? 1u : key == "autoAssign" ? 2u : key == "teamPlay" ? 4u : 0u;
                if (bit == 0) {
                    throw Bad{"\"gameplay\": \"" + key + "\" isn't one (\"teamDamage\", \"autoAssign\" or \"teamPlay\")"};
                }
                if (!v.isBool()) {
                    throw Bad{"\"gameplay\": \"" + key + "\" must be true or false"};
                }
                m.gameplayPinned |= bit;
                m.gameplayValues |= v.boolean ? bit : 0u;
            }
            // (A CTF look turns team play on: configure's.)
            if (m.looksLike == 0x20 && (m.gameplayPinned & 4u) && !(m.gameplayValues & 4u)) {
                throw Bad{"\"gameplay\": a \"ctf\" look plays with teams (\"teamPlay\" can't be false)"};
            }
        }
        if (const json::Value* p = field(doc, "pickup")) {
            if (!p->isObject()) {
                throw Bad{"\"pickup\" must be an object of \"anyClass\" and \"duplicates\" (true or false)"};
            }
            for (const auto& [key, v] : *p->object) {
                if (key != "anyClass" && key != "duplicates") {
                    throw Bad{"\"pickup\": \"" + key + "\" isn't one (\"anyClass\" or \"duplicates\")"};
                }
                if (!v.isBool()) {
                    throw Bad{"\"pickup\": \"" + key + "\" must be true or false"};
                }
                (key == "anyClass" ? m.pickupAnyClass : m.pickupDuplicates) = v.boolean;
            }
        }
        if (const json::Value* c = field(doc, "campaign")) {
            m.info.campaign = campaignOf(*c, m.info.name);
        }
        if (const json::Value* opts = field(doc, "options")) {
            if (!opts->isArray() || opts->array->size() > 32) {
                throw Bad{"\"options\" must be a list of at most 32"};
            }
            for (std::size_t k = 0; k < opts->array->size(); ++k) {
                OptionSpec s = optionOf((*opts->array)[k], k);
                if (std::ranges::any_of(m.info.options, [&](const OptionSpec& o) { return o.key == s.key; })) {
                    throw Bad{"option \"" + s.key + "\" is there twice"};
                }
                m.info.options.push_back(std::move(s));
            }
        }
        out.manifest = std::move(m);
    } catch (const json::ParseError& e) {
        out.error = std::string("mode.json: ") + e.what();
    } catch (const Bad& b) {
        out.error = "mode.json: " + b.why;
    }
    return out;
}

} // namespace nox::modes::script
