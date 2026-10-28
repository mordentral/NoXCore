// The NoxScript compiler (compiler.hpp): a lexer, a recursive-descent parser to a small syntax
// tree, and one pass over it that checks types and emits the code. The code follows the game's
// own compiled scripts exactly:
//
// - an expression statement leaves its value on the stack (there is no pop: a store pushes its
//   value back and nothing takes it off; the function's exit trims the stack);
// - `if`: cond, JumpIfNot else/end, then, [Jump end, else]; `while`: top: cond, JumpIfNot end,
//   body, Jump top; `for`: init, top: cond, JumpIfNot end, body, incr, Jump top;
// - `switch`: tmp = value, then per case (in order) LoadVarInt tmp, case, IntEq, JumpIf case,
//   then Jump default (or end); the temporary is a local taken when the switch closes;
// - `return` is Return0 (its value left on top), the function's end Return;
// - SELF, OTHER, TRUE and FALSE are GLOBAL's first four symbols, read as variables;
// - each string literal is a new string constant, numbered in the order the source has them;
// - a function's index is its first declaration's place (a prototype or its definition).
//
// Object variables (`int object.hp;` at the top level, `object.hp` in code: NoX Core's syntax)
// are function 0's symbols: the loader's size-0 placeholder 0, a one-word pad (unless
// //@raw_object_vars), then each in declaration order. They are LoadVar / LoadVarPtr with the
// symbol negated; an array element is read as its address and a `+= 0` (IndexPtr, PushInt 0,
// StoreIntAdd), since the VM's IndexInt reads object arrays from the wrong words.
#include "noxcore/script/compiler.hpp"

#include "noxcore/script/arg_values.hpp"
#include "noxcore/script/opcodes.hpp"

#include "language.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>

namespace nox::script {

namespace {

// ---------------------------------------------------------------------------------------------
// Lexer

enum class Tk : std::uint8_t { End, Ident, Int, Float, Str, Punct };

struct Token {
    Tk kind = Tk::End;
    std::string text; ///< identifier, punctuation, or the string literal's bytes
    std::uint32_t ival = 0;
    float fval = 0;
    SourceLoc loc;
};

struct Flag {
    std::string name;
    SourceLoc loc;
};

class Lexer {
public:
    Lexer(std::string_view src, std::vector<Diagnostic>& diags) : src_(src), diags_(diags) {}

    std::vector<Token> run(std::vector<Flag>& flags)
    {
        std::vector<Token> out;
        for (;;) {
            skipSpace(flags);
            Token t;
            t.loc = here();
            if (at_ >= src_.size()) {
                out.push_back(t);
                return out;
            }
            const char c = src_[at_];
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                t.kind = Tk::Ident;
                while (at_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[at_])) || src_[at_] == '_')) {
                    t.text += src_[at_];
                    advance();
                }
            } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                       (c == '.' && at_ + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[at_ + 1])))) {
                number(t);
            } else if (c == '"') {
                string(t);
            } else {
                punct(t);
            }
            out.push_back(std::move(t));
        }
    }

private:
    SourceLoc here() const { return {line_, col_}; }
    void advance()
    {
        if (src_[at_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++at_;
    }
    void error(SourceLoc loc, std::string msg) { diags_.push_back({Diagnostic::Severity::Error, loc, std::move(msg)}); }

    void skipSpace(std::vector<Flag>& flags)
    {
        while (at_ < src_.size()) {
            const char c = src_[at_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                advance();
            } else if (c == '/' && at_ + 1 < src_.size() && src_[at_ + 1] == '/') {
                const SourceLoc loc = here();
                std::string text;
                while (at_ < src_.size() && src_[at_] != '\n') {
                    text += src_[at_];
                    advance();
                }
                // "//@flag": a compile flag (nsc's), the rest of the line its name.
                if (text.size() > 3 && text[2] == '@') {
                    std::string name = text.substr(3);
                    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
                        name.pop_back();
                    }
                    const auto first = name.find_first_not_of(" \t");
                    flags.push_back({first == std::string::npos ? std::string() : name.substr(first), loc});
                }
            } else if (c == '/' && at_ + 1 < src_.size() && src_[at_ + 1] == '*') {
                const SourceLoc loc = here();
                advance();
                advance();
                bool closed = false;
                while (at_ < src_.size()) {
                    if (src_[at_] == '*' && at_ + 1 < src_.size() && src_[at_ + 1] == '/') {
                        advance();
                        advance();
                        closed = true;
                        break;
                    }
                    advance();
                }
                if (!closed) {
                    error(loc, "unterminated /* comment");
                }
            } else {
                break;
            }
        }
    }

    void number(Token& t)
    {
        const std::size_t start = at_;
        bool isFloat = false;
        if (src_[at_] == '0' && at_ + 1 < src_.size() && (src_[at_ + 1] == 'x' || src_[at_ + 1] == 'X')) {
            advance();
            advance();
            std::uint64_t v = 0;
            int digits = 0;
            bool big = false;
            while (at_ < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[at_]))) {
                const char d = src_[at_];
                v = v * 16 + static_cast<std::uint64_t>(std::isdigit(static_cast<unsigned char>(d)) ? d - '0' : (std::tolower(d) - 'a' + 10));
                big = big || v > 0xFFFFFFFFull;
                ++digits;
                advance();
            }
            if (digits == 0) {
                error(t.loc, "a hex number needs digits after 0x");
            } else if (big) {
                error(t.loc, "the number is too big for 32 bits");
            }
            t.kind = Tk::Int;
            t.ival = static_cast<std::uint32_t>(v);
            t.text = std::string(src_.substr(start, at_ - start));
            trailing(t);
            return;
        }
        while (at_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[at_]))) {
            advance();
        }
        if (at_ < src_.size() && src_[at_] == '.') {
            isFloat = true;
            advance();
            while (at_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[at_]))) {
                advance();
            }
        }
        if (at_ < src_.size() && (src_[at_] == 'e' || src_[at_] == 'E')) {
            std::size_t k = at_ + 1;
            if (k < src_.size() && (src_[k] == '+' || src_[k] == '-')) {
                ++k;
            }
            if (k < src_.size() && std::isdigit(static_cast<unsigned char>(src_[k]))) {
                isFloat = true;
                while (at_ < k) {
                    advance();
                }
                while (at_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[at_]))) {
                    advance();
                }
            }
        }
        t.text = std::string(src_.substr(start, at_ - start));
        if (isFloat) {
            t.kind = Tk::Float;
            std::string s = t.text;
            if (s.front() == '.') {
                s.insert(s.begin(), '0');
            }
            const auto r = std::from_chars(s.data(), s.data() + s.size(), t.fval);
            if (r.ec == std::errc::result_out_of_range) {
                error(t.loc, "the float " + t.text + " is out of range");
            }
        } else {
            t.kind = Tk::Int;
            std::uint64_t v = 0;
            bool big = false;
            // A leading 0: octal (C's rule, and nsc's).
            const bool octal = t.text.size() > 1 && t.text[0] == '0';
            for (const char d : t.text) {
                const int digit = d - '0';
                if (octal && digit > 7) {
                    error(t.loc, "bad digit in the octal number " + t.text);
                    break;
                }
                v = v * (octal ? 8 : 10) + static_cast<std::uint64_t>(digit);
                big = big || v > 0xFFFFFFFFull;
            }
            if (big) {
                error(t.loc, "the number " + t.text + " is too big for 32 bits");
            }
            t.ival = static_cast<std::uint32_t>(v);
        }
        trailing(t);
    }

    // A number glued to letters ("12ab") is a mistake.
    void trailing(const Token& t)
    {
        if (at_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[at_])) || src_[at_] == '_')) {
            error(t.loc, "bad suffix on the number " + t.text);
            while (at_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[at_])) || src_[at_] == '_')) {
                advance();
            }
        }
    }

    void string(Token& t)
    {
        t.kind = Tk::Str;
        advance(); // "
        for (;;) {
            if (at_ >= src_.size() || src_[at_] == '\n') {
                error(t.loc, "unterminated string");
                return;
            }
            const char c = src_[at_];
            if (c == '"') {
                advance();
                return;
            }
            if (c != '\\') {
                t.text += c;
                advance();
                continue;
            }
            const SourceLoc escLoc = here();
            advance();
            if (at_ >= src_.size()) {
                error(t.loc, "unterminated string");
                return;
            }
            const char e = src_[at_];
            advance();
            switch (e) {
            case 'n': t.text += '\n'; break;
            case 't': t.text += '\t'; break;
            case 'r': t.text += '\r'; break;
            case 'a': t.text += '\a'; break;
            case 'b': t.text += '\b'; break;
            case 'f': t.text += '\f'; break;
            case 'v': t.text += '\v'; break;
            case '\\': t.text += '\\'; break;
            case '"': t.text += '"'; break;
            case '\'': t.text += '\''; break;
            case '?': t.text += '?'; break;
            case 'x': {
                int v = 0;
                int n = 0;
                while (n < 2 && at_ < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[at_]))) {
                    const char d = src_[at_];
                    v = v * 16 + (std::isdigit(static_cast<unsigned char>(d)) ? d - '0' : std::tolower(d) - 'a' + 10);
                    ++n;
                    advance();
                }
                if (n == 0) {
                    error(escLoc, "\\x needs hex digits");
                }
                t.text += static_cast<char>(v);
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    int v = e - '0';
                    int n = 1;
                    while (n < 3 && at_ < src_.size() && src_[at_] >= '0' && src_[at_] <= '7') {
                        v = v * 8 + (src_[at_] - '0');
                        ++n;
                        advance();
                    }
                    if (v > 255) {
                        error(escLoc, "octal escape out of range");
                    }
                    t.text += static_cast<char>(v);
                } else {
                    error(escLoc, std::string("unknown escape \\") + e);
                }
                break;
            }
        }
    }

    void punct(Token& t)
    {
        static constexpr std::string_view kThree[] = {"<<=", ">>="};
        static constexpr std::string_view kTwo[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=",
                                                    "*=", "/=", "%=", "&=", "|=", "^="};
        t.kind = Tk::Punct;
        const std::string_view rest = src_.substr(at_);
        for (const auto p : kThree) {
            if (rest.starts_with(p)) {
                t.text = p;
                advance(), advance(), advance();
                return;
            }
        }
        for (const auto p : kTwo) {
            if (rest.starts_with(p)) {
                t.text = p;
                advance(), advance();
                return;
            }
        }
        const char c = src_[at_];
        // ('.' only in `object.name`, an object variable.)
        if (std::strchr("+-*/%&|^!~=<>()[]{},;:.", c) && c != '\0') {
            t.text = std::string(1, c);
            advance();
            return;
        }
        error(t.loc, std::string("unexpected character '") + (std::isprint(static_cast<unsigned char>(c)) ? std::string(1, c) : "\\x" + std::to_string(static_cast<unsigned char>(c))) + "'");
        advance();
        t.kind = Tk::Punct;
        t.text = "?";
    }

    std::string_view src_;
    std::vector<Diagnostic>& diags_;
    std::size_t at_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t col_ = 1;
};

// ---------------------------------------------------------------------------------------------
// Syntax tree

struct Expr {
    enum class K : std::uint8_t { Int, Float, Str, Name, ObjVar, Index, Call, Unary, Binary, Assign };
    K k = K::Int;
    SourceLoc loc;
    std::string text; ///< a name, an operator, or a string literal's bytes
    std::uint32_t ival = 0;
    float fval = 0;
    bool paren = false;  ///< written in parentheses
    bool quoted = false; ///< a Call by a function name written as a string literal
    std::vector<std::unique_ptr<Expr>> args; ///< operands / call arguments / [array, index]
};
using ExprP = std::unique_ptr<Expr>;

