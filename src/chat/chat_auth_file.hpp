#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace sf::chat {

inline constexpr std::size_t kMaximumAuthFileSize = 65536;
inline constexpr wchar_t kAuthFileName[] = L"chat-auth.bin";

struct AuthCredential final {
    std::string server;
    std::string userId;
    std::string token;
    std::string createdAt;
};

enum class AuthFileReadResult {
    Missing,
    Read,
    Failed,
};

enum class AuthFileDeleteResult {
    Missing,
    Deleted,
    Failed,
};

enum class AuthFileLoadResult {
    Missing,
    Loaded,
    InvalidFile,
    ReadFailed,
    UnprotectFailed,
};

class AuthFilePlatform {
public:
    virtual ~AuthFilePlatform() = default;

    [[nodiscard]] virtual bool MakeDirectory(
        const std::wstring& path) noexcept = 0;

    [[nodiscard]] virtual AuthFileReadResult ReadFile(
        const std::wstring& path,
        std::size_t maximumSize,
        std::vector<std::byte>& contents) noexcept = 0;

    [[nodiscard]] virtual bool WriteFileAtomically(
        const std::wstring& path,
        std::span<const std::byte> contents) noexcept = 0;

    [[nodiscard]] virtual AuthFileDeleteResult DeleteAuthFile(
        const std::wstring& path) noexcept = 0;

    [[nodiscard]] virtual bool Protect(
        std::span<const std::byte> plaintext,
        std::vector<std::byte>& ciphertext) noexcept = 0;

    [[nodiscard]] virtual bool Unprotect(
        std::span<const std::byte> ciphertext,
        std::vector<std::byte>& plaintext) noexcept = 0;
};

// Computes the auth file path next to the executable. The directory is not
// created here; AuthFile::Save creates it before writing.
[[nodiscard]] bool FindProductionAuthFilePath(std::wstring& path) noexcept;

[[nodiscard]] AuthFilePlatform& WindowsAuthFilePlatform() noexcept;

void ClearAuthCredential(AuthCredential& credential) noexcept;

class AuthFile final {
public:
    AuthFile(
        std::wstring path,
        AuthFilePlatform& platform) noexcept;

    [[nodiscard]] AuthFileLoadResult Load(
        AuthCredential& credential) const noexcept;

    [[nodiscard]] bool Save(
        const AuthCredential& credential) const noexcept;

    [[nodiscard]] bool Logout() const noexcept;

private:
    std::wstring path_;
    AuthFilePlatform* platform_{};
};

}  // namespace sf::chat
