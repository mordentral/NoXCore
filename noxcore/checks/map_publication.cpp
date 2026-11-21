#include "fixture.hpp"
#include "../src/map/map_publication.hpp"
#include "noxcore/io/file.hpp"

#include <iostream>

namespace fs = std::filesystem;
using checks::require;

namespace {

int failReplacement = 0;
int replacements = 0;

void replace(const fs::path& staged, const fs::path& final)
{
    if (++replacements == failReplacement) throw std::runtime_error("injected replacement error");
    fs::rename(staged, final);
}

std::uintmax_t failCleanup(const fs::path&, std::error_code& ec)
{
    ec = std::make_error_code(std::errc::permission_denied);
    return static_cast<std::uintmax_t>(-1);
}

} // namespace

int main() try {
    checks::TempDirectory temp;
    const auto folder = temp.path / "Arena";
    const auto map = folder / "Arena.map";
    const auto download = folder / "Arena.nxz";
    const auto sidecar = folder / "Arena.nxe";
    fs::create_directory(folder);
    const std::vector<std::uint8_t> sentinel{42};
    nox::writeFileBytes(folder / "arena.rul", sentinel);
    fs::path retained;
    const auto save = [&](std::uint8_t version, bool side = true, nox::map::detail::RemoveTree cleanup = fs::remove_all) {
        replacements = 0;
        const std::string text(4, static_cast<char>(version));
        const nox::map::EncodedMapFiles files{{version, version}, {version, version, version}};
        return nox::map::detail::publishMap(temp.path, "Arena", files, "check",
            side ? std::optional<std::string_view>(text) : std::nullopt, retained, replace, cleanup);
    };
    const auto checkFiles = [&](int version, int sideVersion) {
        require(nox::readFileBytes(map) == std::vector<std::uint8_t>(2, version), "map bytes");
        require(nox::readFileBytes(download) == std::vector<std::uint8_t>(3, version), "download bytes");
        require(nox::readFileBytes(sidecar) == std::vector<std::uint8_t>(4, sideVersion), "sidecar bytes");
        require(nox::readFileBytes(folder / "arena.rul") == sentinel, "unrelated file changed");
    };
    const auto expectFailure = [&] {
        try {
            save(2);
        } catch (const std::exception&) {
            return;
        }
        throw std::runtime_error("expected save failure");
    };

    failReplacement = 3;
    expectFailure();
    require(!fs::exists(map) && !fs::exists(download) && !fs::exists(sidecar), "partial first save");
    failReplacement = 0;
    require(save(1) == map && retained.empty(), "initial save");
    fs::create_directory(folder / ".nox-save");
    expectFailure();
    checkFiles(1, 1);
    fs::remove(folder / ".nox-save");
    for (failReplacement = 1; failReplacement <= 3; ++failReplacement) {
        expectFailure();
        checkFiles(1, 1);
        require(!fs::exists(folder / ".nox-save"), "rollback left a lock");
    }
    failReplacement = 0;
    require(save(2, false) == map, "sidecar omission");
    checkFiles(2, 1);
    require(save(3, true, failCleanup) == map && fs::is_directory(retained), "committed save needs a cleanup warning");
    checkFiles(3, 3);
    const auto firstCleanup = retained;
    require(save(4, true, failCleanup) == map && fs::is_directory(retained) && retained != firstCleanup, "retry while cleanup is pending");
    require(nox::readFileBytes(firstCleanup / "previous" / "Arena.map") == std::vector<std::uint8_t>(2, 2), "older backup changed");
    require(save(5) == map && retained.empty(), "ordinary retry");
    checkFiles(5, 5);
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
