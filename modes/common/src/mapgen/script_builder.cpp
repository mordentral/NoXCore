// Builds a generated map's compiled NoxScript instruction by instruction (script_builder.hpp).
#include "noxcore/mapgen/script_builder.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace nox::mapgen {

namespace {

using script::Op;

constexpr std::uint32_t word(Op o) { return static_cast<std::uint32_t>(o); }
constexpr int kPresetGlobals = 4; // (self, other, true, false: ScriptHost::setLanguageGlobals)

// A builtin's index in the opcode table, by its name; throws for an unknown one.
std::uint32_t builtinId(std::string_view name)
{
    // (never destroyed (made on the heap, never freed): a Delve level still being built on a
    // worker as the program exits must not read it after its destruction)
    static const std::map<std::string, std::uint32_t, std::less<>>& ids = *[] {
        auto* m = new std::map<std::string, std::uint32_t, std::less<>>();
        for (std::uint32_t i = 0; i < script::kBuiltinCount; ++i) {
            m->emplace(script::builtinName(i), i);
        }
        return m;
    }();
    const auto it = ids.find(name);
    if (it == ids.end()) {
        throw std::runtime_error("script: no builtin " + std::string(name));
    }
    return it->second;
}

// A script::Function whose symbols have the given sizes in words, laid out back to back.
script::Function makeFunction(std::string name, std::vector<std::uint32_t> sizes, std::vector<std::uint32_t> code, std::uint32_t args)
{
    script::Function fn;
    fn.name = std::move(name);
    fn.args = args;
    std::uint32_t total = 0;
    for (const std::uint32_t s : sizes) {
        fn.symbolSizes.push_back(s);
        fn.symbolOffsets.push_back(total);
        total += s;
    }
    fn.localWords = total;
    fn.code = std::move(code);
    return fn;
}

} // namespace

