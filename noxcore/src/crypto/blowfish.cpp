// Standard Blowfish (see blowfish.hpp); Nox encrypts its data files with it in ECB mode.
#include "noxcore/crypto/blowfish.hpp"

#include "blowfish_tables.hpp"

#include <stdexcept>
#include <utility>

namespace nox::crypto {

namespace {

// Blowfish works on big-endian 32-bit halves of each 8-byte block.
std::uint32_t loadBE(const std::uint8_t* b) noexcept
{
    return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) |
           std::uint32_t{b[3]};
}

void storeBE(std::uint8_t* b, std::uint32_t v) noexcept
{
    b[0] = static_cast<std::uint8_t>(v >> 24);
    b[1] = static_cast<std::uint8_t>(v >> 16);
    b[2] = static_cast<std::uint8_t>(v >> 8);
    b[3] = static_cast<std::uint8_t>(v);
}

void requireWholeBlocks(std::span<std::uint8_t> data)
{
    if (data.size() % Blowfish::kBlockSize != 0) {
        throw std::invalid_argument("Blowfish: data size is not a multiple of the block size");
    }
}

} // namespace

Blowfish::Blowfish(std::span<const std::uint8_t> key)
    : p_(detail::kInitP)
    , s_(detail::kInitS)
{
    if (key.empty() || key.size() > kMaxKeySize) {
        throw std::invalid_argument("Blowfish: key must be 1 to 56 bytes");
    }

    // XOR the key, cycled as big-endian words, into the P-array.
    std::size_t k = 0;
    for (auto& word : p_) {
        std::uint32_t data = 0;
        for (int i = 0; i < 4; ++i) {
            data = (data << 8) | key[k];
            k = (k + 1) % key.size();
        }
        word ^= data;
    }

    // Replace P and then S with successive encryptions of an all-zero block.
    std::uint32_t l = 0;
    std::uint32_t r = 0;
    for (std::size_t i = 0; i < p_.size(); i += 2) {
        encryptWords(l, r);
        p_[i] = l;
        p_[i + 1] = r;
    }
    for (auto& box : s_) {
        for (std::size_t i = 0; i < box.size(); i += 2) {
            encryptWords(l, r);
            box[i] = l;
            box[i + 1] = r;
        }
    }
}

// The round function: four S-box look-ups, one per byte of x.
std::uint32_t Blowfish::f(std::uint32_t x) const noexcept
{
    return ((s_[0][x >> 24] + s_[1][(x >> 16) & 0xFF]) ^ s_[2][(x >> 8) & 0xFF]) + s_[3][x & 0xFF];
}

// 16 Feistel rounds, then undo the last swap and whiten with P[16], P[17].
void Blowfish::encryptWords(std::uint32_t& l, std::uint32_t& r) const noexcept
{
    for (std::size_t i = 0; i < 16; ++i) {
        l ^= p_[i];
        r ^= f(l);
        std::swap(l, r);
    }
    std::swap(l, r);
    r ^= p_[16];
    l ^= p_[17];
}

// The same rounds with the P-array used in reverse.
void Blowfish::decryptWords(std::uint32_t& l, std::uint32_t& r) const noexcept
{
    for (std::size_t i = 17; i > 1; --i) {
        l ^= p_[i];
        r ^= f(l);
        std::swap(l, r);
    }
    std::swap(l, r);
    r ^= p_[1];
    l ^= p_[0];
}

void Blowfish::encryptBlock(std::span<std::uint8_t, kBlockSize> block) const noexcept
{
    std::uint32_t l = loadBE(block.data());
    std::uint32_t r = loadBE(block.data() + 4);
    encryptWords(l, r);
    storeBE(block.data(), l);
    storeBE(block.data() + 4, r);
}

void Blowfish::decryptBlock(std::span<std::uint8_t, kBlockSize> block) const noexcept
{
    std::uint32_t l = loadBE(block.data());
    std::uint32_t r = loadBE(block.data() + 4);
    decryptWords(l, r);
    storeBE(block.data(), l);
    storeBE(block.data() + 4, r);
}

void Blowfish::encrypt(std::span<std::uint8_t> data) const
{
    requireWholeBlocks(data);
    for (std::size_t i = 0; i < data.size(); i += kBlockSize) {
        encryptBlock(data.subspan(i).first<kBlockSize>());
    }
}

void Blowfish::decrypt(std::span<std::uint8_t> data) const
{
    requireWholeBlocks(data);
    for (std::size_t i = 0; i < data.size(); i += kBlockSize) {
        decryptBlock(data.subspan(i).first<kBlockSize>());
    }
}

} // namespace nox::crypto
