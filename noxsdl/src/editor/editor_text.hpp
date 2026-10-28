// Text helpers the map editor's panels and test steps share (no SDL or ImGui here: unit tested):
// a path as UTF-8 text, a step's numbers, bits in hex.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl::editor {

/// A path as UTF-8 text (the status bar's, a dialog's).
inline std::string pathText(const std::filesystem::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

/// The numbers in `s` after `prefix` (any separators between them), or nothing when `s` doesn't
/// start with `prefix` or has fewer than `count` numbers ("place:120,340" after "place:").
inline std::optional<std::vector<float>> numbersAfter(std::string_view s, std::string_view prefix, std::size_t count)
{
    if (!s.starts_with(prefix)) return std::nullopt;
    const std::string rest(s.substr(prefix.size()));
    std::vector<float> out;
    for (const char* p = rest.c_str(); *p;) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        out.push_back(v);
        p = end;
    }
    if (out.size() < count) return std::nullopt;
    return out;
}

/// Bits as the panels show them ("0x1F").
inline std::string hexText(std::uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

} // namespace noxsdl::editor
