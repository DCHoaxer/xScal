#include "address/address_library.hpp"

#include "address/address_library_format.hpp"
#include "address/address_library_key.hpp"
#include "address/chacha20.hpp"
#include "config/config.h"
#include "platform/win32_file.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

namespace sf::address {
namespace {

constexpr std::size_t kMaximumDatabaseSize = 1024 * 1024;
constexpr std::size_t kMaximumEntries = 4096;
constexpr int kNetworkTimeoutMilliseconds = 5'000;
constexpr std::size_t kMarkerSize = 64;
constexpr std::size_t kMaximumHeaderSize = 4096;
constexpr std::size_t kMaximumEntrySize = 256;
constexpr std::uint32_t kMaximumVersion = 0xFFFFFF;
// 24 hours in FILETIME units of 100 ns.
constexpr std::uint64_t kUpdateCheckInterval = 24ull * 3600 * 10'000'000;

// Reads a marker file into text. False when the file is missing or unreadable.
[[nodiscard]] bool ReadMarker(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const wchar_t* extension,
    std::string& text) noexcept;

class UniqueInternetHandle final {
public:
    explicit UniqueInternetHandle(HINTERNET handle = nullptr) noexcept
        : handle_(handle) {}

    ~UniqueInternetHandle() {
        if (handle_ != nullptr) {
            (void)::WinHttpCloseHandle(handle_);
        }
    }

    UniqueInternetHandle(const UniqueInternetHandle&) = delete;
    UniqueInternetHandle& operator=(const UniqueInternetHandle&) = delete;

