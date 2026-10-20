#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Plugin signature files and the checks xScal runs on them before loading a
// plugin DLL. xscal-sign uses the same structures and functions to write and
// verify the files.
namespace sf::plugins::signing {

inline constexpr std::size_t kPublicKeySize = 64;  // P-256 X then Y, big-endian
inline constexpr std::size_t kSignatureSize = 64;  // ECDSA r then s, big-endian
inline constexpr std::size_t kHashSize = 32;       // SHA-256
inline constexpr std::size_t kIdPrefixCapacity = 32;

inline constexpr std::uint16_t kCertificateVersion = 1;
inline constexpr std::uint16_t kSignatureVersion = 1;
inline constexpr char kSignatureMagic[4] = {'X', 'S', 'I', 'G'};

using PublicKey = std::array<std::uint8_t, kPublicKeySize>;
using Hash = std::array<std::uint8_t, kHashSize>;

// Both structures are the exact file layout: little-endian, no padding.
#pragma pack(push, 1)
struct DeveloperCertificate {
    std::uint16_t version;
    std::uint32_t developerId;
    // NUL-terminated. A plugin ID must start with this text; an empty prefix
    // allows any ID. The issuer includes the separator, e.g. "acme_".
    char idPrefix[kIdPrefixCapacity];
    std::uint8_t publicKey[kPublicKeySize];
    std::uint8_t rootKeyIndex;
    // Root key signature over the SHA-256 of every byte before this field.
    std::uint8_t rootSignature[kSignatureSize];
};

struct PluginSignature {
    char magic[4];
    std::uint16_t version;
    DeveloperCertificate certificate;
    std::uint64_t fileSize;
    std::uint8_t fileHash[kHashSize];
    // Developer key signature over the SHA-256 of every byte before this field.
    std::uint8_t signature[kSignatureSize];
};
#pragma pack(pop)

static_assert(sizeof(DeveloperCertificate) == 167);
static_assert(sizeof(PluginSignature) == 277);
inline constexpr std::size_t kCertificateSignedSize =
    offsetof(DeveloperCertificate, rootSignature);
inline constexpr std::size_t kPluginSignatureSignedSize =
    offsetof(PluginSignature, signature);

// Compiled-in trust data. src/plugins/root_keys.cpp and
// src/plugins/revocations.cpp define these for xScal; the plugin loader tests
// link their own definitions with test keys.
[[nodiscard]] std::span<const PublicKey> RootPublicKeys() noexcept;
[[nodiscard]] std::span<const std::uint32_t> RevokedDeveloperIds() noexcept;
[[nodiscard]] std::span<const Hash> RevokedPluginHashes() noexcept;

enum class SignatureCheck {
    Valid,
    MissingSignature,     // no <dll>.sig file
    MalformedSignature,   // wrong size, magic or version
    UnknownRootKey,       // rootKeyIndex outside RootPublicKeys()
    BadCertificate,       // root signature does not verify
    DeveloperRevoked,
    FileMismatch,         // DLL size or SHA-256 differs from the signature file
    BuildRevoked,
    BadSignature,         // developer signature does not verify
    ReadError,            // reading the DLL or the signature file failed
    CryptoError,          // bcrypt.dll could not be loaded or a CNG call failed
};

struct SignatureResult {
    SignatureCheck check = SignatureCheck::ReadError;
    // Win32 error for ReadError, NTSTATUS for CryptoError, otherwise 0.
    std::uint32_t error = 0;
    // Filled once the signature file has been parsed.
    std::uint32_t developerId = 0;
    char idPrefix[kIdPrefixCapacity] = {};
    // Filled once the DLL has been hashed.
    Hash fileHash{};
};

// Checks the DLL open in dllFile against signatureFile. dllFile is read from
// offset 0 and must stay open, without write sharing, until the DLL has been
// loaded, so the loaded file is the one that was hashed. signatureFile may be
// INVALID_HANDLE_VALUE when the .sig file does not exist.
[[nodiscard]] SignatureResult CheckPluginFile(
    HANDLE dllFile,
    HANDLE signatureFile) noexcept;

// Runs the same checks as CheckPluginFile on bytes already in memory. The
// signature's fileSize must equal size and its fileHash the SHA-256 of data.
[[nodiscard]] SignatureResult CheckSignedData(
    const void* data,
    std::size_t size,
    const PluginSignature& signature) noexcept;

// Short reason text for log lines, e.g. "developer revoked".
[[nodiscard]] const char* DescribeCheck(SignatureCheck check) noexcept;

// True when id starts with the certificate's idPrefix.
[[nodiscard]] bool IdMatchesPrefix(
    const char* id,
    const char (&idPrefix)[kIdPrefixCapacity]) noexcept;

// CNG helpers shared with xscal-sign. They load bcrypt.dll on first use.
// Each returns an NTSTATUS; negative values are failures.
[[nodiscard]] long HashBytes(const void* data, std::size_t size, Hash& hash) noexcept;
// Hashes the whole file from offset 0 and reports its size.
[[nodiscard]] long HashFile(HANDLE file, std::uint64_t& size, Hash& hash) noexcept;
// STATUS_INVALID_SIGNATURE (0xC000A000) means the signature does not match.
[[nodiscard]] long VerifyDigest(
    const std::uint8_t (&publicKey)[kPublicKeySize],
    const Hash& digest,
    const std::uint8_t (&signature)[kSignatureSize]) noexcept;

}  // namespace sf::plugins::signing