struct Stmt {
    enum class K : std::uint8_t {
        Block, Decl, Expr, If, While, For, Switch, Case, Default, Break, Continue, Return, Goto,
        Label, GotoIf, GotoIfNot, Empty,
    };
    K k = K::Empty;
    SourceLoc loc;
    // Decl
    Type type = Type::Int;
    std::string name; ///< Decl's variable, Goto's / Label's label
    std::uint32_t arraySize = 0;
    ExprP e; ///< Decl's initialiser, Expr's expression, a condition, Return's value, Case's value
    std::vector<std::unique_ptr<Stmt>> body; ///< Block's statements
    std::unique_ptr<Stmt> s1, s2, s3; ///< If: then, else. While: body. For: init, incr, body. Switch: body.
};
using StmtP = std::unique_ptr<Stmt>;

struct Param {
    Type type = Type::Int;
    std::string name;
    SourceLoc loc;
};

struct TopItem {
    enum class K : std::uint8_t { Global, Function, Prototype };
    K k = K::Global;
    SourceLoc loc;
    Type type = Type::Int;
    std::string name;
    std::uint32_t arraySize = 0;
    ExprP init;
    std::vector<Param> params;
    StmtP body;
    bool quoted = false; ///< a function named by a string literal
    bool objectVar = false; ///< a Global declared `type object.name`: an object variable
};

// ---------------------------------------------------------------------------------------------
// Parser

class Parser {
public:
    Parser(std::vector<Token> toks, std::vector<Diagnostic>& diags) : t_(std::move(toks)), diags_(diags) {}

    std::vector<TopItem> program()
    {
        std::vector<TopItem> items;
        while (peek().kind != Tk::End) {
            const std::size_t before = p_;
            const std::size_t errorsBefore = errors();
            topLevel(items);
            if (errors() != errorsBefore) {
                syncTop();
            }
            if (p_ == before) {
                ++p_; // (no progress: skip the token)
            }
        }
        return items;
    }

private:
    const Token& peek(std::size_t k = 0) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
    const Token& next()
    {
        const Token& t = t_[p_];
        if (p_ + 1 < t_.size()) {
            ++p_;
        }
        return t;
    }
    bool isPunct(std::string_view s, std::size_t k = 0) const { return peek(k).kind == Tk::Punct && peek(k).text == s; }
    bool isWord(std::string_view s, std::size_t k = 0) const { return peek(k).kind == Tk::Ident && peek(k).text == s; }
    bool accept(std::string_view s)
    {
        if (isPunct(s)) {
            next();
            return true;
        }
        return false;
    }
    std::size_t errors() const
    {
        return static_cast<std::size_t>(std::count_if(diags_.begin(), diags_.end(), [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Error; }));
    }
    void error(SourceLoc loc, std::string msg)
    {
        // One error per place: a cascade at the same spot says nothing new.
        if (!diags_.empty() && diags_.back().loc == loc) {
            return;
        }
        diags_.push_back({Diagnostic::Severity::Error, loc, std::move(msg)});
    }
    std::string describe(const Token& t) const
    {
        switch (t.kind) {
        case Tk::End: return "the end of the file";
        case Tk::Ident: return "'" + t.text + "'";
        case Tk::Int:
        case Tk::Float: return "the number " + t.text;
        case Tk::Str: return "a string";
        default: return "'" + t.text + "'";
        }
    }
    bool expect(std::string_view s)
    {
        if (accept(s)) {
            return true;
        }
        error(peek().loc, "expected '" + std::string(s) + "' but found " + describe(peek()));
        return false;
    }
    // After an error at the top level: on to the next ';' or the '}' closing a body.
    void syncTop()
    {
        int depth = 0;
        while (peek().kind != Tk::End) {
            if (isPunct("{")) {
                ++depth;
            } else if (isPunct("}")) {
                if (--depth <= 0) {
                    next();
                    return;
                }
            } else if (isPunct(";") && depth == 0) {
                next();
                return;
            }
            next();
        }
    }
    void syncStmt()
    {
        while (peek().kind != Tk::End && !isPunct(";") && !isPunct("}") && !isPunct("{")) {
            next();
        }
        accept(";");
    }

    static std::optional<Type> typeWord(const Token& t)
    {
        if (t.kind != Tk::Ident) {
            return std::nullopt;
        }
        if (t.text == "int") return Type::Int;
        if (t.text == "float") return Type::Float;
        if (t.text == "string") return Type::String;
        if (t.text == "object") return Type::Object;
        if (t.text == "void") return Type::Void;
        return std::nullopt;
    }

    bool identifier(std::string& out, SourceLoc& loc)
    {
        if (peek().kind != Tk::Ident || typeWord(peek()) || isKeyword(peek().text)) {
            error(peek().loc, "expected a name but found " + describe(peek()));
            return false;
        }
        loc = peek().loc;
        out = next().text;
        return true;
    }

    /// `object.` before a name: an object variable (NoX Core's syntax). Taken when there.
    bool objectPrefix()
    {
        if (!isWord("object") || !isPunct(".", 1)) {
            return false;
        }
        next();
        next();
        return true;
    }

    /// A declarator's name: `name` or `object.name` (the location is the name's).
    bool declaratorName(std::string& out, SourceLoc& loc, bool& objectVar)
    {
        objectVar = objectPrefix();
        return identifier(out, loc);
    }

public:
    static bool isKeyword(std::string_view s)
    {
        static const std::set<std::string_view> kw(std::begin(detail::kKeywords), std::end(detail::kKeywords));
        return kw.contains(s);
    }

private:
    void topLevel(std::vector<TopItem>& items)
    {
        const auto type = typeWord(peek());
        if (!type) {
            error(peek().loc, "expected a declaration (a type: int, float, string, object or void) but found " + describe(peek()));
            return;
        }
        next();
        TopItem first;
        first.type = *type;
        // A function's name may be a string literal: any name the game's own scripts use
        // (a prefab's "Name%Prefix%X%Y", or one that isn't an identifier).
        const bool quoted = peek().kind == Tk::Str && isPunct("(", 1);
        if (quoted) {
            first.loc = peek().loc;
            first.name = next().text;
            first.quoted = true;
        } else if (!declaratorName(first.name, first.loc, first.objectVar)) {
            return;
        }
        if (isPunct("(")) {
            if (first.objectVar) {
                error(first.loc, "a function can't be named object." + first.name);
                return;
            }
            first.k = TopItem::K::Function;
            next();
            params(first.params);
            if (accept(";")) {
                first.k = TopItem::K::Prototype;
            } else if (isPunct("{")) {
                first.body = block();
            } else {
                error(peek().loc, "expected '{' or ';' after the function's parameters but found " + describe(peek()));
                return;
            }
            items.push_back(std::move(first));
            return;
        }
        if (*type == Type::Void) {
            error(first.loc, "a variable can't be void");
        }
        // Globals: "type a, b[4], c = expr;".
        TopItem item = std::move(first);
        for (;;) {
            item.k = TopItem::K::Global;
            item.type = *type;
            arraySuffix(item.arraySize);
            if (accept("=")) {
                if (item.objectVar) {
                    error(item.loc, "an object variable can't have an initialiser: every object's start at 0");
                } else if (item.arraySize) {
                    error(item.loc, "an array can't have an initialiser");
                }
                item.init = expression();
            }
            items.push_back(std::move(item));
            if (!accept(",")) {
                break;
            }
            item = TopItem{};
            if (!declaratorName(item.name, item.loc, item.objectVar)) {
                return;
            }
        }
        expect(";");
    }

    void arraySuffix(std::uint32_t& size)
    {
        if (!isPunct("[")) {
            return;
        }
        const SourceLoc loc = next().loc;
        if (peek().kind != Tk::Int) {
            error(peek().loc, "an array's length must be a number");
            syncStmt();
            return;
        }
        size = next().ival;
        if (size < 2) {
            error(loc, "an array's length must be more than 1");
            size = 2;
        }
        expect("]");
    }

    void params(std::vector<Param>& out)
    {
        if (accept(")")) {
            return;
        }
        if (isWord("void") && isPunct(")", 1)) {
            next();
            next();
            return;
        }
        for (;;) {
            Param p;
            p.loc = peek().loc;
            const auto type = typeWord(peek());
            if (!type || *type == Type::Void) {
                error(peek().loc, "expected a parameter's type but found " + describe(peek()));
                syncStmt();
                return;
            }
            next();
            p.type = *type;
            if (!identifier(p.name, p.loc)) {
                return;
            }
            if (isPunct("[")) {
                error(peek().loc, "a parameter can't be an array");
                std::uint32_t ignored = 0;
                arraySuffix(ignored);
            }
            out.push_back(std::move(p));
            if (accept(")")) {
                return;
            }
            if (!expect(",")) {
                return;
            }
        }
    }

    StmtP block()
    {
        auto b = std::make_unique<Stmt>();
        b->k = Stmt::K::Block;
        b->loc = peek().loc;
        expect("{");
        while (!isPunct("}") && peek().kind != Tk::End) {
            const std::size_t before = p_;
            const std::size_t errorsBefore = errors();
            auto s = statement();
            if (errors() != errorsBefore) {
                syncStmt();
            }
            if (s) {
                b->body.push_back(std::move(s));
            }
            if (p_ == before) {
                next();
            }
        }
        expect("}");
        return b;
    }

    StmtP make(Stmt::K k, SourceLoc loc)
    {
        auto s = std::make_unique<Stmt>();
        s->k = k;
        s->loc = loc;
        return s;
    }

    StmtP statement()
    {
        const Token& t = peek();
        const SourceLoc loc = t.loc;
        if (isPunct("{")) {
            return block();
        }
        if (accept(";")) {
            return make(Stmt::K::Empty, loc);
        }
        // (`object.name ...` is a use of an object variable, not a declaration of type object.)
        const bool objectUse = isWord("object") && isPunct(".", 1);
        if (const auto type = objectUse ? std::nullopt : typeWord(t)) {
            next();
            if (*type == Type::Void) {
                error(loc, "a variable can't be void");
            }
            // "type a, b = 1;" as a block of declarations (the block adds no scope: Decls only).
            auto group = make(Stmt::K::Block, loc);
            group->k = Stmt::K::Decl; // a lone declaration, or the first of several (below)
            std::vector<StmtP> decls;
            for (;;) {
                auto d = make(Stmt::K::Decl, peek().loc);
                d->type = *type;
                if (isWord("object") && isPunct(".", 1)) {
                    error(peek().loc, "object variables are declared at the top level, outside functions");
                    return nullptr;
                }
                if (!identifier(d->name, d->loc)) {
                    return nullptr;
                }
                arraySuffix(d->arraySize);
                if (accept("=")) {
                    if (d->arraySize) {
                        error(d->loc, "an array can't have an initialiser");
                    }
                    d->e = expression();
                }
                decls.push_back(std::move(d));
                if (!accept(",")) {
                    break;
                }
            }
            expect(";");
            if (decls.size() == 1) {
                return std::move(decls.front());
            }
            auto multi = make(Stmt::K::Block, loc);
            multi->name = "decls"; // (a declaration list: no scope of its own)
            multi->body = std::move(decls);
            return multi;
        }
        if (t.kind == Tk::Ident) {
            if (t.text == "if") {
                next();
                auto s = make(Stmt::K::If, loc);
                expect("(");
                s->e = expression();
                expect(")");
                s->s1 = statement();
                if (isWord("else")) {
                    next();
                    s->s2 = statement();
                }
                return s;
            }
            if (t.text == "while") {
                next();
                auto s = make(Stmt::K::While, loc);
                expect("(");
                s->e = expression();
                expect(")");
                s->s1 = statement();
                return s;
            }
            if (t.text == "for") {
                next();
                auto s = make(Stmt::K::For, loc);
                expect("(");
                if (!isPunct(";")) {
                    if (typeWord(peek()) && !(isWord("object") && isPunct(".", 1))) {
                        error(peek().loc, "no declarations in a for's first part (declare the variable before the loop)");
                    }
                    s->s1 = simple();
                }
                expect(";");
                if (!isPunct(";")) {
                    s->e = expression();
                }
                expect(";");
                if (!isPunct(")")) {
                    s->s2 = simple();
                }
                expect(")");
                s->s3 = statement();
                return s;
            }
            if (t.text == "switch") {
                next();
                auto s = make(Stmt::K::Switch, loc);
                expect("(");
                s->e = assignment(); // (switch (v = value): v is its temporary)
                expect(")");
                if (!isPunct("{")) {
                    error(peek().loc, "a switch needs a { block }");
                    return nullptr;
                }
                s->s1 = block();
                return s;
            }
            if (t.text == "case") {
                next();
                auto s = make(Stmt::K::Case, loc);
                s->e = expression();
                expect(":");
                return s;
            }
            if (t.text == "default") {
                next();
                auto s = make(Stmt::K::Default, loc);
                expect(":");
                return s;
            }
            if (t.text == "break" || t.text == "continue") {
                next();
                auto s = make(t.text == "break" ? Stmt::K::Break : Stmt::K::Continue, loc);
                expect(";");
                return s;
            }
            if (t.text == "return") {
                next();
                auto s = make(Stmt::K::Return, loc);
                if (!isPunct(";")) {
                    s->e = expression();
                }
                expect(";");
                return s;
            }
            if (t.text == "goto") {
                next();
                auto s = make(Stmt::K::Goto, loc);
                SourceLoc l;
                identifier(s->name, l);
                expect(";");
                return s;
            }
            if (t.text == "__goto_if" || t.text == "__goto_ifnot") {
                next();
                auto s = make(t.text == "__goto_if" ? Stmt::K::GotoIf : Stmt::K::GotoIfNot, loc);
                expect("(");
                s->e = expression();
                expect(",");
                SourceLoc l;
                identifier(s->name, l);
                expect(")");
                expect(";");
                return s;
            }
            if (t.text == "else") {
                error(loc, "'else' without an 'if'");
                next();
                return nullptr;
            }
            if (t.text == "do") {
                error(loc, "do ... while isn't part of the language (use while)");
                next();
                return nullptr;
            }
            if (!isKeyword(t.text) && isPunct(":", 1)) {
                auto s = make(Stmt::K::Label, loc);
                s->name = next().text;
                next();
                return s;
            }
        }
        auto s = simple();
        expect(";");
        return s;
    }

