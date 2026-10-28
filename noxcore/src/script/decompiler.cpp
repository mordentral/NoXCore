// The NoxScript decompiler (decompiler.hpp).
//
// 1. Types: every variable, function argument and result is a type variable; each instruction
//    says what its operands are (LoadVarFloat a float, StoreString a string, a builtin's
//    arguments their declared types...), and unifying them gives each one a type. Waypoints and
//    groups are `object`; a script that mixes them with ints is written with
//    //@ignore_object_type.
// 2. Expressions: a stack of trees over each straight run of code. There is no pop
//    instruction, so values left on the stack (a store's, a call's unused result) are the
//    expression statements.
// 3. Statements: the shapes compiler.cpp makes (and the game's compiler made) are recognised
//    region by region: a JumpIfNot over a block is an if (with a Jump over an else block, an
//    if / else), a Jump back to a condition's start a while or for, a temporary compared with
//    each case a switch. Any other jump is a goto.
// 4. Order: function indices follow declarations and string constants follow the code, so the
//    source declares every function first (in index order) when its bodies must come in
//    another order, and a string constant no code uses is kept as a bare literal statement.
// 5. Check: the source is compiled again; a function that doesn't come back the same is written
//    again with labels and gotos.
#include "noxcore/script/decompiler.hpp"

#include "noxcore/script/opcodes.hpp"

#include "language.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace nox::script {

namespace {

constexpr std::uint8_t kI = 1, kF = 2, kS = 4, kO = 8, kAll = 15;

std::uint8_t maskOf(Type t)
{
    switch (t) {
    case Type::Int: return kI;
    case Type::Float: return kF;
    case Type::String: return kS;
    case Type::Object: return kO;
    default: return kAll;
    }
}

struct Ins {
    std::uint32_t at = 0;
    std::uint32_t op = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t size = 1;
    Op o() const { return static_cast<Op>(op); }
};

// An operator's source text and precedence: language.hpp, the compiler's table.
using detail::binaryText;
using detail::storeText;

bool isBinary(Op o) { return binaryText(o) != "?"; }

bool isUnary(Op o) { return o == Op::BoolNot || o == Op::IntNot || o == Op::IntNeg || o == Op::FloatNeg; }

bool isStore(Op o) { return storeText(o) != "?"; }

int precedence(std::string_view op)
{
    const detail::BinaryOperator* o = detail::binaryOperator(op);
    return o ? o->precedence : 0;
}

struct Node;
using NodeP = std::shared_ptr<Node>;
struct Node {
    enum class K : std::uint8_t { Int, Float, Str, Var, Elem, Addr, Call, Unary, Binary, Assign };
    K k = K::Int;
    std::uint32_t start = 0; ///< its first instruction's word
    std::uint32_t v = 0;     ///< a literal's bits, an operator's opcode, a builtin's id or a function's index
    bool global = false;     ///< Var, Elem, Addr
    std::uint32_t sym = 0;
    bool elem = false;   ///< Addr: an array element's (kids[0] the index)
    bool script = false; ///< Call: a script function's
    std::vector<NodeP> kids;
};

struct VarKey {
    bool global = false;
    std::uint32_t sym = 0;
    auto operator<=>(const VarKey&) const = default;
};

struct DStmt {
    enum class K : std::uint8_t { Expr, If, Loop, Switch, Break, Continue, Return, Goto, GotoIf, GotoIfNot, Marker, Case, Default, Reserve };
    K k = K::Expr;
    NodeP e;                    ///< the expression, condition, returned value, case value or switch value
    std::vector<DStmt> a, b;    ///< then / body, else
    bool hasElse = false;
    std::uint32_t addr = 0;     ///< a marker's or goto's word; a reserved string's id
    VarKey temp;                ///< a switch's temporary
};

// Why a function (or the whole script) can't be written in a form.
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------------------------

class Decompiler {
public:
    Decompiler(const ScriptFile& file, const DecompileOptions& opts) : file_(file), opts_(opts) {}

    DecompileResult run()
    {
        DecompileResult res;
        if (opts_.useEmbeddedSource) {
            if (auto src = embeddedSource(file_)) {
                res.source = *src;
                res.fromEmbeddedSource = true;
                // (Object variables allowed: the check is whether it compiles back, whoever reads it.)
                const auto back = compile(res.source, {.embedSource = true, .allowObjectVariables = true});
                res.compiles = back.ok();
                res.identical = back.ok() && back.file->serialize() == file_.serialize();
                return res;
            }
        }
        const std::size_t n = file_.functions.size();
        if (n == 0) {
            res.compiles = res.identical = file_.strings.empty();
            res.source = "// (no script)\n";
            return res;
        }
        try {
            prepare();
        } catch (const std::exception& f) {
            res.notes.push_back(f.what());
            res.forms.assign(n > 2 ? n - 2 : 0, DecompileResult::Form::Different);
            return res;
        }
        // Each function structured first; a function that doesn't compile back the same gets
        // its goto form.
        std::vector<int> mode(n, 0); // 0 structured, 1 gotos, 2 failed both
        std::vector<std::string> why(n);
        for (int pass = 0; pass < 3; ++pass) {
            try {
                res.source = assemble(mode, why);
            } catch (const std::exception& f) {
                res.notes.push_back(f.what());
                res.forms.assign(n - 2, DecompileResult::Form::Different);
                return res;
            }
            const auto back = compile(res.source, {.allowObjectVariables = true});
            res.compiles = back.ok();
            bool changed = false;
            if (!back.ok()) {
                // Find the functions the errors are in, and move them on a form.
                for (const auto& d : back.diagnostics) {
                    if (d.severity != Diagnostic::Severity::Error) {
                        continue;
                    }
                    const std::size_t f = functionAtLine(d.loc.line);
                    if (f >= 2 && mode[f] < 2) {
                        why[f] = "doesn't compile: " + d.message;
                        ++mode[f];
                        changed = true;
                    } else if (f < 2) {
                        res.notes.push_back("doesn't compile: " + d.format("decompiled"));
                    }
                }
                if (!changed) {
                    break;
                }
                continue;
            }
            const ScriptFile& b = *back.file;
            if (b.strings != file_.strings) {
                res.notes.push_back("the string constants differ");
            }
            if (b.functions.size() != n) {
                res.notes.push_back("the function count differs");
                break;
            }
            for (std::size_t f = 0; f < n; ++f) {
                const std::string d = compareFunctions(file_.functions[f], b.functions[f]);
                if (d.empty()) {
                    continue;
                }
                if (f < 2) {
                    res.notes.push_back(file_.functions[f].name + " (function " + std::to_string(f) + "): " + d);
                    continue;
                }
                if (mode[f] < 2) {
                    why[f] = d;
                    ++mode[f];
                    changed = true;
                }
            }
            if (!changed) {
                res.identical = b.serialize() == file_.serialize();
                break;
            }
        }
        for (std::size_t f = 2; f < n; ++f) {
            res.forms.push_back(mode[f] == 0 ? DecompileResult::Form::Structured
                                : mode[f] == 1 ? DecompileResult::Form::Gotos
                                               : DecompileResult::Form::Different);
            if (mode[f] >= 1 && !why[f].empty()) {
                res.notes.push_back(file_.functions[f].name + ": " + (mode[f] == 1 ? "gotos: " : "different: ") + why[f]);
            }
        }
        return res;
    }

private:
    // ---- instructions ----

