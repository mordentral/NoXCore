// nox.csf as a document, and strings.patch (csf_document.hpp).
#include "noxcore/content/csf_document.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace nox::content {

namespace {

// The u32s the game compares (string_table.cpp's).
constexpr std::uint32_t kMagic = 0x43534620;      // "CSF "
constexpr std::uint32_t kTagLabel = 0x4C424C20;   // "LBL "
constexpr std::uint32_t kTagStr = 0x53545220;     // "STR "
constexpr std::uint32_t kTagStrW = 0x53545257;    // "STRW"
constexpr std::uint32_t kTagStrInv = 0x53747220;  // "Str "
constexpr std::uint32_t kTagStrWInv = 0x53747257; // "StrW"

/// The game's whitespace pass (string_table.cpp's collapseWhitespace).
std::u16string collapse(const std::u16string& in)
{
    std::u16string out;
    char16_t prev = 0xFFFF;
    bool lineStart = true;
    for (char16_t c : in) {
        if (c == u' ') {
            if (prev != u' ' && !lineStart) {
                out.push_back(c);
                lineStart = false;
                prev = c;
            }
        } else if (c == u'\n' || c == u'\t') {
            if (prev == u' ') {
                out.pop_back();
            }
            out.push_back(c);
            lineStart = true;
            prev = c;
        } else {
            out.push_back(c);
            lineStart = false;
            prev = c;
        }
    }
    if (prev == u' ') {
        out.pop_back();
    }
    return out;
}

} // namespace

bool CsfVariant::inverted() const
{
    return tag == kTagStrInv || tag == kTagStrWInv;
}

bool CsfVariant::wide() const
{
    return tag == kTagStrW || tag == kTagStrWInv;
}

std::u16string CsfVariant::text() const
{
    std::u16string s;
    for (const std::uint16_t raw : units) {
        const char16_t c = static_cast<char16_t>(inverted() ? static_cast<std::uint16_t>(~raw) : raw);
        if (raw == 0 || c == 0) {
            break; // (the game stops at a NUL, raw or decoded)
        }
        s.push_back(c);
    }
    return s;
}

std::u16string CsfVariant::shown() const
{
    return collapse(text());
}

void CsfVariant::setText(std::u16string_view t)
{
    units.clear();
    for (const char16_t c : t) {
        if (c == 0) {
            break;
        }
        units.push_back(inverted() ? static_cast<std::uint16_t>(~static_cast<std::uint16_t>(c)) : static_cast<std::uint16_t>(c));
    }
}

CsfDocument CsfDocument::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "csf");
    CsfDocument d;
    if (r.u32() != kMagic) {
        r.fail("not a CSF file");
    }
    d.version_ = r.u32();
    d.headerEntries_ = r.u32();
    d.headerVariants_ = r.u32();
    d.unknown_ = r.u32();
    if (d.version_ >= 2) {
        d.language_ = r.u32();
    }
    while (!r.atEnd()) {
        if (r.u32() != kTagLabel) {
            r.fail("expected LBL");
        }
        CsfEntry e;
        const std::uint32_t n = r.u32();
        e.label = r.rawString(r.u32());
        for (std::uint32_t i = 0; i < n; ++i) {
            CsfVariant v;
            v.tag = r.u32();
            if (v.tag != kTagStr && v.tag != kTagStrW && v.tag != kTagStrInv && v.tag != kTagStrWInv) {
                r.fail("unknown string tag");
            }
            const std::uint32_t length = r.u32();
            v.units.resize(r.count(length, 2));
            for (auto& u : v.units) {
                u = r.u16();
            }
            if (v.wide()) {
                v.speech = r.rawString(r.u32());
            }
            e.variants.push_back(std::move(v));
        }
        d.readVariants_ += e.variants.size();
        d.entries_.push_back(std::move(e));
    }
    d.readEntries_ = d.entries_.size();
    return d;
}

std::vector<std::uint8_t> CsfDocument::write() const
{
    ByteWriter w;
    w.u32(kMagic);
    w.u32(version_);
    // The header's counts as read, moved by as many labels and variants as there are now more (or
    // fewer) than were read: the game sizes its tables from them, so every variant added has to be
    // counted, and a file whose header was one short (stock's) stays as it was.
    std::size_t variants = 0;
    for (const CsfEntry& e : entries_) {
        variants += e.variants.size();
    }
    const auto moved = [](std::uint32_t header, std::size_t read, std::size_t now) {
        const std::int64_t v = std::int64_t(header) + std::int64_t(now) - std::int64_t(read);
        return static_cast<std::uint32_t>(std::max<std::int64_t>(v, 0));
    };
    w.u32(moved(headerEntries_, readEntries_, entries_.size()));
    w.u32(moved(headerVariants_, readVariants_, variants));
    w.u32(unknown_);
    if (version_ >= 2) {
        w.u32(language_);
    }
    for (const CsfEntry& e : entries_) {
        w.u32(kTagLabel);
        w.u32(static_cast<std::uint32_t>(e.variants.size()));
        w.u32(static_cast<std::uint32_t>(e.label.size()));
        w.rawString(e.label);
        for (const CsfVariant& v : e.variants) {
            w.u32(v.tag);
            w.u32(static_cast<std::uint32_t>(v.units.size()));
            for (const std::uint16_t u : v.units) {
                w.u16(u);
            }
            if (v.wide()) {
                w.u32(static_cast<std::uint32_t>(v.speech.size()));
                w.rawString(v.speech);
            }
        }
    }
    return std::move(w).take();
}