    // An assignment or an expression, as a statement (and a for's first and third parts).
    StmtP simple()
    {
        const SourceLoc loc = peek().loc;
        auto s = make(Stmt::K::Expr, loc);
        s->e = assignment();
        return s;
    }

    ExprP assignment()
    {
        auto lhs = expression();
        static constexpr std::string_view kOps[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
        for (const auto op : kOps) {
            if (isPunct(op)) {
                auto a = std::make_unique<Expr>();
                a->k = Expr::K::Assign;
                a->loc = next().loc;
                a->text = op;
                a->args.push_back(std::move(lhs));
                a->args.push_back(assignment()); // (right-associative: a = b = c)
                return a;
            }
        }
        return lhs;
    }

public:
    ExprP expression() { return binary(0); }

private:
    // C's precedence, loosest first (language.hpp); -1 for no binary operator.
    static int precedence(std::string_view op)
    {
        const detail::BinaryOperator* o = detail::binaryOperator(op);
        return o ? o->precedence : -1;
    }

    ExprP binary(int minPrec)
    {
        auto lhs = unary();
        for (;;) {
            if (peek().kind != Tk::Punct) {
                return lhs;
            }
            const int prec = precedence(peek().text);
            if (prec < 0 || prec <= minPrec - 1 || prec < minPrec) {
                return lhs;
            }
            auto e = std::make_unique<Expr>();
            e->k = Expr::K::Binary;
            e->loc = peek().loc;
            e->text = next().text;
            e->args.push_back(std::move(lhs));
            e->args.push_back(binary(prec + 1));
            lhs = std::move(e);
        }
    }

    ExprP unary()
    {
        if (isPunct("-") || isPunct("!") || isPunct("~") || isPunct("+")) {
            auto e = std::make_unique<Expr>();
            e->k = Expr::K::Unary;
            e->loc = peek().loc;
            e->text = next().text;
            e->args.push_back(unary());
            return e;
        }
        return postfix();
    }

    ExprP postfix()
    {
        auto e = primary();
        while (e && isPunct("[")) {
            auto idx = std::make_unique<Expr>();
            idx->k = Expr::K::Index;
            idx->loc = next().loc;
            idx->args.push_back(std::move(e));
            idx->args.push_back(expression());
            expect("]");
            e = std::move(idx);
        }
        return e;
    }

    ExprP primary()
    {
        auto e = std::make_unique<Expr>();
        const Token& t = peek();
        e->loc = t.loc;
        switch (t.kind) {
        case Tk::Int:
            e->k = Expr::K::Int;
            e->ival = next().ival;
            return e;
        case Tk::Float:
            e->k = Expr::K::Float;
            e->fval = next().fval;
            return e;
        case Tk::Str:
            e->k = Expr::K::Str;
            e->text = next().text;
            if (isPunct("(")) {
                e->quoted = true; // a call by a quoted function name
                goto callArgs;
            }
            return e;
        case Tk::Ident:
            if (isWord("object") && isPunct(".", 1)) {
                // object.name: the trigger's (self's) object variable.
                next();
                next();
                e->k = Expr::K::ObjVar;
                SourceLoc nameLoc;
                identifier(e->text, nameLoc);
                return e;
            }
            if (isKeyword(t.text) || typeWord(t)) {
                break;
            }
            e->k = Expr::K::Name;
            e->text = next().text;
        callArgs:
            if (accept("(")) {
                e->k = Expr::K::Call;
                if (!accept(")")) {
                    for (;;) {
                        e->args.push_back(expression());
                        if (accept(")")) {
                            break;
                        }
                        if (!expect(",")) {
                            break;
                        }
                    }
                }
            }
            return e;
        case Tk::Punct:
            if (t.text == "(") {
                next();
                auto inner = assignment();
                expect(")");
                if (inner) {
                    inner->paren = true;
                }
                return inner;
            }
            break;
        default:
            break;
        }
        error(t.loc, "expected an expression but found " + describe(t));
        e->k = Expr::K::Int;
        return e;
    }

    std::vector<Token> t_;
    std::size_t p_ = 0;
    std::vector<Diagnostic>& diags_;
};

// ---------------------------------------------------------------------------------------------
// Checking and code

constexpr std::uint32_t w(Op o) { return static_cast<std::uint32_t>(o); }

struct GlobalVar {
    std::uint32_t symbol = 0;
    Type type = Type::Int;
    std::uint32_t size = 1;
    SourceLoc loc;
    bool readOnly = false;
};

struct FunctionInfo {
    std::uint32_t index = 0;
    Type returns = Type::Void;
    std::vector<Param> params;
    SourceLoc loc;
    bool defined = false;
};

struct LocalVar {
    std::uint32_t symbol = 0;
    Type type = Type::Int;
    std::uint32_t size = 1;
};

class Generator {
public:
    Generator(std::vector<Diagnostic>& diags, bool ignoreObjectType, bool rawObjectVars)
        : diags_(diags)
        , ignoreObject_(ignoreObjectType)
        , rawObject_(rawObjectVars)
    {
    }

    std::optional<ScriptFile> run(std::vector<TopItem>& items, std::string_view source, const CompileOptions& opts, DebugInfo& debug)
    {
        debug_ = &debug;
        lint_ = opts.lint;
        argValues_ = opts.lint ? opts.argValues : nullptr;
        allowObject_ = opts.allowObjectVariables;
        debug.embeddedSource = opts.embedSource;
        if (opts.embedSource) {
            strings_.push_back(std::string(kEmbeddedSourcePrefix) + std::string(source));
        }
        // The language's own globals, GLOBAL's first four symbols (language.hpp).
        for (std::uint32_t i = 0; i < 4; ++i) {
            GlobalVar g;
            g.symbol = i;
            g.readOnly = true;
            globalSizes_.push_back(1);
            std::string upper(detail::kLanguageGlobals[i]);
            std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return static_cast<char>(std::toupper(c)); });
            globals_.emplace(std::string(detail::kLanguageGlobals[i]), g);
            globals_.emplace(upper, g);
            debug.globals.push_back({upper, Type::Int, i, 1, {}});
        }
        // Functions get their index at their first declaration; all are known before any body
        // (a call may come before the callee's definition).
        for (const TopItem& it : items) {
            if (it.k == TopItem::K::Global) {
                continue;
            }
            declareFunction(it);
        }
        for (TopItem& it : items) {
            if (it.k == TopItem::K::Global && it.objectVar) {
                objectVariable(it);
            } else if (it.k == TopItem::K::Global) {
                global(it);
            } else if (it.k == TopItem::K::Function) {
                function(it);
            }
        }
        for (const auto& [name, f] : functions_) {
            if (!f.defined) {
                error(f.loc, "the function " + name + " is declared but never defined");
            }
        }
        if (opts.lint) {
            lintProgram(opts);
        }
        // The game's loader puts the constants in its 1024-entry string table without a check
        // (docs/original_bugs.md, OB-200); the strings made while a script runs share what's left.
        if (strings_.size() > 1024) {
            error({1, 1}, std::to_string(strings_.size()) + " string constants: the game's string table holds 1024");
        } else if (strings_.size() > 960) {
            warning({1, 1}, std::to_string(strings_.size()) + " string constants leave few of the game's 1024 strings for the ones made while it runs");
        }
        if (hasErrors()) {
            return std::nullopt;
        }
        ScriptFile file;
        file.strings = strings_;
        // Function 0: the object variables' layout. Its symbol 0 is the loader's size-0 placeholder
        // (never stored); then, unless //@raw_object_vars, a one-word pad, then the variables in
        // declaration order. With none, only the placeholder, as every stored map has it.
        Function f0;
        f0.name = "GLOBAL";
        layout(f0, objectSizes_);
        f0.code = {w(Op::Return)};
        file.functions.push_back(std::move(f0));
        Function f1;
        f1.name = "GLOBAL";
        layout(f1, globalSizes_);
        f1.code = std::move(globalCode_);
        f1.code.push_back(w(Op::Return));
        file.functions.push_back(std::move(f1));
        file.functions.resize(2 + functionOrder_.size());
        for (std::size_t i = 0; i < functionOrder_.size(); ++i) {
            file.functions[2 + i] = std::move(bodies_[i]);
        }
        for (std::size_t i = 0; i < file.functions.size(); ++i) {
            if (const auto bad = check(file, i)) {
                error({1, 1}, "internal: the code made doesn't check: " + *bad);
                return std::nullopt;
            }
        }
        std::sort(debug.functions.begin(), debug.functions.end(), [](const DebugFunction& a, const DebugFunction& b) { return a.index < b.index; });
        // Through the reader once, so the result is what the game would load (prefab names
        // parsed, symbol offsets laid out).
        return ScriptFile::parse(file.serialize());
    }

