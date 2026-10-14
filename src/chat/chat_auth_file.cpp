#include "chat/chat_auth_file.hpp"

#include "chat/chat_types.hpp"
#include "common/json.hpp"

#include <Windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace sf::chat {
namespace {

constexpr std::string_view kAuthFileMagic{"XSCALCHATV1"};
static_assert(kAuthFileMagic.size() == 11);

class UniqueFile final {
public:
    explicit UniqueFile(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}

    UniqueFile(const UniqueFile&) = delete;
    UniqueFile& operator=(const UniqueFile&) = delete;

    ~UniqueFile() {
        (void)Close();
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool Close() noexcept {
        if (!IsValid()) {
            return true;
        }
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return ::CloseHandle(handle) != FALSE;
    }

private:
    HANDLE handle_;
};

void WipeBytes(void* data, std::size_t size) noexcept {
    if (data != nullptr && size != 0) {
        ::SecureZeroMemory(data, size);
    }
}

void WipeVector(std::vector<std::byte>& value) noexcept {
    WipeBytes(value.data(), value.size());
    value.clear();
}

struct SensitiveString final {
    ~SensitiveString() {
        WipeString(value);
    }

    void Clear() noexcept {
        WipeString(value);
    }

    std::string value;
};

struct SensitiveBytes final {
    ~SensitiveBytes() {
        WipeVector(value);
    }

    std::vector<std::byte> value;
};

[[nodiscard]] bool IsMissingFileError(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] bool ReadExact(
    HANDLE file,
    std::span<std::byte> output) noexcept {
    std::size_t offset = 0;
    while (offset < output.size()) {
        const std::size_t remaining = output.size() - offset;
        const DWORD chunk = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD bytesRead{};
        if (::ReadFile(
                file,
                output.data() + offset,
                chunk,
                &bytesRead,
                nullptr) == FALSE ||
            bytesRead != chunk) {
            return false;
        }
        offset += bytesRead;
    }
    return true;
}

[[nodiscard]] bool WriteExact(
    HANDLE file,
    std::span<const std::byte> input) noexcept {
    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t remaining = input.size() - offset;
        const DWORD chunk = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD bytesWritten{};
        if (::WriteFile(
                file,
                input.data() + offset,
                chunk,
                &bytesWritten,
                nullptr) == FALSE ||
            bytesWritten != chunk) {
            return false;
        }
        offset += bytesWritten;
    }
    return true;
}

[[nodiscard]] bool ParseAuthJson(
    std::string_view plaintext,
    AuthCredential& credential) noexcept {
    ClearAuthCredential(credential);
    json::Document document;
    if (!json::ParseDocument(
            plaintext,
            {json::NumberSyntax::Any,
             kMaximumAuthFileSize,
             8,
             true},
            document)) {
        return false;
    }
    const auto root = document.Root();
    if (root.GetType() != json::Type::Object) {
        return false;
    }
    try {
        std::string_view server;
        std::string_view userId;
        std::string_view token;
        std::string_view createdAt;
        if (root.ReadStringMember("server", server) !=
                json::MemberStatus::Found ||
            root.ReadStringMember("user_id", userId) !=
                json::MemberStatus::Found ||
            root.ReadStringMember("token", token) !=
                json::MemberStatus::Found ||
            root.ReadStringMember("created_at", createdAt) !=
                json::MemberStatus::Found) {
            return false;
        }
        credential.server.assign(server);
        credential.userId.assign(userId);
        credential.token.assign(token);
        credential.createdAt.assign(createdAt);
        return true;
    } catch (...) {
        ClearAuthCredential(credential);
        return false;
    }
}

[[nodiscard]] bool SerializeAuthJson(
    const AuthCredential& credential,
    std::string& plaintext) noexcept {
    WipeString(plaintext);
    if (!json::IsValidUtf8(credential.server) ||
        !json::IsValidUtf8(credential.userId) ||
        !json::IsValidUtf8(credential.token) ||
        !json::IsValidUtf8(credential.createdAt)) {
        return false;
    }
    try {
        plaintext.append("{\"server\":");
        json::AppendString(credential.server, plaintext);
        plaintext.append(",\"user_id\":");
        json::AppendString(credential.userId, plaintext);
        plaintext.append(",\"token\":");
        json::AppendString(credential.token, plaintext);
        plaintext.append(",\"created_at\":");
        json::AppendString(credential.createdAt, plaintext);
        plaintext.push_back('}');
        return plaintext.size() <= kMaximumAuthFileSize;
    } catch (...) {
        WipeString(plaintext);
        return false;
    }
}

class WindowsAuthFile final : public AuthFilePlatform {
public:
    [[nodiscard]] bool MakeDirectory(
        const std::wstring& path) noexcept override {
        if (::CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
            return true;
        }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    [[nodiscard]] AuthFileReadResult ReadFile(
        const std::wstring& path,
        std::size_t maximumSize,
        std::vector<std::byte>& contents) noexcept override {
        contents.clear();
        UniqueFile file{::CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr)};
        if (!file.IsValid()) {
            return IsMissingFileError(::GetLastError())
                ? AuthFileReadResult::Missing
                : AuthFileReadResult::Failed;
        }

        LARGE_INTEGER fileSize{};
        if (::GetFileSizeEx(file.Get(), &fileSize) == FALSE ||
            fileSize.QuadPart < 0 ||
            static_cast<unsigned long long>(fileSize.QuadPart) > maximumSize) {
            return AuthFileReadResult::Failed;
        }
        try {
            contents.resize(static_cast<std::size_t>(fileSize.QuadPart));
        } catch (...) {
            return AuthFileReadResult::Failed;
        }
        if (!ReadExact(file.Get(), contents)) {
            contents.clear();
            return AuthFileReadResult::Failed;
        }
        return AuthFileReadResult::Read;
    }