    [[nodiscard]] HINTERNET Get() const noexcept {
        return handle_;
    }

private:
    HINTERNET handle_{};
};

[[nodiscard]] std::uint32_t TargetValue(TargetKind target) noexcept {
    return target == TargetKind::Steam
        ? XSCAL_TARGET_STEAM
        : XSCAL_TARGET_GAMEPASS;
}

[[nodiscard]] bool FormatFileName(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const wchar_t* extension,
    std::array<wchar_t, 96>& fileName) noexcept {
    const int length = std::swprintf(
        fileName.data(),
        fileName.size(),
        L"version-%hu-%hu-%hu-%hu-%hs.%ls",
        runtime.major,
        runtime.minor,
        runtime.build,
        runtime.revision,
        TargetKindName(target),
        extension);
    return length > 0 &&
        static_cast<std::size_t>(length) < fileName.size();
}

[[nodiscard]] bool ResolveFilePath(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const wchar_t* extension,
    platform::ExecutableRelativePath& path) noexcept {
    std::array<wchar_t, 96> fileName{};
    std::array<wchar_t, 160> relativePath{};
    if (!FormatFileName(target, runtime, extension, fileName)) {
        return false;
    }
    const int length = std::swprintf(
        relativePath.data(),
        relativePath.size(),
        L"Data\\XSCAL\\AddressLibrary\\%ls",
        fileName.data());
    return length > 0 &&
        static_cast<std::size_t>(length) < relativePath.size() &&
        platform::ResolveExecutableRelativePath(relativePath.data(), path);
}

bool ReadMarker(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const wchar_t* extension,
    std::string& text) noexcept {
    platform::ExecutableRelativePath path;
    return ResolveFilePath(target, runtime, extension, path) &&
        platform::QueryFilePresence(path) == platform::FilePresence::Present &&
        platform::ReadBoundedFile(path, kMarkerSize, text);
}

bool WriteMarker(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const wchar_t* extension,
    const char* text) noexcept {
    platform::ExecutableRelativePath path;
    return ResolveFilePath(target, runtime, extension, path) &&
        platform::WriteFileAtomically(path, text);
}

}  // namespace

bool ReadExecutableIdentity(
    std::uintptr_t moduleBase,
    ExecutableIdentity& identity) noexcept {
    if (moduleBase == 0) {
        return false;
    }
    const auto* const dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        moduleBase + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    identity.timeDateStamp = nt->FileHeader.TimeDateStamp;
    identity.imageSize = nt->OptionalHeader.SizeOfImage;
    return true;
}

bool DownloadAddressLibrary(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::string& contents) noexcept {
    contents.clear();
    std::array<wchar_t, 96> fileName{};
    std::array<wchar_t, 160> requestPath{};
    if (!FormatFileName(target, runtime, L"bin", fileName)) {
        return false;
    }
    const int pathLength = std::swprintf(
        requestPath.data(),
        requestPath.size(),
        L"/addlib/v2/%ls",
        fileName.data());
    if (pathLength <= 0 ||
        static_cast<std::size_t>(pathLength) >= requestPath.size()) {
        return false;
    }

    UniqueInternetHandle session{::WinHttpOpen(
        L"xScal Address Library",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0)};
    if (session.Get() == nullptr ||
        ::WinHttpSetTimeouts(
            session.Get(),
            kNetworkTimeoutMilliseconds,
            kNetworkTimeoutMilliseconds,
            kNetworkTimeoutMilliseconds,
            kNetworkTimeoutMilliseconds) == FALSE) {
        return false;
    }

    UniqueInternetHandle connection{::WinHttpConnect(
        session.Get(), L"dcma.pl", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (connection.Get() == nullptr) {
        return false;
    }
    UniqueInternetHandle request{::WinHttpOpenRequest(
        connection.Get(),
        L"GET",
        requestPath.data(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH)};
    if (request.Get() == nullptr ||
        ::WinHttpSendRequest(
            request.Get(),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) == FALSE ||
        ::WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
        return false;
    }

    DWORD status{};
    DWORD statusSize = sizeof(status);
    if (::WinHttpQueryHeaders(
            request.Get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) == FALSE ||
        status != HTTP_STATUS_OK) {
        return false;
    }

    try {
        std::array<char, 4096> buffer{};
        for (;;) {
            DWORD received{};
            if (::WinHttpReadData(
                    request.Get(),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &received) == FALSE) {
                contents.clear();
                return false;
            }
            if (received == 0) {
                break;
            }
            if (contents.size() > kMaximumDatabaseSize - received) {
                contents.clear();
                return false;
            }
            contents.append(buffer.data(), received);
        }
    } catch (...) {
        contents.clear();
        return false;
    }
    return !contents.empty();
}

bool StoreAddressLibrary(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::string_view contents) noexcept {
    platform::ExecutableRelativePath path;
    return ResolveFilePath(target, runtime, L"bin", path) &&
        platform::WriteFileAtomically(path, contents);
}

bool IsUpdateDeclined(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    format::XScalVersion release,
    std::uint32_t revision) noexcept {
    std::string text;
    unsigned int major{}, minor{}, patch{}, declinedRevision{};
    if (!ReadMarker(target, runtime, L"declined", text) ||
        sscanf_s(text.c_str(), "%u.%u.%u %u",
            &major, &minor, &patch, &declinedRevision) != 4 ||
        major > 255 || minor > 255 || patch > 255) {
        return false;
    }
    const format::XScalVersion declined{
        static_cast<std::uint8_t>(major),
        static_cast<std::uint8_t>(minor),
        static_cast<std::uint8_t>(patch)};
    return !format::IsFileNewer(release, revision, declined, declinedRevision);
}

bool StoreDeclinedUpdate(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    format::XScalVersion release,
    std::uint32_t revision) noexcept {
    std::array<char, kMarkerSize> text{};
    std::snprintf(
        text.data(), text.size(), "%u.%u.%u %u",
        static_cast<unsigned int>(release.major),
        static_cast<unsigned int>(release.minor),
        static_cast<unsigned int>(release.patch),
        static_cast<unsigned int>(revision));
    return WriteMarker(target, runtime, L"declined", text.data());
}

bool IsUpdateCheckDue(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::uint64_t nowFileTime) noexcept {
    std::string text;
    unsigned long long checkedAt{};
    unsigned int major{}, minor{}, patch{};
    if (!ReadMarker(target, runtime, L"checked", text) ||
        sscanf_s(text.c_str(), "%llu %u.%u.%u",
            &checkedAt, &major, &minor, &patch) != 4) {
        return true;
    }
    if (major != config::kXScalVersion.major ||
        minor != config::kXScalVersion.minor ||
        patch != config::kXScalVersion.patch) {
        return true;
    }
    return checkedAt > nowFileTime ||
        nowFileTime - checkedAt >= kUpdateCheckInterval;
}

bool StoreUpdateCheck(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::uint64_t nowFileTime) noexcept {
    std::array<char, kMarkerSize> text{};
    std::snprintf(
        text.data(), text.size(), "%llu %u.%u.%u",
        static_cast<unsigned long long>(nowFileTime),
        static_cast<unsigned int>(config::kXScalVersion.major),
        static_cast<unsigned int>(config::kXScalVersion.minor),
        static_cast<unsigned int>(config::kXScalVersion.patch));
    return WriteMarker(target, runtime, L"checked", text.data());
}

Library::Library() noexcept
    : interface_{
          sizeof(XScalAddressLibraryV1),
          XSCAL_ADDRESS_LIBRARY_API_VERSION,
          this,
          &ResolveAddressCallback,
          &LookupRvaCallback,
          &LookupConstantCallback} {}

LoadStatus Library::Load(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const ExecutableIdentity& executable,
    std::uintptr_t moduleBase) noexcept {
    platform::ExecutableRelativePath path;
    if (!ResolveFilePath(target, runtime, L"bin", path)) {
        return LoadStatus::ReadFailed;
    }
    const auto presence = platform::QueryFilePresence(path);
    if (presence == platform::FilePresence::Missing) {
        return LoadStatus::Missing;
    }
    if (presence != platform::FilePresence::Present) {
        return LoadStatus::ReadFailed;
    }

    std::string contents;
    if (!platform::ReadBoundedFile(path, kMaximumDatabaseSize, contents)) {
        return LoadStatus::ReadFailed;
    }
    return Parse(contents, target, runtime, executable, moduleBase);
}

LoadStatus Library::Parse(
    std::string_view contents,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const ExecutableIdentity& executable,
    std::uintptr_t moduleBase) noexcept {
    entries_.clear();
    moduleBase_ = 0;
    requiredXScalVersion_ = {};
    releaseXScalVersion_ = {};
    revision_ = 0;
    fileExecutable_ = {};
    lastSignatureCheck_ = plugins::signing::SignatureCheck::MissingSignature;
    if (contents.size() < sizeof(format::Header)) {
        return LoadStatus::Invalid;
    }

    format::Header header{};
    std::memcpy(&header, contents.data(), sizeof(header));
    if (std::memcmp(header.magic, format::kMagic, sizeof(header.magic)) != 0 ||
        header.schemaVersion != format::kSchemaVersion ||
        header.headerSize < sizeof(header) ||
        header.headerSize > kMaximumHeaderSize ||
        header.entrySize < sizeof(format::Entry) ||
        header.entrySize > kMaximumEntrySize ||
        header.entryCount == 0 || header.entryCount > kMaximumEntries) {
        return LoadStatus::Invalid;
    }
    const std::size_t signedSize = header.headerSize +
        static_cast<std::size_t>(header.entryCount) * header.entrySize;
    if (contents.size() !=
        signedSize + sizeof(plugins::signing::PluginSignature)) {
        return LoadStatus::Invalid;
    }

    plugins::signing::PluginSignature signature{};
    std::memcpy(&signature, contents.data() + signedSize, sizeof(signature));
    const auto signatureResult = plugins::signing::CheckSignedData(
        contents.data(), signedSize, signature);
    lastSignatureCheck_ = signatureResult.check;
    if (signatureResult.check != plugins::signing::SignatureCheck::Valid ||
        signature.certificate.idPrefix[0] != '\0') {
        return LoadStatus::BadSignature;
    }

    // Without the key the records cannot be read. Runtime logs this case.
    if (!kHasLibraryKey) {
        return LoadStatus::Invalid;
    }

    fileExecutable_ = {header.imageTimeDateStamp, header.imageSize};
    if (header.reserved != 0 ||
        header.targetKind != TargetValue(target) ||
        header.runtimeMajor != runtime.major ||
        header.runtimeMinor != runtime.minor ||
        header.runtimeBuild != runtime.build ||
        header.runtimeRevision != runtime.revision) {
        return LoadStatus::Invalid;
    }
    if (fileExecutable_ != executable) {
        return LoadStatus::WrongExecutable;
    }
    if (header.releaseXScalVersion == 0 ||
        header.releaseXScalVersion > kMaximumVersion ||
        header.minimumXScalVersion > kMaximumVersion) {
        return LoadStatus::Invalid;
    }
    const auto release = format::DecodeXScalVersion(header.releaseXScalVersion);
    const auto required = format::DecodeXScalVersion(header.minimumXScalVersion);
    if (header.minimumXScalVersion != 0) {
        constexpr format::XScalVersion available{
            config::kXScalVersion.major,
            config::kXScalVersion.minor,
            config::kXScalVersion.patch};
        if (format::IsVersionNewer(required, available)) {
            requiredXScalVersion_ = required;
            return LoadStatus::RequiresNewerXScal;
        }
    }

    std::string records;
    std::uint8_t key[kChaCha20KeySize];
    const std::size_t recordsSize = signedSize - header.headerSize;
    try {
        records.assign(contents.data() + header.headerSize, recordsSize);
    } catch (...) {
        return LoadStatus::Invalid;
    }
    std::memcpy(key, kLibraryKey, sizeof(key));
    ChaCha20Xor(
        key,
        header.nonce,
        0,
        reinterpret_cast<std::uint8_t*>(records.data()),
        recordsSize);
    SecureZeroMemory(key, sizeof(key));

    const auto wipeRecords = [&records]() noexcept {
        SecureZeroMemory(records.data(), records.size());
    };
    try {
        entries_.reserve(header.entryCount);
        for (std::uint32_t index = 0; index < header.entryCount; ++index) {
            format::Entry record{};
            std::memcpy(
                &record,
                records.data() +
                    static_cast<std::size_t>(index) * header.entrySize,
                sizeof(record));
            if (record.id == 0 || record.reserved != 0 ||
                (record.kind != format::kAddressEntry &&
                 record.kind != format::kConstantEntry)) {
                wipeRecords();
                entries_.clear();
                return LoadStatus::Invalid;
            }
            entries_.push_back({
                record.id,
                record.value,
                record.kind == format::kAddressEntry
                    ? EntryKind::Address
                    : EntryKind::Constant});
        }
        std::sort(
            entries_.begin(),
            entries_.end(),
            [](const Entry& left, const Entry& right) {
                return left.id < right.id;
            });
    } catch (...) {
        wipeRecords();
        entries_.clear();
        return LoadStatus::Invalid;
    }
    wipeRecords();
    if (std::adjacent_find(
            entries_.begin(),
            entries_.end(),
            [](const Entry& left, const Entry& right) {
                return left.id == right.id;
            }) != entries_.end()) {
        entries_.clear();
        return LoadStatus::Invalid;
    }
    requiredXScalVersion_ = required;
    releaseXScalVersion_ = release;
    revision_ = header.revision;
    moduleBase_ = moduleBase;
    return LoadStatus::Loaded;
}

const Library::Entry* Library::Find(std::uint64_t id) const noexcept {
    const auto entry = std::lower_bound(
        entries_.begin(),
        entries_.end(),
        id,
        [](const Entry& candidate, std::uint64_t requested) {
            return candidate.id < requested;
        });
    return entry != entries_.end() && entry->id == id
        ? &*entry
        : nullptr;
}

bool Library::ResolveAddress(
    std::uint64_t id,
    std::uintptr_t& address) const noexcept {
    std::uint64_t rva{};
    if (!LookupRva(id, rva) ||
        rva > (std::numeric_limits<std::uintptr_t>::max)() - moduleBase_) {
        return false;
    }
    address = moduleBase_ + static_cast<std::uintptr_t>(rva);
    return true;
}

bool Library::LookupRva(
    std::uint64_t id,
    std::uint64_t& rva) const noexcept {
    const Entry* const entry = Find(id);
    if (entry == nullptr || entry->kind != EntryKind::Address) {
        return false;
    }
    rva = entry->value;
    return true;
}

bool Library::LookupConstant(
    std::uint64_t id,
    std::uint64_t& value) const noexcept {
    const Entry* const entry = Find(id);
    if (entry == nullptr || entry->kind != EntryKind::Constant) {
        return false;
    }
    value = entry->value;
    return true;
}

std::size_t Library::EntryCount() const noexcept {
    return entries_.size();
}

format::XScalVersion Library::RequiredXScalVersion() const noexcept {
    return requiredXScalVersion_;
}

format::XScalVersion Library::ReleaseXScalVersion() const noexcept {
    return releaseXScalVersion_;
}

std::uint32_t Library::Revision() const noexcept {
    return revision_;
}

plugins::signing::SignatureCheck Library::LastSignatureCheck() const noexcept {
    return lastSignatureCheck_;
}

ExecutableIdentity Library::FileExecutable() const noexcept {
    return fileExecutable_;
}

const XScalAddressLibraryV1* Library::Interface() const noexcept {
    return entries_.empty() ? nullptr : &interface_;
}

int32_t __cdecl Library::ResolveAddressCallback(
    void* context,
    std::uint64_t id,
    std::uintptr_t* address) noexcept {
    return context != nullptr && address != nullptr &&
        static_cast<const Library*>(context)->ResolveAddress(id, *address);
}

int32_t __cdecl Library::LookupRvaCallback(
    void* context,
    std::uint64_t id,
    std::uint64_t* rva) noexcept {
    return context != nullptr && rva != nullptr &&
        static_cast<const Library*>(context)->LookupRva(id, *rva);
}

int32_t __cdecl Library::LookupConstantCallback(
    void* context,
    std::uint64_t id,
    std::uint64_t* value) noexcept {
    return context != nullptr && value != nullptr &&
        static_cast<const Library*>(context)->LookupConstant(id, *value);
}

}  // namespace sf::address
