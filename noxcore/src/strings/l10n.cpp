// NoX Core's own text in the player's language: the slots, the language files, the choice.
#include "noxcore/strings/l10n.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>

namespace nox::l10n {

namespace {

struct Store {
    std::mutex mutex;
    std::deque<Slot> slots; // (a deque: a slot never moves, call sites keep references)
    std::unordered_map<std::string_view, Slot*> byId;
    std::unordered_map<std::string_view, const Slot*> keyCache; // tr(Key)'s, by id
    std::string language = "en";
    Catalog catalog;
    std::atomic<std::uint64_t> revision{0};
};

Store& store()
{
    static Store s;
    return s;
}

std::string imguiLabel(const std::string& text, const std::string& en)
{
    // The English as the id: what follows "###" is all Dear ImGui hashes. An English label with its
    // own "###" id keeps it (the translation must carry the same: the checks see to it).
    if (text == en || en.find("###") != std::string::npos) {
        return text;
    }
    return text + "###" + en;
}

/// Shows `s` in the language in use (store's catalog; its lock held).
void apply(Store& st, Slot& s, std::size_t* applied, std::size_t* stale)
{
    s.translated = false;
    s.text = s.en;
    if (s.plural) {
        s.forms = {};
        s.forms[static_cast<std::size_t>(PluralForm::One)] = s.en;
        s.forms[static_cast<std::size_t>(PluralForm::Other)] = s.enOther;
    }
    if (st.language != "en") {
        if (const auto it = st.catalog.entries.find(s.id); it != st.catalog.entries.end()) {
            const Entry& e = it->second;
            if (e.hash && *e.hash != s.enHash) {
                if (stale) ++*stale;
            } else if (e.plural == s.plural) {
                if (s.plural) {
                    // A form the file lacks: its [other], else the English.
                    const std::string& other = e.forms[static_cast<std::size_t>(PluralForm::Other)];
                    for (std::size_t i = 0; i < kPluralForms; ++i) {
                        if (!e.forms[i].empty()) {
                            s.forms[i] = e.forms[i];
                        } else if (!other.empty()) {
                            s.forms[i] = other;
                        }
                    }
                    s.text = s.forms[static_cast<std::size_t>(PluralForm::Other)];
                    s.translated = true;
                } else if (!e.text.empty()) {
                    s.text = e.text;
                    s.translated = true;
                }
                if (s.translated && applied) ++*applied;
            }
        }
    }
    s.imgui = imguiLabel(s.text, s.en);
}

Slot& make(std::string_view id, std::string_view en, std::string_view other, bool plural)
{
    Store& st = store();
    std::lock_guard lock(st.mutex);
    if (const auto it = st.byId.find(id); it != st.byId.end()) {
        return *it->second;
    }
    Slot& s = st.slots.emplace_back();
    s.id = std::string(id);
    s.en = std::string(en);
    s.plural = plural;
    s.enOther = plural ? std::string(other) : std::string();
    s.enHash = plural ? hashEnglish(en, other) : hashEnglish(en);
    apply(st, s, nullptr, nullptr);
    st.byId.emplace(s.id, &s);
    return s;
}

std::string_view trimView(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string unescape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case 'n': out += '\n'; ++i; continue;
            case 't': out += '\t'; ++i; continue;
            case 's': out += ' '; ++i; continue;
            case '\\': out += '\\'; ++i; continue;
            default: break;
            }
        }
        out += s[i];
    }
    return out;
}

bool validKey(std::string_view k)
{
    if (k.empty()) return false;
    for (const char c : k) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    }
    return k.front() != '.' && k.back() != '.';
}

std::optional<PluralForm> formNamed(std::string_view n)
{
    if (n == "one") return PluralForm::One;
    if (n == "few") return PluralForm::Few;
    if (n == "many") return PluralForm::Many;
    if (n == "other") return PluralForm::Other;
    return std::nullopt;
}

bool placeholderChar(char c, bool first)
{
    return (c >= 'a' && c <= 'z') || c == '_' || (!first && c >= '0' && c <= '9');
}

/// The length of a {name} placeholder at s[i] ('{'), or 0.
std::size_t placeholderAt(std::string_view s, std::size_t i)
{
    std::size_t j = i + 1;
    while (j < s.size() && placeholderChar(s[j], j == i + 1)) ++j;
    return j > i + 1 && j < s.size() && s[j] == '}' ? j - i + 1 : 0;
}

} // namespace

std::uint32_t hashEnglish(std::string_view en, std::string_view enOther)
{
    std::uint32_t h = 2166136261u;
    const auto add = [&](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<unsigned char>(c);
            h *= 16777619u;
        }
    };
    add(en);
    if (!enOther.empty()) {
        add("\x1F");
        add(enOther);
    }
    return h;
}

std::string hashText(std::uint32_t hash)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = kHex[hash & 0xF];
        hash >>= 4;
    }
    return s;
}

