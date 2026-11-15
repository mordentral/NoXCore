// The game's text formats filled with values (text_fields.hpp).
#include "noxcore/client/text_fields.hpp"

#include <cctype>

namespace nox::client::fields {

namespace {

/// The number the width's leading digits spell, capped at kMaxZeroPadWidth (so a width of any
/// length is read without overflow).
std::size_t zeroPadWidth(std::string_view width)
{
    std::size_t wide = 0;
    for (const char c : width) {
        if (c < '0' || c > '9') {
            break;
        }
        wide = wide * 10 + static_cast<std::size_t>(c - '0');
        if (wide >= kMaxZeroPadWidth) {
            return kMaxZeroPadWidth;
        }
    }
    return wide;
}

} // namespace

std::string fill(std::string_view f, std::initializer_list<std::string_view> values, const Rules& rules)
{
    std::string out;
    auto next = values.begin();
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%' || i + 1 >= f.size()) {
            out += f[i];
            continue;
        }
        std::size_t j = i + 1;
        while (j < f.size() && rules.widthChars.find(f[j]) != std::string_view::npos) {
            ++j;
        }
        const std::string_view width = f.substr(i + 1, j - (i + 1));
        if (rules.longPrefix && j < f.size() && f[j] == 'l' && j + 1 < f.size()) {
            ++j;
        }
        if (j < f.size() && rules.kinds.find(f[j]) != std::string_view::npos) {
            if (next != values.end() || !rules.keepPastValues) {
                std::string v = next != values.end() ? std::string(*next++) : std::string();
                if (rules.zeroPad && !width.empty() && width.front() == '0') {
                    const std::size_t wide = zeroPadWidth(width);
                    if (v.size() < wide) {
                        v.insert(0, wide - v.size(), '0');
                    }
                }
                out += v;
                i = j;
                continue;
            }
        } else if (j < f.size() && f[j] == '%' && rules.percentEscape) {
            out += '%';
            i = j;
            continue;
        }
        out += f[i];
    }
    return out;
}

std::string replaceThroughLetter(std::string f, std::initializer_list<std::string_view> values)
{
    auto it = values.begin();
    for (std::size_t at = f.find('%'); at != std::string::npos && it != values.end(); at = f.find('%', at)) {
        std::size_t end = at + 1;
        while (end < f.size() && !std::isalpha(static_cast<unsigned char>(f[end]))) {
            ++end;
        }
        if (end >= f.size()) {
            break;
        }
        f.replace(at, end - at + 1, *it);
        at += it->size();
        ++it;
    }
    return f;
}

std::string replacePercentPairs(std::string f, std::initializer_list<std::string_view> values)
{
    std::size_t at = 0;
    for (const std::string_view a : values) {
        const auto p = f.find('%', at);
        if (p == std::string::npos || p + 1 >= f.size()) {
            break;
        }
        f.replace(p, 2, a);
        at = p + a.size();
    }
    return f;
}

} // namespace nox::client::fields