CsfEntry* CsfDocument::find(std::string_view label)
{
    for (auto& e : entries_) {
        if (util::equalsNoCase(e.label, label)) {
            return &e;
        }
    }
    return nullptr;
}

const CsfEntry* CsfDocument::find(std::string_view label) const
{
    return const_cast<CsfDocument*>(this)->find(label);
}

CsfEntry& CsfDocument::add(std::string label, std::u16string_view text)
{
    CsfEntry e;
    e.label = std::move(label);
    CsfVariant v;
    v.tag = kTagStr;
    v.setText(text);
    e.variants.push_back(std::move(v));
    entries_.push_back(std::move(e));
    return entries_.back();
}

std::string checkStringsLabel(std::string_view label)
{
    if (label.empty()) {
        return "no label";
    }
    if (label.size() > 48) {
        return std::format("a label is at most 48 characters (this one is {})", label.size());
    }
    if (std::ranges::any_of(label, [](char c) { return c < 0x21 || c > 0x7E; })) {
        return "a label is printable ASCII without blanks";
    }
    return {};
}

std::string checkStringsText(std::u16string_view text, bool inverted)
{
    if (text.size() > 10239) {
        return std::format("a text is at most 10,239 characters (this one is {})", text.size());
    }
    if (inverted && text.find(u'\xFFFF') != std::u16string_view::npos) {
        return "this label's texts are stored inverted: U+FFFF would become the end of the text";
    }
    return {};
}

std::string checkSpeechName(std::string_view name)
{
    if (std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E; })) {
        return "a speech file's name is plain ASCII";
    }
    return {};
}

std::vector<std::string> printfConversions(std::u16string_view text)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != u'%') {
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == u'%') {
            ++i; // ("%%": a '%' shown)
            continue;
        }
        // Flags, width, precision and size, then the conversion's letter.
        std::string spec = "%";
        std::size_t j = i + 1;
        while (j < text.size() && text[j] < 0x80 && std::strchr("-+ #0123456789.*hlLIwz", static_cast<char>(text[j])) && text[j] != 0) {
            spec += static_cast<char>(text[j++]);
        }
        if (j < text.size() && text[j] < 0x80) {
            spec += static_cast<char>(text[j]);
        }
        out.push_back(spec);
        i = j;
    }
    return out;
}

// --- strings.patch -------------------------------------------------------------------------

std::string escapeStringsText(std::u16string_view text)
{
    std::string out;
    for (const char c : strings::toUtf8(text)) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\\': out += "\\\\"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::u16string unescapeStringsText(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char n = text[++i];
            if (n == 'n' || n == 't' || n == 'r' || n == '\\') {
                out += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : '\\';
            } else {
                out += '\\'; // (no escape: kept as written)
                out += n;
            }
        } else {
            out += text[i];
        }
    }
    return strings::fromUtf8(out);
}

std::vector<StringsPatchLine> parseStringsPatch(std::string_view text, std::vector<std::string>* problems)
{
    std::vector<StringsPatchLine> out;
    int lineNo = 0;
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3); // (a UTF-8 byte order mark)
    }
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        ++lineNo;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::string_view t = util::trim(line);
        if (t.empty() || t.front() == '#') {
            continue;
        }
        StringsPatchLine l;
        if (t.front() == '-' && t.find('=') == std::string_view::npos) {
            l.op = StringsPatchLine::Remove;
            l.label = std::string(util::trim(t.substr(1)));
            out.push_back(std::move(l));
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            if (problems) problems->push_back(std::format("strings.patch line {}: expected LABEL = text", lineNo));
            continue;
        }
        std::string_view key = util::trim(t.substr(0, eq));
        if (key.ends_with('+')) {
            l.op = StringsPatchLine::Add;
            key = util::trim(key.substr(0, key.size() - 1));
        } else if (key.size() > 7 && util::equalsNoCase(key.substr(key.size() - 7), ".speech")) {
            l.op = StringsPatchLine::Speech;
            key = key.substr(0, key.size() - 7);
        }
        l.label = std::string(key);
        // The text: everything after "= " (one blank after the '=' is the separator; more are kept).
        std::string_view value = t.substr(eq + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        l.text = unescapeStringsText(value);
        out.push_back(std::move(l));
    }
    return out;
}