const Slot& slot(std::string_view id, std::string_view en)
{
    return make(id, en, {}, false);
}

const Slot& pluralSlot(std::string_view id, std::string_view one, std::string_view other)
{
    return make(id, one, other, true);
}

const std::string& tr(const Key& key)
{
    return slot(key.id, key.en).text;
}

const char* trImGui(const Key& key)
{
    return slot(key.id, key.en).imgui.c_str();
}

const Slot& slotOf(const PluralKey& key)
{
    return pluralSlot(key.id, key.one, key.other);
}

PluralForm pluralForm(std::string_view lang, long long n)
{
    const long long a = n < 0 ? -n : n;
    const long long m10 = a % 10, m100 = a % 100;
    if (lang == "ru" || lang == "uk" || lang == "be") {
        if (m10 == 1 && m100 != 11) return PluralForm::One;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return PluralForm::Few;
        return PluralForm::Many;
    }
    if (lang == "pl") {
        if (a == 1) return PluralForm::One;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return PluralForm::Few;
        return PluralForm::Many;
    }
    if (lang == "cs" || lang == "sk") {
        if (a == 1) return PluralForm::One;
        if (a >= 2 && a <= 4) return PluralForm::Few;
        return PluralForm::Other;
    }
    if (lang == "fr") {
        return a == 0 || a == 1 ? PluralForm::One : PluralForm::Other;
    }
    if (lang == "ja" || lang == "ko" || lang == "zh") {
        return PluralForm::Other;
    }
    return a == 1 ? PluralForm::One : PluralForm::Other;
}

const std::string& plural(const Slot& s, long long n)
{
    if (!s.plural) {
        return s.text;
    }
    const auto f = static_cast<std::size_t>(pluralForm(store().language, n));
    if (!s.forms[f].empty()) {
        return s.forms[f];
    }
    // (A form an English or partial plural doesn't have: few and many fall to other.)
    return s.forms[static_cast<std::size_t>(PluralForm::Other)];
}

std::string format(std::string_view pattern, std::initializer_list<Arg> args)
{
    return format(pattern, std::span<const Arg>(args.begin(), args.size()));
}

std::string format(std::string_view pattern, std::span<const Arg> args)
{
    std::string out;
    out.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] == '{') {
            if (const std::size_t n = placeholderAt(pattern, i)) {
                const std::string_view name = pattern.substr(i + 1, n - 2);
                const Arg* found = nullptr;
                for (const Arg& a : args) {
                    if (a.name == name) {
                        found = &a;
                        break;
                    }
                }
                if (found) {
                    out += found->value;
                    i += n;
                    continue;
                }
            }
        }
        out += pattern[i++];
    }
    return out;
}

std::vector<std::string> placeholders(std::string_view text)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            if (const std::size_t n = placeholderAt(text, i)) {
                out.emplace_back(text.substr(i + 1, n - 2));
                i += n - 1;
            }
        }
    }
    return out;
}

Catalog parseCatalog(std::string_view text)
{
    Catalog c;
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    std::optional<std::uint32_t> pendingHash;
    bool pendingMachine = false;
    std::string pendingKey; // the plural whose forms the hash covers
    int lineNo = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        ++lineNo;
        line = trimView(line);
        if (line.empty()) {
            continue;
        }
        if (line.starts_with("#@")) {
            // "#@ <hash> [flags]: <English>"
            std::string_view rest = trimView(line.substr(2));
            const std::size_t colon = rest.find(':');
            std::string_view head = trimView(rest.substr(0, colon));
            const std::size_t sp = head.find(' ');
            const std::string_view hex = head.substr(0, sp);
            std::uint32_t h = 0;
            bool ok = hex.size() == 8;
            for (const char ch : hex) {
                h <<= 4;
                if (ch >= '0' && ch <= '9') h |= static_cast<std::uint32_t>(ch - '0');
                else if (ch >= 'a' && ch <= 'f') h |= static_cast<std::uint32_t>(ch - 'a' + 10);
                else ok = false;
            }
            if (!ok) {
                c.errors.push_back(std::to_string(lineNo) + ": a \"#@\" line without an 8-digit hash");
                pendingHash.reset();
                continue;
            }
            pendingHash = h;
            pendingMachine = sp != std::string_view::npos && head.substr(sp).find("mt") != std::string_view::npos;
            pendingKey.clear();
            continue;
        }
        if (line.front() == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            c.errors.push_back(std::to_string(lineNo) + ": not \"key = text\"");
            continue;
        }
        std::string_view key = trimView(line.substr(0, eq));
        const std::string value = unescape(trimView(line.substr(eq + 1)));
        std::optional<PluralForm> form;
        if (const std::size_t br = key.find('['); br != std::string_view::npos && key.back() == ']') {
            form = formNamed(key.substr(br + 1, key.size() - br - 2));
            if (!form) {
                c.errors.push_back(std::to_string(lineNo) + ": unknown plural form in " + std::string(key));
                continue;
            }
            key = key.substr(0, br);
        }
        if (!validKey(key)) {
            c.errors.push_back(std::to_string(lineNo) + ": bad key \"" + std::string(key) + "\"");
            continue;
        }
        const std::string k(key);
        Entry& e = c.entries[k];
        if (e.line == 0) {
            e.line = lineNo;
        }
        // The hash line covers the entry after it, and a plural's later forms.
        if (pendingHash && (pendingKey.empty() || pendingKey == k)) {
            e.hash = pendingHash;
            e.machine = pendingMachine;
        }
        if (form) {
            e.plural = true;
            e.forms[static_cast<std::size_t>(*form)] = value;
            pendingKey = k;
        } else {
            e.text = value;
            pendingHash.reset();
            pendingKey.clear();
        }
    }
    return c;
}

