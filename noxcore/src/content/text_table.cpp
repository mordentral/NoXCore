// The text tables as documents (text_table.hpp). Each format is read the way its own reader in
// things/ reads it (the same words, comments and line rules), noting where each piece sits.
#include "noxcore/content/text_table.hpp"

#include "noxcore/io/byte_reader.hpp" // (FormatError)
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>

namespace nox::content {

namespace {

using util::equalsNoCase;

constexpr std::size_t npos = std::string::npos;

bool isBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/// White space as the monster and sound set readers take it (the cipher's zero padding too).
bool isSpaceOrNul(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) || c == '\0';
}

/// Where the zero padding at the end starts.
std::size_t paddingStart(const std::string& text)
{
    std::size_t end = text.size();
    while (end > 0 && text[end - 1] == '\0') {
        --end;
    }
    return end;
}

std::size_t lineStart(const std::string& text, std::size_t at)
{
    while (at > 0 && text[at - 1] != '\n') {
        --at;
    }
    return at;
}

/// Just past the end of the line `at` is on (its '\n'), or the text's end.
std::size_t lineEnd(const std::string& text, std::size_t at)
{
    const auto nl = text.find('\n', at);
    return nl == npos ? text.size() : nl + 1;
}

TextSpan trimmed(const std::string& text, std::size_t b, std::size_t e)
{
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) {
        --e;
    }
    return {b, e};
}

std::string str(const std::string& text, TextSpan s)
{
    return text.substr(s.begin, s.size());
}

bool isSolo(std::string_view w)
{
    return equalsNoCase(w, "SOLO");
}

bool isArena(std::string_view w)
{
    return equalsNoCase(w, "ARENA");
}

// --- gamedata.bin: parseGameData's words ---------------------------------------------------

/// A word as parseGameData reads one: white space (but not '\0') ends it, "//" in it drops the
/// rest of the line and the word, a word the text's end cuts off is lost.
struct GdWords {
    const std::string& text;
    std::size_t pos = 0;

    static bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

    void skipLine()
    {
        const auto nl = text.find('\n', pos);
        pos = nl == npos ? text.size() : nl + 1;
    }
    std::optional<TextSpan> word()
    {
        for (;;) {
            while (pos < text.size() && space(text[pos])) {
                ++pos;
            }
            if (pos >= text.size()) {
                return std::nullopt;
            }
            const std::size_t start = pos;
            bool comment = false;
            while (pos < text.size() && !space(text[pos])) {
                if (text[pos] == '/' && pos > start && text[pos - 1] == '/') {
                    skipLine();
                    comment = true;
                    break;
                }
                ++pos;
            }
            if (comment) {
                continue;
            }
            if (pos >= text.size()) {
                return std::nullopt;
            }
            return TextSpan{start, pos};
        }
    }
};

void parseGameData(const std::string& text, std::size_t contentEnd, std::vector<TableRecord>& out)
{
    TableRecord all;
    all.whole = {0, contentEnd};
    all.insertAt = contentEnd;
    GdWords w{text};
    for (;;) {
        GameKind kind = GameKind::Both;
        std::size_t prefixAt = npos;
        std::optional<TextSpan> name = w.word();
        while (name) {
            const std::string_view s(text.data() + name->begin, name->size());
            if (s.front() == '#') {
                w.skipLine();
                kind = GameKind::Both;
                prefixAt = npos;
            } else if (isSolo(s) || isArena(s)) {
                kind = isSolo(s) ? GameKind::Solo : GameKind::Arena;
                if (prefixAt == npos) {
                    prefixAt = name->begin;
                }
            } else {
                break;
            }
            name = w.word();
        }
        if (!name || !w.word()) { // (the '=', not checked)
            break;
        }
        const auto semi = text.find(';', w.pos);
        const std::size_t valueEnd = semi == npos ? text.size() : semi;
        TableField f;
        f.kind = kind;
        f.key = str(text, *name);
        f.valueAt = trimmed(text, w.pos, valueEnd);
        if (f.valueAt.empty()) {
            f.valueAt = {valueEnd, valueEnd};
        }
        f.value = str(text, f.valueAt);
        f.whole = {kind == GameKind::Both || prefixAt == npos ? name->begin : prefixAt, semi == npos ? text.size() : semi + 1};
        all.fields.push_back(std::move(f));
        w.pos = semi == npos ? text.size() : semi + 1;
    }
    out.push_back(std::move(all));
}