    void prepare()
    {
        const auto& fns = file_.functions;
        if (fns.size() < 2) {
            throw Failure("a script needs its two GLOBAL functions");
        }
        // Function 0: the object variables' layout (its symbols after the loader's placeholder),
        // with no code of its own.
        const auto& f0 = fns[0];
        if (f0.symbolSizes.empty() || f0.symbolSizes[0] != 0 || f0.code.size() != 1 || f0.code[0] != static_cast<std::uint32_t>(Op::Return) ||
            f0.args || f0.returns || f0.name != "GLOBAL") {
            throw Failure("function 0 isn't a GLOBAL holding only the object variables' layout");
        }
        for (std::size_t s = 1; s < f0.symbolSizes.size(); ++s) {
            if (f0.symbolSizes[s] == 0) {
                throw Failure("an object variable of size 0");
            }
        }
        objectUsed_.clear();
        if (fns[1].name != "GLOBAL" || fns[1].symbolSizes.size() < 4 || fns[1].args || fns[1].returns) {
            throw Failure("function 1 isn't a GLOBAL with the four language globals");
        }
        for (int i = 0; i < 4; ++i) {
            if (fns[1].symbolSizes[static_cast<std::size_t>(i)] != 1) {
                throw Failure("GLOBAL's first four symbols aren't one word each");
            }
        }
        ins_.resize(fns.size());
        index_.resize(fns.size());
        targets_.resize(fns.size());
        for (std::size_t f = 0; f < fns.size(); ++f) {
            const auto& code = fns[f].code;
            if (code.empty() || code.back() != static_cast<std::uint32_t>(Op::Return)) {
                throw Failure(fns[f].name + ": the code doesn't end with Return");
            }
            std::size_t at = 0;
            while (at < code.size()) {
                Ins in;
                in.at = static_cast<std::uint32_t>(at);
                in.op = code[at];
                if (in.op >= kOpCount) {
                    throw Failure(fns[f].name + ": unknown opcode " + std::to_string(in.op));
                }
                const std::uint32_t k = operandCount(in.op);
                if (at + k >= code.size() && k) {
                    throw Failure(fns[f].name + ": code cut short");
                }
                in.size = 1 + k;
                if (k >= 1) in.a = code[at + 1];
                if (k >= 2) in.b = code[at + 2];
                index_[f][in.at] = ins_[f].size();
                ins_[f].push_back(in);
                at += in.size;
            }
            if (ins_[f].back().o() != Op::Return) {
                throw Failure(fns[f].name + ": the code doesn't end with Return");
            }
            for (std::size_t i = 0; i + 1 < ins_[f].size(); ++i) {
                const Ins& in = ins_[f][i];
                if (in.o() == Op::Return) {
                    throw Failure(fns[f].name + ": a Return before the end");
                }
                if (in.o() == Op::Jump || in.o() == Op::JumpIf || in.o() == Op::JumpIfNot) {
                    if (!index_[f].contains(in.a)) {
                        throw Failure(fns[f].name + ": a jump into an instruction");
                    }
                    targets_[f].insert(in.a);
                }
                if (in.o() == Op::LoadVarInt || in.o() == Op::LoadVarFloat || in.o() == Op::LoadVarString || in.o() == Op::LoadVarPtr) {
                    const auto var = static_cast<std::int32_t>(in.b);
                    if (in.a > 1) {
                        throw Failure(fns[f].name + ": a variable's global flag that isn't 0 or 1");
                    }
                    if (f == 1 && !in.a) {
                        throw Failure("GLOBAL reads its own symbols as locals, or an object variable");
                    }
                    if (var < 0) {
                        if (in.a) {
                            throw Failure(fns[f].name + ": a negative global");
                        }
                        // An object variable: function 0's symbol -var.
                        const auto index = static_cast<std::uint64_t>(-static_cast<std::int64_t>(var));
                        if (index >= f0.symbolSizes.size()) {
                            throw Failure(fns[f].name + ": an object variable that isn't there");
                        }
                        objectUsed_.insert(static_cast<std::uint32_t>(index));
                        continue;
                    }
                    const auto& owner = in.a ? fns[1] : fns[f];
                    if (static_cast<std::size_t>(var) >= owner.symbolSizes.size()) {
                        throw Failure(fns[f].name + ": a variable that isn't there");
                    }
                }
                if (in.o() == Op::CallScript && (in.a < 2 || in.a >= fns.size())) {
                    throw Failure(fns[f].name + ": a call to function " + std::to_string(in.a));
                }
                if (in.o() == Op::CallBuiltin && in.a >= kBuiltinCount) {
                    throw Failure(fns[f].name + ": no builtin " + std::to_string(in.a));
                }
                if (in.o() == Op::PushString && in.a >= file_.strings.size()) {
                    throw Failure(fns[f].name + ": no string " + std::to_string(in.a));
                }
            }
            for (std::size_t s = 0; s < fns[f].args && f >= 2; ++s) {
                if (s >= fns[f].symbolSizes.size() || fns[f].symbolSizes[s] != 1) {
                    throw Failure(fns[f].name + ": an argument that isn't one word");
                }
            }
            if (f >= 2 && (fns[f].name.empty() || fns[f].name == "GLOBAL")) {
                throw Failure("a function named '" + fns[f].name + "'");
            }
        }
        // Object variables in the compiler's own layout: a one-word pad first that nothing uses,
        // and no element read as IndexInt / IndexFloat (it reads them as `+= 0`). Anything else
        // is written with //@raw_object_vars (no pad; element reads as the VM's index ops).
        padded_ = f0.symbolSizes.size() >= 3 && f0.symbolSizes[1] == 1 && !objectUsed_.contains(1);
        typeAll(false);
        if (padded_ && objectIndexRead_) {
            padded_ = false;
            typeAll(false);
        }
        if (conflicts_ > 0 && objectConflictsOnly_) {
            typeAll(true);
        }
        rawObjects_ = !padded_ && f0.symbolSizes.size() >= 2;
        name();
        order();
    }

    // ---- object variables ----

    std::set<std::uint32_t> objectUsed_; ///< function 0's symbols the code names
    bool padded_ = false;                ///< the compiler's layout (above)
    bool rawObjects_ = false;            ///< written with //@raw_object_vars
    bool objectIndexRead_ = false;       ///< an object array element read by IndexInt / IndexFloat
    std::vector<int> ov_;                ///< type variables, by function 0's symbol
    std::vector<std::string> oname_;
    std::vector<Type> otype_;

    /// A LoadVar's operands naming an object variable (a local's index, negative).
    static bool isObject(bool global, std::uint32_t sym) { return !global && static_cast<std::int32_t>(sym) < 0; }
    static std::uint32_t objectIndex(std::uint32_t sym) { return static_cast<std::uint32_t>(-static_cast<std::int64_t>(static_cast<std::int32_t>(sym))); }
    /// The object variables' declarations, in symbol order (the pad left to the compiler).
    std::string objectDeclarations() const
    {
        std::string out;
        const auto& sizes = file_.functions[0].symbolSizes;
        for (std::size_t s = padded_ ? 2 : 1; s < sizes.size(); ++s) {
            out += typeText(otype_[s]) + " object." + oname_[s] + (sizes[s] > 1 ? "[" + std::to_string(sizes[s]) + "]" : "") + ";\n";
        }
        return out;
    }

