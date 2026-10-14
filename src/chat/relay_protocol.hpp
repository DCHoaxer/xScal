#pragma once

#include "chat/chat_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sf::chat {

struct AuthReply final {
    bool parsed{};
    bool success{};
    std::string userId;
    std::string token;
    std::string role;
    std::string createdAt;
    std::string code;
    std::string message;
};

struct RelayOperationReply final {
    bool parsed{};
    bool success{};
    std::string messageId;
    std::string code;
    std::string message;
};

struct SubscriberReply final {
    bool parsed{};
    bool rejected{};
    std::uint64_t cursor{};
    std::string code;
    std::string message;
    std::vector<ChatEvent> events;
};

[[nodiscard]] bool BuildRegisterRequest(
    std::string_view displayName,
    std::string_view clientVersion,
    std::string& request) noexcept;
[[nodiscard]] bool BuildHelloRequest(
    std::string_view token,
    std::string_view displayName,
    std::string_view clientVersion,
    std::string& request) noexcept;
[[nodiscard]] bool BuildSendMessageRequest(
    std::string_view token,
    const ChatMessageRequest& message,
    std::string& request) noexcept;
[[nodiscard]] bool BuildReportMessageRequest(
    std::string_view token,
    const ChatReportRequest& report,
    std::string& request) noexcept;
[[nodiscard]] bool BuildModerationRequest(
    std::string_view token,
    const ModerationRequest& moderation,
    std::string& request) noexcept;
[[nodiscard]] bool BuildSubscribeRequest(
    std::string_view token,
    std::string_view clientVersion,
    std::uint64_t cursor,
    std::string& request) noexcept;

[[nodiscard]] AuthReply ParseAuthReply(std::string_view response) noexcept;
[[nodiscard]] RelayOperationReply ParseRelayOperationReply(
    std::string_view response) noexcept;
[[nodiscard]] SubscriberReply ParseSubscriberReply(
    std::string_view response) noexcept;

}  // namespace sf::chat