// --- monster.bin: parseMonsterDb's tokens --------------------------------------------------

struct Tokens {
    const std::string& text;
    std::size_t at = 0;

    std::optional<TextSpan> next()
    {
        while (at < text.size() && isSpaceOrNul(text[at])) {
            ++at;
        }
        if (at >= text.size()) {
            return std::nullopt;
        }
        const std::size_t start = at;
        while (at < text.size() && !isSpaceOrNul(text[at])) {
            ++at;
        }
        return TextSpan{start, at};
    }
};

void parseMonsters(const std::string& text, std::size_t contentEnd, std::vector<TableRecord>& out)
{
    Tokens t{text};
    while (const auto nameTok = t.next()) {
        TableRecord r;
        r.name = str(text, *nameTok);
        r.nameAt = *nameTok;
        r.whole.begin = nameTok->begin;
        r.insertAt = contentEnd;
        r.whole.end = contentEnd;
        // A SOLO / ARENA word governs the rest of its line (the other kind's game skips it).
        GameKind kind = GameKind::Both;
        TextSpan prefixAt;
        std::size_t kindLineEnd = 0;
        for (auto tok = t.next(); tok; tok = t.next()) {
            const std::string_view s(text.data() + tok->begin, tok->size());
            if (equalsNoCase(s, "END")) {
                r.insertAt = lineStart(text, tok->begin);
                r.whole.end = tok->end;
                break;
            }
            if (isSolo(s) || isArena(s)) {
                kind = isSolo(s) ? GameKind::Solo : GameKind::Arena;
                prefixAt = *tok;
                kindLineEnd = lineEnd(text, tok->end);
                continue;
            }
            TableField f;
            const bool prefixed = kind != GameKind::Both && tok->begin < kindLineEnd;
            f.kind = prefixed ? kind : GameKind::Both;
            f.key = std::string(s);
            if (const auto v = t.next()) {
                f.valueAt = *v;
            } else {
                f.valueAt = {text.size(), text.size()};
            }
            f.value = str(text, f.valueAt);
            f.whole = {tok->begin, f.valueAt.end};
            if (prefixed) {
                f.prefixAt = prefixAt;
            } else {
                kind = GameKind::Both;
            }
            r.fields.push_back(std::move(f));
        }
        out.push_back(std::move(r));
    }
}

// --- soundset.bin: parseSoundSets' words ---------------------------------------------------

struct SsWords {
    const std::string& text;
    std::size_t at = 0;

    std::optional<TextSpan> next()
    {
        std::size_t start = npos;
        while (at < text.size()) {
            const char c = text[at++];
            if (isSpaceOrNul(c)) {
                if (start != npos) {
                    return TextSpan{start, at - 1};
                }
                continue;
            }
            if (c == '/' && start != npos && text[at - 2] == '/') {
                while (at < text.size() && text[at] != '\n') {
                    ++at;
                }
                start = npos;
                continue;
            }
            if (start == npos) {
                start = at - 1;
            }
        }
        return std::nullopt; // (a word the end cuts off is lost)
    }
};

