// The NoxScript interpreter's loop; the stack, locals and string rules are described in vm.hpp.
#include "noxcore/script/vm.hpp"

#include "noxcore/script/opcodes.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>

namespace nox::script {

namespace {

// The builtins a prefab function's name prefix (table 7 entries; one is the count of
// the next table and matches nothing) and offset (5 entries) reach.
constexpr std::array<std::uint32_t, 6> kPrefixed{60, 110, 112, 111, 113, 0}; // Object .. Wall
constexpr std::array<std::uint32_t, 5> kOffset{0, 67, 68, 72, 97};            // Wall .. Walk

bool among(std::uint32_t id, std::span<const std::uint32_t> list)
{
    for (const auto x : list) {
        if (x == id) {
            return true;
        }
    }
    return false;
}

const std::string kEmpty;

// Two strings joined, cut to 255 characters (sprintf "%s%s" into 256 bytes on the
// interpreter's stack: the game's runs over it past that, an original bug).
std::string joined255(const std::string& a, const std::string& b)
{
    std::string joined = a + b;
    joined.resize(std::min<std::size_t>(joined.size(), 255));
    return joined;
}

// A string as the joins' sprintf("%s%s") reads it: an id with no string (a slot freed, or past
// the strings made) is a null pointer, which the C library prints as "(null)".
const std::string kNullText = "(null)";
const std::string& sprintfText(const Vm& vm, std::uint32_t id)
{
    return id < vm.stringCount() ? vm.string(id) : kNullText;
}

// ---- The float instructions as the game's x87 does them ----
// The game runs the FPU in chop mode (rounding towards zero) at 53 bits: a float result is the
// exact one chopped to a float (chopping twice chops once). Loads go through the FPU (FLD /
// FSTP), which quiets a signalling NaN. Its compares (FCOMP; FNSTSW; TEST AH) take an unordered
// pair (a NaN) as equal and below at once: == and < and <= are true, != and > and >= false.

std::uint32_t quieted(std::uint32_t bits)
{
    const bool nan = (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0;
    return nan ? bits | 0x00400000u : bits;
}

// The arithmetic: util::chopAdd / chopSub / chopMul / chopDiv (util/x87.hpp: the exact result
// chopped, past the largest float stopping there; a NaN passed as the FPU passes it).
using util::chopAdd;
using util::chopDiv;
using util::chopMul;
using util::chopSub;

// FCOMP's verdicts on (left, right).
bool fpuEq(float l, float r) { return std::isnan(l) || std::isnan(r) || l == r; }
bool fpuLt(float l, float r) { return std::isnan(l) || std::isnan(r) || l < r; }
bool fpuLte(float l, float r) { return std::isnan(l) || std::isnan(r) || l <= r; }
bool fpuGt(float l, float r) { return !std::isnan(l) && !std::isnan(r) && l > r; }
bool fpuGte(float l, float r) { return !std::isnan(l) && !std::isnan(r) && l >= r; }

} // namespace

Vm::Vm(const ScriptFile& file, Host& host)
    : file_(&file)
    , host_(&host)
    , strings_(file.strings)
    , constants_(file.strings.size())
{
    locals_.reserve(file.functions.size());
    // One set of locals per function, zeroed, kept for the whole run (not per call).
    for (const auto& fn : file.functions) {
        locals_.emplace_back(fn.localWords, 0u);
    }
}

void Vm::push(std::uint32_t v) noexcept
{
    if (top_ < kStackWords) {
        stack_[top_++] = v;
    }
}

// An empty stack's pop reads the bottom word again, as the game's does.
std::uint32_t Vm::pop() noexcept
{
    if (top_ > 0) {
        --top_;
    }
    return stack_[top_];
}

const std::string& Vm::string(std::uint32_t id) const noexcept
{
    return id < strings_.size() ? strings_[id] : kEmpty;
}

std::uint32_t Vm::addString(std::string s)
{
    // Past the cap, the last string's id is handed out again (its text unchanged).
    if (strings_.size() < kMaxStrings) {
        strings_.push_back(std::move(s));
    }
    return static_cast<std::uint32_t>(strings_.size() - 1);
}

void Vm::freeRuntimeStrings() noexcept
{
    // (The game frees each one and zeroes its slot; the next string made takes the first.)
    if (strings_.size() > constants_) {
        strings_.erase(strings_.begin() + static_cast<std::ptrdiff_t>(constants_), strings_.end());
    }
}

std::span<std::uint32_t> Vm::globals() noexcept
{
    return locals_.size() > 1 ? std::span<std::uint32_t>(locals_[1]) : std::span<std::uint32_t>();
}

bool Vm::call(std::size_t index, std::uint32_t caller, std::uint32_t trigger)
{
    if (index >= file_->functions.size()) {
        return true;
    }
    caller_ = caller;
    trigger_ = trigger;
    return enter(index, 0, SIZE_MAX);
}

bool Vm::enter(std::size_t index, std::size_t begin, std::size_t end)
{
    // A call made while another runs (a handler one of its builtins set off) runs on the outer
    // call's budgets: a loop that sets one off each turn still runs out, and a handler that sets
    // itself off nests only so deep.
    const bool outer = active_ == 0;
    stopped_ = false;
    if (outer) {
        steps_ = 0;
        halted_ = Stop::None;
        depth_ = 0;
        const std::uint64_t frameLeft = frameSteps_ < kMaxFrameSteps ? kMaxFrameSteps - frameSteps_ : 0;
        limit_ = std::min(kMaxSteps, frameLeft);
    } else if (active_ > kMaxNested) {
        halt(Stop::Nested);
        return false;
    }
    // (Counted down, and the depth put back, even when a builtin throws through the call.)
    struct Active {
        int& n;
        int& depth;
        int saved;
        Active(int& count, int& d) : n(count), depth(d), saved(d) { ++n; }
        ~Active()
        {
            --n;
            depth = saved;
        }
        Active(const Active&) = delete;
        Active& operator=(const Active&) = delete;
    };
    bool ran = false;
    {
        const Active active(active_, depth_);
        ran = run(index, outer ? 0 : depth_ + 1, begin, end);
    }
    if (outer) {
        // (The instruction that found the budget spent didn't run.)
        lastSteps_ = std::min(steps_, limit_);
        frameSteps_ += lastSteps_;
        lastStop_ = ran                      ? Stop::None
                  : halted_ != Stop::None    ? halted_
                  : steps_ <= limit_         ? Stop::None
                  : limit_ < kMaxSteps       ? Stop::FrameSteps
                                             : Stop::Steps;
        if (lastStop_ != Stop::None) {
            // A stopped call is never resumed: the words it left would only hold up the map's
            // later handlers (each waits for an empty stack), so they go.
            resetStack();
        }
    }
    return ran;
}

void Vm::halt(Stop why) noexcept
{
    stopped_ = true;
    if (halted_ == Stop::None) {
        halted_ = why;
    }
    // The budget ends here: the call's next instruction, whichever function it's in, stops it.
    limit_ = std::min(limit_, steps_);
}

bool Vm::call(std::string_view name, std::uint32_t caller, std::uint32_t trigger)
{
    const auto index = file_->find(name);
    return index ? call(*index, caller, trigger) : true;
}

bool Vm::callRange(std::size_t index, std::size_t begin, std::size_t end)
{
    if (index >= file_->functions.size() || begin >= end) {
        return true;
    }
    caller_ = 0;
    trigger_ = 0;
    return enter(index, begin, end);
}

// LoadVar: the variable `var` is a symbol index, turned into a word offset through the owning
// function's symbol table (function 1's for globals, function 0's for object variables).
std::uint32_t Vm::loadVar(std::size_t fn, bool global, std::int32_t var) const
{
    if (global) {
        if (file_->functions.size() < 2 || var < 0 || static_cast<std::size_t>(var) >= file_->functions[1].symbolOffsets.size()) {
            return 0;
        }
        return read(fn, true, static_cast<std::int32_t>(file_->functions[1].symbolOffsets[static_cast<std::size_t>(var)]));
    }
    if (var < 0) {
        const auto& offsets = file_->functions[0].symbolOffsets;
        const auto index = static_cast<std::size_t>(-static_cast<std::int64_t>(var));
        return index < offsets.size() ? host_->objectVar(static_cast<std::int32_t>(offsets[index])) : 0;
    }
    const auto& offsets = file_->functions[fn].symbolOffsets;
    if (static_cast<std::size_t>(var) >= offsets.size()) {
        return 0;
    }
    return read(fn, false, static_cast<std::int32_t>(offsets[static_cast<std::size_t>(var)]));
}

std::uint32_t Vm::read(std::size_t fn, bool global, std::int32_t offset) const
{
    // Negative offsets (locals only) are the trigger object's script variables.
    if (!global && offset < 0) {
        return host_->objectVar(-offset);
    }
    const auto& words = locals_[global ? 1 : fn];
    return offset >= 0 && static_cast<std::size_t>(offset) < words.size() ? words[static_cast<std::size_t>(offset)] : 0;
}

void Vm::write(std::size_t fn, bool global, std::int32_t offset, std::uint32_t value)
{
    if (!global && offset < 0) {
        host_->setObjectVar(-offset, value);
        return;
    }
    if (global && locals_.size() < 2) {
        return;
    }
    auto& words = locals_[global ? 1 : fn];
    if (offset >= 0 && static_cast<std::size_t>(offset) < words.size()) {
        words[static_cast<std::size_t>(offset)] = value;
    }
}

int Vm::compare(std::uint32_t left, std::uint32_t right) const
{
    return std::strcmp(string(left).c_str(), string(right).c_str());
}

bool Vm::run(std::size_t index, int depth, std::size_t begin, std::size_t end)
{
    if (depth > kMaxDepth) {
        halt(Stop::Depth);
        return false;
    }
    depth_ = depth; // (put back by the call's end: enter's Active; a nested call goes on from it)
    const Function& fn = file_->functions[index];
    auto& locals = locals_[index];
    // A ranged call (callRange: the editor's hot reload) pops no arguments.
    const bool ranged = begin != 0 || end != SIZE_MAX;
    // Arguments into the first local words: the last one pushed is popped first, into word 0.
    for (std::uint32_t i = 0; i < fn.args && !ranged; ++i) {
        const std::uint32_t v = pop();
        if (i < locals.size()) {
            locals[i] = v;
        }
    }
    // The stack depth at entry; finish() trims back to it plus the return value.
    const std::size_t entry = top_;
    const auto& code = fn.code;
    std::size_t pc = begin;
    const std::size_t limit = end < code.size() ? end : code.size();
    // Running off the end of the code acts as Return0.
    const auto next = [&]() -> std::uint32_t { return pc < limit ? code[pc++] : static_cast<std::uint32_t>(Op::Return0); };
    const auto i32 = [](std::uint32_t v) { return static_cast<std::int32_t>(v); };
    const auto f32 = [](std::uint32_t v) { return std::bit_cast<float>(v); };
    const auto u32 = [](float v) { return std::bit_cast<std::uint32_t>(v); };
    const auto finish = [&] {
        // script_callByIndex's exit: leave the declared return value (if any) and nothing else.
        const std::size_t target = entry + fn.returns;
        if (top_ != target) {
            if (fn.returns != 0) {
                const std::uint32_t v = top_ != 0 ? pop() : 0;
                top_ = entry;
                push(v);
            } else {
                top_ = target;
            }
        }
    };
    for (;;) {
        // The step budget is shared by the whole top-level call, nested script calls included
        // (limit_: kMaxSteps, or what's left of the frame's kMaxFrameSteps).
        if (++steps_ > limit_) {
            stopped_ = true;
            return false;
        }
        const std::uint32_t op = next();
        switch (static_cast<Op>(op)) {
        case Op::LoadVarInt:
        case Op::LoadVarFloat:
        case Op::LoadVarString: {
            const bool global = next() != 0;
            const std::uint32_t v = loadVar(index, global, i32(next()));
            push(op == static_cast<std::uint32_t>(Op::LoadVarFloat) ? quieted(v) : v); // (LoadVarFloat: FLD / FSTP)
            break;
        }
        // A variable's address for stores and indexing: [size, if an array] global flag, word offset
        // (negated for object variables).
        case Op::LoadVarPtr: {
            const std::uint32_t globalWord = next();
            const bool global = globalWord != 0;
            const std::int32_t var = i32(next());
            std::uint32_t size = 0;
            std::int32_t offset = 0;
            const Function* owner = global ? (file_->functions.size() > 1 ? &file_->functions[1] : nullptr)
                                    : var < 0 ? &file_->functions[0]
                                              : &fn;
            const auto symbol = static_cast<std::size_t>(var < 0 && !global ? -static_cast<std::int64_t>(var) : var);
            if (owner && symbol < owner->symbolSizes.size()) {
                size = owner->symbolSizes[symbol];
                offset = static_cast<std::int32_t>(owner->symbolOffsets[symbol]);
                if (var < 0 && !global) {
                    offset = -offset;
                }
            }
            if (size > 1) {
                push(size);
            }
            push(globalWord); // (as it came)
            push(static_cast<std::uint32_t>(offset));
            break;
        }
        case Op::PushInt:
        case Op::PushString:
            push(next());
            break;
        case Op::PushFloat: // (through the FPU)
            push(quieted(next()));
            break;
        // Binary operators: the right operand is on top, so it is popped first.
        case Op::IntAdd: { const auto r = pop(); push(pop() + r); break; }
        case Op::FloatAdd: { const auto r = f32(pop()); push(u32(chopAdd(f32(pop()), r))); break; }
        case Op::IntSub: { const auto r = pop(); push(pop() - r); break; }
        case Op::FloatSub: { const auto r = f32(pop()); push(u32(chopSub(f32(pop()), r))); break; }
        case Op::IntMul: { const auto r = pop(); push(pop() * r); break; }
        case Op::FloatMul: { const auto r = f32(pop()); push(u32(chopMul(f32(pop()), r))); break; }
        case Op::IntDiv:
        case Op::IntMod: {
            const auto r = i32(pop());
            const auto l = i32(pop());
            // (The game faults on a zero divisor or INT_MIN / -1.)
            const bool bad = r == 0 || (l == INT_MIN && r == -1);
            push(static_cast<std::uint32_t>(bad ? 0 : op == static_cast<std::uint32_t>(Op::IntDiv) ? l / r : l % r));
            break;
        }
        case Op::FloatDiv: { const auto r = f32(pop()); push(u32(chopDiv(f32(pop()), r))); break; }
        case Op::IntAnd: { const auto r = pop(); push(pop() & r); break; }
        case Op::IntOr: { const auto r = pop(); push(pop() | r); break; }
        case Op::IntXor: { const auto r = pop(); push(pop() ^ r); break; }
        case Op::Jump:
            pc = next();
            break;
        case Op::JumpIf:
        case Op::JumpIfNot: {
            const std::uint32_t to = next();
            if ((pop() != 0) == (op == static_cast<std::uint32_t>(Op::JumpIf))) {
                pc = to;
            }
            break;
        }
        case Op::StoreInt:
        case Op::StoreFloat:
        // Stores pop value, offset and global flag (the address LoadVarPtr pushed) and push the value.
        case Op::StoreString: {
            const std::uint32_t v = pop();
            const std::int32_t offset = i32(pop());
            const bool global = pop() != 0;
            write(index, global, offset, v);
            push(v);
            break;
        }
        case Op::StoreIntMul:
        case Op::StoreIntDiv:
        case Op::StoreIntAdd:
        case Op::StoreIntSub:
        case Op::StoreIntMod:
        case Op::StoreIntLsh:
        case Op::StoreIntRsh:
        case Op::StoreIntAnd:
        case Op::StoreIntOr:
        case Op::StoreIntXor: {
            // Compound assignment: read the variable, combine, write back and push the result. Integer
            // math wraps (done unsigned), division by zero gives 0 and shift counts use their low 5 bits.
            const std::int32_t v = i32(pop());
            const std::int32_t offset = i32(pop());
            const bool global = pop() != 0;
            const std::int32_t cur = i32(read(index, global, offset));
            std::int32_t r = cur;
            switch (static_cast<Op>(op)) {
            case Op::StoreIntMul: r = static_cast<std::int32_t>(static_cast<std::uint32_t>(cur) * static_cast<std::uint32_t>(v)); break;
            case Op::StoreIntDiv: r = v == 0 || (cur == INT_MIN && v == -1) ? 0 : cur / v; break;
            case Op::StoreIntMod: r = v == 0 || (cur == INT_MIN && v == -1) ? 0 : cur % v; break;
            case Op::StoreIntAdd: r = static_cast<std::int32_t>(static_cast<std::uint32_t>(cur) + static_cast<std::uint32_t>(v)); break;
            case Op::StoreIntSub: r = static_cast<std::int32_t>(static_cast<std::uint32_t>(cur) - static_cast<std::uint32_t>(v)); break;
            case Op::StoreIntLsh: r = static_cast<std::int32_t>(static_cast<std::uint32_t>(cur) << (v & 0x1F)); break;
            case Op::StoreIntRsh: r = cur >> (v & 0x1F); break;
            case Op::StoreIntAnd: r = cur & v; break;
            case Op::StoreIntOr: r = cur | v; break;
            default: r = cur ^ v; break;
            }
            write(index, global, offset, static_cast<std::uint32_t>(r));
            push(static_cast<std::uint32_t>(r));
            break;
        }
        case Op::StoreFloatMul:
        case Op::StoreFloatDiv:
        case Op::StoreFloatAdd:
        case Op::StoreFloatSub: {
            const float v = f32(pop());
            const std::int32_t offset = i32(pop());
            const bool global = pop() != 0;
            const float cur = f32(read(index, global, offset));
            float r = cur;
            switch (static_cast<Op>(op)) {
            case Op::StoreFloatMul: r = chopMul(v, cur); break;
            case Op::StoreFloatDiv: r = chopDiv(cur, v); break;
            case Op::StoreFloatAdd: r = chopAdd(v, cur); break;
            default: r = chopSub(cur, v); break;
            }
            write(index, global, offset, u32(r));
            push(u32(r));
            break;
        }
        case Op::StoreStringAdd: {
            const std::uint32_t v = pop();
            const std::int32_t offset = i32(pop());
            const bool global = pop() != 0;
            // sprintf("%s%s") into 256 bytes, interned (script_getString).
            const std::uint32_t id = addString(joined255(sprintfText(*this, read(index, global, offset)), sprintfText(*this, v)));
            write(index, global, offset, id);
            push(id);
            break;
        }
        case Op::IntEq: { const auto r = pop(); push(pop() == r); break; }
        case Op::FloatEq: { const auto r = f32(pop()); push(fpuEq(f32(pop()), r)); break; }
        case Op::StringEq: { const auto r = pop(); push(compare(pop(), r) == 0); break; }
        case Op::IntLsh: { const auto r = pop(); push(pop() << (r & 0x1F)); break; }
        case Op::IntRsh: { const auto r = pop(); push(static_cast<std::uint32_t>(i32(pop()) >> (r & 0x1F))); break; }
        case Op::IntLt: { const auto r = i32(pop()); push(i32(pop()) < r); break; }
        case Op::FloatLt: { const auto r = f32(pop()); push(fpuLt(f32(pop()), r)); break; }
        case Op::StringLt: { const auto r = pop(); push(compare(pop(), r) < 0); break; }
        case Op::IntGt: { const auto r = i32(pop()); push(i32(pop()) > r); break; }
        case Op::FloatGt: { const auto r = f32(pop()); push(fpuGt(f32(pop()), r)); break; }
        case Op::StringGt: { const auto r = pop(); push(compare(pop(), r) > 0); break; }
        case Op::IntLte: { const auto r = i32(pop()); push(i32(pop()) <= r); break; }
        case Op::FloatLte: { const auto r = f32(pop()); push(fpuLte(f32(pop()), r)); break; }
        case Op::StringLte: { const auto r = pop(); push(compare(pop(), r) <= 0); break; }
        case Op::IntGte: { const auto r = i32(pop()); push(i32(pop()) >= r); break; }
        case Op::FloatGte: { const auto r = f32(pop()); push(fpuGte(f32(pop()), r)); break; }
        case Op::StringGte: { const auto r = pop(); push(compare(pop(), r) >= 0); break; }
        case Op::IntNeq: { const auto r = pop(); push(pop() != r); break; }
        case Op::FloatNeq: { const auto r = f32(pop()); push(!fpuEq(f32(pop()), r)); break; }
        case Op::StringNeq: { const auto r = pop(); push(compare(pop(), r) != 0); break; }
        case Op::BoolAnd: { const auto r = pop(); const auto l = pop(); push(l != 0 && r != 0); break; }
        case Op::BoolOr: { const auto r = pop(); const auto l = pop(); push(l != 0 || r != 0); break; }
        case Op::BoolNot: push(pop() == 0); break;
        case Op::IntNot: push(~pop()); break;
        case Op::IntNeg: push(0u - pop()); break;
        case Op::FloatNeg: push(quieted(pop()) ^ 0x80000000u); break; // (FLD, FCHS, FSTP)
        // Index: pops index, offset, global flag and array size; IndexPtr pushes the element's address,
        // the others its value. An index out of range ends the function after the push.
        case Op::IndexInt:
        case Op::IndexFloat:
        case Op::IndexPtr: {
            const std::int32_t i = i32(pop());
            const std::int32_t offset = i32(pop());
            const std::uint32_t globalWord = pop();
            const bool global = globalWord != 0;
            const std::int32_t size = i32(pop());
            // IndexFloat and IndexPtr let an index equal to the size through (the game's checks
            // there are index <= size; IndexInt's is index < size), so a store to a[size] goes to
            // the word after the array (OB-211; past the function's words ours reads 0 and stores
            // nothing).
            const bool out = i < 0 || (op == static_cast<std::uint32_t>(Op::IndexInt) ? size <= i : size < i);
            if (op == static_cast<std::uint32_t>(Op::IndexPtr)) {
                push(globalWord);
                // Object-variable offsets are negative, so their elements count downwards.
                push(static_cast<std::uint32_t>(offset < 0 ? offset - i : offset + i));
            } else if (!global && offset < 0) {
                // (The game reads object arrays below the variables here, a word -offset-i off:
                // its own bug, kept, OB-210. The host has no such word and gives 0.)
                push(host_->objectVar(offset - i));
            } else {
                push(read(index, global, offset + i));
            }
            if (out) {
                finish();
                return true;
            }
            break;
        }
        // Prefab functions pass their name prefix and offset only to the builtins that use them.
        case Op::CallBuiltin: {
            const std::uint32_t id = next();
            context_ = {};
            if (fn.prefab) {
                context_.prefab = true;
                if (among(id, kPrefixed)) {
                    context_.prefix = fn.prefab->prefix;
                }
                if (among(id, kOffset)) {
                    context_.x = fn.prefab->x;
                    context_.y = fn.prefab->y;
                }
            }
            const int r = id < kBuiltinCount ? host_->builtin(id, *this) : 0;
            context_ = {};
            // 1: the builtin ended the script function (StopScript and the like).
            if (r == 1) {
                finish();
                return true;
            }
            break;
        }
        case Op::CallScript: {
            const std::uint32_t callee = next();
            // A script call that ran out of budget unwinds every caller too.
            if (callee < file_->functions.size() && !run(callee, depth + 1)) {
                return false;
            }
            depth_ = depth; // (back in this function: a handler set off from here nests from it)
            if (stopped_) {
                return false;
            }
            break;
        }
        case Op::StringAdd: {
            const auto r = pop();
            const auto l = pop();
            push(addString(joined255(sprintfText(*this, l), sprintfText(*this, r))));
            break;
        }
        default: // Return0, Return, and anything the interpreter doesn't know
            finish();
            return true;
        }
    }
}

} // namespace nox::script