    static bool validName(const std::string& s)
    {
        if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) {
            return false;
        }
        for (const char c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                return false;
            }
        }
        static const std::set<std::string_view> kw(std::begin(detail::kKeywords), std::end(detail::kKeywords));
        return !kw.contains(s) && s != "GLOBAL" && !findBuiltin(s);
    }

    // ---- types ----

    std::vector<int> parent_;
    std::vector<std::uint8_t> mask_;
    std::vector<int> gv_;
    std::vector<std::vector<int>> lv_;
    std::vector<int> rv_;
    int conflicts_ = 0;
    bool objectConflictsOnly_ = true;
    bool ignoreObject_ = false;

    int fresh(std::uint8_t m)
    {
        parent_.push_back(static_cast<int>(parent_.size()));
        mask_.push_back(m);
        return parent_.back();
    }
    int find(int x)
    {
        while (parent_[static_cast<std::size_t>(x)] != x) {
            parent_[static_cast<std::size_t>(x)] = parent_[static_cast<std::size_t>(parent_[static_cast<std::size_t>(x)])];
            x = parent_[static_cast<std::size_t>(x)];
        }
        return x;
    }
    std::uint8_t fix(std::uint8_t m) const { return ignoreObject_ && (m & kO) ? static_cast<std::uint8_t>((m & ~kO) | kI) : m; }
    void restrict(int x, std::uint8_t m)
    {
        x = find(x);
        m = fix(m);
        const std::uint8_t both = mask_[static_cast<std::size_t>(x)] & m;
        if (!both) {
            noteConflict(mask_[static_cast<std::size_t>(x)], m);
            return; // (keep what it was)
        }
        mask_[static_cast<std::size_t>(x)] = both;
    }
    void unify(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        const std::uint8_t both = mask_[static_cast<std::size_t>(a)] & mask_[static_cast<std::size_t>(b)];
        if (!both) {
            noteConflict(mask_[static_cast<std::size_t>(a)], mask_[static_cast<std::size_t>(b)]);
            return; // (left apart)
        }
        parent_[static_cast<std::size_t>(b)] = a;
        mask_[static_cast<std::size_t>(a)] = both;
    }
    void noteConflict(std::uint8_t a, std::uint8_t b)
    {
        ++conflicts_;
        // Would it go away with objects as ints?
        const auto io = [](std::uint8_t m) { return (m & kO) ? static_cast<std::uint8_t>(m | kI) : m; };
        if (!(io(a) & io(b))) {
            objectConflictsOnly_ = false;
        }
    }
    // A type the debug info names, where the code allows it.
    Type chosen(int x, std::optional<Type> hint)
    {
        if (hint) {
            const Type h = ignoreObject_ && *hint == Type::Object ? Type::Int : *hint;
            if (mask_[static_cast<std::size_t>(find(x))] & maskOf(h)) {
                return h;
            }
        }
        return chosen(x);
    }
    Type chosen(int x)
    {
        const std::uint8_t m = mask_[static_cast<std::size_t>(find(x))];
        if (m & kI) return Type::Int;
        if (m & kO) return Type::Object;
        if (m & kS) return Type::String;
        return Type::Float;
    }

    struct TEntry {
        int tv = -1;
        bool addr = false;
        int var = -1; ///< an Addr's variable's type
        bool object = false; ///< an Addr of an object variable
        bool elem = false;   ///< an Addr of an array element
        bool zero = false;   ///< PushInt 0
    };

    void typeAll(bool ignoreObject)
    {
        ignoreObject_ = ignoreObject;
        parent_.clear();
        mask_.clear();
        conflicts_ = 0;
        objectConflictsOnly_ = true;
        const auto& fns = file_.functions;
        gv_.clear();
        for (std::size_t s = 0; s < fns[1].symbolSizes.size(); ++s) {
            gv_.push_back(fresh(s < 4 ? kI : kAll));
        }
        ov_.clear();
        for (std::size_t s = 0; s < fns[0].symbolSizes.size(); ++s) {
            ov_.push_back(fresh(kAll));
        }
        objectIndexRead_ = false;
        lv_.assign(fns.size(), {});
        rv_.assign(fns.size(), -1);
        for (std::size_t f = 2; f < fns.size(); ++f) {
            for (std::size_t s = 0; s < fns[f].symbolSizes.size(); ++s) {
                lv_[f].push_back(fresh(kAll));
            }
            rv_[f] = fresh(fns[f].returns ? kAll : kI);
        }
        for (std::size_t f = 1; f < fns.size(); ++f) {
            typeFunction(f);
        }
        // The debug info's types, where the code leaves them open (a whole class of unified
        // variables at once). Anything still open is an int.
        if (const DebugInfo* d = opts_.names) {
            const auto hint = [&](int tv, Type t) {
                const std::uint8_t m = fix(maskOf(t));
                if (mask_[static_cast<std::size_t>(find(tv))] & m) {
                    restrict(tv, m);
                }
            };
            for (const auto& g : d->globals) {
                if (g.symbol >= 4 && g.symbol < gv_.size()) {
                    hint(gv_[g.symbol], g.type);
                }
            }
            for (const auto& v : d->objectVars) {
                if (v.symbol >= 1 && v.symbol < ov_.size()) {
                    hint(ov_[v.symbol], v.type);
                }
            }
            for (std::size_t f = 2; f < fns.size(); ++f) {
                if (const DebugFunction* df = d->function(fns[f].name)) {
                    for (const auto& v : df->symbols) {
                        if (v.symbol < lv_[f].size()) {
                            hint(lv_[f][v.symbol], v.type);
                        }
                    }
                    if (fns[f].returns && df->returns != Type::Void) {
                        hint(rv_[f], df->returns);
                    }
                }
            }
        }
    }

    int varTv(std::size_t f, bool global, std::uint32_t sym)
    {
        return isObject(global, sym) ? ov_[objectIndex(sym)] : global ? gv_[sym] : lv_[f][sym];
    }

    void typeFunction(std::size_t f)
    {
        const auto& fn = file_.functions[f];
        std::vector<TEntry> st;
        const auto pop = [&]() -> TEntry {
            if (st.empty()) {
                return {fresh(kAll), false, -1};
            }
            TEntry e = st.back();
            st.pop_back();
            return e;
        };
        const auto value = [&](std::uint8_t m) {
            TEntry e = pop();
            if (!e.addr) {
                restrict(e.tv, m);
            }
            return e;
        };
        for (const Ins& in : ins_[f]) {
            if (targets_[f].contains(in.at)) {
                st.clear();
            }
            const Op o = in.o();
            switch (o) {
            case Op::LoadVarInt:
            case Op::LoadVarString: {
                const int v = varTv(f, in.a, in.b);
                restrict(v, o == Op::LoadVarString ? kS : kI | kS | kO);
                st.push_back({v});
                break;
            }
            case Op::LoadVarFloat: {
                const int v = varTv(f, in.a, in.b);
                restrict(v, kF);
                st.push_back({v});
                break;
            }
            case Op::LoadVarPtr:
                st.push_back({-1, true, varTv(f, in.a, in.b), isObject(in.a, in.b)});
                break;
            case Op::PushInt: {
                TEntry e{fresh(kI)};
                e.zero = in.a == 0;
                st.push_back(e);
                break;
            }
            case Op::PushFloat: st.push_back({fresh(kF)}); break;
            case Op::PushString: st.push_back({fresh(kS)}); break;
            case Op::IndexInt:
            case Op::IndexFloat:
            case Op::IndexPtr: {
                value(kI);
                const TEntry base = pop();
                if (!base.addr) {
                    st.push_back({fresh(kAll)});
                    break;
                }
                if (o == Op::IndexPtr) {
                    TEntry e = base;
                    e.elem = true;
                    st.push_back(e);
                } else {
                    if (base.object) {
                        objectIndexRead_ = true;
                    }
                    restrict(base.var, o == Op::IndexFloat ? kF : kI | kS | kO);
                    st.push_back({base.var});
                }
                break;
            }
            case Op::CallBuiltin: {
                const auto& sig = builtinSignature(in.a);
                for (std::size_t i = sig.args.size(); i-- > 0;) {
                    const TEntry e = pop();
                    if (!e.addr && sig.args[i] != Type::Function) {
                        restrict(e.tv, maskOf(sig.args[i]));
                    }
                }
                if (sig.returns != Type::Void) {
                    st.push_back({fresh(maskOf(sig.returns))});
                }
                break;
            }
            case Op::CallScript: {
                const auto& callee = file_.functions[in.a];
                for (std::size_t i = callee.args; i-- > 0;) {
                    const TEntry e = pop();
                    if (!e.addr) {
                        unify(e.tv, lv_[in.a][i]);
                    }
                }
                if (callee.returns) {
                    st.push_back({rv_[in.a]});
                }
                break;
            }
            case Op::Return0:
                if (fn.returns && f >= 2) {
                    const TEntry e = pop();
                    if (!e.addr) {
                        unify(e.tv, rv_[f]);
                    }
                }
                st.clear();
                break;
            case Op::Jump:
                st.clear();
                break;
            case Op::JumpIf:
            case Op::JumpIfNot:
                value(kI);
                st.clear();
                break;
            case Op::Return:
                break;
            default:
                if (isBinary(o)) {
                    const TEntry r = pop();
                    const TEntry l = pop();
                    std::uint8_t m = kI;
                    bool compare = false;
                    switch (o) {
                    case Op::FloatAdd: case Op::FloatSub: case Op::FloatMul: case Op::FloatDiv: m = kF; break;
                    case Op::StringAdd: m = kS; break;
                    case Op::IntEq: case Op::IntNeq: m = kI | kO; compare = true; break;
                    case Op::FloatEq: case Op::FloatNeq: case Op::FloatLt: case Op::FloatGt: case Op::FloatLte: case Op::FloatGte: m = kF; compare = true; break;
                    case Op::StringEq: case Op::StringNeq: case Op::StringLt: case Op::StringGt: case Op::StringLte: case Op::StringGte: m = kS; compare = true; break;
                    case Op::IntLt: case Op::IntGt: case Op::IntLte: case Op::IntGte: compare = true; break;
                    default: break;
                    }
                    if (!l.addr && !r.addr) {
                        restrict(l.tv, m);
                        restrict(r.tv, m);
                        unify(l.tv, r.tv);
                    }
                    st.push_back({compare ? fresh(kI) : (l.addr ? fresh(m) : l.tv)});
                } else if (isUnary(o)) {
                    TEntry e = pop();
                    if (!e.addr) {
                        restrict(e.tv, o == Op::FloatNeg ? kF : kI);
                    }
                    st.push_back({o == Op::FloatNeg || o == Op::IntNeg ? (e.addr ? fresh(kI) : e.tv) : fresh(kI)});
                } else if (isStore(o)) {
                    const TEntry v = pop();
                    const TEntry a = pop();
                    // The compiler's read of an object array's element: `+= 0` on its address,
                    // whatever its type.
                    if (padded_ && o == Op::StoreIntAdd && a.addr && a.object && a.elem && v.zero && !v.addr) {
                        st.push_back({a.var});
                        break;
                    }
                    std::uint8_t m = kI;
                    switch (o) {
                    case Op::StoreInt: m = kI | kO; break;
                    case Op::StoreFloat: case Op::StoreFloatMul: case Op::StoreFloatDiv: case Op::StoreFloatAdd: case Op::StoreFloatSub: m = kF; break;
                    case Op::StoreString: case Op::StoreStringAdd: m = kS; break;
                    default: break;
                    }
                    if (a.addr) {
                        restrict(a.var, m);
                        if (!v.addr) {
                            unify(v.tv, a.var);
                        }
                        st.push_back({a.var});
                    } else {
                        st.push_back({fresh(m)});
                    }
                }
                break;
            }
        }
    }

    // ---- names and order ----

    std::vector<std::string> gname_;
    std::vector<std::vector<std::string>> lname_;
    std::vector<Type> gtype_;
    std::vector<std::vector<Type>> ltype_;
    std::vector<Type> rtype_;
    std::vector<std::size_t> bodyOrder_;
    bool prototypes_ = false;
    std::vector<std::vector<std::uint32_t>> reserveStart_, reserveEnd_;

    void name()
    {
        const auto& fns = file_.functions;
        std::set<std::string> taken;
        for (std::size_t f = 2; f < fns.size(); ++f) {
            taken.insert(fns[f].name);
        }
        const auto unique = [&](std::string s) {
            while (taken.contains(s) || findBuiltin(s)) {
                s += "_";
            }
            return s;
        };
        gname_.clear();
        gtype_.clear();
        for (std::size_t s = 0; s < fns[1].symbolSizes.size(); ++s) {
            std::string n;
            std::optional<Type> hint;
            if (s < 4) {
                n = detail::kLanguageGlobals[s];
            } else {
                if (opts_.names) {
                    for (const auto& g : opts_.names->globals) {
                        if (g.symbol == s && validName(g.name)) {
                            n = g.name;
                            hint = g.type;
                        }
                    }
                }
                n = unique(n.empty() ? "g" + std::to_string(s) : n);
                taken.insert(n);
            }
            gname_.push_back(n);
            gtype_.push_back(s < 4 ? Type::Int : chosen(gv_[s], hint));
        }
        // Object variables: their own names (after `object.`), from the debug info or ovN.
        const std::size_t objects = fns[0].symbolSizes.size();
        oname_.assign(objects, std::string());
        otype_.assign(objects, Type::Int);
        std::set<std::string> objectNames;
        for (std::size_t s = 1; s < objects; ++s) {
            std::string n;
            std::optional<Type> hint;
            if (opts_.names) {
                for (const auto& v : opts_.names->objectVars) {
                    if (v.symbol == s) {
                        hint = v.type;
                        if (validName(v.name)) {
                            n = v.name;
                        }
                    }
                }
            }
            if (n.empty() || objectNames.contains(n)) {
                n = "ov" + std::to_string(s);
            }
            while (objectNames.contains(n)) {
                n += "_";
            }
            objectNames.insert(n);
            oname_[s] = n;
            otype_[s] = chosen(ov_[s], hint);
        }
        lname_.assign(fns.size(), {});
        ltype_.assign(fns.size(), {});
        rtype_.assign(fns.size(), Type::Void);
        for (std::size_t f = 2; f < fns.size(); ++f) {
            const DebugFunction* dbg = opts_.names ? opts_.names->function(fns[f].name) : nullptr;
            std::set<std::string> local;
            for (std::size_t s = 0; s < fns[f].symbolSizes.size(); ++s) {
                std::string n;
                std::optional<Type> hint;
                if (dbg) {
                    for (const auto& v : dbg->symbols) {
                        if (v.symbol == s) {
                            hint = v.type;
                            if (validName(v.name)) {
                                n = v.name;
                            }
                        }
                    }
                }
                if (n.empty() || local.contains(n) || taken.contains(n)) {
                    n = (s < fns[f].args ? "a" : "v") + std::to_string(s);
                }
                while (local.contains(n) || taken.contains(n) || findBuiltin(n)) {
                    n += "_";
                }
                local.insert(n);
                lname_[f].push_back(n);
                ltype_[f].push_back(chosen(lv_[f][s], hint));
            }
            rtype_[f] = fns[f].returns ? chosen(rv_[f], dbg ? std::optional<Type>(dbg->returns) : std::nullopt) : Type::Void;
        }
    }

    // Bodies in the order their string constants say; strings no code uses as literal statements.
    void order()
    {
        const auto& fns = file_.functions;
        std::vector<std::pair<std::uint32_t, std::size_t>> withStrings;
        std::vector<std::size_t> without;
        std::vector<bool> used(file_.strings.size(), false);
        for (std::size_t f = 2; f < fns.size(); ++f) {
            std::optional<std::uint32_t> first;
            std::uint32_t expect = 0;
            for (const Ins& in : ins_[f]) {
                if (in.o() != Op::PushString) {
                    continue;
                }
                if (used[in.a]) {
                    throw Failure("string " + std::to_string(in.a) + " is pushed twice");
                }
                used[in.a] = true;
                if (!first) {
                    first = in.a;
                } else if (in.a != expect) {
                    throw Failure(fns[f].name + ": its strings aren't numbered in code order");
                }
                expect = in.a + 1;
            }
            if (first) {
                withStrings.emplace_back(*first, f);
            } else {
                without.push_back(f);
            }
        }
        // The globals come first in the source, so their initialisers' strings must be the first.
        std::optional<std::uint32_t> globalEnd;
        for (const Ins& in : ins_[1]) {
            if (in.o() != Op::PushString) {
                continue;
            }
            if (used[in.a] || in.a != (globalEnd ? *globalEnd : 0u)) {
                throw Failure("a global's initialiser has a string out of the source's order");
            }
            used[in.a] = true;
            globalEnd = in.a + 1;
        }
        std::sort(withStrings.begin(), withStrings.end());
        if (globalEnd && !withStrings.empty() && withStrings.front().first < *globalEnd) {
            throw Failure("a global's initialiser has a string out of the source's order");
        }
        bodyOrder_.clear();
        for (const auto& [s, f] : withStrings) {
            bodyOrder_.push_back(f);
        }
        // A function without strings goes right after the nearest one before it by index.
        for (const std::size_t f : without) {
            std::size_t pos = 0;
            std::size_t best = 0;
            for (std::size_t i = 0; i < bodyOrder_.size(); ++i) {
                if (bodyOrder_[i] < f && bodyOrder_[i] >= best) {
                    best = bodyOrder_[i];
                    pos = i + 1;
                }
            }
            bodyOrder_.insert(bodyOrder_.begin() + static_cast<std::ptrdiff_t>(pos), f);
        }
        prototypes_ = !std::is_sorted(bodyOrder_.begin(), bodyOrder_.end());
        // Unused strings: at the end of the body whose strings come before them (or the start of
        // the first).
        reserveStart_.assign(fns.size(), {});
        reserveEnd_.assign(fns.size(), {});
        for (std::uint32_t s = 0; s < used.size(); ++s) {
            if (used[s]) {
                continue;
            }
            std::optional<std::size_t> before;
            for (const auto& [first, f] : withStrings) {
                if (first < s) {
                    before = f;
                }
            }
            if (before) {
                reserveEnd_[*before].push_back(s);
            } else if (!bodyOrder_.empty()) {
                reserveStart_[bodyOrder_.front()].push_back(s);
            } else {
                throw Failure("string constants with no function to keep them in");
            }
        }
    }

    // ---- expressions ----

    using Stack = std::vector<NodeP>;

    NodeP popValue(Stack& st)
    {
        if (st.empty()) {
            throw Failure("the stack runs dry");
        }
        NodeP n = st.back();
        st.pop_back();
        if (n->k == Node::K::Addr) {
            throw Failure("an address used as a value");
        }
        return n;
    }
    NodeP popAddr(Stack& st)
    {
        if (st.empty() || st.back()->k != Node::K::Addr) {
            throw Failure("a store or index without an address");
        }
        NodeP n = st.back();
        st.pop_back();
        return n;
    }

    // One straight-line instruction onto the stack of trees.
    void apply(std::size_t f, const Ins& in, Stack& st)
    {
        auto n = std::make_shared<Node>();
        n->start = in.at;
        const Op o = in.o();
        switch (o) {
        case Op::LoadVarInt:
        case Op::LoadVarFloat:
        case Op::LoadVarString: {
            n->k = Node::K::Var;
            n->global = in.a;
            n->sym = in.b;
            const Type t = varType(f, n->global, n->sym);
            // The load must be the one the compiler would make.
            const Op want = t == Type::Float ? Op::LoadVarFloat : Op::LoadVarInt;
            if (o != want || varSize(f, n->global, n->sym) > 1) {
                throw Failure("a load the compiler wouldn't make");
            }
            break;
        }
        case Op::LoadVarPtr:
            n->k = Node::K::Addr;
            n->global = in.a;
            n->sym = in.b;
            break;
        case Op::PushInt: n->k = Node::K::Int; n->v = in.a; break;
        case Op::PushFloat: n->k = Node::K::Float; n->v = in.a; break;
        case Op::PushString: n->k = Node::K::Str; n->v = in.a; break;
        case Op::IndexInt:
        case Op::IndexFloat:
        case Op::IndexPtr: {
            NodeP idx = popValue(st);
            NodeP base = popAddr(st);
            if (base->elem || varSize(f, base->global, base->sym) < 2) {
                throw Failure("an index of something that isn't an array");
            }
            n->global = base->global;
            n->sym = base->sym;
            n->start = base->start;
            n->kids.push_back(idx);
            if (o == Op::IndexPtr) {
                n->k = Node::K::Addr;
                n->elem = true;
            } else {
                n->k = Node::K::Elem;
                const Type t = varType(f, n->global, n->sym);
                if ((o == Op::IndexFloat) != (t == Type::Float)) {
                    throw Failure("an index the compiler wouldn't make");
                }
            }
            break;
        }
        case Op::CallBuiltin:
        case Op::CallScript: {
            const bool script = o == Op::CallScript;
            const std::size_t argc = script ? file_.functions[in.a].args : builtinSignature(in.a).args.size();
            n->k = Node::K::Call;
            n->script = script;
            n->v = in.a;
            n->kids.resize(argc);
            for (std::size_t i = argc; i-- > 0;) {
                n->kids[i] = popValue(st);
            }
            if (argc) {
                n->start = n->kids[0]->start;
            }
            const bool returns = script ? file_.functions[in.a].returns != 0 : builtinSignature(in.a).returns != Type::Void;
            if (!returns) {
                // A call without a value is a statement of its own; it can't be inside anything.
                voidCalls_.insert(n.get());
            }
            break;
        }
        default:
            if (isBinary(o)) {
                NodeP r = popValue(st);
                NodeP l = popValue(st);
                n->k = Node::K::Binary;
                n->v = in.op;
                n->start = l->start;
                n->kids = {l, r};
            } else if (isUnary(o)) {
                NodeP x = popValue(st);
                n->k = Node::K::Unary;
                n->v = in.op;
                n->start = x->start;
                n->kids = {x};
            } else if (isStore(o)) {
                NodeP v = popValue(st);
                NodeP a = popAddr(st);
                if (padded_ && o == Op::StoreIntAdd && a->elem && isObject(a->global, a->sym) && v->k == Node::K::Int && v->v == 0) {
                    // The compiler's read of an object array's element (`+= 0` on its address).
                    n->k = Node::K::Elem;
                    n->global = false;
                    n->sym = a->sym;
                    n->start = a->start;
                    n->kids = a->kids;
                    break;
                }
                n->k = Node::K::Assign;
                n->v = in.op;
                n->start = a->start;
                n->kids = {a, v};
                const Type t = varType(f, a->global, a->sym);
                if (!storeFits(o, t) || (!a->elem && varSize(f, a->global, a->sym) > 1)) {
                    throw Failure("a store the compiler wouldn't make");
                }
            } else {
                throw Failure("an instruction out of place: " + std::string(opName(in.op).value_or("?")));
            }
            break;
        }
        for (const auto& k : n->kids) {
            if (voidCalls_.contains(k.get())) {
                throw Failure("a call without a value used as one");
            }
        }
        st.push_back(n);
    }

    static bool storeFits(Op o, Type t)
    {
        switch (o) {
        case Op::StoreInt: return t == Type::Int || t == Type::Object;
        case Op::StoreFloat: return t == Type::Float;
        case Op::StoreString: return t == Type::String;
        case Op::StoreStringAdd: return t == Type::String;
        case Op::StoreFloatMul: case Op::StoreFloatDiv: case Op::StoreFloatAdd: case Op::StoreFloatSub: return t == Type::Float;
        default: return t == Type::Int;
        }
    }

    std::set<const Node*> voidCalls_;

    Type varType(std::size_t f, bool global, std::uint32_t sym) const
    {
        return isObject(global, sym) ? otype_[objectIndex(sym)] : global ? gtype_[sym] : ltype_[f][sym];
    }
    std::uint32_t varSize(std::size_t f, bool global, std::uint32_t sym) const
    {
        return isObject(global, sym) ? file_.functions[0].symbolSizes[objectIndex(sym)]
               : global              ? file_.functions[1].symbolSizes[sym]
                                     : file_.functions[f].symbolSizes[sym];
    }

    void flush(Stack& st, std::vector<DStmt>& out)
    {
        for (auto& n : st) {
            if (n->k == Node::K::Addr) {
                throw Failure("an address left over");
            }
            DStmt s;
            s.k = DStmt::K::Expr;
            s.e = n;
            out.push_back(std::move(s));
        }
        st.clear();
    }

    // ---- structured statements ----

    struct Ctx {
        std::optional<std::uint32_t> brk, cont;
        const std::set<std::uint32_t>* caseLabels = nullptr;
    };

    std::size_t f_ = 0;
    std::uint32_t codeEnd_ = 0; ///< the final Return's word
    std::set<std::uint32_t> markersPlaced_;

    const Ins& insAt(std::uint32_t at) const { return ins_[f_][index_[f_].at(at)]; }
    const Ins* insEndingAt(std::uint32_t at) const
    {
        const auto it = index_[f_].lower_bound(at);
        if (it == index_[f_].begin()) {
            return nullptr;
        }
        const Ins& prev = ins_[f_][std::prev(it)->second];
        return prev.at + prev.size == at ? &prev : nullptr;
    }
    bool isControl(Op o) const
    {
        return o == Op::Jump || o == Op::JumpIf || o == Op::JumpIfNot || o == Op::Return0 || o == Op::Return;
    }

    void marker(std::uint32_t at, std::vector<DStmt>& out)
    {
        DStmt m;
        m.k = DStmt::K::Marker;
        m.addr = at;
        out.push_back(std::move(m));
        markersPlaced_.insert(at);
    }

    // Statements of [lo, hi). Case labels (a switch body's) are placed at their words.
    void region(std::uint32_t lo, std::uint32_t hi, const Ctx& ctx, std::vector<DStmt>& out,
                const std::multimap<std::uint32_t, DStmt>* labels = nullptr, std::size_t* placed = nullptr)
    {
        Stack st;
        std::uint32_t pc = lo;
        const auto placeLabels = [&](std::uint32_t at) {
            if (!labels) {
                return;
            }
            const auto [b, e] = labels->equal_range(at);
            for (auto it = b; it != e; ++it) {
                out.push_back(it->second);
                ++*placed;
            }
        };
        while (pc < hi) {
            const bool target = targets_[f_].contains(pc);
            if (target || (labels && labels->contains(pc))) {
                flush(st, out);
                placeLabels(pc);
                if (target) {
                    marker(pc, out);
                }
            }
            // A loop: a Jump back here from inside the region.
            if (target) {
                std::optional<std::uint32_t> back;
                for (const Ins& in : ins_[f_]) {
                    if (in.o() == Op::Jump && in.a == pc && in.at > pc && in.at < hi) {
                        back = in.at;
                    }
                }
                if (back) {
                    pc = loop(pc, *back, out);
                    continue;
                }
            }
            const Ins& in = insAt(pc);
            const Op o = in.o();
            if (!isControl(o)) {
                apply(f_, in, st);
                pc += in.size;
                continue;
            }
            switch (o) {
            case Op::Return:
                throw Failure("Return inside a region");
            case Op::Return0: {
                DStmt r;
                r.k = DStmt::K::Return;
                if (file_.functions[f_].returns) {
                    r.e = popValue(st);
                }
                flush(st, out);
                out.push_back(std::move(r));
                pc += in.size;
                break;
            }
            case Op::Jump: {
                flush(st, out);
                DStmt j;
                if (ctx.brk && in.a == *ctx.brk) {
                    j.k = DStmt::K::Break;
                } else if (ctx.cont && in.a == *ctx.cont) {
                    j.k = DStmt::K::Continue;
                } else {
                    j.k = DStmt::K::Goto;
                    j.addr = in.a;
                }
                out.push_back(std::move(j));
                pc += in.size;
                break;
            }
            case Op::JumpIf: {
                NodeP cond = popValue(st);
                if (switchStmt(cond, st, in, hi, ctx, out, pc)) {
                    break;
                }
                flush(st, out);
                DStmt g;
                g.k = DStmt::K::GotoIf;
                g.e = cond;
                g.addr = in.a;
                out.push_back(std::move(g));
                pc += in.size;
                break;
            }
            case Op::JumpIfNot: {
                NodeP cond = popValue(st);
                flush(st, out);
                const std::uint32_t t = in.a;
                const std::uint32_t after = pc + in.size;
                if (t >= after && t <= hi) {
                    DStmt s;
                    s.k = DStmt::K::If;
                    s.e = cond;
                    const Ins* j = t > after ? insEndingAt(t) : nullptr;
                    bool isElse = false;
                    // (A Jump just to here is the then block's own: a break or the like, or an
                    // empty else, sorted out below.)
                    if (j && j->o() == Op::Jump && j->a > t && j->a <= hi && j->at >= after) {
                        isElse = true;
                        // A Jump to the end of a switch with case labels between is a break.
                        if (ctx.caseLabels) {
                            const auto lb = ctx.caseLabels->lower_bound(t);
                            if (lb != ctx.caseLabels->end() && *lb < j->a) {
                                isElse = false;
                            }
                            if (lb != ctx.caseLabels->end() && *lb == j->a && j->a == *ctx.brk) {
                                isElse = false;
                            }
                        }
                    }
                    if (isElse) {
                        Ctx inner = ctx;
                        inner.caseLabels = nullptr;
                        region(after, j->at, inner, s.a);
                        s.hasElse = true;
                        region(t, j->a, inner, s.b);
                        pc = j->a;
                    } else {
                        Ctx inner = ctx;
                        inner.caseLabels = nullptr;
                        region(after, t, inner, s.a);
                        pc = t;
                        // A then block ending in a goto to just past it: an empty else.
                        if (!s.a.empty() && s.a.back().k == DStmt::K::Goto && s.a.back().addr == t) {
                            s.a.pop_back();
                            s.hasElse = true;
                        }
                    }
                    out.push_back(std::move(s));
                } else {
                    DStmt g;
                    g.k = DStmt::K::GotoIfNot;
                    g.e = cond;
                    g.addr = t;
                    out.push_back(std::move(g));
                    pc = after;
                }
                break;
            }
            default:
                break;
            }
        }
        flush(st, out);
        if (labels) {
            placeLabels(hi);
        }
    }


    // A loop from `top` to the Jump back at `back`: while / for (a condition first), else for (;;).
    std::uint32_t loop(std::uint32_t top, std::uint32_t back, std::vector<DStmt>& out)
    {
        const std::uint32_t exit = back + 2;
        DStmt s;
        s.k = DStmt::K::Loop;
        // The condition: straight code from the top to a JumpIfNot leaving the loop.
        Stack st;
        std::uint32_t pc = top;
        bool conditional = false;
        try {
            while (pc < back) {
                const Ins& in = insAt(pc);
                if (isControl(in.o())) {
                    if (in.o() == Op::JumpIfNot && in.a == exit && st.size() == 1 && !targets_[f_].contains(pc)) {
                        conditional = true;
                    }
                    break;
                }
                if (pc != top && targets_[f_].contains(pc)) {
                    break;
                }
                apply(f_, in, st);
                pc += in.size;
            }
        } catch (const Failure&) {
            conditional = false;
        }
        Ctx inner;
        inner.brk = exit;
        inner.cont = top;
        if (conditional) {
            s.e = st.back();
            region(pc + 2, back, inner, s.a);
        } else {
            region(top, back, inner, s.a);
        }
        out.push_back(std::move(s));
        return exit;
    }

    // tmp = value; (LoadVarInt tmp, case, IntEq, JumpIf)...; Jump default-or-end; bodies.
    bool switchStmt(const NodeP& firstCond, Stack& st, const Ins& firstJump, std::uint32_t hi, const Ctx& ctx,
                    std::vector<DStmt>& out, std::uint32_t& pc)
    {
        const auto caseOf = [&](const NodeP& c, VarKey& key) -> NodeP {
            if (c->k != Node::K::Binary || static_cast<Op>(c->v) != Op::IntEq || c->kids[0]->k != Node::K::Var) {
                return nullptr;
            }
            key = {c->kids[0]->global, c->kids[0]->sym};
            return c->kids[1];
        };
        VarKey temp;
        NodeP firstCase = caseOf(firstCond, temp);
        if (!firstCase || st.empty()) {
            return false;
        }
        const NodeP assign = st.back();
        if (assign->k != Node::K::Assign || static_cast<Op>(assign->v) != Op::StoreInt || assign->kids[0]->elem ||
            VarKey{assign->kids[0]->global, assign->kids[0]->sym} != temp) {
            return false;
        }
        std::vector<std::pair<NodeP, std::uint32_t>> cases{{firstCase, firstJump.a}};
        std::uint32_t at = firstJump.at + firstJump.size;
        for (;;) {
            // Another case: straight code to a JumpIf on (tmp == case).
            Stack cs;
            std::uint32_t q = at;
            bool ok = false;
            try {
                while (q < hi) {
                    if (targets_[f_].contains(q)) {
                        break;
                    }
                    const Ins& in = insAt(q);
                    if (isControl(in.o())) {
                        if (in.o() == Op::JumpIf && cs.size() == 1) {
                            VarKey k;
                            NodeP c = caseOf(cs.back(), k);
                            if (c && k == temp) {
                                cases.emplace_back(c, in.a);
                                q += in.size;
                                ok = true;
                            }
                        }
                        break;
                    }
                    apply(f_, in, cs);
                    q += in.size;
                }
            } catch (const Failure&) {
                ok = false;
            }
            if (!ok) {
                break;
            }
            at = q;
        }
        if (at >= hi || targets_[f_].contains(at)) {
            return false;
        }
        const Ins& tail = insAt(at);
        if (tail.o() != Op::Jump) {
            return false;
        }
        const std::uint32_t bodyStart = at + tail.size;
        const std::uint32_t dflt = tail.a;
        std::uint32_t maxLabel = dflt;
        for (const auto& [c, t] : cases) {
            if (t < bodyStart || t > hi) {
                return false;
            }
            maxLabel = std::max(maxLabel, t);
        }
        if (dflt < bodyStart || dflt > hi) {
            return false;
        }
        // The end: past the last label, as far as the breaks reach.
        std::uint32_t end = maxLabel;
        for (bool grew = true; grew;) {
            grew = false;
            for (const Ins& in : ins_[f_]) {
                if (in.at < bodyStart || in.at >= end || in.o() != Op::Jump) {
                    continue;
                }
                if (in.a > end && in.a <= hi && in.a != ctx.brk && in.a != ctx.cont) {
                    end = in.a;
                    grew = true;
                }
            }
        }
        // The cases' labels: in the dispatch's order; the default (when it isn't the end) after
        // the cases at its word.
        std::multimap<std::uint32_t, DStmt> labels;
        std::set<std::uint32_t> labelWords;
        for (const auto& [c, t] : cases) {
            DStmt l;
            l.k = DStmt::K::Case;
            l.e = c;
            labels.emplace(t, l);
            labelWords.insert(t);
        }
        const bool hasDefault = dflt != end;
        if (hasDefault) {
            DStmt l;
            l.k = DStmt::K::Default;
            labels.emplace(dflt, l);
            labelWords.insert(dflt);
        }
        // Their order in the body must be the dispatch's (the compiler compares in label order).
        std::uint32_t last = 0;
        for (const auto& [c, t] : cases) {
            if (t < last) {
                return false;
            }
            last = t;
        }
        st.pop_back(); // the assignment
        flush(st, out);
        DStmt s;
        s.k = DStmt::K::Switch;
        s.e = assign->kids[1];
        s.temp = temp;
        Ctx inner;
        inner.brk = end;
        inner.cont = ctx.cont;
        inner.caseLabels = &labelWords;
        std::size_t placed = 0;
        region(bodyStart, end, inner, s.a, &labels, &placed);
        if (placed != labels.size()) {
            throw Failure("a switch's case labels aren't all at its top level");
        }
        out.push_back(std::move(s));
        pc = end;
        return true;
    }

    // ---- the goto form ----

    void flat(std::vector<DStmt>& out)
    {
        Stack st;
        const auto& list = ins_[f_];
        for (std::size_t i = 0; i + 1 < list.size(); ++i) {
            const Ins& in = list[i];
            if (targets_[f_].contains(in.at)) {
                flush(st, out);
                marker(in.at, out);
            }
            const Op o = in.o();
            if (!isControl(o)) {
                apply(f_, in, st);
                continue;
            }
            DStmt s;
            if (o == Op::Return0) {
                s.k = DStmt::K::Return;
                if (file_.functions[f_].returns) {
                    s.e = popValue(st);
                }
            } else if (o == Op::Jump) {
                s.k = DStmt::K::Goto;
                s.addr = in.a;
            } else {
                s.k = o == Op::JumpIf ? DStmt::K::GotoIf : DStmt::K::GotoIfNot;
                s.e = popValue(st);
                s.addr = in.a;
            }
            flush(st, out);
            out.push_back(std::move(s));
        }
        flush(st, out);
        if (targets_[f_].contains(codeEnd_)) {
            marker(codeEnd_, out);
        }
    }

    // ---- printing ----

    std::string lit(const Node& n) const
    {
        if (n.k == Node::K::Int) {
            if (n.v >= 0x80000000u) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "0x%08X", n.v);
                return buf;
            }
            return std::to_string(n.v);
        }
        if (n.k == Node::K::Float) {
            const float v = std::bit_cast<float>(n.v);
            if (std::signbit(v) || !std::isfinite(v)) {
                throw Failure("a float constant that can't be written as a literal");
            }
            char buf[64];
            const auto r = std::to_chars(buf, buf + sizeof buf, v);
            std::string s(buf, r.ptr);
            // A float literal needs its dot.
            const auto e = s.find_first_of("eE");
            if (s.find('.') == std::string::npos) {
                s.insert(e == std::string::npos ? s.size() : e, ".0");
            }
            return s;
        }
        return quoteString(file_.strings[n.v]);
    }

    static std::string quoteString(const std::string& s)
    {
        std::string out = "\"";
        for (const char c : s) {
            const auto u = static_cast<unsigned char>(c);
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (u < 0x20 || u >= 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\%03o", u);
                    out += buf;
                } else {
                    out += c;
                }
            }
        }
        return out + "\"";
    }

    std::string varName(bool global, std::uint32_t sym) const
    {
        return isObject(global, sym) ? "object." + oname_[objectIndex(sym)] : global ? gname_[sym] : lname_[f_][sym];
    }

    std::string expr(const Node& n, int minPrec = 0) const
    {
        switch (n.k) {
        case Node::K::Int:
        case Node::K::Float:
        case Node::K::Str:
            return lit(n);
        case Node::K::Var:
            return varName(n.global, n.sym);
        case Node::K::Elem:
            return varName(n.global, n.sym) + "[" + expr(*n.kids[0]) + "]";
        case Node::K::Addr:
            return varName(n.global, n.sym) + (n.elem ? "[" + expr(*n.kids[0]) + "]" : "");
        case Node::K::Call: {
            std::string out = n.script ? fnName(n.v) : std::string(builtinSignature(n.v).name);
            // A renamed builtin whose new name is one of the script's functions: its old name.
            if (!n.script && builtinAlias(n.v) && file_.find(out)) {
                out = std::string(*builtinAlias(n.v));
            }
            out += "(";
            for (std::size_t i = 0; i < n.kids.size(); ++i) {
                if (i) {
                    out += ", ";
                }
                const Node& a = *n.kids[i];
                // A function argument: the function's name for its index.
                if (!n.script && builtinSignature(n.v).args[i] == Type::Function) {
                    if (a.k != Node::K::Int || a.v < 2 || a.v >= file_.functions.size()) {
                        throw Failure("a function argument that isn't a function");
                    }
                    out += fnName(a.v);
                    continue;
                }
                out += expr(a);
            }
            return out + ")";
        }
        case Node::K::Unary: {
            const Op o = static_cast<Op>(n.v);
            const std::string op = o == Op::BoolNot ? "!" : o == Op::IntNot ? "~" : "-";
            const Node& x = *n.kids[0];
            const bool plain = x.k == Node::K::Int || x.k == Node::K::Float || x.k == Node::K::Var || x.k == Node::K::Elem ||
                               x.k == Node::K::Call || x.k == Node::K::Str;
            return op + (plain ? expr(x) : "(" + expr(x) + ")");
        }
        case Node::K::Binary: {
            const std::string_view op = binaryText(static_cast<Op>(n.v));
            const int p = precedence(op);
            std::string s = expr(*n.kids[0], p) + " " + std::string(op) + " " + expr(*n.kids[1], p + 1);
            return p < minPrec ? "(" + s + ")" : s;
        }
        case Node::K::Assign:
            return "(" + assignText(n) + ")";
        }
        return "?";
    }

    std::string assignText(const Node& n) const
    {
        return expr(*n.kids[0]) + " " + std::string(storeText(static_cast<Op>(n.v))) + " " + expr(*n.kids[1]);
    }

    // Inside an expression an assignment needs parentheses; at the top of a statement it doesn't.
    std::string exprStmt(const Node& n) const
    {
        if (n.k == Node::K::Str) {
            return "(" + lit(n) + ")"; // (a bare literal statement would only reserve its string)
        }
        return n.k == Node::K::Assign ? assignText(n) : expr(n);
    }
    std::string sub(const Node& n) const { return expr(n); }

    std::set<std::uint32_t> gotoTargets_;

    void collectGotos(const std::vector<DStmt>& list)
    {
        for (const auto& s : list) {
            if (s.k == DStmt::K::Goto || s.k == DStmt::K::GotoIf || s.k == DStmt::K::GotoIfNot) {
                gotoTargets_.insert(s.addr);
            }
            collectGotos(s.a);
            collectGotos(s.b);
        }
    }

    static bool hasContinue(const std::vector<DStmt>& list)
    {
        for (const auto& s : list) {
            if (s.k == DStmt::K::Continue) {
                return true;
            }
            if (s.k != DStmt::K::Loop && (hasContinue(s.a) || hasContinue(s.b))) {
                return true;
            }
        }
        return false;
    }

    bool printable(const DStmt& s) const { return s.k != DStmt::K::Marker || gotoTargets_.contains(s.addr); }

    static bool isAssignStmt(const DStmt& s) { return s.k == DStmt::K::Expr && s.e->k == Node::K::Assign; }

    void print(const std::vector<DStmt>& list, int depth, std::string& out) const
    {
        const std::string pad(static_cast<std::size_t>(depth) * 4, ' ');
        for (std::size_t i = 0; i < list.size(); ++i) {
            const DStmt& s = list[i];
            if (!printable(s)) {
                continue;
            }
            // An assignment just before a loop that counts by an assignment: the for's first part.
            if (isAssignStmt(s)) {
                std::size_t k = i + 1;
                while (k < list.size() && !printable(list[k])) {
                    ++k;
                }
                if (k < list.size() && forLoop(list[k])) {
                    printLoop(list[k], &s, depth, out);
                    i = k;
                    continue;
                }
            }
            switch (s.k) {
            case DStmt::K::Expr:
                out += pad + exprStmt(*s.e) + ";\n";
                break;
            case DStmt::K::Reserve:
                out += pad + quoteString(file_.strings[s.addr]) + "; // (a string constant no code uses)\n";
                break;
            case DStmt::K::If: {
                out += pad + "if (" + expr(*s.e) + ") {\n";
                print(s.a, depth + 1, out);
                const DStmt* els = s.hasElse ? &s : nullptr;
                while (els) {
                    // else if: an else holding nothing but an if.
                    std::vector<const DStmt*> shown;
                    for (const auto& b : els->b) {
                        if (printable(b)) {
                            shown.push_back(&b);
                        }
                    }
                    if (shown.size() == 1 && shown[0]->k == DStmt::K::If) {
                        out += pad + "} else if (" + expr(*shown[0]->e) + ") {\n";
                        print(shown[0]->a, depth + 1, out);
                        els = shown[0]->hasElse ? shown[0] : nullptr;
                        continue;
                    }
                    out += pad + "} else {\n";
                    print(els->b, depth + 1, out);
                    els = nullptr;
                }
                out += pad + "}\n";
                break;
            }
            case DStmt::K::Loop:
                printLoop(s, nullptr, depth, out);
                break;
            case DStmt::K::Switch: {
                const bool hidden = implicitTemps_;
                out += pad + "switch (" + (hidden ? expr(*s.e) : varName(s.temp.global, s.temp.sym) + " = " + expr(*s.e)) + ") {\n";
                print(s.a, depth + 1, out);
                out += pad + "}\n";
                break;
            }
            case DStmt::K::Case:
                out += std::string(static_cast<std::size_t>(std::max(0, depth - 1)) * 4, ' ') + "case " + expr(*s.e) + ":\n";
                break;
            case DStmt::K::Default:
                out += std::string(static_cast<std::size_t>(std::max(0, depth - 1)) * 4, ' ') + "default:\n";
                break;
            case DStmt::K::Break:
                out += pad + "break;\n";
                break;
            case DStmt::K::Continue:
                out += pad + "continue;\n";
                break;
            case DStmt::K::Return:
                out += pad + (s.e ? "return " + expr(*s.e) + ";\n" : "return;\n");
                break;
            case DStmt::K::Goto:
                out += pad + "goto " + label(s.addr) + ";\n";
                break;
            case DStmt::K::GotoIf:
                out += pad + "__goto_if(" + expr(*s.e) + ", " + label(s.addr) + ");\n";
                break;
            case DStmt::K::GotoIfNot:
                out += pad + "__goto_ifnot(" + expr(*s.e) + ", " + label(s.addr) + ");\n";
                break;
            case DStmt::K::Marker:
                out += std::string(static_cast<std::size_t>(std::max(0, depth - 1)) * 4, ' ') + label(s.addr) + ":\n";
                break;
            }
        }
    }

    // A loop written as a for: its body ends with an assignment and has no continue (a for's
    // continue goes to that assignment, a while's to the condition).
    bool forLoop(const DStmt& s) const
    {
        if (s.k != DStmt::K::Loop || hasContinue(s.a)) {
            return false;
        }
        for (auto it = s.a.rbegin(); it != s.a.rend(); ++it) {
            if (!printable(*it)) {
                continue;
            }
            return isAssignStmt(*it);
        }
        return false;
    }

    void printLoop(const DStmt& s, const DStmt* init, int depth, std::string& out) const
    {
        const std::string pad(static_cast<std::size_t>(depth) * 4, ' ');
        if (!forLoop(s)) {
            if (init) {
                out += pad + exprStmt(*init->e) + ";\n";
            }
            out += pad + (s.e ? "while (" + expr(*s.e) + ") {\n" : "for (;;) {\n");
            print(s.a, depth + 1, out);
            out += pad + "}\n";
            return;
        }
        // The last statement is the third part.
        std::size_t last = s.a.size();
        while (last-- > 0 && !printable(s.a[last])) {
        }
        std::vector<DStmt> body(s.a.begin(), s.a.begin() + static_cast<std::ptrdiff_t>(last));
        for (std::size_t k = last + 1; k < s.a.size(); ++k) {
            body.push_back(s.a[k]); // (unprinted markers)
        }
        out += pad + "for (" + (init ? exprStmt(*init->e) : std::string()) + "; " + (s.e ? expr(*s.e) : std::string()) + "; " +
               exprStmt(*s.a[last].e) + ") {\n";
        print(body, depth + 1, out);
        out += pad + "}\n";
    }

    std::string label(std::uint32_t at) const { return "L" + std::to_string(at); }

    // A function's name as source: as it is, or as a string literal when it isn't an identifier
    // (or is a keyword's or a builtin's).
    std::string fnName(std::size_t f) const
    {
        const std::string& n = file_.functions[f].name;
        return validName(n) ? n : quoteString(n);
    }

    bool implicitTemps_ = true;

    // Switch temporaries left to the compiler when the symbols it would hand out are these:
    // the arguments, the declared locals in order, then each switch's temporary as it closes.
    void collectSwitches(const std::vector<DStmt>& list, std::vector<VarKey>& closing) const
    {
        for (const auto& s : list) {
            collectSwitches(s.a, closing);
            collectSwitches(s.b, closing);
            if (s.k == DStmt::K::Switch) {
                closing.push_back(s.temp);
            }
        }
    }

    std::set<std::uint32_t> declaredHidden_;

    void chooseTemps(const std::vector<DStmt>& body)
    {
        std::vector<VarKey> closing;
        collectSwitches(body, closing);
        declaredHidden_.clear();
        implicitTemps_ = false;
        if (closing.empty()) {
            return;
        }
        const auto& fn = file_.functions[f_];
        std::set<std::uint32_t> temps;
        for (const auto& t : closing) {
            if (t.global || t.sym < fn.args || !temps.insert(t.sym).second) {
                return;
            }
        }
        // A temporary used anywhere but its switch is a variable of the source's.
        std::map<std::uint32_t, int> uses;
        for (const Ins& in : ins_[f_]) {
            if ((in.o() == Op::LoadVarInt || in.o() == Op::LoadVarPtr || in.o() == Op::LoadVarFloat) && !in.a && temps.contains(in.b)) {
                ++uses[in.b];
            }
        }
        // (each switch: its store, then one load per case)
        std::map<std::uint32_t, int> expected;
        countSwitchUses(body, expected);
        for (const auto t : temps) {
            if (uses[t] != expected[t]) {
                return;
            }
        }
        std::uint32_t next = 0;
        std::vector<std::uint32_t> layout;
        for (std::uint32_t s = 0; s < fn.symbolSizes.size(); ++s) {
            if (!temps.contains(s)) {
                layout.push_back(s);
            }
        }
        for (const auto& t : closing) {
            layout.push_back(t.sym);
        }
        for (const auto s : layout) {
            if (s != next++) {
                return;
            }
        }
        implicitTemps_ = true;
        declaredHidden_ = temps;
    }

    static void countSwitchUses(const std::vector<DStmt>& list, std::map<std::uint32_t, int>& out)
    {
        for (const auto& s : list) {
            countSwitchUses(s.a, out);
            countSwitchUses(s.b, out);
            if (s.k == DStmt::K::Switch) {
                int cases = 0;
                for (const auto& c : s.a) {
                    cases += c.k == DStmt::K::Case;
                }
                out[s.temp.sym] += 1 + cases;
            }
        }
    }

    // ---- the whole source ----

    std::vector<std::pair<std::uint32_t, std::size_t>> lineOf_; // a function's first line

    std::size_t functionAtLine(std::uint32_t line) const
    {
        std::size_t f = 0;
        for (const auto& [l, fn] : lineOf_) {
            if (l <= line) {
                f = fn;
            }
        }
        return f;
    }

    std::string typeText(Type t) const { return std::string(typeName(t)); }

    std::string renderFunction(std::size_t f, int mode)
    {
        f_ = f;
        codeEnd_ = ins_[f].back().at;
        markersPlaced_.clear();
        gotoTargets_.clear();
        voidCalls_.clear();
        std::vector<DStmt> body;
        for (const auto s : reserveStart_[f]) {
            DStmt r;
            r.k = DStmt::K::Reserve;
            r.addr = s;
            body.push_back(r);
        }
        if (mode == 0) {
            region(0, codeEnd_, Ctx{}, body);
            if (targets_[f].contains(codeEnd_)) {
                marker(codeEnd_, body);
            }
        } else {
            flat(body);
        }
        for (const auto s : reserveEnd_[f]) {
            DStmt r;
            r.k = DStmt::K::Reserve;
            r.addr = s;
            body.push_back(r);
        }
        collectGotos(body);
        for (const auto t : gotoTargets_) {
            if (!markersPlaced_.contains(t)) {
                throw Failure("a goto to a place no label can go");
            }
        }
        if (mode == 0) {
            chooseTemps(body);
        } else {
            implicitTemps_ = false;
            declaredHidden_.clear();
        }
        const auto& fn = file_.functions[f];
        std::string out = signature(f) + "\n{\n";
        for (std::uint32_t s = fn.args; s < fn.symbolSizes.size(); ++s) {
            if (implicitTemps_ && declaredHidden_.contains(s)) {
                continue;
            }
            out += "    " + typeText(ltype_[f][s]) + " " + lname_[f][s] +
                   (fn.symbolSizes[s] > 1 ? "[" + std::to_string(fn.symbolSizes[s]) + "]" : "") + ";\n";
        }
        print(body, 1, out);
        out += "}\n";
        return out;
    }

    std::string signature(std::size_t f) const
    {
        const auto& fn = file_.functions[f];
        std::string out = typeText(rtype_[f]) + " " + fnName(f) + "(";
        for (std::uint32_t a = 0; a < fn.args; ++a) {
            out += (a ? ", " : "") + typeText(ltype_[f][a]) + " " + lname_[f][a];
        }
        return out + ")";
    }

    std::string globals()
    {
        f_ = 1;
        voidCalls_.clear();
        // GLOBAL's code: one initialiser after another.
        std::vector<std::pair<std::uint32_t, NodeP>> inits;
        Stack st;
        for (std::size_t i = 0; i + 1 < ins_[1].size(); ++i) {
            const Ins& in = ins_[1][i];
            if (isControl(in.o())) {
                throw Failure("GLOBAL's code isn't only initialisers");
            }
            apply(1, in, st);
        }
        for (const auto& n : st) {
            if (n->k != Node::K::Assign || static_cast<Op>(n->v) != storeOpFor(gtype_[n->kids[0]->sym]) || !n->kids[0]->global ||
                n->kids[0]->elem || n->kids[0]->sym < 4) {
                throw Failure("GLOBAL's code isn't only initialisers");
            }
            inits.emplace_back(n->kids[0]->sym, n->kids[1]);
        }
        std::set<std::uint32_t> seen;
        for (const auto& [s, e] : inits) {
            if (!seen.insert(s).second) {
                throw Failure("a global initialised twice");
            }
        }
        const auto& sizes = file_.functions[1].symbolSizes;
        std::string out;
        const auto decl = [&](std::uint32_t s, const Node* init) {
            out += typeText(gtype_[s]) + " " + gname_[s] + (sizes[s] > 1 ? "[" + std::to_string(sizes[s]) + "]" : "");
            if (init) {
                out += " = " + sub(*init);
            }
            out += ";\n";
        };
        std::uint32_t next = 4;
        for (const auto& [s, e] : inits) {
            if (s >= next) {
                while (next < s) {
                    decl(next++, nullptr);
                }
                decl(s, e.get());
                next = s + 1;
            } else {
                decl(s, e.get()); // (declared again with its initialiser: its place in GLOBAL's code)
            }
        }
        while (next < sizes.size()) {
            decl(next++, nullptr);
        }
        return out;
    }

    static Op storeOpFor(Type t) { return t == Type::Float ? Op::StoreFloat : t == Type::String ? Op::StoreString : Op::StoreInt; }

    static std::uint32_t lines(const std::string& s) { return static_cast<std::uint32_t>(std::count(s.begin(), s.end(), '\n')); }

    std::string assemble(std::vector<int>& mode, std::vector<std::string>& why)
    {
        std::string out = "// Decompiled by NoX Core.\n";
        if (ignoreObject_) {
            out += "//@ignore_object_type\n";
        }
        if (rawObjects_) {
            out += "//@raw_object_vars\n";
        }
        out += "\n" + globals();
        if (const std::string objects = objectDeclarations(); !objects.empty()) {
            out += "\n" + objects;
        }
        if (prototypes_) {
            out += "\n";
            for (std::size_t f = 2; f < file_.functions.size(); ++f) {
                out += signature(f) + ";\n";
            }
        }
        lineOf_.clear();
        for (const std::size_t f : bodyOrder_) {
            std::string text;
            while (mode[f] < 2) {
                try {
                    text = renderFunction(f, mode[f]);
                    break;
                } catch (const std::exception& e) {
                    why[f] = e.what();
                    ++mode[f];
                }
            }
            if (mode[f] >= 2 && text.empty()) {
                // Neither form: a stub that keeps the function's index and strings honest is
                // impossible, so the source is marked and won't compile back the same.
                text = signature(f) + "\n{\n    // (couldn't be decompiled: " + why[f] + ")\n}\n";
            }
            out += "\n";
            lineOf_.emplace_back(lines(out) + 1, f);
            out += text;
        }
        return out;
    }

    const ScriptFile& file_;
    const DecompileOptions& opts_;
    std::vector<std::vector<Ins>> ins_;
    std::vector<std::map<std::uint32_t, std::size_t>> index_;
    std::vector<std::set<std::uint32_t>> targets_;
};

} // namespace

std::string compareFunctions(const Function& a, const Function& b)
{
    if (a.name != b.name) {
        return "named " + b.name + ", not " + a.name;
    }
    if (a.args != b.args || a.returns != b.returns) {
        return "arguments or result differ";
    }
    if (a.symbolSizes != b.symbolSizes) {
        return "symbols differ (" + std::to_string(a.symbolSizes.size()) + " vs " + std::to_string(b.symbolSizes.size()) + ")";
    }
    if (a.symbUnused != b.symbUnused) {
        return "SYMB's unused word differs";
    }
    if (a.code != b.code) {
        const std::size_t n = std::min(a.code.size(), b.code.size());
        std::size_t i = 0;
        while (i < n && a.code[i] == b.code[i]) {
            ++i;
        }
        return "code differs at word " + std::to_string(i) + " (" + std::to_string(a.code.size()) + " vs " + std::to_string(b.code.size()) + " words)";
    }
    return {};
}

DecompileResult decompile(const ScriptFile& file, const DecompileOptions& options)
{
    return Decompiler(file, options).run();
}

} // namespace nox::script