constexpr std::string_view kSoundSetKeys[] = {
    "LISTEN_SOUND",       "TAUNT_SOUND",          "THREATEN_SOUND",          "IDLE_SOUND",
    "ENGAGE_SOUND",       "MELEE_ATTACK_INIT_SOUND", "MELEE_ATTACK_SOUND",   "MELEE_ATTACK_HITTING_SOUND",
    "MELEE_ATTACK_MISSING_SOUND", "MISSILE_ATTACK_INIT_SOUND", "MISSILE_ATTACK_SOUND", "FLEE_SOUND",
    "RETREAT_SOUND",      "SPELL_INIT_SOUND",     "DIE_SOUND",               "INJURED_SOUND",
    "RECOGNIZE_SOUND",    "MOVE_SOUND",
};

bool soundSetKey(std::string_view k)
{
    return std::ranges::find(kSoundSetKeys, k) != std::end(kSoundSetKeys);
}

void parseSoundSets(const std::string& text, std::size_t contentEnd, std::vector<TableRecord>& out, bool patch, std::string& stopped)
{
    SsWords w{text};
    while (const auto nameTok = w.next()) {
        TableRecord r;
        r.name = str(text, *nameTok);
        r.nameAt = *nameTok;
        r.whole = {nameTok->begin, contentEnd};
        r.insertAt = contentEnd;
        bool stop = false;
        while (const auto key = w.next()) {
            const std::string k = str(text, *key);
            if (k == "END") {
                r.insertAt = lineStart(text, key->begin);
                r.whole.end = key->end;
                break;
            }
            const auto value = w.next();
            if (!value) {
                stop = true;
                break;
            }
            TableField f;
            f.key = k;
            f.valueAt = *value;
            f.value = str(text, *value);
            f.whole = {key->begin, value->end};
            r.fields.push_back(std::move(f));
            // An unknown key ends the game's reading there. (A patch is read on: its keys are checked
            // when it's applied.)
            if (!patch && !soundSetKey(k)) {
                stopped = r.name + ": " + k;
                stop = true;
                break;
            }
        }
        out.push_back(std::move(r));
        if (stop) {
            break;
        }
    }
}

// --- modifier.bin: parseModifierDb's lines -------------------------------------------------

bool modifierSection(std::string_view s)
{
    return s == "WEAPON_DEFINITIONS" || s == "ARMOR_DEFINITIONS" || s == "EFFECTIVENESS" || s == "MATERIAL" || s == "ENCHANTMENT";
}

/// A line trimmed as the reader trims it (spaces, tabs, '\r' and '\0').
TextSpan modTrim(const std::string& text, std::size_t b, std::size_t e)
{
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\0'; };
    while (b < e && blank(text[b])) {
        ++b;
    }
    while (e > b && blank(text[e - 1])) {
        --e;
    }
    return {b, e};
}

