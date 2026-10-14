#include "platform/win32_file.hpp"

#include <Windows.h>

#include <algorithm>
#include <limits>

namespace sf::platform {
namespace {

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

[[nodiscard]] bool IsPathSeparator(wchar_t character) noexcept {
    return character == L'\\' || character == L'/';
}

[[nodiscard]] std::size_t RootLength(
    const ExecutableRelativePath& path) noexcept {
    if (path.length >= 3 && path.storage[1] == L':' &&
        IsPathSeparator(path.storage[2])) {
        return 3;
    }
    if (path.length < 5 || !IsPathSeparator(path.storage[0]) ||
        !IsPathSeparator(path.storage[1])) {
        return 0;
    }

    std::size_t cursor = 2;
    while (cursor < path.length && !IsPathSeparator(path.storage[cursor])) {
        ++cursor;
    }
    if (cursor == 2 || cursor == path.length) {
        return 0;
    }
    ++cursor;
    const std::size_t shareStart = cursor;
    while (cursor < path.length && !IsPathSeparator(path.storage[cursor])) {
        ++cursor;
    }
    return cursor == shareStart ? 0 : cursor;
}

[[nodiscard]] bool ExistingDirectory(const wchar_t* path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

[[nodiscard]] bool EnsureParentDirectories(
    ExecutableRelativePath& targetPath) noexcept {
    const std::size_t rootLength = RootLength(targetPath);
    if (rootLength == 0) {
        ::SetLastError(ERROR_INVALID_NAME);
        return false;
    }

    for (std::size_t index = rootLength; index < targetPath.length; ++index) {
        if (!IsPathSeparator(targetPath.storage[index])) {
            continue;
        }
        targetPath.storage[index] = L'\0';
        const bool created = ::CreateDirectoryW(targetPath.c_str(), nullptr) != FALSE;
        const DWORD error = created ? ERROR_SUCCESS : ::GetLastError();
        const bool usable = created ||
            (error == ERROR_ALREADY_EXISTS &&
             ExistingDirectory(targetPath.c_str()));
        targetPath.storage[index] = L'\\';
        if (!usable) {
            const DWORD reportedError = error == ERROR_ALREADY_EXISTS
                ? ERROR_DIRECTORY
                : error;
            ::SetLastError(reportedError);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AppendSuffix(
    const ExecutableRelativePath& path,
    std::wstring_view suffix,
    ExecutableRelativePath& result) noexcept {
    if (path.length + suffix.size() >= result.storage.size()) {
        return false;
    }
    result = path;
    std::copy(suffix.begin(), suffix.end(), result.storage.begin() + path.length);
    result.length = path.length + suffix.size();
    result.storage[result.length] = L'\0';
    return true;
}

[[nodiscard]] bool ReadExact(
    HANDLE file,
    char* destination,
    std::size_t size) noexcept {
    std::size_t offset{};
    while (offset < size) {
        const auto remaining = size - offset;
        const DWORD request = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD received{};
        if (::ReadFile(
                file,
                destination + offset,
                request,
                &received,
                nullptr) == FALSE ||
            received == 0) {
            return false;
        }
        offset += received;
    }
    return true;
}

[[nodiscard]] bool WriteExact(
    HANDLE file,
    std::string_view contents) noexcept {
    std::size_t offset{};
    while (offset < contents.size()) {
        const auto remaining = contents.size() - offset;
        const DWORD request = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written{};
        if (::WriteFile(
                file,
                contents.data() + offset,
                request,
                &written,
                nullptr) == FALSE) {
            return false;
        }
        if (written == 0) {
            ::SetLastError(ERROR_WRITE_FAULT);
            return false;
        }
        offset += written;
    }
    return true;
}

}

bool ResolveExecutableRelativePath(
    const wchar_t* relativePath,
    ExecutableRelativePath& resolvedPath) noexcept {
    resolvedPath = {};
    if (relativePath == nullptr || relativePath[0] == L'\0' ||
        IsPathSeparator(relativePath[0])) {
        return false;
    }

    const DWORD executableLength = ::GetModuleFileNameW(
        nullptr,
        resolvedPath.storage.data(),
        static_cast<DWORD>(resolvedPath.storage.size()));
    if (executableLength == 0 ||
        executableLength >= resolvedPath.storage.size()) {
        resolvedPath = {};
        return false;
    }

    std::size_t directoryEnd = executableLength;
    while (directoryEnd != 0 &&
           !IsPathSeparator(resolvedPath.storage[directoryEnd - 1])) {
        --directoryEnd;
    }
    if (directoryEnd == 0) {
        resolvedPath = {};
        return false;
    }

    std::size_t output = directoryEnd;
    for (const wchar_t* current = relativePath; *current != L'\0'; ++current) {
        if (output + 1 >= resolvedPath.storage.size()) {
            resolvedPath = {};
            return false;
        }
        resolvedPath.storage[output++] =
            IsPathSeparator(*current) ? L'\\' : *current;
    }
    resolvedPath.storage[output] = L'\0';
    resolvedPath.length = output;
    return true;
}

FilePresence QueryFilePresence(
    const ExecutableRelativePath& path) noexcept {
    if (path.length == 0) {
        return FilePresence::Error;
    }
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return FilePresence::Present;
    }
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? FilePresence::Missing
        : FilePresence::Error;
}

bool ReadBoundedFile(
    const ExecutableRelativePath& path,
    std::size_t maximumSize,
    std::string& contents) noexcept {
    contents.clear();
    if (path.length == 0) {
        return false;
    }

    UniqueFile file{::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr)};
    if (!file.IsValid()) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (::GetFileSizeEx(file.Get(), &fileSize) == FALSE ||
        fileSize.QuadPart < 0 ||
        static_cast<unsigned long long>(fileSize.QuadPart) > maximumSize) {
        return false;
    }

    try {
        contents.assign(static_cast<std::size_t>(fileSize.QuadPart), '\0');
    } catch (...) {
        return false;
    }
    if (!contents.empty() &&
        !ReadExact(file.Get(), contents.data(), contents.size())) {
        contents.clear();
        return false;
    }
    return true;
}

bool WriteFileAtomically(
    ExecutableRelativePath& targetPath,
    std::string_view contents) noexcept {
    if (targetPath.length == 0) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (!EnsureParentDirectories(targetPath)) {
        return false;
    }

    ExecutableRelativePath temporaryPath;
    if (!AppendSuffix(targetPath, L".tmp", temporaryPath)) {
        ::SetLastError(ERROR_FILENAME_EXCED_RANGE);
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

    if (!WriteExact(temporaryFile.Get(), contents)) {
        const DWORD error = ::GetLastError();
        (void)temporaryFile.Close();
        (void)::DeleteFileW(temporaryPath.c_str());
        ::SetLastError(error);
        return false;
    }
    if (::FlushFileBuffers(temporaryFile.Get()) == FALSE) {
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
            targetPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD error = ::GetLastError();
        (void)::DeleteFileW(temporaryPath.c_str());
        ::SetLastError(error);
        return false;
    }
    return true;
}

}
