#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace sf::chat {

inline constexpr std::size_t kMaximumRelayMessageBytes = 8'192;
inline constexpr std::size_t kMaximumChatBodyCharacters = 512;
inline constexpr std::size_t kMaximumClientVersionBytes = 64;
inline constexpr std::size_t kMaximumQueuedEvents = 128;
inline constexpr std::uint32_t kMaximumPollEvents = 64;

enum class AuthState {
    Unauthenticated,
    Connecting,
    Authenticated,
    Rejected,
};

struct ChatPermissions final {
    bool canReport{};
    bool canDeleteMessage{};
    bool canMuteUser{};
    bool canBanUser{};
    bool canSetSlowMode{};
};

struct ConnectOptions final {
    std::string displayName;
    std::string clientVersion;
    bool autoRegister{true};
};

struct ChatMessageRequest final {
    std::string channel;
    std::string targetUserId;
    std::string body;
};

struct ChatReportRequest final {
    std::string messageId;
    std::string reason;
};

struct ModerationRequest final {
    std::string action;
    std::string messageId;
    std::string targetUserId;
    std::string reason;
    std::string category;
    std::int64_t durationMinutes{};
    bool durationSupplied{};
    bool categorySupplied{};
};

struct ChatEvent final {
    std::uint64_t id{};
    std::uint64_t requestId{};
    std::string kind;
    std::string status;
    std::string channel;
    std::string senderUserId;
    std::string senderDisplayName;
    std::string nameColor;
    std::string body;
    std::string messageId;
    std::string createdAt;
    std::string targetUserId;
    std::string code;
    std::string message;
    std::string errorCode;
    std::string errorMessage;
    std::uint64_t dropped{};
};

[[nodiscard]] const char* AuthStateName(AuthState state) noexcept;
[[nodiscard]] bool IsSendableChannel(std::string_view channel) noexcept;

// Appends text unless the output would exceed maximumBytes or the append
// allocation fails. The output must not already exceed maximumBytes.
[[nodiscard]] bool AppendLiteral(
    std::string_view text,
    std::size_t maximumBytes,
    std::string& output) noexcept;

// Zeroes the string contents before clearing it.
void WipeString(std::string& value) noexcept;

}  // namespace sf::chat