void parseModifiers(const std::string& text, std::size_t contentEnd, std::vector<TableRecord>& out)
{
    enum class Where { Top, Section, Entry } where = Where::Top;
    std::string section;
    TableRecord* rec = nullptr;
    std::size_t statementAt = npos; // where the statement under way (not yet ended by ';') starts
    int lineNo = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        const std::size_t rawEnd = nl == npos ? text.size() : nl;
        const std::size_t next = nl == npos ? text.size() : nl + 1;
        const TextSpan line = modTrim(text, pos, rawEnd);
        const std::size_t thisLine = pos;
        pos = next;
        ++lineNo;
        if (line.empty()) {
            continue;
        }
        const std::string_view s(text.data() + line.begin, line.size());
        switch (where) {
        case Where::Top:
            if (!modifierSection(s)) {
                throw FormatError("modifier.bin line " + std::to_string(lineNo) + ": unknown section");
            }
            section = std::string(s);
            where = Where::Section;
            break;
        case Where::Section:
            if (s == "END") {
                where = Where::Top;
                break;
            }
            out.emplace_back();
            rec = &out.back();
            rec->name = std::string(s);
            rec->section = section;
            rec->nameAt = line;
            rec->whole = {line.begin, contentEnd};
            rec->insertAt = contentEnd;
            where = Where::Entry;
            break;
        case Where::Entry: {
            if (statementAt == npos && s == "END") {
                rec->insertAt = thisLine;
                rec->whole.end = line.end;
                where = Where::Section;
                break;
            }
            if (statementAt == npos) {
                statementAt = line.begin;
            }
            if (s.back() != ';') {
                break;
            }
            // A whole statement, KEY = value; (perhaps over several lines).
            const std::size_t semi = line.end - 1;
            const auto eq = text.find('=', statementAt);
            if (eq == npos || eq > semi) {
                throw FormatError("modifier.bin line " + std::to_string(lineNo) + ": expected KEY = value");
            }
            TableField f;
            const TextSpan key = modTrim(text, statementAt, eq);
            f.key = str(text, key);
            f.valueAt = modTrim(text, eq + 1, semi);
            if (f.valueAt.empty()) {
                f.valueAt = {semi, semi};
            }
            // (the value as the reader joins it: its lines trimmed, a space between them)
            std::string value;
            for (std::size_t p = f.valueAt.begin; p < f.valueAt.end;) {
                const auto e = std::min(f.valueAt.end, text.find('\n', p) == npos ? text.size() : text.find('\n', p));
                const TextSpan part = modTrim(text, p, e);
                if (!part.empty()) {
                    value += (value.empty() ? "" : " ") + str(text, part);
                }
                p = e + 1;
            }
            f.value = std::move(value);
            f.whole = {statementAt, semi + 1};
            rec->fields.push_back(std::move(f));
            statementAt = npos;
            break;
        }
        }
    }
}

bool reads(GameKind line, GameKind game)
{
    return line == GameKind::Both || line == game;
}

} // namespace

const char* tableFileName(TableFormat format)
{
    switch (format) {
    case TableFormat::GameData: return "gamedata.bin";
    case TableFormat::Monster: return "monster.bin";
    case TableFormat::SoundSet: return "soundset.bin";
    case TableFormat::Modifier: return "modifier.bin";
    }
    return "?";
}

crypto::FileKey tableKey(TableFormat format)
{
    switch (format) {
    case TableFormat::GameData: return crypto::FileKey::GameDataBin;
    case TableFormat::Monster: return crypto::FileKey::MonsterBin;
    case TableFormat::SoundSet: return crypto::FileKey::SoundSetBin;
    case TableFormat::Modifier: return crypto::FileKey::ModifierBin;
    }
    return crypto::FileKey::GameDataBin;
}

const char* gameKindPrefix(GameKind kind)
{
    switch (kind) {
    case GameKind::Solo: return "SOLO";
    case GameKind::Arena: return "ARENA";
    default: return "";
    }
}

// Keys compare without case, as monster.bin's and gamedata.bin's readers compare them; soundset.bin's
// and modifier.bin's readers compare exactly, and their keys are all capitals (a key they wouldn't
// take is a problem the editor's checks report, not one this finds).
std::optional<std::size_t> TableRecord::effectiveIndex(std::string_view key, GameKind kind) const
{
    const auto last = [&](GameKind game) -> std::optional<std::size_t> {
        for (std::size_t i = fields.size(); i-- > 0;) {
            if (equalsNoCase(fields[i].key, key) && reads(fields[i].kind, game)) {
                return i;
            }
        }
        return std::nullopt;
    };
    if (kind != GameKind::Both) {
        return last(kind);
    }
    const auto s = last(GameKind::Solo);
    const auto a = last(GameKind::Arena);
    return s == a ? s : std::nullopt;
}

std::optional<std::string> TableRecord::effective(std::string_view key, GameKind kind) const
{
    const auto i = effectiveIndex(key, kind);
    return i ? std::optional<std::string>(fields[*i].value) : std::nullopt;
}

TextTable TextTable::parse(TableFormat format, std::string text)
{
    TextTable t;
    t.format_ = format;
    t.text_ = std::move(text);
    t.crlf_ = t.text_.find("\r\n") != npos;
    t.reparse();
    return t;
}

