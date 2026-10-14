#include "chat/chat_types.hpp"

#include <Windows.h>

#include <array>

namespace sf::chat {

const char* AuthStateName(AuthState state) noexcept {
    switch (state) {
    case AuthState::Unauthenticated:
        return "unauthenticated";
    case AuthState::Connecting:
        return "connecting";
    case AuthState::Authenticated:
        return "authenticated";
    case AuthState::Rejected:
        return "rejected";
    }
    return "unauthenticated";
}

bool IsSendableChannel(std::string_view channel) noexcept {
    constexpr std::array<std::string_view, 6> channels{
        "global", "trade", "events", "raids", "infests", "server"};
    for (const auto candidate : channels) {
        if (channel == candidate) {
            return true;
        }
    }
    return false;
}

bool AppendLiteral(
    std::string_view text,
    std::size_t maximumBytes,
    std::string& output) noexcept {
    if (text.size() > maximumBytes - output.size()) {
        return false;
    }
    try {
        output.append(text);
        return true;
    } catch (...) {
        return false;
    }
}

void WipeString(std::string& value) noexcept {
    if (!value.empty()) {
        ::SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

}  // namespace sf::chat
