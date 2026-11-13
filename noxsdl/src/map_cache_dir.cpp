// The default download folder (map_cache_dir.hpp).
#include "map_cache_dir.hpp"

#include <fstream>
#include <system_error>

namespace noxsdl {

namespace fs = std::filesystem;

bool canWriteIn(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        return false;
    }
    const fs::path probe = dir / ".noxcore-write-test";
    bool written = false;
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        written = static_cast<bool>(out << 'x') && static_cast<bool>(out.flush());
    }
    fs::remove(probe, ec);
    return written;
}

fs::path defaultMapCache(const fs::path& exeDir, const std::function<fs::path()>& prefDir)
{
    constexpr const char* kFolder = "noxsdl_maps";
    if (!exeDir.empty() && canWriteIn(exeDir / kFolder)) {
        return exeDir / kFolder;
    }
    const fs::path pref = prefDir ? prefDir() : fs::path();
    if (!pref.empty() && canWriteIn(pref / kFolder)) {
        return pref / kFolder;
    }
    return !exeDir.empty() ? exeDir / kFolder : !pref.empty() ? pref / kFolder : fs::path(kFolder);
}

} // namespace noxsdl