private:
    // A variable by name: a local (innermost first), else a global; or an object variable
    // (objectRef).
    struct VarRef {
        bool found = false;
        bool global = false;
        bool readOnly = false;
        std::uint32_t symbol = 0;
        Type type = Type::Int;
        std::uint32_t size = 1;
        bool object = false; ///< an object variable: function 0's symbol, LoadVar's index negated
        std::string name;    ///< as the source writes it ("object.x" for one)
    };

    struct FnState; // (a function being compiled: below)

    struct ObjectVar {
        std::uint32_t symbol = 0; ///< function 0's
        Type type = Type::Int;
        std::uint32_t size = 1;
        SourceLoc loc;
    };

    bool hasErrors() const
    {
        return std::any_of(diags_.begin(), diags_.end(), [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Error; });
    }
    void error(SourceLoc loc, std::string msg) { diags_.push_back({Diagnostic::Severity::Error, loc, std::move(msg)}); }
    void warning(SourceLoc loc, std::string msg) { diags_.push_back({Diagnostic::Severity::Warning, loc, std::move(msg)}); }

    // ---- lint (CompileOptions::lint) ----

    /// The whole program's lints, once every function is through.
    void lintProgram(const CompileOptions& opts)
    {
        for (const auto& [name, g] : globals_) {
            if (!g.readOnly && !globalReads_.contains(g.symbol)) {
                warning(g.loc, "the global " + name + " is never read");
            }
        }
        for (const auto& [name, v] : objectVars_) {
            if (!objectReads_.contains(v.symbol)) {
                warning(v.loc, "the object variable object." + name + " is never read");
            }
        }
        for (const std::string& name : functionOrder_) {
            const FunctionInfo& f = functions_.at(name);
            if (f.defined && f.returns != Type::Void && isEntryPointName(name)) {
                warning(f.loc, "the entry point " + name + " returns a value: the game calls it without taking the value, which "
                               "stays on the stack and holds up every handler after it (OB-204); declare it void");
            }
            // Object variables where the game calls with no object (self).
            if (f.defined && isEntryPointName(name)) {
                if (const auto used = objectUseFrom(name)) {
                    warning(f.loc, noObjectText(name, *used, "the game calls the entry point " + name));
                }
            }
        }
        for (const auto& [name, loc, builtin] : timerSites_) {
            if (const auto used = objectUseFrom(name)) {
                warning(loc, noObjectText(name, *used, builtin + " calls " + name));
            }
        }
        if (!opts.externalNames) {
            return;
        }
        const std::set<std::string_view> external(opts.externalNames->begin(), opts.externalNames->end());
        for (const std::string& name : functionOrder_) {
            const FunctionInfo& f = functions_.at(name);
            if (f.defined && !usedFunctions_.contains(name) && !external.contains(name) && !isEntryPointName(name) &&
                !name.starts_with(kTestPrefix)) {
                warning(f.loc, "the function " + name + " is never called, given to a builtin or named by the map");
            }
        }
    }

    /// The object variable a function uses, itself or through the script functions it calls
    /// (the first found), if any.
    std::optional<std::string> objectUseFrom(const std::string& name) const
    {
        std::set<std::string> seen;
        std::vector<std::string> todo{name};
        while (!todo.empty()) {
            const std::string f = std::move(todo.back());
            todo.pop_back();
            if (!seen.insert(f).second) {
                continue;
            }
            if (const auto u = objectUse_.find(f); u != objectUse_.end()) {
                return f == name ? u->second : u->second + " (in " + f + ")";
            }
            if (const auto c = callees_.find(f); c != callees_.end()) {
                todo.insert(todo.end(), c->second.rbegin(), c->second.rend());
            }
        }
        return std::nullopt;
    }

    static std::string noObjectText(const std::string& function, const std::string& used, const std::string& caller)
    {
        return function + " uses object." + used + ", but " + caller + " with no object (self): there object variables read 0 and "
               "their stores are lost, and GAME.EXE crashes";
    }

public:
    bool objectVariablesRefused() const { return refusedObject_; }

