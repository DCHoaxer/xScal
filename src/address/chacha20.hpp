#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sf::address {

inline constexpr std::size_t kChaCha20KeySize = 32;
inline constexpr std::size_t kChaCha20NonceSize = 12;

namespace chacha20_detail {

[[nodiscard]] inline constexpr std::uint32_t RotateLeft(
    std::uint32_t value,
    unsigned count) noexcept {
    return (value << count) | (value >> (32 - count));
}

inline constexpr void QuarterRound(
    std::uint32_t& a,
    std::uint32_t& b,
    std::uint32_t& c,
    std::uint32_t& d) noexcept {
    a += b; d ^= a; d = RotateLeft(d, 16);
    c += d; b ^= c; b = RotateLeft(b, 12);
    a += b; d ^= a; d = RotateLeft(d, 8);
    c += d; b ^= c; b = RotateLeft(b, 7);
}

[[nodiscard]] inline std::uint32_t ReadLittle32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

}  // namespace chacha20_detail

// XORs the ChaCha20 keystream (RFC 8439: 256-bit key, 96-bit nonce, 32-bit
// block counter) into data. Encrypting and decrypting are the same call.
inline void ChaCha20Xor(
    const std::uint8_t (&key)[kChaCha20KeySize],
    const std::uint8_t (&nonce)[kChaCha20NonceSize],
    std::uint32_t counter,
    std::uint8_t* data,
    std::size_t size) noexcept {
    using namespace chacha20_detail;
    std::uint32_t state[16] = {
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    for (int word = 0; word < 8; ++word) {
        state[4 + word] = ReadLittle32(key + word * 4);
    }
    state[12] = counter;
    for (int word = 0; word < 3; ++word) {
        state[13 + word] = ReadLittle32(nonce + word * 4);
    }

    std::size_t offset = 0;
    while (offset < size) {
        std::uint32_t block[16];
        for (int word = 0; word < 16; ++word) {
            block[word] = state[word];
        }
        for (int round = 0; round < 10; ++round) {
            QuarterRound(block[0], block[4], block[8], block[12]);
            QuarterRound(block[1], block[5], block[9], block[13]);
            QuarterRound(block[2], block[6], block[10], block[14]);
            QuarterRound(block[3], block[7], block[11], block[15]);
            QuarterRound(block[0], block[5], block[10], block[15]);
            QuarterRound(block[1], block[6], block[11], block[12]);
            QuarterRound(block[2], block[7], block[8], block[13]);
            QuarterRound(block[3], block[4], block[9], block[14]);
        }
        const std::size_t count = size - offset < 64 ? size - offset : 64;
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint32_t word = block[index / 4] + state[index / 4];
            data[offset + index] ^=
                static_cast<std::uint8_t>(word >> ((index % 4) * 8));
        }
        offset += count;
        ++state[12];
    }
}

}  // namespace sf::address
