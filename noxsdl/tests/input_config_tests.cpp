#include "input_config_files.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

void require(bool ok, const char* message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

nox::config::Config binding(const char* key)
{
    nox::config::Config config;
    config.bindings.push_back({{nox::config::findKey(key)->code}, {"Jump"}});
    return config;
}

} // namespace

int main()
{
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("nox-input-config-" + std::to_string(suffix));
    // Never remove a directory this test did not create.
    if (!fs::create_directory(root)) {
        return 1;
    }
    int result = 0;
    try {
        const auto assets = root / "assets", user = root / "user";
        fs::create_directories(assets);
        fs::create_directories(user);
        const auto original = binding("A"), saved = binding("B"), defaults = binding("D");
        nox::config::saveConfig(assets, original);
        nox::config::saveConfig(user, saved);
        std::ofstream(assets / "DEFAULT.CFG") << nox::config::writeConfig(defaults);
        std::ofstream(user / "default.cfg") << nox::config::writeConfig(original);

        const auto reset = noxsdl::readInputConfig(assets, user, false);
        require(reset && reset->bindings == saved.bindings, "Reset must restore saved user bindings");
        const auto factory = noxsdl::readInputConfig(assets, user, true);
        require(factory && factory->bindings == defaults.bindings, "Defaults must read game assets, ignoring case");
        fs::remove(assets / "nox.cfg");
        require(noxsdl::readInputConfig(assets, user, false)->bindings == saved.bindings,
                "Reset must work without a configuration in the game folder");
        fs::remove(user / "nox.cfg");
        require(!noxsdl::readInputConfig(assets, user, false), "missing saved bindings must not silently load defaults");
        nox::config::saveConfig(assets, original);
        const auto portable = noxsdl::readInputConfig(assets, assets, false);
        require(portable && portable->bindings == original.bindings, "shared asset/settings directory still works");
        std::cout << "PASS: Reset, Defaults, missing game config, missing saved config, portable settings\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        result = 1;
    }
    fs::remove_all(root);
    return result;
}