    [[nodiscard]] bool WriteFileAtomically(
        const std::wstring& path,
        std::span<const std::byte> contents) noexcept override {
        std::wstring temporaryPath;
        try {
            temporaryPath = path;
            temporaryPath += L".tmp";
        } catch (...) {
            return false;
        }

        UniqueFile temporaryFile{::CreateFileW(
            temporaryPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr)};
        if (!temporaryFile.IsValid()) {
            return false;
        }
        if (!WriteExact(temporaryFile.Get(), contents) ||
            ::FlushFileBuffers(temporaryFile.Get()) == FALSE) {
            const DWORD error = ::GetLastError();
            (void)temporaryFile.Close();
            (void)::DeleteFileW(temporaryPath.c_str());
            ::SetLastError(error);
            return false;
        }
        if (!temporaryFile.Close()) {
            const DWORD error = ::GetLastError();
            (void)::DeleteFileW(temporaryPath.c_str());
            ::SetLastError(error);
            return false;
        }
        if (::MoveFileExW(
                temporaryPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            const DWORD error = ::GetLastError();
            (void)::DeleteFileW(temporaryPath.c_str());
            ::SetLastError(error);
            return false;
        }
        return true;
    }

    [[nodiscard]] AuthFileDeleteResult DeleteAuthFile(
        const std::wstring& path) noexcept override {
        if (::DeleteFileW(path.c_str()) != FALSE) {
            return AuthFileDeleteResult::Deleted;
        }
        return IsMissingFileError(::GetLastError())
            ? AuthFileDeleteResult::Missing
            : AuthFileDeleteResult::Failed;
    }

    [[nodiscard]] bool Protect(
        std::span<const std::byte> plaintext,
        std::vector<std::byte>& ciphertext) noexcept override {
        ciphertext.clear();
        if (plaintext.size() > (std::numeric_limits<DWORD>::max)()) {
            return false;
        }
        DATA_BLOB input{
            static_cast<DWORD>(plaintext.size()),
            reinterpret_cast<BYTE*>(
                const_cast<std::byte*>(plaintext.data()))};
        DATA_BLOB output{};
        if (::CryptProtectData(
                &input,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &output) == FALSE) {
            return false;
        }
        bool copied = false;
        try {
            const auto* first = reinterpret_cast<const std::byte*>(output.pbData);
            ciphertext.assign(first, first + output.cbData);
            copied = true;
        } catch (...) {
            ciphertext.clear();
        }
        if (output.pbData != nullptr) {
            (void)::LocalFree(output.pbData);
        }
        return copied;
    }

    [[nodiscard]] bool Unprotect(
        std::span<const std::byte> ciphertext,
        std::vector<std::byte>& plaintext) noexcept override {
        WipeVector(plaintext);
        if (ciphertext.size() > (std::numeric_limits<DWORD>::max)()) {
            return false;
        }
        DATA_BLOB input{
            static_cast<DWORD>(ciphertext.size()),
            reinterpret_cast<BYTE*>(
                const_cast<std::byte*>(ciphertext.data()))};
        DATA_BLOB output{};
        if (::CryptUnprotectData(
                &input,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &output) == FALSE) {
            return false;
        }
        bool copied = false;
        try {
            const auto* first = reinterpret_cast<const std::byte*>(output.pbData);
            plaintext.assign(first, first + output.cbData);
            copied = true;
        } catch (...) {
            plaintext.clear();
        }
        WipeBytes(output.pbData, output.cbData);
        if (output.pbData != nullptr) {
            (void)::LocalFree(output.pbData);
        }
        return copied;
    }
};

}  // namespace

bool FindProductionAuthFilePath(std::wstring& path) noexcept {
    path.clear();
    try {
        std::array<wchar_t, 32768> executable{};
        const DWORD executableLength = ::GetModuleFileNameW(
            nullptr,
            executable.data(),
            static_cast<DWORD>(executable.size()));
        if (executableLength == 0 || executableLength >= executable.size()) {
            return false;
        }
        std::size_t directoryEnd = executableLength;
        while (directoryEnd != 0 &&
               executable[directoryEnd - 1] != L'\\' &&
               executable[directoryEnd - 1] != L'/') {
            --directoryEnd;
        }
        if (directoryEnd == 0) {
            return false;
        }
        std::wstring directory{executable.data(), directoryEnd};
        directory.append(L"Data\\XSCAL\\");
        directory.append(kAuthFileName);
        path = std::move(directory);
        return true;
    } catch (...) {
        path.clear();
        return false;
    }
}

AuthFilePlatform& WindowsAuthFilePlatform() noexcept {
    static WindowsAuthFile platform;
    return platform;
}

void ClearAuthCredential(AuthCredential& credential) noexcept {
    WipeString(credential.server);
    WipeString(credential.userId);
    WipeString(credential.token);
    WipeString(credential.createdAt);
}

AuthFile::AuthFile(
    std::wstring path,
    AuthFilePlatform& platform) noexcept
    : path_(std::move(path)), platform_(&platform) {}

AuthFileLoadResult AuthFile::Load(
    AuthCredential& credential) const noexcept {
    ClearAuthCredential(credential);
    std::vector<std::byte> fileContents;
    const AuthFileReadResult read = platform_->ReadFile(
        path_, kMaximumAuthFileSize, fileContents);
    if (read == AuthFileReadResult::Missing) {
        return AuthFileLoadResult::Missing;
    }
    if (read != AuthFileReadResult::Read) {
        return AuthFileLoadResult::ReadFailed;
    }
    if (fileContents.size() <= kAuthFileMagic.size() ||
        fileContents.size() > kMaximumAuthFileSize ||
        std::memcmp(
            fileContents.data(),
            kAuthFileMagic.data(),
            kAuthFileMagic.size()) != 0) {
        return AuthFileLoadResult::InvalidFile;
    }

    SensitiveBytes plaintext;
    const auto ciphertext = std::span<const std::byte>{fileContents}.subspan(
        kAuthFileMagic.size());
    if (!platform_->Unprotect(ciphertext, plaintext.value)) {
        return AuthFileLoadResult::UnprotectFailed;
    }
    if (plaintext.value.empty()) {
        return AuthFileLoadResult::InvalidFile;
    }
    const std::string_view document{
        reinterpret_cast<const char*>(plaintext.value.data()),
        plaintext.value.size()};
    if (!ParseAuthJson(document, credential)) {
        return AuthFileLoadResult::InvalidFile;
    }
    return AuthFileLoadResult::Loaded;
}

bool AuthFile::Save(const AuthCredential& credential) const noexcept {
    SensitiveString plaintext;
    if (!SerializeAuthJson(credential, plaintext.value)) {
        return false;
    }

    std::vector<std::byte> ciphertext;
    const std::span<const std::byte> plaintextBytes{
        reinterpret_cast<const std::byte*>(plaintext.value.data()),
        plaintext.value.size()};
    if (!platform_->Protect(plaintextBytes, ciphertext)) {
        return false;
    }
    plaintext.Clear();

    if (ciphertext.empty() ||
        ciphertext.size() > kMaximumAuthFileSize - kAuthFileMagic.size()) {
        return false;
    }
    try {
        std::vector<std::byte> fileContents;
        fileContents.reserve(kAuthFileMagic.size() + ciphertext.size());
        const auto* magic =
            reinterpret_cast<const std::byte*>(kAuthFileMagic.data());
        fileContents.insert(
            fileContents.end(), magic, magic + kAuthFileMagic.size());
        fileContents.insert(
            fileContents.end(), ciphertext.begin(), ciphertext.end());
        const auto separator = path_.find_last_of(L"\\/");
        if (separator != std::wstring::npos &&
            !platform_->MakeDirectory(path_.substr(0, separator))) {
            return false;
        }
        return platform_->WriteFileAtomically(path_, fileContents);
    } catch (...) {
        return false;
    }
}

bool AuthFile::Logout() const noexcept {
    return platform_->DeleteAuthFile(path_) != AuthFileDeleteResult::Failed;
}

}  // namespace sf::chat
