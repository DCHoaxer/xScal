#include "chat/chat_interface.hpp"

#include "chat/chat_session.hpp"
#include "scaleform/native_handler.hpp"

#include <atomic>

namespace sf::chat {
namespace {

std::atomic<ChatSession*> activeChatSession{};

}  // namespace

void InvokeChatInterfaceFunction(
    ChatFunction function,
    const FunctionParams* params) noexcept {
    if (params == nullptr || params->result == nullptr) {
        return;
    }
    auto* const session = activeChatSession.load(std::memory_order_acquire);
    if (session == nullptr) {
        return;
    }

    ScaleformResult result;
    const ScaleformCall call{
        params->arguments,
        params->argumentCount,
        &result,
        params->movie,
        params->thisObject,
        params->argumentFrame,
    };
    if (InvokeChatFunction(function, call, *session)) {
        (void)CommitScaleformResult(*params, result);
    }
}

std::array<ChatInterfaceMember, 11>
ChatInterfaceFunctions::Members() noexcept {
    return {{
        {"getRuntimeInfo", getRuntimeInfo_.AsScaleformHandler()},
        {"connect", connect_.AsScaleformHandler()},
        {"getAuthState", getAuthState_.AsScaleformHandler()},
        {"getConnectionState", getConnectionState_.AsScaleformHandler()},
        {"sendMessage", sendMessage_.AsScaleformHandler()},
        {"disconnect", disconnect_.AsScaleformHandler()},
        {"reportMessage", reportMessage_.AsScaleformHandler()},
        {"moderationAction", moderationAction_.AsScaleformHandler()},
        {"logout", logout_.AsScaleformHandler()},
        {"clearChatAuth", clearChatAuth_.AsScaleformHandler()},
        {"pollEvents", pollEvents_.AsScaleformHandler()},
    }};
}

void SetChatInterfaceSession(ChatSession* session) noexcept {
    activeChatSession.store(session, std::memory_order_release);
}

bool IsChatInterfaceAvailable() noexcept {
    return activeChatSession.load(std::memory_order_acquire) != nullptr;
}

ChatInterfaceFunctions& GlobalChatInterfaceFunctions() noexcept {
    static ChatInterfaceFunctions functions;
    return functions;
}

static_assert(
    sizeof(ChatInterfaceFunctionHandler<ChatFunction::GetRuntimeInfo>) == 0x10);

}  // namespace sf::chat