private:
    /// An object variable's declaration or use without CompileOptions::allowObjectVariables: an
    /// error at each (the first saying why and how to turn them on). The variable is still laid
    /// out and typed, so nothing else is reported for it.
    void refuseObjectVariable(SourceLoc loc, const std::string& name)
    {
        if (allowObject_) {
            return;
        }
        if (!refusedObject_) {
            refusedObject_ = true;
            error(loc, "object." + name + ": object variables are a NoX Core-only feature and are off. The original game never gives "
                       "objects their storage and crashes on a script that uses them, so a map with them needs a NoX Core server. "
                       "To allow them: " + std::string(kObjectVariablesHowToEnable));
            return;
        }
        error(loc, "object." + name + ": object variables are off (a NoX Core-only feature; the first error says how to allow them)");
    }

    /// `type object.name[N];`: the next symbol of function 0 (the pad first, unless raw).
    void objectVariable(TopItem& it)
    {
        const Type type = norm(it.type);
        const std::uint32_t size = it.arraySize ? it.arraySize : 1;
        if (objectVars_.contains(it.name)) {
            error(it.loc, "object." + it.name + " is declared twice");
            return;
        }
        refuseObjectVariable(it.loc, it.name);
        if (objectVars_.empty()) {
            // (Once, at the first.) Measured in GAME.EXE: an object's script-variable pointer is set
            // to null when it's made and nothing ever allocates it, so the VM's object-variable
            // loads and stores fault there.
            if (allowObject_) {
                warning(it.loc, "object variables work only in NoX Core: GAME.EXE never gives objects their storage, so a script "
                                "that reads or writes one crashes the game there (docs/original_bugs.md)");
            }
            if (!rawObject_) {
                // The pad: the first variable would sit at offset 0, which the VM's store negates to
                // 0, a local word (the game's own quirk). Never named, never used.
                objectSizes_.push_back(1);
                debug_->objectVars.push_back({"", Type::Int, 1, 1, {}});
            }
        }
        ObjectVar v;
        v.symbol = static_cast<std::uint32_t>(objectSizes_.size());
        v.type = type;
        v.size = size;
        v.loc = it.loc;
        objectSizes_.push_back(size);
        objectVars_.emplace(it.name, v);
        debug_->objectVars.push_back({it.name, type, v.symbol, size, it.loc});
    }

    /// An object variable by name, as a use in code: none (reported) when undeclared, or in a
    /// global's initialiser (GLOBAL runs with no object).
    VarRef objectRef(const Expr& e)
    {
        refuseObjectVariable(e.loc, e.text);
        const auto it = objectVars_.find(e.text);
        if (it == objectVars_.end()) {
            error(e.loc, "no object variable object." + e.text + " (declare it at the top level: int object." + e.text + ";)");
            return {};
        }
        if (!fn_) {
            error(e.loc, "object." + e.text + " in a global's initialiser: GLOBAL runs with no object");
            return {};
        }
        if (!objectUse_.contains(fn_->name)) {
            objectUse_.emplace(fn_->name, e.text);
        }
        VarRef v;
        v.found = true;
        v.object = true;
        v.symbol = it->second.symbol;
        v.type = it->second.type;
        v.size = it->second.size;
        v.name = "object." + e.text;
        return v;
    }

    /// An object variable at offset 0 of the object's words (the first, with //@raw_object_vars):
    /// a store's offset, negated, is 0, so it goes to the function's own first local word.
    void warnOffsetZero(const VarRef& v, SourceLoc loc)
    {
        if (v.object && v.symbol == 1) {
            warning(loc, "a store to " + v.name + " goes to the function's own first local, not the object: it is at offset 0 "
                         "of the object's variables and the VM negates that offset to 0 (declare it after another, or drop "
                         "//@raw_object_vars)");
        }
    }

    /// LoadVar's operands for a variable: the global flag, the symbol (an object variable's negated).
    static std::uint32_t globalWord(const VarRef& v) { return v.global ? 1u : 0u; }
    static std::uint32_t symbolWord(const VarRef& v)
    {
        return v.object ? static_cast<std::uint32_t>(-static_cast<std::int32_t>(v.symbol)) : v.symbol;
    }

    /// A variable read (not only written): for the never-read lint.
    template <class Ref> void markRead(const Ref& v)
    {
        if (v.object) {
            objectReads_.insert(v.symbol);
        } else if (v.global) {
            globalReads_.insert(v.symbol);
        } else if (fn_) {
            fn_->reads.insert(v.symbol);
        }
    }

    /// A string value the VM makes while the script runs (a concatenation, IntToString,
    /// FloatToString), as opposed to a constant's id.
    static bool builtString(const Expr& e)
    {
        switch (e.k) {
        case Expr::K::Binary:
            return e.text == "+";
        case Expr::K::Call:
            return !e.quoted && (e.text == "IntToString" || e.text == "FloatToString");
        case Expr::K::Assign:
            return e.text == "+=" || builtString(*e.args[1]);
        default:
            return false;
        }
    }

    /// `name` as the source writes it; an object variable's ("object.x") outlives the handler too.
    void lintBuiltStringInGlobal(const Expr& value, const std::string& name, SourceLoc loc)
    {
        if (lint_ && builtString(value)) {
            const std::string what = name.starts_with("object.") ? "the object variable " + name : "the global " + name;
            warning(loc, "a built string kept in " + what + ": the game drops it at the end of the next handler "
                         "and gives its id to the next string made, so " + name + " then reads another string (OB-201)");
        }
    }

    // ---- assert (NoX Core's) -------------------------------------------

    /// assert(cond[, "message"]): `cond; JumpIf over; PushString "ASSERT line N: message";
    /// CallBuiltin PrintToAll; over:`. Plain stock code: the test runner (server/script_tests.hpp)
    /// catches the print.
    Type assertCall(Expr& e, bool statement)
    {
        if (e.args.empty() || e.args.size() > 2) {
            error(e.loc, "assert takes a condition and, optionally, a message (a string literal)");
            return Type::Void;
        }
        if (e.args.size() == 2 && e.args[1]->k != Expr::K::Str) {
            error(e.args[1]->loc, "assert's message must be a string literal");
            return Type::Void;
        }
        if (!statement) {
            error(e.loc, "assert is a statement, not a value");
            return Type::Void;
        }
        condition(*e.args[0], "assert");
        emit(w(Op::JumpIf), 0);
        const std::size_t over = here() - 1;
        Expr msg;
        msg.k = Expr::K::Str;
        msg.loc = e.args.size() == 2 ? e.args[1]->loc : e.loc;
        msg.text = std::string(kAssertPrefix) + "line " + std::to_string(e.loc.line) + ": " +
                   (e.args.size() == 2 ? e.args[1]->text : std::string("assertion failed"));
        emit(w(Op::PushString), addString(msg));
        emit(w(Op::CallBuiltin), *findBuiltin("PrintToAll"));
        (*code_)[over] = here();
        return Type::Void;
    }

    // ---- Wait (NoX Core's, test-only) ----------------------------------

    /// `Wait` is the intrinsic unless the script defines a function of that name (as for assert).
    bool waitIsIntrinsic() const { return !functions_.contains("Wait"); }

    static std::size_t countWaits(const Expr& e)
    {
        std::size_t n = e.k == Expr::K::Call && !e.quoted && e.text == "Wait" ? 1 : 0;
        for (const auto& a : e.args) {
            n += countWaits(*a);
        }
        return n;
    }
    static std::size_t countWaits(const Stmt& s)
    {
        std::size_t n = s.e ? countWaits(*s.e) : 0;
        for (const auto& b : s.body) {
            n += countWaits(*b);
        }
        for (const Stmt* c : {s.s1.get(), s.s2.get(), s.s3.get()}) {
            n += c ? countWaits(*c) : 0;
        }
        return n;
    }

    /// A function that may Wait: a test, as the runner calls them (void, named Test*, no arguments).
    static bool isWaitingTest(const TopItem& it, const FunctionInfo& info)
    {
        return it.name.starts_with(kTestPrefix) && it.params.empty() && info.returns == Type::Void;
    }

    /// Wait(frames) in a test: the call ends and FrameTimer calls the test again after the frames,
    /// where the prologue jumps back in after the Wait. Locals persist per function (the VM's), so
    /// the test's locals are all still there:
    ///   `LoadVarPtr r; PushInt k; StoreInt; frames; PushInt <the test>; CallBuiltin FrameTimer;
    ///    Return0; RESUME_k: LoadVarPtr r; PushInt 0; StoreInt`
    /// Plain stock code: GAME.EXE runs it too (nothing there calls a Test* function, but one that
    /// was called would wait the same way).
    Type waitCall(Expr& e, bool statement)
    {
        if (!fn_ || !fn_->resume) {
            error(e.loc, "Wait(frames) works only in a test: a void function named " + std::string(kTestPrefix) +
                             "* with no arguments (the test runner calls it again after the frames)");
            return Type::Void;
        }
        if (e.args.size() != 1) {
            error(e.loc, "Wait takes one argument: the frames to wait (an int)");
            return Type::Void;
        }
        if (!statement) {
            error(e.loc, "Wait is a statement, not a value");
            return Type::Void;
        }
        const auto k = static_cast<std::uint32_t>(++fn_->waits);
        emit(w(Op::LoadVarPtr), 0, *fn_->resume, w(Op::PushInt), k, w(Op::StoreInt));
        const Type t = expr(*e.args[0], Type::Int);
        if (t != Type::Int && t != Type::Void) {
            error(e.args[0]->loc, "Wait's frames must be an int, not a " + std::string(typeName(t)));
        }
        emit(w(Op::PushInt), fn_->info->index, w(Op::CallBuiltin), *findBuiltin("FrameTimer"), w(Op::Return0));
        if (k <= fn_->resumeJumps.size()) {
            (*code_)[fn_->resumeJumps[k - 1]] = here();
        }
        emit(w(Op::LoadVarPtr), 0, *fn_->resume, w(Op::PushInt), 0, w(Op::StoreInt));
        return Type::Void;
    }

    // ---- lint: a built string kept in a local across calls (OB-201) ----

    /// A string local read or stored (not the arguments' nor arrays': `symbol` a scalar local).
    void noteStringUse(const VarRef& v, std::uint32_t word, bool store, SourceLoc loc)
    {
        if (lint_ && fn_ && !v.global && !v.object && v.type == Type::String && v.size == 1) {
            fn_->stringUses.push_back({word, v.symbol, store, loc});
        }
    }

    /// Locals persist across calls (the VM's per-function words), so a string local read where
    /// this call may not have set it yet reads what an earlier call left: if that can be a built
    /// string, the game has dropped it at the end of a handler since and given its id to another
    /// string. Warned when such a local is ever given a built string and some read of it can come
    /// before every store in the call: a forward flow over the function's code (jumps, gotos,
    /// switches, loops and returns as they are), "set on every path" meeting by intersection.
    void lintBuiltStringInLocals(const FnState& st, const std::vector<std::uint32_t>& code, std::size_t params)
    {
        std::map<std::uint32_t, std::size_t> bit; // the candidates: locals given a built string
        for (const std::uint32_t s : st.builtLocals) {
            if (s >= params) {
                bit.emplace(s, bit.size());
            }
        }
        if (bit.empty()) {
            return;
        }
        std::map<std::uint32_t, std::vector<const FnState::StringUse*>> at; // by instruction word
        for (const auto& u : st.stringUses) {
            if (bit.contains(u.symbol)) {
                at[u.word].push_back(&u);
            }
        }
        // Per instruction: the candidates set on every path from the call's start (nothing yet
        // seen: no state).
        std::vector<std::optional<std::vector<bool>>> in(code.size());
        std::vector<std::size_t> todo{0};
        if (!code.empty()) {
            in[0] = std::vector<bool>(bit.size(), false);
        }
        const auto apply = [&](std::size_t word, std::vector<bool> set, const FnState::StringUse** unset) {
            if (const auto u = at.find(static_cast<std::uint32_t>(word)); u != at.end()) {
                for (const auto* use : u->second) { // (reads first: `s += x` reads s, then stores)
                    const std::size_t b = bit.at(use->symbol);
                    if (!use->store && !set[b] && unset && !unset[b]) {
                        unset[b] = use;
                    }
                }
                for (const auto* use : u->second) {
                    if (use->store) {
                        set[bit.at(use->symbol)] = true;
                    }
                }
            }
            return set;
        };
        while (!todo.empty()) {
            const std::size_t pc = todo.back();
            todo.pop_back();
            const std::uint32_t op = code[pc];
            const std::size_t next = pc + 1 + (op < kOpCount ? operandCount(op) : 0);
            const std::vector<bool> out = apply(pc, *in[pc], nullptr);
            std::vector<std::size_t> succ;
            if (op == w(Op::Jump) || op == w(Op::JumpIf) || op == w(Op::JumpIfNot)) {
                if (pc + 1 < code.size()) {
                    succ.push_back(code[pc + 1]);
                }
                if (op != w(Op::Jump)) {
                    succ.push_back(next);
                }
            } else if (op != w(Op::Return) && op != w(Op::Return0) && op < kOpCount) {
                succ.push_back(next);
            }
            for (const std::size_t s : succ) {
                if (s >= code.size()) {
                    continue;
                }
                if (!in[s]) {
                    in[s] = out;
                    todo.push_back(s);
                    continue;
                }
                bool changed = false;
                for (std::size_t b = 0; b < out.size(); ++b) {
                    if ((*in[s])[b] && !out[b]) {
                        (*in[s])[b] = false;
                        changed = true;
                    }
                }
                if (changed) {
                    todo.push_back(s);
                }
            }
        }
        std::vector<const FnState::StringUse*> unset(bit.size(), nullptr);
        for (const auto& [word, uses] : at) {
            if (word < in.size() && in[word]) {
                apply(word, *in[word], unset.data());
            }
        }
        for (const auto& [symbol, b] : bit) {
            if (!unset[b]) {
                continue;
            }
            const std::string& name = st.debug.symbols.at(symbol).name;
            warning(unset[b]->loc, "the local " + name + " in " + st.name + " is given a built string and can be read before this call "
                                   "sets it" + (st.resume ? " (a Wait ends the call)" : "") + ": locals keep their values between calls, "
                                   "and the game drops a built string at the end of the next handler and gives its id to the next "
                                   "string made, so " + name + " then reads another string (OB-201)");
        }
    }

    Type norm(Type t) const { return ignoreObject_ && t == Type::Object ? Type::Int : t; }

    static void layout(Function& f, const std::vector<std::uint32_t>& sizes)
    {
        std::uint32_t total = 0;
        for (const auto s : sizes) {
            f.symbolSizes.push_back(s);
            f.symbolOffsets.push_back(total);
            total += s;
        }
        f.localWords = total;
    }

    // Is a name already taken where a new one would go? (nsc: names never clash with another
    // name in scope, a function's or a builtin's.)
    bool nameTaken(const std::string& name, SourceLoc loc, bool checkLocals)
    {
        if (Parser::isKeyword(name)) {
            error(loc, "'" + name + "' is a keyword");
            return true;
        }
        if (findBuiltin(name) && !isNewBuiltinName(name)) {
            error(loc, "'" + name + "' is a builtin's name");
            return true;
        }
        if (functions_.contains(name)) {
            error(loc, "'" + name + "' is a function's name");
            return true;
        }
        if (const auto it = globals_.find(name); it != globals_.end()) {
            error(loc, it->second.readOnly ? "'" + name + "' is a name of the language" : "'" + name + "' is already a global variable");
            return true;
        }
        if (checkLocals) {
            for (const auto& scope : scopes_) {
                if (scope.contains(name)) {
                    error(loc, "'" + name + "' is already declared in this function");
                    return true;
                }
            }
        }
        return false;
    }

    void declareFunction(const TopItem& it)
    {
        const Type ret = norm(it.type);
        const auto found = functions_.find(it.name);
        if (found != functions_.end()) {
            FunctionInfo& f = found->second;
            // A prototype and the definition must agree.
            bool same = f.returns == ret && f.params.size() == it.params.size();
            for (std::size_t i = 0; same && i < it.params.size(); ++i) {
                same = norm(f.params[i].type) == norm(it.params[i].type);
            }
            if (!same) {
                error(it.loc, "the function " + it.name + " doesn't match its earlier declaration");
            }
            if (it.k == TopItem::K::Function) {
                if (f.defined) {
                    error(it.loc, "the function " + it.name + " is defined twice");
                }
                f.defined = true;
                f.params = it.params;
            }
            return;
        }
        if (it.name == "GLOBAL") {
            error(it.loc, "GLOBAL is the name of the script's own first two functions");
            return;
        }
        if (it.name.empty() || it.name.size() > 1023) {
            error(it.loc, "a function's name must be 1 to 1023 bytes");
            return;
        }
        if (!it.quoted && (Parser::isKeyword(it.name) || (findBuiltin(it.name) && !isNewBuiltinName(it.name)))) {
            error(it.loc, "'" + it.name + "' is " + (findBuiltin(it.name) ? "a builtin's name" : "a keyword"));
            return;
        }
        FunctionInfo f;
        f.index = static_cast<std::uint32_t>(2 + functionOrder_.size());
        f.returns = ret;
        f.params = it.params;
        f.loc = it.loc;
        f.defined = it.k == TopItem::K::Function;
        functions_.emplace(it.name, f);
        functionOrder_.push_back(it.name);
        bodies_.emplace_back();
    }

    void global(TopItem& it)
    {
        const Type type = norm(it.type);
        const std::uint32_t size = it.arraySize ? it.arraySize : 1;
        GlobalVar* g = nullptr;
        if (const auto found = globals_.find(it.name); found != globals_.end() && !found->second.readOnly) {
            // A global declared again (C's tentative definitions): the same variable, which must
            // match; its initialiser runs where this declaration is.
            g = &found->second;
            if (g->type != type || g->size != size) {
                error(it.loc, "the global " + it.name + " is declared again with another type");
                return;
            }
            if (!it.init) {
                return;
            }
            if (initialised_.contains(g->symbol)) {
                error(it.loc, "the global " + it.name + " has two initialisers");
                return;
            }
        } else {
            if (nameTaken(it.name, it.loc, false)) {
                return;
            }
            GlobalVar nv;
            nv.symbol = static_cast<std::uint32_t>(globalSizes_.size());
            nv.type = type;
            nv.size = size;
            nv.loc = it.loc;
            globalSizes_.push_back(size);
            g = &globals_.emplace(it.name, nv).first->second;
            debug_->globals.push_back({it.name, type, nv.symbol, size, it.loc});
        }
        if (!it.init) {
            return;
        }
        initialised_.insert(g->symbol);
        // The initialiser is GLOBAL's code: address, value, store.
        code_ = &globalCode_;
        fn_ = nullptr;
        const std::uint32_t begin = here();
        emit(w(Op::LoadVarPtr), 1, g->symbol);
        const Type t = expr(*it.init, type);
        if (t != type && t != Type::Void) {
            error(it.init->loc, "can't initialise the " + std::string(typeName(type)) + " " + it.name + " with a " + std::string(typeName(t)));
        }
        emit(storeOp(type));
        if (type == Type::String) {
            lintBuiltStringInGlobal(*it.init, it.name, it.init->loc);
        }
        debug_->globalInits.push_back({g->symbol, begin, here(), it.loc});
        code_ = nullptr;
    }

    // ---- functions ----

    struct Loop {
        bool isSwitch = false;
        std::vector<std::size_t> breaks;
        std::vector<std::size_t> continues;
    };

    struct FnState {
        const FunctionInfo* info = nullptr;
        std::string name;
        std::vector<std::uint32_t> sizes;
        DebugFunction debug;
        std::map<std::string, std::pair<std::uint32_t, SourceLoc>> labels;
        std::vector<std::tuple<std::size_t, std::string, SourceLoc>> gotos;
        std::vector<Loop> loops;
        std::set<std::string> usedLabels;
        std::set<std::uint32_t> reads; ///< the symbols read (lint)
        // Wait(frames) (a test's: waitCall): the hidden local saying where the next call resumes,
        // and the prologue's jump operands, one per Wait in order.
        std::optional<std::uint32_t> resume;
        std::vector<std::size_t> resumeJumps;
        std::size_t waits = 0;
        // (lint, OB-201 in locals) a string local's reads and stores, at their instruction's word;
        // the locals ever given a built string.
        struct StringUse {
            std::uint32_t word = 0;
            std::uint32_t symbol = 0;
            bool store = false;
            SourceLoc loc;
        };
        std::vector<StringUse> stringUses;
        std::set<std::uint32_t> builtLocals;
    };

    void function(TopItem& it)
    {
        const auto found = functions_.find(it.name);
        if (found == functions_.end()) {
            return; // (its declaration failed)
        }
        const FunctionInfo& info = found->second;
        FnState st;
        st.info = &info;
        st.name = it.name;
        st.debug.name = it.name;
        st.debug.index = info.index;
        st.debug.returns = info.returns;
        st.debug.loc = it.loc;
        fn_ = &st;
        std::vector<std::uint32_t> code;
        code_ = &code;
        scopes_.clear();
        scopes_.emplace_back();
        for (const Param& p : it.params) {
            if (!nameTaken(p.name, p.loc, true)) {
                addLocal(p.name, norm(p.type), 1, p.loc);
            }
        }
        // A test that waits (Wait(frames), NoX Core's, test-only): a hidden local says where the
        // next call resumes (0: the start), and the prologue jumps there. Functions without a Wait
        // get none of this (their code is unchanged).
        if (const std::size_t waits = waitIsIntrinsic() ? countWaits(*it.body) : 0; waits > 0 && isWaitingTest(it, info)) {
            st.resume = addLocal("", Type::Int, 1, it.loc);
            for (std::size_t k = 1; k <= waits; ++k) {
                emit(w(Op::LoadVarInt), 0, *st.resume, w(Op::PushInt), k, w(Op::IntEq), w(Op::JumpIf), 0);
                st.resumeJumps.push_back(here() - 1);
            }
            for (const std::size_t at : st.resumeJumps) {
                code[at] = here(); // (patched to each Wait's resume point; this only if one errs)
            }
        }
        // The body's block shares the parameters' scope (C's rule).
        for (auto& s : it.body->body) {
            stmt(*s);
        }
        scopes_.clear();
        emit(w(Op::Return));
        for (const auto& [at, label, loc] : st.gotos) {
            const auto l = st.labels.find(label);
            if (l == st.labels.end()) {
                error(loc, "no label " + label + " in " + it.name);
                continue;
            }
            code[at] = l->second.first;
        }
        for (const auto& [label, where] : st.labels) {
            if (!st.usedLabels.contains(label)) {
                warning(where.second, "the label " + label + " is never used");
            }
        }
        if (info.returns != Type::Void && !returnsValue_) {
            warning(it.loc, "the function " + it.name + " never returns a value");
        }
        if (lint_) {
            // Locals (not the arguments, nor a switch's temporary) that nothing reads.
            for (std::size_t i = it.params.size(); i < st.debug.symbols.size(); ++i) {
                const DebugVar& v = st.debug.symbols[i];
                if (!v.name.empty() && !st.reads.contains(v.symbol)) {
                    warning(v.loc, "the local " + v.name + " in " + it.name + " is never read");
                }
            }
            lintBuiltStringInLocals(st, code, it.params.size());
        }
        returnsValue_ = false;
        Function f;
        f.name = it.name;
        f.returns = info.returns == Type::Void ? 0 : 1;
        f.args = static_cast<std::uint32_t>(it.params.size());
        layout(f, st.sizes);
        f.code = std::move(code);
        bodies_[info.index - 2] = std::move(f);
        debug_->functions.push_back(std::move(st.debug));
        code_ = nullptr;
        fn_ = nullptr;
    }

    std::uint32_t addLocal(const std::string& name, Type type, std::uint32_t size, SourceLoc loc)
    {
        const auto symbol = static_cast<std::uint32_t>(fn_->sizes.size());
        fn_->sizes.push_back(size);
        if (!name.empty()) {
            scopes_.back().emplace(name, LocalVar{symbol, type, size});
        }
        fn_->debug.symbols.push_back({name, type, symbol, size, loc});
        return symbol;
    }

    std::uint32_t here() const { return static_cast<std::uint32_t>(code_->size()); }
    template <typename... W> void emit(W... words) { (code_->push_back(static_cast<std::uint32_t>(words)), ...); }

    void stmt(Stmt& s)
    {
        if (fn_ && s.k != Stmt::K::Block && s.k != Stmt::K::Label && s.k != Stmt::K::Case && s.k != Stmt::K::Default) {
            fn_->debug.lines.push_back({here(), s.loc});
        }
        switch (s.k) {
        case Stmt::K::Empty:
            break;
        case Stmt::K::Block:
            if (s.name == "decls") { // a declaration list: no scope of its own
                for (auto& d : s.body) {
                    stmt(*d);
                }
                break;
            }
            scopes_.emplace_back();
            for (auto& b : s.body) {
                stmt(*b);
            }
            scopes_.pop_back();
            break;
        case Stmt::K::Decl: {
            const Type type = norm(s.type);
            if (nameTaken(s.name, s.loc, true)) {
                break;
            }
            const std::uint32_t symbol = addLocal(s.name, type, s.arraySize ? s.arraySize : 1, s.loc);
            if (s.e) {
                emit(w(Op::LoadVarPtr), 0, symbol);
                const Type t = expr(*s.e, type);
                if (t != type && t != Type::Void) {
                    error(s.e->loc, "can't initialise the " + std::string(typeName(type)) + " " + s.name + " with a " + std::string(typeName(t)));
                }
                if (type == Type::String && !s.arraySize) {
                    noteStringUse(lookup(s.name), here(), true, s.loc);
                    if (lint_ && builtString(*s.e)) {
                        fn_->builtLocals.insert(symbol);
                    }
                }
                emit(storeOp(type));
            }
            break;
        }
        case Stmt::K::Expr:
            exprStatement(*s.e);
            break;
        case Stmt::K::If: {
            condition(*s.e, "if");
            emit(w(Op::JumpIfNot), 0);
            const std::size_t skip = here() - 1;
            scoped(*s.s1);
            if (s.s2) {
                emit(w(Op::Jump), 0);
                const std::size_t over = here() - 1;
                (*code_)[skip] = here();
                scoped(*s.s2);
                (*code_)[over] = here();
            } else {
                (*code_)[skip] = here();
            }
            break;
        }
        case Stmt::K::While: {
            const std::uint32_t top = here();
            condition(*s.e, "while");
            emit(w(Op::JumpIfNot), 0);
            const std::size_t exit = here() - 1;
            fn_->loops.push_back({});
            scoped(*s.s1);
            Loop loop = std::move(fn_->loops.back());
            fn_->loops.pop_back();
            emit(w(Op::Jump), top);
            (*code_)[exit] = here();
            patch(loop.breaks, here());
            patch(loop.continues, top);
            break;
        }
        case Stmt::K::For: {
            if (s.s1) {
                exprStatement(*s.s1->e);
            }
            const std::uint32_t top = here();
            std::optional<std::size_t> exit;
            if (s.e) {
                condition(*s.e, "for");
                emit(w(Op::JumpIfNot), 0);
                exit = here() - 1;
            }
            fn_->loops.push_back({});
            scoped(*s.s3);
            Loop loop = std::move(fn_->loops.back());
            fn_->loops.pop_back();
            const std::uint32_t cont = here();
            if (s.s2) {
                exprStatement(*s.s2->e);
            }
            emit(w(Op::Jump), top);
            if (exit) {
                (*code_)[*exit] = here();
            }
            patch(loop.breaks, here());
            patch(loop.continues, cont);
            break;
        }
        case Stmt::K::Switch:
            switchStmt(s);
            break;
        case Stmt::K::Case:
        case Stmt::K::Default:
            error(s.loc, std::string(s.k == Stmt::K::Case ? "case" : "default") + " outside a switch's own block");
            break;
        case Stmt::K::Break:
        case Stmt::K::Continue: {
            const bool isBreak = s.k == Stmt::K::Break;
            Loop* target = nullptr;
            for (auto it = fn_->loops.rbegin(); it != fn_->loops.rend(); ++it) {
                if (isBreak || !it->isSwitch) {
                    target = &*it;
                    break;
                }
            }
            if (!target) {
                error(s.loc, isBreak ? "break outside a loop or switch" : "continue outside a loop");
                break;
            }
            emit(w(Op::Jump), 0);
            (isBreak ? target->breaks : target->continues).push_back(here() - 1);
            break;
        }
        case Stmt::K::Return: {
            const Type want = fn_->info->returns;
            if (s.e) {
                const Type t = expr(*s.e, want);
                if (want == Type::Void) {
                    error(s.loc, "the void function " + fn_->name + " can't return a value");
                } else if (t != want && t != Type::Void) {
                    error(s.e->loc, "the function " + fn_->name + " returns " + std::string(typeName(want)) + ", not " + std::string(typeName(t)));
                }
                returnsValue_ = true;
            } else if (want != Type::Void) {
                error(s.loc, "the function " + fn_->name + " must return a " + std::string(typeName(want)));
            }
            emit(w(Op::Return0));
            break;
        }
        case Stmt::K::Goto:
            emit(w(Op::Jump), 0);
            fn_->gotos.emplace_back(here() - 1, s.name, s.loc);
            fn_->usedLabels.insert(s.name);
            break;
        case Stmt::K::GotoIf:
        case Stmt::K::GotoIfNot:
            condition(*s.e, s.k == Stmt::K::GotoIf ? "__goto_if" : "__goto_ifnot");
            emit(w(s.k == Stmt::K::GotoIf ? Op::JumpIf : Op::JumpIfNot), 0);
            fn_->gotos.emplace_back(here() - 1, s.name, s.loc);
            fn_->usedLabels.insert(s.name);
            break;
        case Stmt::K::Label:
            if (fn_->labels.contains(s.name)) {
                error(s.loc, "the label " + s.name + " is defined twice");
            } else {
                fn_->labels.emplace(s.name, std::make_pair(here(), s.loc));
            }
            break;
        }
    }

    // A statement that isn't a block gets a scope of its own anyway (its declarations end there).
    void scoped(Stmt& s)
    {
        scopes_.emplace_back();
        stmt(s);
        scopes_.pop_back();
    }

    void patch(const std::vector<std::size_t>& at, std::uint32_t to)
    {
        for (const auto a : at) {
            (*code_)[a] = to;
        }
    }

    void switchStmt(Stmt& s)
    {
        // The value into a temporary, then each case compared with it in order, then the default
        // (or the end). The temporary is taken when the switch closes, after any local declared
        // inside it, so its symbol is patched in at the end.
        std::vector<std::size_t> tempRefs;
        // switch (v = value): the variable v is the temporary (NoX Core's; the decompiler writes
        // it where the compiler's own temporary would land on another symbol).
        std::optional<VarRef> named;
        if (s.e->k == Expr::K::Assign && s.e->text == "=" && s.e->args[0]->k == Expr::K::Name) {
            const VarRef v = lookup(s.e->args[0]->text);
            if (v.found && v.size == 1 && !v.readOnly && (v.type == Type::Int || v.type == Type::Object)) {
                named = v;
            }
        }
        Type t = Type::Int;
        if (named) {
            t = expr(*s.e, Type::Int);
        } else {
            emit(w(Op::LoadVarPtr), 0, 0);
            tempRefs.push_back(here() - 1);
            t = expr(*s.e, Type::Int);
            emit(w(Op::StoreInt));
        }
        if (t != Type::Int && t != Type::Object && t != Type::Void) {
            error(s.e->loc, "a switch's value must be an int, not a " + std::string(typeName(t)));
        }
        std::vector<Stmt*> labels;
        for (auto& b : s.s1->body) {
            if (b->k == Stmt::K::Case || b->k == Stmt::K::Default) {
                labels.push_back(b.get());
            }
        }
        std::vector<std::size_t> caseJumps;
        std::optional<std::size_t> defaultLabel;
        int defaults = 0;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (labels[i]->k == Stmt::K::Default) {
                if (++defaults > 1) {
                    error(labels[i]->loc, "a switch with two defaults");
                }
                defaultLabel = i;
                caseJumps.push_back(0);
                continue;
            }
            if (named) {
                markRead(*named);
                emit(w(Op::LoadVarInt), named->global ? 1 : 0, named->symbol);
            } else {
                emit(w(Op::LoadVarInt), 0, 0);
                tempRefs.push_back(here() - 1);
            }
            const Type ct = expr(*labels[i]->e, Type::Int);
            if (ct != t && ct != Type::Void) {
                error(labels[i]->e->loc, "the case is a " + std::string(typeName(ct)) + ", the switch's value a " + std::string(typeName(t)));
            }
            emit(w(Op::IntEq), w(Op::JumpIf), 0);
            caseJumps.push_back(here() - 1);
        }
        emit(w(Op::Jump), 0);
        const std::size_t fallOut = here() - 1;
        fn_->loops.push_back({true, {}, {}});
        scopes_.emplace_back();
        std::size_t label = 0;
        std::optional<std::uint32_t> defaultAt;
        for (auto& b : s.s1->body) {
            if (b->k == Stmt::K::Case || b->k == Stmt::K::Default) {
                if (b->k == Stmt::K::Default) {
                    defaultAt = here();
                } else {
                    (*code_)[caseJumps[label]] = here();
                }
                ++label;
                continue;
            }
            stmt(*b);
        }
        scopes_.pop_back();
        Loop loop = std::move(fn_->loops.back());
        fn_->loops.pop_back();
        const std::uint32_t end = here();
        (*code_)[fallOut] = defaultAt ? *defaultAt : end;
        patch(loop.breaks, end);
        if (!loop.continues.empty()) {
            // (continue inside a switch goes to the loop around it)
            for (auto it = fn_->loops.rbegin(); it != fn_->loops.rend(); ++it) {
                if (!it->isSwitch) {
                    it->continues.insert(it->continues.end(), loop.continues.begin(), loop.continues.end());
                    break;
                }
            }
        }
        if (!named) {
            const std::uint32_t temp = addLocal("", Type::Int, 1, s.loc);
            for (const auto at : tempRefs) {
                (*code_)[at] = temp;
            }
        }
    }

    void condition(Expr& e, std::string_view what)
    {
        const Type t = expr(e, Type::Int);
        if (t != Type::Int && t != Type::Void) {
            error(e.loc, std::string(what) + "'s condition must be an int, not a " + std::string(typeName(t)));
        }
    }

    void exprStatement(Expr& e)
    {
        // A string literal alone (not in parentheses) takes its string constant and makes no code.
        if (e.k == Expr::K::Str && !e.paren) {
            addString(e);
            return;
        }
        expr(e, Type::Void, true);
    }

    std::uint32_t addString(const Expr& e)
    {
        const auto id = static_cast<std::uint32_t>(strings_.size());
        strings_.push_back(e.text);
        debug_->strings.push_back({id, e.loc});
        return id;
    }

    static Op storeOp(Type t)
    {
        return t == Type::Float ? Op::StoreFloat : t == Type::String ? Op::StoreString : Op::StoreInt;
    }

    VarRef lookup(const std::string& name) const
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (const auto f = it->find(name); f != it->end()) {
                return {true, false, false, f->second.symbol, f->second.type, f->second.size, false, name};
            }
        }
        if (const auto g = globals_.find(name); g != globals_.end()) {
            return {true, true, g->second.readOnly, g->second.symbol, g->second.type, g->second.size, false, name};
        }
        return {};
    }

    // Emits an expression's code; returns its type (Void for a call without a value, or after
    // an error already reported).
    Type expr(Expr& e, Type expected, bool statement = false)
    {
        switch (e.k) {
        case Expr::K::Int:
            emit(w(Op::PushInt), e.ival);
            return Type::Int;
        case Expr::K::Float:
            emit(w(Op::PushFloat), std::bit_cast<std::uint32_t>(e.fval));
            return Type::Float;
        case Expr::K::Str:
            if (expected == Type::Function) {
                // A function argument naming its function by a string literal.
                const auto f = functions_.find(e.text);
                if (f == functions_.end()) {
                    error(e.loc, "no function named \"" + e.text + "\"");
                    return Type::Void;
                }
                functionArgument(f->first, f->second, e.loc);
                emit(w(Op::PushInt), f->second.index);
                return Type::Function;
            }
            emit(w(Op::PushString), addString(e));
            return Type::String;
        case Expr::K::Name: {
            const VarRef v = lookup(e.text);
            if (!v.found) {
                if (const auto f = functions_.find(e.text); f != functions_.end()) {
                    if (expected == Type::Function) {
                        functionArgument(f->first, f->second, e.loc);
                        emit(w(Op::PushInt), f->second.index);
                        return Type::Function;
                    }
                    error(e.loc, "the function " + e.text + " is used as a value (only a builtin's function argument takes one)");
                    return Type::Void;
                }
                error(e.loc, findBuiltin(e.text) ? "the builtin " + e.text + " needs ( ) to be called" : "no variable or function named " + e.text);
                return Type::Void;
            }
            if (v.size > 1) {
                error(e.loc, "the array " + e.text + " must be indexed");
                return Type::Void;
            }
            markRead(v);
            noteStringUse(v, here(), false, e.loc);
            emit(w(v.type == Type::Float ? Op::LoadVarFloat : Op::LoadVarInt), v.global ? 1 : 0, v.symbol);
            return v.type;
        }
        case Expr::K::ObjVar: {
            // The trigger's (self's) variable: LoadVar with the symbol negated.
            const VarRef v = objectRef(e);
            if (!v.found) {
                return Type::Void;
            }
            if (v.size > 1) {
                error(e.loc, "the array " + v.name + " must be indexed");
                return Type::Void;
            }
            markRead(v);
            emit(w(v.type == Type::Float ? Op::LoadVarFloat : Op::LoadVarInt), 0, symbolWord(v));
            return v.type;
        }
        case Expr::K::Index: {
            const Expr& base = *e.args[0];
            if (base.k != Expr::K::Name && base.k != Expr::K::ObjVar) {
                error(e.loc, "only an array variable can be indexed");
                return Type::Void;
            }
            const VarRef v = base.k == Expr::K::ObjVar ? objectRef(base) : lookup(base.text);
            if (base.k == Expr::K::ObjVar && !v.found) {
                return Type::Void;
            }
            if (!v.found || v.size <= 1) {
                error(base.loc, v.found ? v.name + " isn't an array" : "no variable named " + base.text);
                return Type::Void;
            }
            markRead(v);
            emit(w(Op::LoadVarPtr), globalWord(v), symbolWord(v));
            index(*e.args[1]);
            if (v.object && !rawObject_) {
                // An object array's element read as the element's address and a `+= 0` (which
                // pushes the value and writes it back unchanged): the VM's IndexInt / IndexFloat
                // read an object array's elements from below the object's variables (the game's
                // own quirk).
                emit(w(Op::IndexPtr), w(Op::PushInt), 0, w(Op::StoreIntAdd));
                return v.type;
            }
            if (v.object) {
                warning(e.loc, "reading an element of " + v.name + " with //@raw_object_vars: the VM's IndexInt reads below the "
                               "object's variables there, so this reads 0 (drop the flag to read it)");
            }
            emit(w(v.type == Type::Float ? Op::IndexFloat : Op::IndexInt));
            return v.type;
        }
        case Expr::K::Call:
            return call(e, statement);
        case Expr::K::Unary: {
            const Type t = expr(*e.args[0], expected);
            if (t == Type::Void) {
                return Type::Void;
            }
            if (e.text == "+") {
                if (t != Type::Int && t != Type::Float) {
                    error(e.loc, "unary + needs an int or a float");
                }
                return t;
            }
            if (e.text == "-") {
                if (t == Type::Int) {
                    emit(w(Op::IntNeg));
                } else if (t == Type::Float) {
                    emit(w(Op::FloatNeg));
                } else {
                    error(e.loc, "can't negate a " + std::string(typeName(t)));
                }
                return t;
            }
            if (t != Type::Int) {
                error(e.loc, "'" + e.text + "' needs an int, not a " + std::string(typeName(t)));
            }
            emit(w(e.text == "!" ? Op::BoolNot : Op::IntNot));
            return Type::Int;
        }
        case Expr::K::Binary:
            return binary(e);
        case Expr::K::Assign:
            return assign(e);
        }
        return Type::Void;
    }

    void index(Expr& e)
    {
        const Type t = expr(e, Type::Int);
        if (t != Type::Int && t != Type::Void) {
            error(e.loc, "an array index must be an int, not a " + std::string(typeName(t)));
        }
    }

    Type binary(Expr& e)
    {
        const Type l = expr(*e.args[0], Type::Int);
        const Type r = expr(*e.args[1], Type::Int);
        if (l == Type::Void || r == Type::Void) {
            return Type::Void;
        }
        const std::string& op = e.text;
        const auto bad = [&] {
            if (l != r) {
                error(e.loc, "the operands of '" + op + "' are a " + std::string(typeName(l)) + " and a " + std::string(typeName(r)));
            } else {
                error(e.loc, "'" + op + "' doesn't work on " + std::string(typeName(l)) + "s");
            }
            return Type::Void;
        };
        if (l != r) {
            return bad();
        }
        const Type t = l;
        const detail::BinaryOperator* row = detail::binaryOperator(op);
        if (!row) {
            return bad();
        }
        Op o = detail::kNoOp;
        if (t == Type::Int) {
            o = row->i;
        } else if (t == Type::Float) {
            o = row->f;
        } else if (t == Type::String) {
            o = row->s;
        } else if (t == Type::Object && (op == "==" || op == "!=")) {
            o = row->i; // (an object is compared as its handle)
        }
        if (o == detail::kNoOp) {
            return bad();
        }
        emit(w(o));
        return row->compare ? Type::Int : t;
    }

    Type assign(Expr& e)
    {
        Expr& lhs = *e.args[0];
        VarRef v;
        std::string name;
        if (lhs.k == Expr::K::Name) {
            name = lhs.text;
            v = lookup(name);
            if (!v.found) {
                error(lhs.loc, "no variable named " + name);
                return Type::Void;
            }
            if (v.size > 1) {
                error(lhs.loc, "the array " + name + " must be indexed");
                return Type::Void;
            }
            if (v.readOnly) {
                error(lhs.loc, "can't assign to " + name);
                return Type::Void;
            }
            emit(w(Op::LoadVarPtr), v.global ? 1 : 0, v.symbol);
        } else if (lhs.k == Expr::K::ObjVar) {
            v = objectRef(lhs);
            if (!v.found) {
                return Type::Void;
            }
            name = v.name;
            if (v.size > 1) {
                error(lhs.loc, "the array " + name + " must be indexed");
                return Type::Void;
            }
            warnOffsetZero(v, lhs.loc);
            emit(w(Op::LoadVarPtr), 0, symbolWord(v));
        } else if (lhs.k == Expr::K::Index && (lhs.args[0]->k == Expr::K::Name || lhs.args[0]->k == Expr::K::ObjVar)) {
            const bool object = lhs.args[0]->k == Expr::K::ObjVar;
            v = object ? objectRef(*lhs.args[0]) : lookup(lhs.args[0]->text);
            if (object && !v.found) {
                return Type::Void;
            }
            name = object ? v.name : lhs.args[0]->text;
            if (!v.found || v.size <= 1) {
                error(lhs.loc, v.found ? name + " isn't an array" : "no variable named " + name);
                return Type::Void;
            }
            warnOffsetZero(v, lhs.loc);
            emit(w(Op::LoadVarPtr), globalWord(v), symbolWord(v));
            index(*lhs.args[1]);
            emit(w(Op::IndexPtr));
        } else {
            error(lhs.loc, "can only assign to a variable or an array element");
            return Type::Void;
        }
        const Type t = v.type;
        const Type r = expr(*e.args[1], t);
        if (r == Type::Void) {
            return Type::Void;
        }
        if (r != t) {
            error(e.loc, "can't assign a " + std::string(typeName(r)) + " to the " + std::string(typeName(t)) + " " + name);
            return Type::Void;
        }
        const std::string& op = e.text;
        Op o = detail::kNoOp;
        if (op == "=") {
            o = storeOp(t);
        } else if (const detail::AssignOperator* row = detail::assignOperator(op)) {
            // (the parser takes only the assignments language.hpp lists)
            o = t == Type::Int ? row->i : t == Type::Float ? row->f : t == Type::String ? row->s : detail::kNoOp;
        }
        if (o == detail::kNoOp) {
            error(e.loc, "'" + op + "' doesn't work on " + std::string(typeName(t)) + "s");
            return Type::Void;
        }
        if ((v.global || v.object) && t == Type::String) {
            if (op == "+=") {
                lintBuiltStringInGlobal(e, name, e.loc);
            } else {
                lintBuiltStringInGlobal(*e.args[1], name, e.loc);
            }
        }
        if (lhs.k == Expr::K::Name && t == Type::String) {
            // (a local's: OB-201 across calls, checked at the function's end)
            if (op == "+=") {
                noteStringUse(v, here(), false, lhs.loc);
            }
            noteStringUse(v, here(), true, lhs.loc);
            if (lint_ && fn_ && !v.global && (op == "+=" || builtString(*e.args[1]))) {
                fn_->builtLocals.insert(v.symbol);
            }
        }
        emit(w(o));
        return t;
    }

    /// A function's name given to a builtin's function argument: it's used, and (lint) the
    /// timers and SetDialog call it as the game calls an entry point, never taking a value.
    void functionArgument(const std::string& name, const FunctionInfo& f, SourceLoc loc)
    {
        usedFunctions_.insert(name);
        // (A timer's function runs with no object: the object-variable lint, at the end.)
        if (currentBuiltin_) {
            const std::string_view b = builtinSignature(*currentBuiltin_).name;
            const bool withArg = b == "SecondTimerWithArg" || b == "FrameTimerWithArg";
            if (b == "SecondTimer" || b == "FrameTimer" || withArg) {
                timerSites_.emplace_back(name, loc, std::string(b));
                lintTimerArguments(std::string(b), withArg, name, f, loc);
            }
        }
        if (lint_ && f.returns != Type::Void && currentBuiltin_ != *findBuiltin("SetCallback")) {
            warning(loc, "the function " + name + " returns a value: the game calls a timer's or a dialog's function without "
                         "taking the value, which stays on the stack and holds up every handler after it (OB-204); declare it void");
        }
    }

    /// A timer's function and the argument it gets. When a timer is due the game pushes its
    /// argument only for a function that takes one (the timer runner checks the function's
    /// argument count), and the VM pops as many as the function takes: so *WithArg's argument is
    /// lost on a function with none, a plain timer's function with an argument reads 0 (a plain
    /// timer's argument), and arguments past the first take whatever is on the stack.
    void lintTimerArguments(const std::string& builtin, bool withArg, const std::string& name, const FunctionInfo& f, SourceLoc loc)
    {
        if (!lint_) {
            return;
        }
        const std::size_t n = f.params.size();
        if (withArg && n == 0) {
            warning(loc, builtin + " gives " + name + " an argument, but " + name + " takes none: the game passes a timer's "
                         "argument only to a function that has one, so the value is lost (use " +
                             builtin.substr(0, builtin.size() - 7) + ", or give " + name + " a parameter)");
        } else if (!withArg && n > 0) {
            warning(loc, name + " takes " + (n == 1 ? std::string("an argument") : std::to_string(n) + " arguments") + ", but " + builtin +
                             " gives it none: " + f.params[0].name + " reads 0" + (n > 1 ? " and the rest whatever is on the stack" : "") +
                             " (use " + builtin + "WithArg)");
        } else if (withArg && n > 1) {
            warning(loc, name + " takes " + std::to_string(n) + " arguments, but " + builtin + " gives it one: the rest read whatever is "
                         "on the stack");
        }
    }

    /// (lint, CompileOptions::argValues) Each string literal given to a builtin parameter that
    /// takes a set's values, checked against the set (computed strings never are). The map's
    /// names aren't checked in a prefab's function ("Name%Prefix...": its lookups add the prefix).
    void lintArgValues(const Expr& e, std::uint32_t builtin)
    {
        const bool prefab = fn_ && fn_->name.find('%') != std::string::npos;
        for (std::size_t i = 0; i < e.args.size(); ++i) {
            const Expr& a = *e.args[i];
            if (a.k != Expr::K::Str) {
                continue;
            }
            const auto set = argValueSet(builtin, i);
            if (!set || (prefab && isMapValueSet(*set))) {
                continue;
            }
            if (auto msg = checkArgValue(*argValues_, builtin, i, a.text)) {
                warning(a.loc, std::move(*msg));
            }
        }
    }

    Type call(Expr& e, bool statement)
    {
        if (e.text == "assert" && !e.quoted && !functions_.contains("assert")) {
            return assertCall(e, statement);
        }
        if (e.text == "Wait" && !e.quoted && waitIsIntrinsic()) {
            return waitCall(e, statement);
        }
        std::vector<Type> want;
        Type returns = Type::Void;
        std::uint32_t callOp = 0;
        std::uint32_t operand = 0;
        std::optional<std::uint32_t> builtin;
        // (A name NoX Core gave a builtin yields to a function of the script's own of that name.)
        const bool ownFunction = isNewBuiltinName(e.text) && functions_.contains(e.text);
        if (const auto b = e.quoted || ownFunction ? std::nullopt : findBuiltin(e.text)) {
            builtin = b;
            const auto& sig = builtinSignature(*b);
            for (const Type t : sig.args) {
                want.push_back(norm(t));
            }
            returns = norm(sig.returns);
            callOp = w(Op::CallBuiltin);
            operand = *b;
        } else if (const auto f = functions_.find(e.text); f != functions_.end()) {
            for (const Param& p : f->second.params) {
                want.push_back(norm(p.type));
            }
            returns = f->second.returns;
            callOp = w(Op::CallScript);
            operand = f->second.index;
            usedFunctions_.insert(f->first);
            if (fn_) {
                callees_[fn_->name].push_back(f->first);
            }
        } else {
            error(e.loc, lookup(e.text).found ? e.text + " is a variable, not a function" : "no function named " + e.text);
            return Type::Void;
        }
        if (e.args.size() != want.size()) {
            error(e.loc, e.text + " takes " + std::to_string(want.size()) + " argument" + (want.size() == 1 ? "" : "s") + ", not " + std::to_string(e.args.size()));
            return Type::Void;
        }
        if (builtin && argValues_) {
            lintArgValues(e, *builtin);
        }
        bool ok = true;
        const auto outerBuiltin = std::exchange(currentBuiltin_, builtin);
        for (std::size_t i = 0; i < want.size(); ++i) {
            currentBuiltin_ = builtin; // (an argument's own calls set theirs)
            const Type t = expr(*e.args[i], want[i]);
            if (t == Type::Void) {
                ok = false;
                continue;
            }
            if (t != want[i]) {
                error(e.args[i]->loc, e.text + "'s argument " + std::to_string(i + 1) + " is a " + std::string(typeName(want[i])) +
                                          (want[i] == Type::Function ? " (a function's name)" : "") + ", not a " + std::string(typeName(t)));
                ok = false;
            }
        }
        currentBuiltin_ = outerBuiltin;
        emit(callOp, operand);
        if (returns == Type::Void && !statement) {
            error(e.loc, e.text + " doesn't return a value");
            return Type::Void;
        }
        return ok ? returns : Type::Void;
    }

    std::vector<Diagnostic>& diags_;
    bool ignoreObject_ = false;
    DebugInfo* debug_ = nullptr;
    std::vector<std::string> strings_;
    std::map<std::string, GlobalVar> globals_;
    std::vector<std::uint32_t> globalSizes_;
    std::set<std::uint32_t> initialised_;
    std::vector<std::uint32_t> globalCode_;
    std::map<std::string, FunctionInfo> functions_;
    std::vector<std::string> functionOrder_;
    std::vector<Function> bodies_;
    std::vector<std::uint32_t>* code_ = nullptr;
    FnState* fn_ = nullptr;
    std::vector<std::map<std::string, LocalVar>> scopes_;
    bool returnsValue_ = false;
    // Object variables: function 0's symbols (its loader's placeholder first).
    bool rawObject_ = false; ///< //@raw_object_vars: no pad, element reads as IndexInt / IndexFloat
    bool allowObject_ = false;   ///< CompileOptions::allowObjectVariables
    bool refusedObject_ = false; ///< one was refused (CompileResult::objectVariablesRefused)
    std::map<std::string, ObjectVar> objectVars_;
    std::vector<std::uint32_t> objectSizes_{0};
    // (lint)
    bool lint_ = false;
    const ArgValueSets* argValues_ = nullptr; ///< CompileOptions::argValues (with lint)
    std::set<std::uint32_t> objectReads_;
    std::map<std::string, std::string> objectUse_;              ///< function: the first object variable it uses
    std::map<std::string, std::vector<std::string>> callees_;   ///< function: the script functions it calls
    std::vector<std::tuple<std::string, SourceLoc, std::string>> timerSites_; ///< function, where, timer builtin
    std::set<std::uint32_t> globalReads_;
    std::set<std::string> usedFunctions_;
    std::optional<std::uint32_t> currentBuiltin_; ///< the builtin whose arguments are being compiled
};

} // namespace

