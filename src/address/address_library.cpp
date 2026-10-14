#include "address/address_library.hpp"

#include "address/address_library_format.hpp"
#include "config/config.h"
#include "platform/win32_file.hpp"
#include "xscal/address_ids.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <limits>

namespace sf::address {
namespace {

constexpr std::size_t kMaximumDatabaseSize = 1024 * 1024;
constexpr std::size_t kMaximumEntries = 4096;
constexpr int kNetworkTimeoutMilliseconds = 5'000;

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
    std::array<wchar_t, 96>& fileName) noexcept {
    const int length = std::swprintf(
        fileName.data(),
        fileName.size(),
        L"version-%hu-%hu-%hu-%hu-%hs.bin",
        runtime.major,
        runtime.minor,
        runtime.build,
        runtime.revision,
        TargetKindName(target));
    return length > 0 &&
        static_cast<std::size_t>(length) < fileName.size();
}

[[nodiscard]] bool ResolveFilePath(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    platform::ExecutableRelativePath& path) noexcept {
    std::array<wchar_t, 96> fileName{};
    std::array<wchar_t, 160> relativePath{};
    if (!FormatFileName(target, runtime, fileName)) {
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

}  // namespace

bool DownloadAddressLibrary(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::string& contents) noexcept {
    contents.clear();
    std::array<wchar_t, 96> fileName{};
    std::array<wchar_t, 160> requestPath{};
    if (!FormatFileName(target, runtime, fileName)) {
        return false;
    }
    const int pathLength = std::swprintf(
        requestPath.data(),
        requestPath.size(),
        L"/addlib/%ls",
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
    return ResolveFilePath(target, runtime, path) &&
        platform::WriteFileAtomically(path, contents);
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
    std::uintptr_t moduleBase) noexcept {
    platform::ExecutableRelativePath path;
    if (!ResolveFilePath(target, runtime, path)) {
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
    return ParseContents(contents, target, runtime, moduleBase);
}

bool Library::Parse(
    std::string_view contents,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::uintptr_t moduleBase) noexcept {
    return ParseContents(contents, target, runtime, moduleBase) ==
        LoadStatus::Loaded;
}

LoadStatus Library::ParseContents(
    std::string_view contents,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::uintptr_t moduleBase) noexcept {
    entries_.clear();
    moduleBase_ = 0;
    requiredXScalVersion_ = {};
    if (contents.size() < sizeof(format::Header)) {
        return LoadStatus::Invalid;
    }

    format::Header header{};
    std::memcpy(&header, contents.data(), sizeof(header));
    if (std::memcmp(header.magic, format::kMagic, sizeof(header.magic)) != 0 ||
        header.schemaVersion != format::kSchemaVersion ||
        header.targetKind != TargetValue(target) ||
        header.runtimeMajor != runtime.major ||
        header.runtimeMinor != runtime.minor ||
        header.runtimeBuild != runtime.build ||
        header.runtimeRevision != runtime.revision ||
        header.entryCount == 0 || header.entryCount > kMaximumEntries ||
        header.reserved != 0) {
        return LoadStatus::Invalid;
    }
    const std::size_t entryBytes =
        static_cast<std::size_t>(header.entryCount) * sizeof(format::Entry);
    if (contents.size() != sizeof(header) + entryBytes) {
        return LoadStatus::Invalid;
    }

    try {
        std::vector<format::Entry> records(header.entryCount);
        std::memcpy(
            records.data(),
            contents.data() + sizeof(header),
            entryBytes);
        if (format::HashEntries(records.data(), records.size()) !=
            header.entriesHash) {
            return LoadStatus::Invalid;
        }
        entries_.reserve(records.size());
        for (const auto& record : records) {
            if (record.id == 0 || record.reserved != 0 ||
                (record.kind != format::kAddressEntry &&
                 record.kind != format::kConstantEntry)) {
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
        entries_.clear();
        return LoadStatus::Invalid;
    }
    if (std::adjacent_find(
            entries_.begin(),
            entries_.end(),
            [](const Entry& left, const Entry& right) {
                return left.id == right.id;
            }) != entries_.end()) {
        entries_.clear();
        return LoadStatus::Invalid;
    }
    const Entry* const requirement = Find(XSCAL_CONSTANT_MINIMUM_XSCAL_VERSION);
    if (requirement != nullptr) {
        if (requirement->kind != EntryKind::Constant ||
            requirement->value == 0 ||
            (requirement->value & 0xFFFFFFFFFF000000ull) != 0) {
            entries_.clear();
            return LoadStatus::Invalid;
        }
        requiredXScalVersion_ = format::DecodeXScalVersion(
            static_cast<std::uint32_t>(requirement->value));
        constexpr format::XScalVersion available{
            config::kXScalVersion.major,
            config::kXScalVersion.minor,
            config::kXScalVersion.patch};
        if (format::IsVersionNewer(requiredXScalVersion_, available)) {
            entries_.clear();
            return LoadStatus::RequiresNewerXScal;
        }
    }
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
