#include "user_data.hpp"
#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
std::string read(const fs::path& p) { std::ifstream in(p); return {std::istreambuf_iterator<char>(in), {}}; }
int main()
{
    std::string pattern = (fs::temp_directory_path() / "nox-user-data-XXXXXX").string();
    const char* temp = mkdtemp(pattern.data());
    if (!temp) return 1;
    const fs::path root = temp, assets = root / "assets", user = root / "user";
    int result = 0;
    try {
        fs::create_directories(assets / "SAVE/Slot1");
        fs::create_directories(user / "Save/Slot1");
        std::ofstream(assets / "DEFAULT.CFG") << "VideoMode = 800 600 16\n";
        std::ofstream(assets / "noxcore.cfg") << "OutputScale = 350\n";
        std::ofstream(assets / "SAVE/new.plr") << "old character";
        std::ofstream(assets / "SAVE/Slot1/Player.plr") << "old progress";
        std::ofstream(user / "Save/Slot1/Player.plr") << "new progress";
        fs::permissions(assets / "noxcore.cfg", fs::perms::owner_read);
        fs::permissions(assets, fs::perms::owner_read | fs::perms::owner_exec);
        noxsdl::prepareUserData(assets, user, true);
        require(read(user / "default.cfg").find("VideoMode") != std::string::npos, "case-insensitive settings import");
        require(read(user / "Save/new.plr") == "old character", "character import");
        require(read(user / "Save/Slot1/Player.plr") == "new progress", "must preserve existing progress");
        std::ofstream(user / "noxcore.cfg") << "OutputScale = 200\n";
        require(read(user / "noxcore.cfg").find("200") != std::string::npos, "imported settings must be writable");
        fs::remove(user / "Save/new.plr");
        noxsdl::prepareUserData(assets, user, true);
        require(!fs::exists(user / "Save/new.plr"), "must not resurrect deleted character");
        require(read(user / "noxcore.cfg").find("200") != std::string::npos, "must preserve newer settings");
        noxsdl::prepareUserData(assets, root / "override", false);
        require(!fs::exists(root / "override/Save/new.plr"), "explicit save override skips migration");
        noxsdl::prepareUserData(assets, root / "override", true);
        require(read(root / "override/Save/new.plr") == "old character", "later default saves still import");
        require(read(assets / "SAVE/new.plr") == "old character", "source saves unchanged");
        require(!fs::exists(assets / ".initial-import-complete"), "no writes into source assets");
        std::ofstream(root / "not-directory") << "blocked";
        bool failed = false;
        try { noxsdl::prepareUserData(assets, root / "not-directory/state", true); }
        catch (const fs::filesystem_error&) { failed = true; }
        require(failed, "invalid destination must report failure");
        std::cout << "PASS: read-only assets, save import, no overwrite/resurrection, writable config, override, failure\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
    fs::permissions(assets, fs::perms::owner_all);
    fs::remove_all(root);
    return result;
}