std::string Diagnostic::format(std::string_view fileName) const
{
    return std::string(fileName) + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) + ": " +
           (severity == Severity::Error ? "error: " : "warning: ") + message;
}

std::string CompileResult::report(std::string_view fileName) const
{
    std::string out;
    for (const auto& d : diagnostics) {
        out += d.format(fileName) + "\n";
    }
    return out;
}

CompileResult compile(std::string_view source, const CompileOptions& options)
{
    CompileResult result;
    std::vector<Flag> flags;
    auto tokens = Lexer(source, result.diagnostics).run(flags);
    bool ignoreObject = false;
    bool rawObjectVars = false;
    for (const Flag& f : flags) {
        if (f.name == "ignore_object_type") {
            ignoreObject = true;
        } else if (f.name == "raw_object_vars") {
            rawObjectVars = true;
        } else {
            result.diagnostics.push_back({Diagnostic::Severity::Warning, f.loc, "unknown compile flag //@" + f.name});
        }
    }
    auto items = Parser(std::move(tokens), result.diagnostics).program();
    const bool parsed = std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                                     [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Error; });
    if (parsed) {
        Generator gen(result.diagnostics, ignoreObject, rawObjectVars);
        result.file = gen.run(items, source, options, result.debug);
        result.objectVariablesRefused = gen.objectVariablesRefused();
    }
    std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.loc.line != b.loc.line ? a.loc.line < b.loc.line : a.loc.column < b.loc.column;
    });
    return result;
}