TextTable TextTable::load(TableFormat format, std::span<const std::uint8_t> encrypted)
{
    std::vector<std::uint8_t> data(encrypted.begin(), encrypted.end());
    data.resize(data.size() / crypto::Blowfish::kBlockSize * crypto::Blowfish::kBlockSize); // (whole blocks, as the game decrypts)
    crypto::cipherForKey(tableKey(format)).decrypt(data);
    return parse(format, std::string(data.begin(), data.end()));
}

TextTable TextTable::parsePatch(TableFormat format, std::string text)
{
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3); // (a UTF-8 byte order mark)
    }
    // Comments become blanks of the same length (so a problem's place stays right): a '#' at a
    // line's start or after a blank, to the line's end; a line starting with "//".
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t end = lineEnd(text, at);
        std::size_t p = at;
        while (p < end && isBlank(text[p])) {
            ++p;
        }
        std::size_t from = npos;
        if (p + 1 < end && text[p] == '/' && text[p + 1] == '/') {
            from = p;
        }
        for (std::size_t i = p; from == npos && i < end; ++i) {
            if (text[i] == '#' && (i == p || isBlank(text[i - 1]))) {
                from = i;
            }
        }
        for (std::size_t i = from; from != npos && i < end && text[i] != '\n' && text[i] != '\r'; ++i) {
            text[i] = ' ';
        }
        at = end;
    }
    TextTable t;
    t.format_ = format;
    t.text_ = std::move(text);
    t.crlf_ = t.text_.find("\r\n") != npos;
    t.patch_ = true;
    t.reparse();
    return t;
}

void TextTable::reparse()
{
    records_.clear();
    stopped_.clear();
    contentEnd_ = paddingStart(text_);
    switch (format_) {
    case TableFormat::GameData: parseGameData(text_, contentEnd_, records_); break;
    case TableFormat::Monster: parseMonsters(text_, contentEnd_, records_); break;
    case TableFormat::SoundSet: parseSoundSets(text_, contentEnd_, records_, patch_, stopped_); break;
    case TableFormat::Modifier: parseModifiers(text_, contentEnd_, records_); break;
    }
}