std::string escapeText(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\t') out += "\\t";
        else if (ch == ' ' && (i == 0 || i + 1 == text.size())) out += "\\s";
        else out += ch;
    }
    return out;
}

LoadReport setLanguage(std::string_view code, Catalog catalog)
{
    const auto start = std::chrono::steady_clock::now();
    LoadReport r;
    r.language = std::string(code);
    r.loaded = true;
    r.entries = catalog.entries.size();
    r.errors = catalog.errors;
    Store& st = store();
    {
        std::lock_guard lock(st.mutex);
        st.language = std::string(code);
        st.catalog = std::move(catalog);
        for (Slot& s : st.slots) {
            apply(st, s, &r.applied, &r.stale);
        }
        ++st.revision;
    }
    r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return r;
}

LoadReport setLanguage(std::string_view code, const std::filesystem::path& dir)
{
    const auto start = std::chrono::steady_clock::now();
    if (code.empty() || code == "en") {
        LoadReport r = setLanguage("en", Catalog{});
        r.loaded = false;
        return r;
    }
    const std::filesystem::path file = dir / (std::string(code) + ".txt");
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        LoadReport r = setLanguage("en", Catalog{});
        r.language = std::string(code);
        r.file = file;
        r.loaded = false;
        r.errors.push_back("not found");
        return r;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    LoadReport r = setLanguage(code, parseCatalog(text));
    r.file = file;
    r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return r;
}

std::string_view language()
{
    return store().language;
}

std::uint64_t languageRevision()
{
    return store().revision.load();
}

std::vector<std::string> availableLanguages(const std::filesystem::path& dir)
{
    std::vector<std::string> out{"en"};
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".txt") continue;
        const std::string stem = e.path().stem().string();
        const bool code = (stem.size() == 2 || stem.size() == 3) &&
                          std::all_of(stem.begin(), stem.end(), [](char c) { return c >= 'a' && c <= 'z'; });
        if (code && stem != "en") out.push_back(stem);
    }
    std::sort(out.begin() + 1, out.end());
    return out;
}

std::string_view languageName(std::string_view code)
{
    static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
        {"en", "English"},  {"de", "Deutsch"},  {"fr", "Fran\xC3\xA7" "ais"}, {"it", "Italiano"},
        {"es", "Espa\xC3\xB1ol"}, {"pl", "Polski"}, {"cs", "\xC4\x8C" "e\xC5\xA1tina"},
        {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"}, {"ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"},
        {"ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"}};
    for (const auto& [c, n] : kNames) {
        if (c == code) return n;
    }
    return code;
}

std::string languageOfLocale(std::string_view locale)
{
    std::string out;
    for (const char c : locale) {
        if (!std::isalpha(static_cast<unsigned char>(c))) break;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out.size() >= 2 && out.size() <= 3 ? out : std::string();
}

LanguageChoice chooseLanguage(std::string_view configured, std::string_view csfLanguage,
                              std::span<const std::string> systemLanguages, std::span<const std::string> available)
{
    const auto have = [&](std::string_view c) {
        return std::find(available.begin(), available.end(), c) != available.end();
    };
    LanguageChoice ch;
    const std::string cfg = languageOfLocale(configured);
    if (!cfg.empty() && cfg != "auto") {
        if (have(cfg)) {
            ch.code = cfg;
            ch.reason = "noxcore.cfg's Language";
            return ch;
        }
        ch.reason = "noxcore.cfg's Language = " + cfg + " has no lang file; ";
    }
    if (!csfLanguage.empty() && csfLanguage != "en") {
        if (have(csfLanguage)) {
            ch.code = std::string(csfLanguage);
            ch.reason += "nox.csf's language";
            return ch;
        }
        ch.reason += "nox.csf reads as " + std::string(csfLanguage) + ", no lang file; ";
    }
    for (const std::string& s : systemLanguages) {
        const std::string l = languageOfLocale(s);
        if (l.empty()) continue;
        if (l == "en" || have(l)) {
            ch.code = l;
            ch.reason += "the system's language";
            return ch;
        }
    }
    ch.code = "en";
    ch.reason += "English (nothing else asked for)";
    return ch;
}

} // namespace nox::l10n
