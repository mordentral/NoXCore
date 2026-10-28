// Reading and writing a map's compiled NoxScript (the layout is in script_file.hpp).
#include "noxcore/script/script_file.hpp"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace nox::script {

namespace {

// Little-endian reads over the section's bytes; throws on reading past the end.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    /// `n` items of at least `each` bytes each checked against what's left, before anything is
    /// sized by the count (a u32 that may say billions).
    std::size_t count(std::uint64_t n, std::size_t each)
    {
        if (n > (data_.size() - at_) / each) {
            throw std::runtime_error("script: count " + std::to_string(n) + " past the end at " + std::to_string(at_));
        }
        return static_cast<std::size_t>(n);
    }

    std::uint32_t u32()
    {
        need(4);
        const std::uint32_t v = data_[at_] | (data_[at_ + 1] << 8) | (data_[at_ + 2] << 16) |
                                (static_cast<std::uint32_t>(data_[at_ + 3]) << 24);
        at_ += 4;
        return v;
    }
    std::span<const std::uint8_t> raw(std::size_t n)
    {
        need(n);
        const auto s = data_.subspan(at_, n);
        at_ += n;
        return s;
    }
    std::string bytes(std::size_t n)
    {
        const auto b = raw(n);
        std::string s(reinterpret_cast<const char*>(b.data()), n);
        // (ends it with a NUL: a string stops at its first NUL.)
        if (const auto nul = s.find('\0'); nul != std::string::npos) {
            s.resize(nul);
        }
        return s;
    }
    /// the tag's length in bytes, compared with it.
    void expect(std::string_view tag)
    {
        if (bytes(tag.size()) != tag) {
            throw std::runtime_error("script: expected " + std::string(tag) + " at " + std::to_string(at_ - tag.size()));
        }
    }

private:
    void need(std::size_t n) const
    {
        if (at_ + n > data_.size()) {
            throw std::runtime_error("script: truncated at " + std::to_string(at_));
        }
    }
    std::span<const std::uint8_t> data_;
    std::size_t at_ = 0;
};

/// strtok(…, "%") over a copy: the non-empty pieces.
std::vector<std::string> pieces(const std::string& name)
{
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < name.size()) {
        const auto end = name.find('%', at);
        const auto piece = name.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!piece.empty()) {
            out.push_back(piece);
        }
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    return out;
}

} // namespace

ScriptFile ScriptFile::parse(std::span<const std::uint8_t> data)
{
    ScriptFile f;
    if (data.empty()) {
        return f; // a map without a script (28 of the game's own)
    }
    Reader r(data);
    r.expect("SCRIPT03");
    r.expect("STRG");
    const std::uint32_t strings = r.u32();
    f.strings.reserve(r.count(strings, 4)); // (each string at least its length)
    for (std::uint32_t i = 0; i < strings; ++i) {
        f.strings.push_back(r.bytes(r.u32()));
    }
    r.expect("CODE");
    const std::uint32_t count = r.u32();
    for (std::uint32_t index = 0; index < count; ++index) {
        Function fn;
        r.expect("FUNC");
        fn.name = r.bytes(r.u32());
        if (fn.name.size() > 0x3FF) {
            throw std::runtime_error("script: a function's name is over 1023 bytes");
        }
        // A name with '%' pieces after the first is a prefab function: "Name%Prefix%X%Y".
        if (const auto p = pieces(fn.name); !p.empty() && p.front() != fn.name) {
            Function::Prefab prefab;
            prefab.prefix = "%" + (p.size() > 1 ? p[1] : std::string("(null)"));
            prefab.x = p.size() > 2 ? std::atoi(p[2].c_str()) : 0;
            prefab.y = p.size() > 3 ? std::atoi(p[3].c_str()) : 0;
            fn.prefab = prefab;
        }
        fn.returns = r.u32();
        fn.args = r.u32();
        r.expect("SYMB");
        std::uint32_t symbols = r.u32();
        // Function 0's symbol 0 isn't stored: it is a size-0 placeholder, so object-variable symbol
        // indexes (negated in the code) start at 1.
        if (index == 0) {
            ++symbols;
        }
        fn.symbUnused = r.u32();
        // (Each stored symbol is a u32 size: function 0's placeholder isn't stored.)
        (void)r.count(symbols - (index == 0 ? 1u : 0u), 4);
        fn.symbolSizes.assign(symbols, 0);
        fn.symbolOffsets.assign(symbols, 0);
        // Each symbol's word offset is the sum of the sizes before it.
        std::uint32_t total = 0;
        for (std::uint32_t i = index == 0 ? 1 : 0; i < symbols; ++i) {
            fn.symbolSizes[i] = r.u32();
            fn.symbolOffsets[i] = total;
            // (The VM gives each function its locals' words: sizes past kMaxLocalWords in all
            // would have it allocate gigabytes for one map's script.)
            if (std::uint64_t{total} + fn.symbolSizes[i] > kMaxLocalWords) {
                throw std::runtime_error("script: function " + std::to_string(index) + "'s locals are too big");
            }
            total += fn.symbolSizes[i];
        }
        fn.localWords = total;
        r.expect("DATA");
        const auto code = r.raw(r.u32());
        // A code length that isn't a whole number of words is zero-padded.
        fn.code.assign((code.size() + 3) / 4, 0);
        std::memcpy(fn.code.data(), code.data(), code.size()); // (little-endian words)
        f.functions.push_back(std::move(fn));
    }
    r.expect("DONE");
    f.reindex();
    return f;
}

std::vector<std::uint8_t> ScriptFile::serialize() const
{
    std::vector<std::uint8_t> out;
    if (strings.empty() && functions.empty()) {
        return out;
    }
    const auto tag = [&](std::string_view t) { out.insert(out.end(), t.begin(), t.end()); };
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    const auto text = [&](std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        tag(s);
    };
    tag("SCRIPT03");
    tag("STRG");
    u32(static_cast<std::uint32_t>(strings.size()));
    for (const auto& s : strings) {
        text(s);
    }
    tag("CODE");
    u32(static_cast<std::uint32_t>(functions.size()));
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const Function& fn = functions[index];
        tag("FUNC");
        text(fn.name);
        u32(fn.returns);
        u32(fn.args);
        tag("SYMB");
        // Function 0's placeholder symbol isn't written back.
        const std::size_t first = index == 0 ? 1 : 0;
        u32(static_cast<std::uint32_t>(fn.symbolSizes.size() - std::min(first, fn.symbolSizes.size())));
        u32(fn.symbUnused);
        for (std::size_t i = first; i < fn.symbolSizes.size(); ++i) {
            u32(fn.symbolSizes[i]);
        }
        tag("DATA");
        u32(static_cast<std::uint32_t>(fn.code.size() * 4));
        for (const auto w : fn.code) {
            u32(w);
        }
    }
    tag("DONE");
    return out;
}

void ScriptFile::reindex()
{
    byName.clear();
    byName.reserve(functions.size());
    for (std::size_t i = 0; i < functions.size(); ++i) {
        byName.try_emplace(functions[i].name, static_cast<std::uint32_t>(i)); // (the first of a name kept)
    }
    indexedOf = functions.size();
}

std::optional<std::size_t> ScriptFile::find(std::string_view name) const
{
    if (indexedOf == functions.size()) {
        const auto it = byName.find(name);
        return it == byName.end() ? std::nullopt : std::optional<std::size_t>(it->second);
    }
    for (std::size_t i = 0; i < functions.size(); ++i) {
        if (functions[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace nox::script