std::optional<std::size_t> TextTable::find(std::string_view name, std::string_view section) const
{
    switch (format_) {
    case TableFormat::GameData:
        return records_.empty() ? std::nullopt : std::optional<std::size_t>(0);
    case TableFormat::Monster:
        for (std::size_t i = 0; i < records_.size(); ++i) {
            if (equalsNoCase(records_[i].name, name)) return i;
        }
        return std::nullopt;
    case TableFormat::SoundSet:
        for (std::size_t i = records_.size(); i-- > 0;) {
            if (records_[i].name == name) return i;
        }
        return std::nullopt;
    case TableFormat::Modifier:
        for (std::size_t i = 0; i < records_.size(); ++i) {
            if (records_[i].name == name && (section.empty() || records_[i].section == section)) return i;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void TextTable::replace(TextSpan span, std::string_view with)
{
    text_.replace(span.begin, span.size(), with);
    changed_ = true;
    reparse();
}

std::string TextTable::fieldLine(const TableRecord& r, GameKind kind, std::string_view key, std::string_view value) const
{
    // Laid out like the record's own lines: its fields' indentation and the space between a key
    // and its value (monster.bin, soundset.bin).
    std::string indent = format_ == TableFormat::GameData ? "" : "    ";
    std::string gap = " ";
    if (!r.fields.empty()) {
        const TableField& f = r.fields.back();
        const std::size_t ls = lineStart(text_, f.whole.begin);
        std::size_t p = ls;
        while (p < f.whole.begin && isBlank(text_[p])) {
            ++p;
        }
        indent = text_.substr(ls, p - ls);
        if (format_ == TableFormat::Monster || format_ == TableFormat::SoundSet) {
            // (the key's end: the value's start less the blanks before it)
            std::size_t g = f.valueAt.begin;
            while (g > f.whole.begin && isBlank(text_[g - 1])) {
                --g;
            }
            const std::string between = text_.substr(g, f.valueAt.begin - g);
            if (!between.empty()) {
                gap = between;
            }
        }
    }
    std::string line = indent;
    if (kind != GameKind::Both) {
        line += gameKindPrefix(kind);
        line += ' ';
    }
    line += key;
    switch (format_) {
    case TableFormat::GameData:
    case TableFormat::Modifier:
        line += " = ";
        line += value;
        line += ';';
        break;
    default:
        line += gap;
        line += value;
        break;
    }
    line += newline();
    return line;
}

void TextTable::insertField(std::size_t record, std::size_t at, GameKind kind, std::string_view key, std::string_view value)
{
    std::string line = fieldLine(records_[record], kind, key, value);
    if (at > 0 && text_[at - 1] != '\n') {
        line.insert(0, newline()); // (after a last line without its newline)
    }
    text_.insert(at, line);
    changed_ = true;
    reparse();
}

void TextTable::setOne(std::size_t record, std::string_view key, GameKind kind, std::string_view value)
{
    const TableRecord& r = records_[record];
    const auto i = r.effectiveIndex(key, kind);
    if (i && r.fields[*i].kind == kind) {
        if (r.fields[*i].value != value) {
            replace(r.fields[*i].valueAt, value);
        }
        return;
    }
    // A line of this kind after the one read now (so it's read instead), or at the record's end;
    // never past its END's line or into the padding.
    const std::size_t at = i ? std::min(lineEnd(text_, r.fields[*i].whole.end), r.insertAt) : r.insertAt;
    insertField(record, std::min(at, contentEnd_), kind, i ? r.fields[*i].key : key, value);
}

std::optional<std::string> TextTable::valueFor(TableFormat format, std::string_view rawValue)
{
    // One word in monster.bin and soundset.bin (a blank would split it: words are joined only
    // across a '+', as in "CAN_DODGE + ALERT"); one line up to the ';' in gamedata.bin and
    // modifier.bin (a SOLO / ARENA prefix covers its line only, and a ';' would end the statement).
    const bool oneWord = format == TableFormat::Monster || format == TableFormat::SoundSet;
    std::string value;
    bool gap = false;
    for (const char c : util::trim(rawValue)) {
        const bool blank = std::isspace(static_cast<unsigned char>(c)) || c == '\0';
        if (oneWord) {
            if (blank) {
                gap = true;
                continue;
            }
            if (gap && !value.ends_with('+') && c != '+') {
                return std::nullopt; // ("25 0" would read as 25, then a key "0")
            }
            gap = false;
            value += c;
        } else if (c != ';') {
            value += blank ? ' ' : c;
        }
    }
    value = std::string(util::trim(value));
    if (value.empty() && oneWord) {
        return std::nullopt; // (no word: the reader would take the next key for the value)
    }
    return value;
}

bool TextTable::set(std::size_t record, std::string_view key, GameKind kind, std::string_view rawValue)
{
    if (record >= records_.size()) {
        return false;
    }
    const auto taken = valueFor(format_, rawValue);
    if (!taken) {
        return false;
    }
    const std::string& value = *taken;
    if (format_ == TableFormat::SoundSet || format_ == TableFormat::Modifier) {
        kind = GameKind::Both;
    }
    if (kind != GameKind::Both) {
        setOne(record, key, kind, value);
        return true;
    }
    const TableRecord& r = records_[record];
    const auto s = r.effectiveIndex(key, GameKind::Solo);
    const auto a = r.effectiveIndex(key, GameKind::Arena);
    if (s == a) {
        setOne(record, key, GameKind::Both, value);
        return true;
    }
    // Solo and arena games read different lines: an unprefixed line after both, read by both.
    const std::size_t after = std::max(s.value_or(0), a.value_or(0));
    insertField(record, lineEnd(text_, r.fields[after].whole.end), GameKind::Both, r.fields[after].key, value);
    return true;
}

std::size_t TextTable::remove(std::size_t record, std::string_view key, GameKind kind)
{
    if (record >= records_.size()) {
        return 0;
    }
    std::vector<TextSpan> spans;
    const auto& fields = records_[record].fields;
    const auto removing = [&](const TableField& f) { return equalsNoCase(f.key, key) && f.kind == kind; };
    for (const TableField& f : fields) {
        if (removing(f)) {
            // Its SOLO / ARENA word too, unless a field it isn't taking shares it; the whole line
            // when nothing else is on it.
            std::size_t b = f.whole.begin;
            if (!f.prefixAt.empty() && std::ranges::none_of(fields, [&](const TableField& o) {
                    return !removing(o) && o.prefixAt.begin == f.prefixAt.begin && !o.prefixAt.empty();
                })) {
                b = std::min(b, f.prefixAt.begin);
            }
            std::size_t e = f.whole.end;
            const std::size_t ls = lineStart(text_, b);
            const std::size_t le = lineEnd(text_, e);
            const auto blankBetween = [&](std::size_t x, std::size_t y) {
                for (std::size_t p = x; p < y; ++p) {
                    if (!std::isspace(static_cast<unsigned char>(text_[p]))) return false;
                }
                return true;
            };
            if (blankBetween(ls, b) && blankBetween(e, le)) {
                b = ls;
                e = le;
            }
            spans.push_back({b, e});
        }
    }
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        text_.erase(it->begin, it->size());
    }
    if (!spans.empty()) {
        changed_ = true;
        reparse();
    }
    return spans.size();
}

void TextTable::appendRecord(std::string_view recordText)
{
    std::string add;
    if (contentEnd_ > 0 && text_[contentEnd_ - 1] != '\n') {
        add += newline();
    }
    add += recordText;
    if (add.empty() || add.back() != '\n') {
        add += newline();
    }
    text_.insert(contentEnd_, add);
    changed_ = true;
    reparse();
}

std::vector<std::uint8_t> TextTable::encrypt() const
{
    std::vector<std::uint8_t> out(text_.begin(), text_.end());
    constexpr std::size_t kBlock = crypto::Blowfish::kBlockSize;
    if (changed_ || out.size() % kBlock != 0) {
        // The text, then zeros to the next whole block (at least one, as the game's files end).
        out.resize(contentEnd_);
        out.resize((contentEnd_ / kBlock + 1) * kBlock, 0);
    }
    crypto::cipherForKey(tableKey(format_)).encrypt(out);
    return out;
}

namespace {

constexpr std::string_view kMonsterKeys[] = {
    "EXPERIENCE",           "HEALTH",           "QUESTHEALTH",          "SPEED",
    "RUN_MULTIPLIER",       "RETREAT_RATIO",    "RESUME_RATIO",         "STATUS",
    "MOVE_SOUND_FRAME_A",   "MOVE_SOUND_FRAME_B", "FLEE_RANGE",         "MELEE_ATTACK_DAMAGE",
    "MELEE_ATTACK_DAMAGE_TYPE", "MELEE_ATTACK_IMPACT", "MELEE_ATTACK_RANGE", "MELEE_ATTACK_FRAME",
    "MELEE_ATTACK_MIN_DELAY", "MELEE_ATTACK_MAX_DELAY", "MELEE_ATTACK_POISON_CHANCE", "MELEE_ATTACK_POISON_STRENGTH",
    "MELEE_ATTACK_POISON_MAX", "MISSILE_NAME",  "MISSILE_ATTACK_RANGE", "MISSILE_ATTACK_FRAME",
    "MISSILE_ATTACK_MIN_DELAY", "MISSILE_ATTACK_MAX_DELAY", "MELEE_STRIKE_FUNCTION", "DIE_FUNCTION",
    "DEAD_FUNCTION",
};

bool equipSection(std::string_view s)
{
    return s == "WEAPON_DEFINITIONS" || s == "ARMOR_DEFINITIONS";
}

/// Whether the game takes `key` in a record of `section`: soundset.bin's slots (exact: an unknown
/// one ends its reading); monster.bin's keys (case aside); in modifier.bin a key the file's records
/// of that kind use (exact; the game refuses the file over others); gamedata.bin any name.
bool keyTaken(const TextTable& base, std::string_view section, std::string_view key)
{
    switch (base.format()) {
    case TableFormat::GameData: return true;
    case TableFormat::SoundSet: return soundSetKey(key);
    case TableFormat::Monster:
        return std::ranges::any_of(kMonsterKeys, [&](std::string_view k) { return equalsNoCase(k, key); });
    case TableFormat::Modifier:
        for (const TableRecord& r : base.records()) {
            if (equipSection(r.section) == equipSection(section) && (equipSection(section) ? r.section == section : true)) {
                for (const TableField& f : r.fields) {
                    if (f.key == key) return true;
                }
            }
        }
        return false;
    }
    return false;
}

} // namespace

PatchResult applyPatch(TextTable& base, const TextTable& patch)
{
    PatchResult result;
    if (!patch.stopped().empty()) {
        result.problems.push_back("the patch's reading stopped at " + patch.stopped());
    }
    const auto refused = [&](std::string_view key) {
        return std::string(tableFileName(base.format())) + " takes no key " + std::string(key) + " there (refused: the game would stop reading the file or refuse it)";
    };
    const auto label = [](const TableRecord& r, const TableField& f) {
        std::string s = r.name.empty() ? std::string() : r.name + ".";
        if (f.kind != GameKind::Both) {
            s += std::string(gameKindPrefix(f.kind)) + " ";
        }
        return s + f.key;
    };
    for (const TableRecord& pr : patch.records()) {
        const auto at = base.find(pr.name, pr.section);
        if (!at && base.format() == TableFormat::Modifier) {
            // (A record belongs inside its section, and a new modifier would be given an id the
            // network carries: not added.)
            result.problems.push_back(pr.section + "/" + pr.name + ": modifier.bin has no such record (new ones aren't added)");
            continue;
        }
        if (!at) {
            const auto bad = std::ranges::find_if(pr.fields, [&](const TableField& f) { return f.key.starts_with('-') || !keyTaken(base, pr.section, f.key); });
            if (bad != pr.fields.end()) {
                result.problems.push_back(pr.name + " (new): " + (bad->key.starts_with('-') ? std::string("a new record has nothing to remove") : refused(bad->key)));
                continue;
            }
            base.appendRecord(std::string_view(patch.text()).substr(pr.whole.begin, pr.whole.size()));
            result.added.push_back(pr.section.empty() ? pr.name : pr.section + "/" + pr.name);
            continue;
        }
        for (const TableField& f : pr.fields) {
            const std::string_view bare = f.key.starts_with('-') ? std::string_view(f.key).substr(1) : std::string_view(f.key);
            if (!keyTaken(base, pr.section, bare)) {
                result.problems.push_back(label(pr, f) + ": " + refused(bare));
                continue;
            }
            if (f.key.starts_with('-')) {
                const std::string key = f.key.substr(1);
                if (base.remove(*at, key, f.kind) == 0) {
                    result.problems.push_back(label(pr, f) + ": nothing to remove");
                } else {
                    TableField shown = f;
                    shown.key = key;
                    result.removed.push_back(label(pr, shown));
                }
                continue;
            }
            if (!base.set(*at, f.key, f.kind, f.value)) {
                result.problems.push_back(label(pr, f) + ": no value the game could read back (a key with nothing after it)");
                continue;
            }
            result.changed.push_back(label(pr, f));
        }
    }
    return result;
}

} // namespace nox::content
