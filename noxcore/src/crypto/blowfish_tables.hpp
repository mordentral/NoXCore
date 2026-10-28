// The cipher's fixed initial P-array and S-boxes (defined in blowfish_tables.cpp).
#pragma once

#include <array>
#include <cstdint>

namespace nox::crypto::detail {

extern const std::array<std::uint32_t, 18> kInitP;
extern const std::array<std::array<std::uint32_t, 256>, 4> kInitS;

} // namespace nox::crypto::detail
