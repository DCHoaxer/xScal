#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace sf::platform {

inline constexpr std::size_t kMaximumWin32PathLength = 32768;

struct ExecutableRelativePath final {
    // The buffer is 64 KiB, so construction and copies only touch the used
    // part plus the terminator instead of the whole array.
    std::array<wchar_t, kMaximumWin32PathLength> storage;
    std::size_t length{};

    ExecutableRelativePath() noexcept {
        storage[0] = L'\0';
    }

    ExecutableRelativePath(const ExecutableRelativePath& other) noexcept
        : length(other.length) {
        std::copy_n(other.storage.begin(), other.length + 1, storage.begin());
    }

    ExecutableRelativePath& operator=(const ExecutableRelativePath& other) noexcept {
        if (this != &other) {
            std::copy_n(other.storage.begin(), other.length + 1, storage.begin());
            length = other.length;
        }
        return *this;
    }

    [[nodiscard]] const wchar_t* c_str() const noexcept {
        return storage.data();
    }
};

enum class FilePresence {
    Missing,
    Present,
    Error,
};

[[nodiscard]] bool ResolveExecutableRelativePath(
    const wchar_t* relativePath,
    ExecutableRelativePath& resolvedPath) noexcept;

[[nodiscard]] FilePresence QueryFilePresence(
    const ExecutableRelativePath& path) noexcept;

[[nodiscard]] bool ReadBoundedFile(
    const ExecutableRelativePath& path,
    std::size_t maximumSize,
    std::string& contents) noexcept;

[[nodiscard]] bool WriteFileAtomically(
    ExecutableRelativePath& targetPath,
    std::string_view contents) noexcept;

}