bool isEntryPointName(std::string_view name)
{
    for (const std::string_view prefix : {"MapInitialize", "MapEntry", "MapExit", "MapShutdown", "PlayerDeath"}) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> embeddedSource(const ScriptFile& file)
{
    if (file.strings.empty() || !file.strings.front().starts_with(kEmbeddedSourcePrefix)) {
        return std::nullopt;
    }
    return file.strings.front().substr(kEmbeddedSourcePrefix.size());
}

bool usesObjectVariables(const ScriptFile& file)
{
    if (!file.functions.empty() && file.functions[0].symbolSizes.size() > 1) {
        return true; // (laid out: symbols past the loader's size-0 placeholder)
    }
    for (std::size_t f = 1; f < file.functions.size(); ++f) {
        const auto& code = file.functions[f].code;
        for (std::size_t at = 0; at < code.size();) {
            const std::uint32_t op = code[at];
            const std::uint32_t n = op < kOpCount ? operandCount(op) : 0;
            if (op <= static_cast<std::uint32_t>(Op::LoadVarString) && n == 2 && at + 2 < code.size() && code[at + 1] == 0 &&
                static_cast<std::int32_t>(code[at + 2]) < 0) {
                return true; // (LoadVarInt / Float / Ptr / String of a local below 0)
            }
            at += 1 + n;
        }
    }
    return false;
}

} // namespace nox::script
