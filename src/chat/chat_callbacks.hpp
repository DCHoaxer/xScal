#pragma once

#include <cstddef>

namespace sf {
struct ScaleformCall;
}

namespace sf::chat {

class ChatSession;

enum class ChatFunction {
    GetRuntimeInfo,
    Connect,
    GetAuthState,
    GetConnectionState,
    SendChatMessage,
    Disconnect,
    ReportMessage,
    ModerationAction,
    Logout,
    ClearChatAuth,
    PollEvents,
};

[[nodiscard]] bool InvokeChatFunction(
    ChatFunction function,
    const ScaleformCall& call,
    ChatSession& session) noexcept;

}  // namespace sf::chat
