#include "plugins/plugin_signature.hpp"

#include <bcrypt.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace sf::plugins::signing {
namespace {

constexpr long kStatusInvalidSignature = static_cast<long>(0xC000A000L);
constexpr long kStatusDllNotFound = static_cast<long>(0xC0000135L);
constexpr long kStatusProcedureNotFound = static_cast<long>(0xC000007AL);
constexpr long kStatusInvalidParameter = static_cast<long>(0xC000000DL);

// Win32 errors from reading a file are returned as NTSTATUS_FROM_WIN32 so
// CheckPluginFile can tell them apart from CNG failures.
[[nodiscard]] long StatusFromWin32(DWORD error) noexcept {
    return static_cast<long>(0xC0070000UL | (error & 0xFFFFUL));
}
[[nodiscard]] bool IsWin32Status(long status) noexcept {
    return (static_cast<unsigned long>(status) & 0xFFFF0000UL) == 0xC0070000UL;
}

// bcrypt.dll is loaded on first use instead of imported, so xScal installs
// with no plugins do not load it. BCryptHash is Windows 10 only, so hashing
// goes through BCryptCreateHash.
struct Cng {
    decltype(&::BCryptOpenAlgorithmProvider) openAlgorithmProvider;
    decltype(&::BCryptCreateHash) createHash;
    decltype(&::BCryptHashData) hashData;
    decltype(&::BCryptFinishHash) finishHash;
    decltype(&::BCryptDestroyHash) destroyHash;
    decltype(&::BCryptImportKeyPair) importKeyPair;
    decltype(&::BCryptDestroyKey) destroyKey;
    decltype(&::BCryptVerifySignature) verifySignature;
    BCRYPT_ALG_HANDLE sha256;
    BCRYPT_ALG_HANDLE ecdsaP256;
    long status;
};

INIT_ONCE cngOnce = INIT_ONCE_STATIC_INIT;
Cng cng{};

template <typename Function>
[[nodiscard]] bool LoadFunction(HMODULE library, const char* name, Function& function) noexcept {
    function = reinterpret_cast<Function>(::GetProcAddress(library, name));
    return function != nullptr;
}

BOOL CALLBACK LoadCng(PINIT_ONCE, PVOID, PVOID*) noexcept {
    const HMODULE library = ::LoadLibraryExW(
        L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) {
        cng.status = kStatusDllNotFound;
        return TRUE;
    }
    if (!LoadFunction(library, "BCryptOpenAlgorithmProvider", cng.openAlgorithmProvider) ||
        !LoadFunction(library, "BCryptCreateHash", cng.createHash) ||
        !LoadFunction(library, "BCryptHashData", cng.hashData) ||
        !LoadFunction(library, "BCryptFinishHash", cng.finishHash) ||
        !LoadFunction(library, "BCryptDestroyHash", cng.destroyHash) ||
        !LoadFunction(library, "BCryptImportKeyPair", cng.importKeyPair) ||
        !LoadFunction(library, "BCryptDestroyKey", cng.destroyKey) ||
        !LoadFunction(library, "BCryptVerifySignature", cng.verifySignature)) {
        cng.status = kStatusProcedureNotFound;
        return TRUE;
    }
    // The algorithm handles stay open for the life of the process.
    cng.status = cng.openAlgorithmProvider(
        &cng.sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (cng.status >= 0)
        cng.status = cng.openAlgorithmProvider(
            &cng.ecdsaP256, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
    return TRUE;
}

[[nodiscard]] long LoadCngOnce() noexcept {
    (void)::InitOnceExecuteOnce(&cngOnce, &LoadCng, nullptr, nullptr);
    return cng.status;
}

[[nodiscard]] bool ReadExact(HANDLE file, void* buffer, DWORD size) noexcept {
    DWORD read = 0;
    return ::ReadFile(file, buffer, size, &read, nullptr) && read == size;
}

[[nodiscard]] bool IsTerminated(const char (&text)[kIdPrefixCapacity]) noexcept {
    return std::memchr(text, '\0', sizeof(text)) != nullptr;
}

// Runs every check on a parsed signature. hashContent hashes the signed bytes
// and returns an NTSTATUS, so files and memory share one implementation.
template <typename HashContent>
[[nodiscard]] SignatureResult CheckSignature(
    const PluginSignature& file,
    HashContent&& hashContent) noexcept {
    SignatureResult result{};
    const DeveloperCertificate& certificate = file.certificate;
    if (std::memcmp(file.magic, kSignatureMagic, sizeof(file.magic)) != 0 ||
        file.version != kSignatureVersion ||
        certificate.version != kCertificateVersion ||
        !IsTerminated(certificate.idPrefix)) {
        result.check = SignatureCheck::MalformedSignature;
        return result;
    }
    result.developerId = certificate.developerId;
    std::memcpy(result.idPrefix, certificate.idPrefix, sizeof(result.idPrefix));

    const auto failWithStatus = [&result](long status, SignatureCheck invalid) {
        if (status == kStatusInvalidSignature) {
            result.check = invalid;
        } else if (IsWin32Status(status)) {
            result.check = SignatureCheck::ReadError;
            result.error = static_cast<std::uint32_t>(status) & 0xFFFFU;
        } else {
            result.check = SignatureCheck::CryptoError;
            result.error = static_cast<std::uint32_t>(status);
        }
        return result;
    };

    const auto rootKeys = RootPublicKeys();
    if (certificate.rootKeyIndex >= rootKeys.size()) {
        result.check = SignatureCheck::UnknownRootKey;
        return result;
    }
    Hash digest{};
    long status = HashBytes(&certificate, kCertificateSignedSize, digest);
    if (status >= 0) {
        std::uint8_t rootKey[kPublicKeySize];
        std::memcpy(rootKey, rootKeys[certificate.rootKeyIndex].data(), sizeof(rootKey));
        status = VerifyDigest(rootKey, digest, certificate.rootSignature);
    }
    if (status < 0)
        return failWithStatus(status, SignatureCheck::BadCertificate);

    const auto revokedDevelopers = RevokedDeveloperIds();
    if (std::find(revokedDevelopers.begin(), revokedDevelopers.end(),
            certificate.developerId) != revokedDevelopers.end()) {
        result.check = SignatureCheck::DeveloperRevoked;
        return result;
    }

    std::uint64_t contentSize = 0;
    status = hashContent(contentSize, result.fileHash);
    if (status < 0)
        return failWithStatus(status, SignatureCheck::CryptoError);
    if (contentSize != file.fileSize ||
        std::memcmp(result.fileHash.data(), file.fileHash, kHashSize) != 0) {
        result.check = SignatureCheck::FileMismatch;
        return result;
    }
    const auto revokedBuilds = RevokedPluginHashes();
    if (std::find(revokedBuilds.begin(), revokedBuilds.end(),
            result.fileHash) != revokedBuilds.end()) {
        result.check = SignatureCheck::BuildRevoked;
        return result;
    }

    status = HashBytes(&file, kPluginSignatureSignedSize, digest);
    if (status >= 0)
        status = VerifyDigest(certificate.publicKey, digest, file.signature);
    if (status < 0)
        return failWithStatus(status, SignatureCheck::BadSignature);

    result.check = SignatureCheck::Valid;
    return result;
}


}  // namespace

long HashBytes(const void* data, std::size_t size, Hash& hash) noexcept {
    long status = LoadCngOnce();
    if (status < 0)
        return status;
    if (size > ULONG_MAX)
        return kStatusInvalidParameter;
    BCRYPT_HASH_HANDLE handle{};
    status = cng.createHash(cng.sha256, &handle, nullptr, 0, nullptr, 0, 0);
    if (status < 0)
        return status;
    status = cng.hashData(
        handle, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0);
    if (status >= 0)
        status = cng.finishHash(handle, hash.data(), static_cast<ULONG>(hash.size()), 0);
    (void)cng.destroyHash(handle);
    return status;
}

long HashFile(HANDLE file, std::uint64_t& size, Hash& hash) noexcept {
    long status = LoadCngOnce();
    if (status < 0)
        return status;
    LARGE_INTEGER start{};
    if (!::SetFilePointerEx(file, start, nullptr, FILE_BEGIN))
        return StatusFromWin32(::GetLastError());
    BCRYPT_HASH_HANDLE handle{};
    status = cng.createHash(cng.sha256, &handle, nullptr, 0, nullptr, 0, 0);
    if (status < 0)
        return status;
    size = 0;
    std::uint8_t buffer[16384];
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            status = StatusFromWin32(::GetLastError());
            break;
        }
        if (read == 0) {
            status = cng.finishHash(handle, hash.data(), static_cast<ULONG>(hash.size()), 0);
            break;
        }
        size += read;
        status = cng.hashData(handle, buffer, read, 0);
        if (status < 0)
            break;
    }
    (void)cng.destroyHash(handle);
    return status;
}

