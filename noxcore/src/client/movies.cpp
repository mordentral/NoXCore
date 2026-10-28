// Which movie plays when, how input skips it, and where its file (or its HD replacement) is found.
#include "noxcore/client/movies.hpp"

#include "noxcore/util/text.hpp"

#include <array>
#include <cctype>
#include <system_error>

namespace nox::client::movies {

namespace {

using util::lowered;

/// `dir`'s entry named `name`, case aside.
std::optional<std::filesystem::path> findNoCase(const std::filesystem::path& dir, std::string_view name)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return std::nullopt;
    }
    const std::string want = lowered(name);
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (lowered(e.path().filename().string()) == want) {
            return e.path();
        }
    }
    return std::nullopt;
}

} // namespace

bool skips(Input input, Skip how)
{
    // The stop check's n: 1 for Escape, 0 for other keys and the left button.
    const int n = input == Input::Escape ? 1 : 0;
    return how == Skip::AnyInput || n == 1;
}

std::optional<Input> keyInput(bool escape, bool systemKey)
{
    // The window procedure's WM_KEYDOWN (0x100): wParam 0x1B is the stop check with 1, any other with 0.
    if (systemKey) {
        return std::nullopt;
    }
    return escape ? Input::Escape : Input::OtherKey;
}

bool introAtStartup(int trackData, bool gameFlag2000000)
{
    return (trackData & 0x80) == 0 && !gameFlag2000000;
}

int trackDataAfterIntro(int trackData)
{
    return trackDataByte(trackData | 0x80);
}

int trackDataByte(int value)
{
    return value & 0xFF;
}

std::string endingMovie(int kind)
{
    static constexpr std::array<std::string_view, 3> kNames{"Warrior", "Wizard", "Conjurer"};
    if (kind < 0 || kind >= static_cast<int>(kNames.size())) {
        return {};
    }
    return std::string(kNames[static_cast<std::size_t>(kind)]) + ".vqa";
}

std::optional<std::filesystem::path> findMovie(const std::filesystem::path& gameDir, std::string_view name)
{
    // "movies\" + name, stat'ed.
    const auto dir = findNoCase(gameDir, "movies");
    if (!dir) {
        return std::nullopt;
    }
    return findNoCase(*dir, name);
}

std::optional<std::filesystem::path> findHdMovie(const std::filesystem::path& hdDir, std::string_view name)
{
    const std::string stem = std::filesystem::path(std::string(name)).stem().string();
    return findNoCase(hdDir, stem + ".mkv");
}

} // namespace nox::client::movies
