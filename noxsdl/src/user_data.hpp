#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace noxsdl {

// Keep writable state separate from installed assets. In particular, macOS App
// Translocation mounts downloaded bundles read-only, even when their original folder is writable.
inline void prepareUserData(const std::filesystem::path& assets, const std::filesystem::path& user,
                            bool importSaves)
{
    namespace fs = std::filesystem;
    fs::create_directories(user);
    if (fs::equivalent(assets, user)) {
        return;
    }
    const auto find = [](const fs::path& dir, std::string name) {
        if (fs::exists(dir / name)) {
            return dir / name;
        }
        for (const auto& entry : fs::directory_iterator(dir)) {
            std::string leaf = entry.path().filename().string();
            std::ranges::transform(leaf, leaf.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (leaf == name) {
                return entry.path();
            }
        }
        return dir / name;
    };
    const auto copyMissing = [](const fs::path& from, const fs::path& to) {
        if (fs::is_regular_file(from) && fs::copy_file(from, to, fs::copy_options::skip_existing)) {
            // Files copied from read-only media must become writable in the user's directory.
            fs::permissions(to, fs::perms::owner_write, fs::perm_options::add);
        }
    };
    const auto mark = [](const fs::path& marker) {
        std::ofstream done(marker);
        done << "Imported without overwriting existing user files.\n";
        done.close();
        if (!done) {
            throw std::runtime_error("Cannot write user-data import marker: " + marker.string());
        }
    };
    const fs::path settingsMarker = user / ".settings-import-complete";
    if (!fs::exists(settingsMarker)) {
        for (const char* name : {"default.cfg", "nox.cfg", "noxcore.cfg", "noxsdl.cfg"}) {
            copyMissing(find(assets, name), user / name);
        }
        mark(settingsMarker);
    }
    // Separate markers let an explicit --save-dir skip importing saves until they are needed.
    // A completed import must not resurrect characters that the player later deletes.
    const fs::path savesMarker = user / ".saves-import-complete";
    if (importSaves && !fs::exists(savesMarker)) {
        const fs::path saves = find(assets, "save");
        if (fs::is_directory(saves)) {
            for (const auto& entry : fs::recursive_directory_iterator(saves)) {
                const fs::path relative = entry.path().lexically_relative(saves);
                const fs::path target = user / "Save" / relative;
                if (entry.is_directory() && !entry.is_symlink()) {
                    fs::create_directories(target);
                } else if (entry.is_regular_file()) {
                    fs::create_directories(target.parent_path());
                    copyMissing(entry.path(), target);
                }
            }
        }
        mark(savesMarker);
    }
    fs::create_directories(user / "Save");
}

} // namespace noxsdl