long VerifyDigest(
    const std::uint8_t (&publicKey)[kPublicKeySize],
    const Hash& digest,
    const std::uint8_t (&signature)[kSignatureSize]) noexcept {
    long status = LoadCngOnce();
    if (status < 0)
        return status;
    struct {
        BCRYPT_ECCKEY_BLOB header;
        std::uint8_t key[kPublicKeySize];
    } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = kPublicKeySize / 2;
    std::memcpy(blob.key, publicKey, sizeof(blob.key));
    static_assert(sizeof(blob) == sizeof(BCRYPT_ECCKEY_BLOB) + kPublicKeySize);
    BCRYPT_KEY_HANDLE key{};
    status = cng.importKeyPair(
        cng.ecdsaP256, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
        reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0);
    if (status < 0)
        return status;
    status = cng.verifySignature(
        key, nullptr,
        const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
        const_cast<PUCHAR>(signature), kSignatureSize, 0);
    (void)cng.destroyKey(key);
    return status;
}

SignatureResult CheckPluginFile(HANDLE dllFile, HANDLE signatureFile) noexcept {
    SignatureResult result{};
    if (signatureFile == INVALID_HANDLE_VALUE) {
        result.check = SignatureCheck::MissingSignature;
        return result;
    }

    LARGE_INTEGER signatureSize{};
    if (!::GetFileSizeEx(signatureFile, &signatureSize)) {
        result.error = ::GetLastError();
        return result;
    }
    if (signatureSize.QuadPart != static_cast<LONGLONG>(sizeof(PluginSignature))) {
        result.check = SignatureCheck::MalformedSignature;
        return result;
    }
    PluginSignature file{};
    if (!ReadExact(signatureFile, &file, sizeof(file))) {
        result.error = ::GetLastError();
        return result;
    }
    return CheckSignature(file, [dllFile](std::uint64_t& size, Hash& hash) noexcept {
        return HashFile(dllFile, size, hash);
    });
}

