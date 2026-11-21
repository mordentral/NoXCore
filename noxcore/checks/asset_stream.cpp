#include "fixture.hpp"
#include "noxcore/io/file.hpp"

#include <fstream>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif

int main() try {
    checks::TempDirectory temp;
    const auto file = temp.path / "MixedCase.bin";
    for (const std::size_t n : {0, 1, 65535, 65536, 65537, 131072}) {
        std::vector<std::uint8_t> expected(n);
        for (std::size_t i = 0; i < n; ++i) expected[i] = static_cast<std::uint8_t>(i % 251);
        nox::writeFileBytes(file, expected);
        checks::require(nox::readFileBytes(temp.path / "mixedcase.BIN") == expected, "block boundary or case lookup");
    }
    bool rejected = false;
    try {
        nox::readFileBytes(temp.path);
    } catch (const std::system_error&) {
        rejected = true;
    }
    checks::require(rejected, "reading a directory must fail");
#ifndef _WIN32
    const auto fifo = temp.path / "pipe";
    checks::require(mkfifo(fifo.c_str(), 0600) == 0, "could not create fixture FIFO");
    std::jthread writer([&] { std::ofstream(fifo, std::ios::binary) << "stream"; });
    const auto bytes = nox::readFileBytes(fifo);
    checks::require(std::string(bytes.begin(), bytes.end()) == "stream", "unsized stream truncated");
#endif
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
