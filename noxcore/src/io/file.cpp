// Whole-file reads and writes, and case-insensitive path lookup for game data.
#include "noxcore/io/file.hpp"

#include "noxcore/util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <memory>
#include <system_error>
#ifdef _WIN32
#include <share.h>
#endif

namespace nox {

namespace {

/// Windows' device names (CON, PRN, AUX, NUL, COM0-9, LPT0-9 and COM/LPT with a superscript 1, 2 or
/// 3), any case, alone or before a dot ("nul.map"): opened as a file, each is the device.
bool isDeviceName(std::string_view name) noexcept
{
    const std::string_view stem = name.substr(0, name.find('.'));
    for (const std::string_view device : {"CON", "PRN", "AUX", "NUL"}) {
        if (util::equalsNoCase(stem, device)) {
            return true;
        }
    }
    if (stem.size() < 4 || !(util::equalsNoCase(stem.substr(0, 3), "COM") || util::equalsNoCase(stem.substr(0, 3), "LPT"))) {
        return false;
    }
    const std::string_view n = stem.substr(3);
    return (n.size() == 1 && n[0] >= '0' && n[0] <= '9') || n == "\xC2\xB9" || n == "\xC2\xB2" || n == "\xC2\xB3"; // (UTF-8 ¹ ² ³)
}

} // namespace

bool isPlainFileName(std::string_view name) noexcept
{
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    // (Windows drops a trailing dot or space: "Estate." is Estate's file, " " none.)
    if (name.back() == '.' || name.back() == ' ' || isDeviceName(name)) {
        return false;
    }
    return std::none_of(name.begin(), name.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return c == '/' || c == '\\' || c == ':' || u < 0x20 || u == 0x7F;
    });
}

std::string pathUtf8(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path)
{
    std::filesystem::path opened = path;
#ifdef _WIN32
    std::FILE* file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
#else
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        if (const auto found = resolveNoCase(path); found != path) {
            file = std::fopen(found.c_str(), "rb");
            opened = found;
        }
    }
#endif
    if (!file) {
        throw std::system_error(errno, std::generic_category(), "cannot open " + pathUtf8(path));
    }
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> input(file, std::fclose);
    std::vector<std::uint8_t> out;
    // (The size as it is now, when it can be had: one allocation for a file that doesn't grow.)
    std::error_code sizeError;
    if (const auto known = std::filesystem::file_size(opened, sizeError); !sizeError) {
        out.reserve(static_cast<std::size_t>(known));
    }
    std::array<std::uint8_t, 64 * 1024> block;
    for (;;) {
        const auto size = std::fread(block.data(), 1, block.size(), input.get());
        if (std::ferror(input.get())) {
            throw std::system_error(errno, std::generic_category(), "cannot read " + pathUtf8(path));
        }
        out.insert(out.end(), block.data(), block.data() + size);
        if (size < block.size()) break;
    }
    return out;
}

void writeFileBytes(const std::filesystem::path& path, std::span<const std::uint8_t> data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::system_error(errno, std::generic_category(), "cannot create " + pathUtf8(path));
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
        throw std::system_error(errno, std::generic_category(), "cannot write " + pathUtf8(path));
    }
}

std::filesystem::path findNoCase(const std::filesystem::path& dir, const std::filesystem::path& name)
{
    std::filesystem::path at = dir;
    std::error_code ec;
    // Each path part: the exact name if it exists, else the first directory entry matching it
    // ignoring case, else the exact name (so a missing file still yields a usable path).
    for (const auto& part : name) {
        const std::filesystem::path exact = at / part;
        if (std::filesystem::exists(exact, ec)) {
            at = exact;
            continue;
        }
        // (UTF-8 both: a name the code page can't hold, a mod's in another script, is compared as
        // any, not thrown on.)
        const std::string want = pathUtf8(part);
        std::filesystem::path found;
        for (const auto& e : std::filesystem::directory_iterator(at, ec)) {
            if (util::equalsNoCase(pathUtf8(e.path().filename()), want)) {
                found = e.path();
                break;
            }
        }
        at = found.empty() ? exact : found;
    }
    return at;
}

#ifndef _WIN32
std::filesystem::path resolveNoCase(const std::filesystem::path& path)
{
    std::error_code ec;
    if (path.empty() || std::filesystem::exists(path, ec)) {
        return path;
    }
    // The deepest folder of `path` that exists, then each part after it matched ignoring case.
    std::filesystem::path base = path.parent_path();
    std::vector<std::filesystem::path> rest{path.filename()};
    while (!base.empty() && !std::filesystem::exists(base, ec)) {
        if (base == base.parent_path()) {
            return path;
        }
        rest.push_back(base.filename());
        base = base.parent_path();
    }
    std::filesystem::path at = base;
    for (auto part = rest.rbegin(); part != rest.rend(); ++part) {
        if (part->empty()) {
            continue; // (a trailing separator)
        }
        const std::string want = part->string();
        std::filesystem::path found;
        for (const auto& e : std::filesystem::directory_iterator(at.empty() ? std::filesystem::path(".") : at, ec)) {
            if (util::equalsNoCase(e.path().filename().string(), want)) {
                found = at / e.path().filename(); // (a relative `path` stays relative)
                break;
            }
        }
        if (found.empty()) {
            return path;
        }
        at = found;
    }
    return at;
}
#endif

} // namespace nox