std::string writeStringsPatch(const std::vector<StringsPatchLine>& lines, std::string_view header)
{
    if (lines.empty()) {
        return {};
    }
    std::string out(header);
    for (const auto& l : lines) {
        switch (l.op) {
        case StringsPatchLine::Set: out += l.label + " = " + escapeStringsText(l.text) + "\n"; break;
        case StringsPatchLine::Add: out += l.label + " += " + escapeStringsText(l.text) + "\n"; break;
        case StringsPatchLine::Speech: out += l.label + ".speech = " + escapeStringsText(l.text) + "\n"; break;
        case StringsPatchLine::Remove: out += "-" + l.label + "\n"; break;
        }
    }
    return out;
}

StringsPatchResult applyStringsPatch(CsfDocument& doc, const std::vector<StringsPatchLine>& patch)
{
    StringsPatchResult result;
    std::vector<std::string> reset; // (labels a Set has given new variants in this patch)
    // A text's printf conversions as its label's stock text has them (the game formats some with
    // arguments: one more %s reads past them); a new label's text has none; %n never.
    const auto conversionsOk = [&](const std::string& label, const std::u16string& text, const CsfEntry* stock) -> std::string {
        const auto now = printfConversions(text);
        if (std::ranges::any_of(now, [](const std::string& c) { return c.find('n') != std::string::npos; })) {
            return label + ": %n isn't allowed in a text";
        }
        std::vector<std::string> want;
        if (stock && !stock->variants.empty()) {
            want = printfConversions(stock->variants.front().text());
        }
        if (now != want) {
            return label + (want.empty() ? ": a new or plain text can't hold printf conversions (% other than %%)"
                                         : ": the text must keep the stock text's conversions in order (" + [&] {
                                               std::string s;
                                               for (const auto& c : want) s += (s.empty() ? "" : " ") + c;
                                               return s;
                                           }() + ")");
        }
        return {};
    };
    const CsfDocument before = doc; // (the labels as they were: what "stock" means for the conversions)

    for (const auto& l : patch) {
        if (const std::string why = checkStringsLabel(l.label); !why.empty()) {
            result.problems.push_back(l.label + ": " + why);
            continue;
        }
        CsfEntry* e = doc.find(l.label);
        switch (l.op) {
        case StringsPatchLine::Remove:
            if (!e) {
                result.problems.push_back(l.label + ": nothing to remove");
                break;
            }
            std::erase_if(doc.entries(), [&](const CsfEntry& o) { return &o == e; });
            result.changed.push_back("-" + l.label);
            break;
        case StringsPatchLine::Set:
        case StringsPatchLine::Add: {
            const bool inverted = e && !e->variants.empty() && e->variants.front().inverted();
            if (const std::string why = checkStringsText(l.text, inverted); !why.empty()) {
                result.problems.push_back(l.label + ": " + why);
                break;
            }
            if (const std::string why = conversionsOk(l.label, l.text, before.find(l.label)); !why.empty()) {
                result.problems.push_back(why);
                break;
            }
            if (!e) {
                doc.add(l.label, l.text);
                result.added.push_back(l.label);
                reset.push_back(util::lowered(l.label));
                break;
            }
            const bool first = std::ranges::find(reset, util::lowered(l.label)) == reset.end();
            if (l.op == StringsPatchLine::Set && first) {
                // Its variants become this one, in the first variant's own form (tag, speech file).
                CsfVariant v = e->variants.empty() ? CsfVariant{kTagStr, {}, {}} : e->variants.front();
                v.setText(l.text);
                e->variants.assign(1, std::move(v));
                reset.push_back(util::lowered(l.label));
                result.changed.push_back(l.label);
                break;
            }
            if (e->variants.size() >= kMaxStringVariants) {
                result.problems.push_back(std::format("{}: a label has at most {} texts", l.label, kMaxStringVariants));
                break;
            }
            // One more variant (a += line, or a second Set of the label in one patch), with no
            // speech file of its own.
            CsfVariant v = e->variants.empty() ? CsfVariant{kTagStr, {}, {}} : e->variants.back();
            v.speech.clear();
            if (v.wide()) {
                v.tag = v.inverted() ? kTagStrInv : kTagStr;
            }
            v.setText(l.text);
            e->variants.push_back(std::move(v));
            result.changed.push_back(l.label + " (+)");
            break;
        }
        case StringsPatchLine::Speech:
            if (!e || e->variants.empty()) {
                result.problems.push_back(l.label + ": no text to give a speech file");
                break;
            }
            if (const std::string why = checkSpeechName(strings::toUtf8(l.text)); !why.empty()) {
                result.problems.push_back(l.label + ": " + why);
                break;
            }
            {
                CsfVariant& v = e->variants.front();
                if (!v.wide()) {
                    v.tag = v.inverted() ? kTagStrWInv : kTagStrW;
                }
                v.speech = strings::toUtf8(l.text);
                result.changed.push_back(l.label + ".speech");
            }
            break;
        }
    }
    return result;
}

} // namespace nox::content