#pragma once

#include <cstddef>
#include <cstdint>

namespace sf::address::format {

inline constexpr char kMagic[8]{'X', 'S', 'C', 'A', 'L', 'A', 'L', '\0'};
inline constexpr std::uint32_t kSchemaVersion = 2;
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

// File layout, little-endian:
//   Header (headerSize bytes)
//   entryCount records of entrySize bytes each
//   plugins::signing::PluginSignature over everything before it
// fileSize in the signature is headerSize + entryCount * entrySize, and its
// fileHash is the SHA-256 of those bytes. Only certificates with an empty ID
// prefix may sign a database.
// The record bytes (entryCount * entrySize, starting at headerSize) are
// encrypted with ChaCha20 under the build's Address Library key and the
// header nonce, with the block counter starting at 0. The header is not
// encrypted. The signature covers the encrypted bytes. This only keeps the
// RVAs out of plain view in a hex viewer.
#pragma pack(push, 1)
struct Header final {
    char magic[8];
    std::uint32_t schemaVersion;
    // At least sizeof(Header). Bytes past the fields below belong to later
    // revisions of this schema and are skipped.
    std::uint32_t headerSize;
    std::uint32_t targetKind;
    std::uint16_t runtimeMajor;
    std::uint16_t runtimeMinor;
    std::uint16_t runtimeBuild;
    std::uint16_t runtimeRevision;
    // IMAGE_FILE_HEADER::TimeDateStamp and IMAGE_OPTIONAL_HEADER::SizeOfImage
    // of the executable the records were taken from. Two executables can
    // report the same version.
    std::uint32_t imageTimeDateStamp;
    std::uint32_t imageSize;
    // EncodeXScalVersion values. 0 means any xScal release may load the file.
    std::uint32_t minimumXScalVersion;
    // The xScal release the file was published with. Never 0.
    std::uint32_t releaseXScalVersion;
    // Raised for every published fix of a file with the same release.
    std::uint32_t revision;
    std::uint32_t entryCount;
    // At least sizeof(Entry). Bytes past the Entry fields are skipped.
    std::uint32_t entrySize;
    // ChaCha20 nonce for the record bytes. Random for every file written.
    std::uint8_t nonce[12];
    std::uint32_t reserved;
};

struct Entry final {
    std::uint64_t id;
    std::uint64_t value;
    std::uint32_t kind;
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 72);
static_assert(sizeof(Entry) == 24);

// True when release and revision describe a newer file than the other pair.
[[nodiscard]] inline constexpr bool IsFileNewer(
    XScalVersion release,
    std::uint32_t revision,
    XScalVersion otherRelease,
    std::uint32_t otherRevision) noexcept {
    if (IsVersionNewer(release, otherRelease)) {
        return true;
    }
    if (IsVersionNewer(otherRelease, release)) {
        return false;
    }
    return revision > otherRevision;
}

}  // namespace sf::address::format
