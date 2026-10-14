#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace sf::platform {

inline constexpr std::size_t kMaximumWin32PathLength = 32768;

struct ExecutableRelativePath final {
    std::array<wchar_t, kMaximumWin32PathLength> storage{};
    std::size_t length{};

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