ScriptBuilder::Fn& ScriptBuilder::Fn::pushInt(std::int32_t v)
{
    emit(word(Op::PushInt));
    emit(static_cast<std::uint32_t>(v));
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::pushString(std::string_view s)
{
    emit(word(Op::PushString));
    emit(owner_.string(s));
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::pushFunction(std::string_view name)
{
    emit(word(Op::PushInt));
    // The function's index is only known at build(): leave a 0 and remember where.
    functions_.emplace_back(code_.size(), std::string(name));
    emit(0);
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::object(std::string_view name)
{
    return pushString(name).call("Object");
}

ScriptBuilder::Fn& ScriptBuilder::Fn::call(std::string_view builtin)
{
    emit(word(Op::CallBuiltin));
    emit(builtinId(builtin));
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::callScript(std::string_view name)
{
    emit(word(Op::CallScript));
    functions_.emplace_back(code_.size(), std::string(name));
    emit(0);
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::op(script::Op o)
{
    emit(static_cast<std::uint32_t>(o));
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::loadGlobal(int g)
{
    // Operands: the scope (1 the globals, which are function 1's locals; 0 this function's own),
    // then the symbol's index.
    emit(word(Op::LoadVarInt));
    emit(1);
    emit(static_cast<std::uint32_t>(g));
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::storeGlobal(int g)
{
    emit(word(Op::LoadVarPtr));
    emit(1);
    emit(static_cast<std::uint32_t>(g));
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::endStore()
{
    emit(word(Op::StoreInt));
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::setGlobal(int g, std::int32_t v)
{
    return storeGlobal(g).pushInt(v).endStore();
}

ScriptBuilder::Fn& ScriptBuilder::Fn::loadLocal(int l)
{
    emit(word(Op::LoadVarInt));
    emit(0);
    emit(static_cast<std::uint32_t>(l));
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::label(std::string_view name)
{
    if (!labels_.emplace(std::string(name), static_cast<std::uint32_t>(code_.size())).second) {
        throw std::runtime_error("script: label " + std::string(name) + " twice");
    }
    return *this;
}

ScriptBuilder::Fn& ScriptBuilder::Fn::jumpIf(std::string_view l)
{
    emit(word(Op::JumpIf));
    // The label's offset is filled in at build(), so a jump can come before its label.
    jumps_.emplace_back(code_.size(), std::string(l));
    emit(0);
    return *this;
}

ScriptBuilder::ScriptBuilder() = default;

int ScriptBuilder::global(std::string_view name)
{
    const auto it = std::ranges::find(globals_, name);
    if (it != globals_.end()) {
        return kPresetGlobals + static_cast<int>(it - globals_.begin());
    }
    globals_.emplace_back(name);
    return kPresetGlobals + static_cast<int>(globals_.size()) - 1;
}

void ScriptBuilder::setGlobalAtLoad(int g, std::int32_t v)
{
    atLoad_.emplace_back(g, v);
}

ScriptBuilder::Fn& ScriptBuilder::function(std::string_view name, std::uint32_t args, std::uint32_t locals)
{
    for (Entry& e : functions_) {
        if (e.name == name) {
            return *e.fn;
        }
    }
    if (name.empty() || 1023 < name.size() || name == "GLOBAL") {
        throw std::runtime_error("script: a function can't be named " + std::string(name));
    }
    Entry e;
    e.name = std::string(name);
    e.args = args;
    e.locals = std::max(args, locals);
    // Script function indices start at 2; 0 and 1 are the two GLOBAL functions.
    e.fn.reset(new Fn(*this, functions_.size() + 2));
    functions_.push_back(std::move(e));
    return *functions_.back().fn;
}

bool ScriptBuilder::has(std::string_view name) const
{
    return std::ranges::any_of(functions_, [&](const Entry& e) { return e.name == name; });
}

std::uint32_t ScriptBuilder::string(std::string_view s)
{
    const auto it = std::ranges::find(strings_, s);
    if (it != strings_.end()) {
        return static_cast<std::uint32_t>(it - strings_.begin());
    }
    strings_.emplace_back(s);
    return static_cast<std::uint32_t>(strings_.size() - 1);
}

script::ScriptFile ScriptBuilder::build() const
{
    script::ScriptFile file;
    file.strings = strings_;
    // Function 0: an empty GLOBAL. Function 1: the globals, one word each, and code giving the
    // setGlobalAtLoad ones their first values.
    file.functions.push_back(makeFunction("GLOBAL", {0}, {word(Op::Return0)}, 0));
    std::vector<std::uint32_t> init;
    for (const auto& [g, v] : atLoad_) {
        init.insert(init.end(), {word(Op::LoadVarPtr), 1, static_cast<std::uint32_t>(g), word(Op::PushInt), static_cast<std::uint32_t>(v), word(Op::StoreInt)});
    }
    init.push_back(word(Op::Return0));
    file.functions.push_back(makeFunction("GLOBAL", std::vector<std::uint32_t>(kPresetGlobals + globals_.size(), 1), std::move(init), 0));
    const auto indexOf = [&](const std::string& name) -> std::uint32_t {
        for (std::size_t i = 0; i < functions_.size(); ++i) {
            if (functions_[i].name == name) {
                return static_cast<std::uint32_t>(i + 2);
            }
        }
        throw std::runtime_error("script: no function " + name);
    };
    for (const Entry& e : functions_) {
        std::vector<std::uint32_t> code = e.fn->code_;
        // Patch each jump with its label's offset, and each function reference with its index.
        for (const auto& [at, l] : e.fn->jumps_) {
            const auto it = e.fn->labels_.find(l);
            if (it == e.fn->labels_.end()) {
                throw std::runtime_error("script: " + e.name + ": no label " + l);
            }
            code[at] = it->second;
        }
        for (const auto& [at, name] : e.fn->functions_) {
            code[at] = indexOf(name);
        }
        code.push_back(word(Op::Return0)); // (a label at the end lands here)
        file.functions.push_back(makeFunction(e.name, std::vector<std::uint32_t>(e.locals, 1), std::move(code), e.args));
    }
    for (std::size_t i = 0; i < file.functions.size(); ++i) {
        if (const auto wrong = script::check(file, i)) {
            throw std::runtime_error("script: " + file.functions[i].name + ": " + *wrong);
        }
    }
    return file;
}

} // namespace nox::mapgen
