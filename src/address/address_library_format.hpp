#pragma once

#include <cstddef>
#include <cstdint>

namespace sf::address::format {

inline constexpr char kMagic[8]{'X', 'S', 'C', 'A', 'L', 'A', 'L', '\0'};
inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::uint32_t kAddressEntry = 1;
inline constexpr std::uint32_t kConstantEntry = 2;

struct XScalVersion final {
    std::uint8_t major{};
    std::uint8_t minor{};
    std::uint8_t patch{};
};

[[nodiscard]] inline constexpr std::uint32_t EncodeXScalVersion(
    XScalVersion version) noexcept {
    return static_cast<std::uint32_t>(version.major) |
        (static_cast<std::uint32_t>(version.minor) << 8) |
        (static_cast<std::uint32_t>(version.patch) << 16);
}

[[nodiscard]] inline constexpr XScalVersion DecodeXScalVersion(
    std::uint32_t encoded) noexcept {
    return {
        static_cast<std::uint8_t>(encoded & 0xFFu),
        static_cast<std::uint8_t>((encoded >> 8) & 0xFFu),
        static_cast<std::uint8_t>((encoded >> 16) & 0xFFu)};
}

[[nodiscard]] inline constexpr bool IsVersionNewer(
    XScalVersion required,
    XScalVersion available) noexcept {
    if (required.major != available.major) {
        return required.major > available.major;
    }
    if (required.minor != available.minor) {
        return required.minor > available.minor;
    }
    return required.patch > available.patch;
}

#pragma pack(push, 1)
struct Header final {
    char magic[8];
    std::uint32_t schemaVersion;
    std::uint32_t targetKind;
    std::uint16_t runtimeMajor;
    std::uint16_t runtimeMinor;
    std::uint16_t runtimeBuild;
    std::uint16_t runtimeRevision;
    std::uint32_t entryCount;
    std::uint32_t reserved;
    std::uint64_t entriesHash;
};

struct Entry final {
    std::uint64_t id;
    std::uint64_t value;
    std::uint32_t kind;
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 40);
static_assert(sizeof(Entry) == 24);

[[nodiscard]] inline std::uint64_t HashEntries(
    const Entry* entries,
    std::size_t count) noexcept {
    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offsetBasis;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(entries);
    const std::size_t byteCount = count * sizeof(Entry);
    for (std::size_t index = 0; index < byteCount; ++index) {
        hash ^= bytes[index];
        hash *= prime;
    }
    return hash;
}

}  // namespace sf::address::format