SignatureResult CheckSignedData(
    const void* data,
    std::size_t size,
    const PluginSignature& signature) noexcept {
    return CheckSignature(signature, [data, size](std::uint64_t& hashedSize, Hash& hash) noexcept {
        hashedSize = size;
        return HashBytes(data, size, hash);
    });
}

const char* DescribeCheck(SignatureCheck check) noexcept {
    switch (check) {
    case SignatureCheck::Valid: return "valid signature";
    case SignatureCheck::MissingSignature: return "missing signature file";
    case SignatureCheck::MalformedSignature: return "malformed signature file";
    case SignatureCheck::UnknownRootKey: return "certificate names an unknown root key";
    case SignatureCheck::BadCertificate: return "certificate not issued by xScal";
    case SignatureCheck::DeveloperRevoked: return "developer revoked";
    case SignatureCheck::FileMismatch: return "file does not match signature";
    case SignatureCheck::BuildRevoked: return "build revoked";
    case SignatureCheck::BadSignature: return "signature does not verify";
    case SignatureCheck::ReadError: return "could not read plugin or signature file";
    case SignatureCheck::CryptoError: return "CNG signature check failed";
    }
    return "unknown signature check result";
}

bool IdMatchesPrefix(const char* id, const char (&idPrefix)[kIdPrefixCapacity]) noexcept {
    const std::size_t prefixLength = ::strnlen(idPrefix, kIdPrefixCapacity);
    return std::strncmp(id, idPrefix, prefixLength) == 0;
}

}  // namespace sf::plugins::signing
