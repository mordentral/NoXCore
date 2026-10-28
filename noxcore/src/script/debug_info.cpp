// The compiler's debug info as JSON (compiler.hpp DebugInfo), for the editor's sidecar:
//
//   {"version":1, "embeddedSource":false,
//    "globals":[{"name":"SELF","type":"int","symbol":0,"size":1,"line":0,"column":0}, ...],
//    "objectVars":[<as globals>],          (only with object variables: function 0's symbols)
//    "functions":[{"name":"MapInitialize","index":2,"returns":"void","line":3,"column":6,
//                  "symbols":[<as globals>], "lines":[[word,line,column], ...]}, ...],
//    "globalInits":[[symbol,begin,end,line,column], ...],
//    "strings":[[id,line,column], ...]}
#include "noxcore/script/compiler.hpp"

#include <charconv>
#include <cstdio>
#include <map>
#include <memory>
#include <variant>

namespace nox::script {

namespace {

void quote(std::string& out, std::string_view s)
{
    out += '"';
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20 || u >= 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", u);
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
}

void var(std::string& out, const DebugVar& v)
{
    out += "{\"name\":";
    quote(out, v.name);
    out += ",\"type\":\"" + std::string(typeName(v.type)) + "\",\"symbol\":" + std::to_string(v.symbol) +
           ",\"size\":" + std::to_string(v.size) + ",\"line\":" + std::to_string(v.loc.line) +
           ",\"column\":" + std::to_string(v.loc.column) + "}";
}

// A small JSON reader for what toJson writes (and anything shaped like it).
struct Value;
using Object = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;
struct Value {
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v;
    const Array* array() const { auto p = std::get_if<std::shared_ptr<Array>>(&v); return p ? p->get() : nullptr; }
    const Object* object() const { auto p = std::get_if<std::shared_ptr<Object>>(&v); return p ? p->get() : nullptr; }
    double number(double fallback = 0) const { auto p = std::get_if<double>(&v); return p ? *p : fallback; }
    std::string text() const { auto p = std::get_if<std::string>(&v); return p ? *p : std::string(); }
    bool boolean() const { auto p = std::get_if<bool>(&v); return p && *p; }
};

class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}
    std::optional<Value> value()
    {
        space();
        if (at_ >= s_.size()) {
            return std::nullopt;
        }
        const char c = s_[at_];
        if (c == '{') {
            ++at_;
            auto obj = std::make_shared<Object>();
            space();
            if (peek('}')) {
                ++at_;
                return Value{obj};
            }
            for (;;) {
                space();
                auto key = string();
                space();
                if (!key || !peek(':')) {
                    return std::nullopt;
                }
                ++at_;
                auto v = value();
                if (!v) {
                    return std::nullopt;
                }
                obj->emplace(*key, std::move(*v));
                space();
                if (peek(',')) {
                    ++at_;
                    continue;
                }
                if (peek('}')) {
                    ++at_;
                    return Value{obj};
                }
                return std::nullopt;
            }
        }
        if (c == '[') {
            ++at_;
            auto arr = std::make_shared<Array>();
            space();
            if (peek(']')) {
                ++at_;
                return Value{arr};
            }
            for (;;) {
                auto v = value();
                if (!v) {
                    return std::nullopt;
                }
                arr->push_back(std::move(*v));
                space();
                if (peek(',')) {
                    ++at_;
                    continue;
                }
                if (peek(']')) {
                    ++at_;
                    return Value{arr};
                }
                return std::nullopt;
            }
        }
        if (c == '"') {
            auto s = string();
            return s ? std::optional<Value>(Value{*s}) : std::nullopt;
        }
        if (s_.substr(at_).starts_with("true")) {
            at_ += 4;
            return Value{true};
        }
        if (s_.substr(at_).starts_with("false")) {
            at_ += 5;
            return Value{false};
        }
        if (s_.substr(at_).starts_with("null")) {
            at_ += 4;
            return Value{nullptr};
        }
        double d = 0;
        const auto r = std::from_chars(s_.data() + at_, s_.data() + s_.size(), d);
        if (r.ec != std::errc()) {
            return std::nullopt;
        }
        at_ = static_cast<std::size_t>(r.ptr - s_.data());
        return Value{d};
    }
    bool atEnd()
    {
        space();
        return at_ >= s_.size();
    }

private:
    bool peek(char c) const { return at_ < s_.size() && s_[at_] == c; }
    void space()
    {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\n' || s_[at_] == '\r' || s_[at_] == '\t')) {
            ++at_;
        }
    }
    std::optional<std::string> string()
    {
        if (!peek('"')) {
            return std::nullopt;
        }
        ++at_;
        std::string out;
        while (at_ < s_.size() && s_[at_] != '"') {
            char c = s_[at_++];
            if (c == '\\' && at_ < s_.size()) {
                const char e = s_[at_++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    unsigned v = 0;
                    if (at_ + 4 > s_.size()) {
                        return std::nullopt;
                    }
                    std::from_chars(s_.data() + at_, s_.data() + at_ + 4, v, 16);
                    at_ += 4;
                    c = static_cast<char>(v); // (only what toJson writes: bytes)
                    break;
                }
                default: c = e; break;
                }
            }
            out += c;
        }
        if (!peek('"')) {
            return std::nullopt;
        }
        ++at_;
        return out;
    }
    std::string_view s_;
    std::size_t at_ = 0;
};

