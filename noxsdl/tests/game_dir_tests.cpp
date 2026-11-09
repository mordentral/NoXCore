#include "noxcore/io/game_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

void require(bool ok, const char* message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

void gameData(const fs::path& dir)
{
    fs::create_directories(dir);
    for (const char* name : {"GAMEDATA.BIN", "modifier.bin", "monster.bin", "thing.bin"}) {
        std::ofstream(dir / name).put('\0');
    }
}

int main()
{
    const auto oldCwd = fs::current_path();
    const char* env = std::getenv("NOX_DATA");
    const std::optional<std::string> oldData = env ? std::optional<std::string>(env) : std::nullopt;
    std::string pattern = (fs::temp_directory_path() / "nox-mac-data-XXXXXX").string();
    const char* made = mkdtemp(pattern.data());
    if (!made) {
        return 1;
    }
    const fs::path root = made;
    int result = 0;
    try {
        unsetenv("NOX_DATA");
        fs::create_directories(root / "unrelated");
        fs::current_path(root / "unrelated"); // Finder does not start in the app's parent folder.
        const auto beside = root / "Nox";
        const auto contents = beside / "NoX Core.app/Contents";
        const auto resources = contents / "Resources";
        fs::create_directories(resources);
        gameData(beside); // No Windows executable is needed for detection.
        const auto expect = [&](const fs::path& base, const fs::path& wanted) {
            const auto found = nox::io::locateGameDir(base, {});
            require(found && fs::equivalent(found->dir, wanted), "wrong game-data location");
        };
        expect(resources, beside);
        expect(resources / "", beside); // SDL's trailing separator.
        expect(contents / "MacOS/", beside);
        gameData(resources / "GameData");
        expect(resources / "", resources / "GameData");
        gameData(resources);
        expect(resources, resources); // Legacy files directly in Resources still work.
        gameData(root / "override");
        setenv("NOX_DATA", (root / "override").c_str(), 1);
        expect(resources, root / "override");
        unsetenv("NOX_DATA");
        for (const auto& candidate : nox::io::gameDirSearch(root / "not-a-bundle/Contents/Resources")) {
            require(candidate.reason != "next to the Mac app", "ordinary folders must not be treated as app bundles");
        }
        std::cout << "PASS: Finder-style launch, trailing slash, bundled/adjacent data, case-insensitive markers, NOX_DATA priority\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    fs::current_path(oldCwd);
    if (oldData) {
        setenv("NOX_DATA", oldData->c_str(), 1);
    } else {
        unsetenv("NOX_DATA");
    }
    fs::remove_all(root); // Only the temporary test tree made above.
    return result;
}
