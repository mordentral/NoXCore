#pragma once

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>

namespace checks {

inline void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory()
    {
        std::random_device random;
        do {
            path = std::filesystem::temp_directory_path() / ("nox-check-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(path));
    }
    ~TempDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
};

} // namespace checks