Type typeFrom(const std::string& s)
{
    for (const Type t : {Type::Void, Type::Int, Type::Float, Type::String, Type::Object, Type::Function}) {
        if (typeName(t) == s) {
            return t;
        }
    }
    return Type::Int;
}

std::uint32_t u(const Value* v) { return v ? static_cast<std::uint32_t>(v->number()) : 0; }

const Value* field(const Object& o, std::string_view k)
{
    const auto it = o.find(k);
    return it == o.end() ? nullptr : &it->second;
}

DebugVar varFrom(const Value& v)
{
    DebugVar out;
    if (const Object* o = v.object()) {
        if (const Value* n = field(*o, "name")) out.name = n->text();
        if (const Value* t = field(*o, "type")) out.type = typeFrom(t->text());
        out.symbol = u(field(*o, "symbol"));
        out.size = u(field(*o, "size"));
        out.loc = {u(field(*o, "line")), u(field(*o, "column"))};
    }
    return out;
}

} // namespace

const DebugFunction* DebugInfo::function(std::string_view name) const
{
    for (const auto& f : functions) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

std::string DebugInfo::toJson() const
{
    std::string out = "{\"version\":" + std::to_string(version) + ",\"embeddedSource\":" + (embeddedSource ? "true" : "false");
    out += ",\n\"globals\":[";
    for (std::size_t i = 0; i < globals.size(); ++i) {
        out += i ? ",\n" : "\n";
        var(out, globals[i]);
    }
    out += "]";
    // (Only a script with object variables has the key.)
    if (!objectVars.empty()) {
        out += ",\n\"objectVars\":[";
        for (std::size_t i = 0; i < objectVars.size(); ++i) {
            out += i ? ",\n" : "\n";
            var(out, objectVars[i]);
        }
        out += "]";
    }
    out += ",\n\"functions\":[";
    for (std::size_t i = 0; i < functions.size(); ++i) {
        const auto& f = functions[i];
        out += i ? ",\n" : "\n";
        out += "{\"name\":";
        quote(out, f.name);
        out += ",\"index\":" + std::to_string(f.index) + ",\"returns\":\"" + std::string(typeName(f.returns)) +
               "\",\"line\":" + std::to_string(f.loc.line) + ",\"column\":" + std::to_string(f.loc.column) + ",\"symbols\":[";
        for (std::size_t k = 0; k < f.symbols.size(); ++k) {
            out += k ? "," : "";
            var(out, f.symbols[k]);
        }
        out += "],\"lines\":[";
        for (std::size_t k = 0; k < f.lines.size(); ++k) {
            out += (k ? ",[" : "[") + std::to_string(f.lines[k].word) + "," + std::to_string(f.lines[k].loc.line) + "," +
                   std::to_string(f.lines[k].loc.column) + "]";
        }
        out += "]}";
    }
    out += "],\n\"globalInits\":[";
    for (std::size_t i = 0; i < globalInits.size(); ++i) {
        const auto& g = globalInits[i];
        out += (i ? ",[" : "[") + std::to_string(g.symbol) + "," + std::to_string(g.begin) + "," + std::to_string(g.end) + "," +
               std::to_string(g.loc.line) + "," + std::to_string(g.loc.column) + "]";
    }
    out += "],\n\"strings\":[";
    for (std::size_t i = 0; i < strings.size(); ++i) {
        out += (i ? ",[" : "[") + std::to_string(strings[i].id) + "," + std::to_string(strings[i].loc.line) + "," +
               std::to_string(strings[i].loc.column) + "]";
    }
    out += "]}\n";
    return out;
}

std::optional<DebugInfo> DebugInfo::fromJson(std::string_view text)
{
    Reader r(text);
    const auto root = r.value();
    if (!root || !root->object() || !r.atEnd()) {
        return std::nullopt;
    }
    const Object& o = *root->object();
    DebugInfo d;
    d.version = static_cast<int>(u(field(o, "version")));
    if (const Value* e = field(o, "embeddedSource")) {
        d.embeddedSource = e->boolean();
    }
    if (const Value* g = field(o, "globals"); g && g->array()) {
        for (const auto& v : *g->array()) {
            d.globals.push_back(varFrom(v));
        }
    }
    if (const Value* ov = field(o, "objectVars"); ov && ov->array()) {
        for (const auto& v : *ov->array()) {
            d.objectVars.push_back(varFrom(v));
        }
    }
    if (const Value* fs = field(o, "functions"); fs && fs->array()) {
        for (const auto& v : *fs->array()) {
            const Object* fo = v.object();
            if (!fo) {
                return std::nullopt;
            }
            DebugFunction f;
            if (const Value* n = field(*fo, "name")) f.name = n->text();
            f.index = u(field(*fo, "index"));
            if (const Value* t = field(*fo, "returns")) f.returns = typeFrom(t->text());
            f.loc = {u(field(*fo, "line")), u(field(*fo, "column"))};
            if (const Value* s = field(*fo, "symbols"); s && s->array()) {
                for (const auto& sv : *s->array()) {
                    f.symbols.push_back(varFrom(sv));
                }
            }
            if (const Value* l = field(*fo, "lines"); l && l->array()) {
                for (const auto& lv : *l->array()) {
                    const Array* a = lv.array();
                    if (a && a->size() == 3) {
                        f.lines.push_back({u(&(*a)[0]), {u(&(*a)[1]), u(&(*a)[2])}});
                    }
                }
            }
            d.functions.push_back(std::move(f));
        }
    }
    if (const Value* gi = field(o, "globalInits"); gi && gi->array()) {
        for (const auto& v : *gi->array()) {
            const Array* a = v.array();
            if (a && a->size() == 5) {
                d.globalInits.push_back({u(&(*a)[0]), u(&(*a)[1]), u(&(*a)[2]), {u(&(*a)[3]), u(&(*a)[4])}});
            }
        }
    }
    if (const Value* ss = field(o, "strings"); ss && ss->array()) {
        for (const auto& v : *ss->array()) {
            const Array* a = v.array();
            if (a && a->size() == 3) {
                d.strings.push_back({u(&(*a)[0]), {u(&(*a)[1]), u(&(*a)[2])}});
            }
        }
    }
    return d;
}

} // namespace nox::script
