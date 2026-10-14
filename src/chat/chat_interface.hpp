#pragma once

#include "chat/chat_callbacks.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace sf {

struct FunctionParams;

namespace chat {

class ChatSession;

struct ChatInterfaceMember final {
    const char* name{};
    void* functionHandler{};
};

void InvokeChatInterfaceFunction(
    ChatFunction function,
    const FunctionParams* params) noexcept;

// One handler type per ChatFunction. Each type owns a static vtable whose
// invoke entry dispatches to its own function, so the handler layout matches
// the 0x10-byte GFx function handler.
template <ChatFunction Function>
class ChatInterfaceFunctionHandler final {
public:
    [[nodiscard]] void* AsScaleformHandler() noexcept { return this; }

private:
    using HandlerVtableEntry = void(__fastcall*)();

    [[nodiscard]] static void* __fastcall Identity(void* handler) noexcept {
        return handler;
    }

    static void __fastcall Invoke(
        void*,
        const FunctionParams* params) noexcept {
        InvokeChatInterfaceFunction(Function, params);
    }

    [[nodiscard]] static const HandlerVtableEntry* Vtable() noexcept {
        static const HandlerVtableEntry entries[]{
            reinterpret_cast<HandlerVtableEntry>(&Identity),
            reinterpret_cast<HandlerVtableEntry>(&Invoke),
        };
        return entries;
    }

    const HandlerVtableEntry* vtable_{Vtable()};
    volatile std::int32_t referenceCount_{1};
    std::uint32_t referenceCountPadding_{};
};

class ChatInterfaceFunctions final {
public:
    [[nodiscard]] std::array<ChatInterfaceMember, 11> Members() noexcept;

private:
    ChatInterfaceFunctionHandler<ChatFunction::GetRuntimeInfo> getRuntimeInfo_;
    ChatInterfaceFunctionHandler<ChatFunction::Connect> connect_;
    ChatInterfaceFunctionHandler<ChatFunction::GetAuthState> getAuthState_;
    ChatInterfaceFunctionHandler<ChatFunction::GetConnectionState> getConnectionState_;
    ChatInterfaceFunctionHandler<ChatFunction::SendChatMessage> sendMessage_;
    ChatInterfaceFunctionHandler<ChatFunction::Disconnect> disconnect_;
    ChatInterfaceFunctionHandler<ChatFunction::ReportMessage> reportMessage_;
    ChatInterfaceFunctionHandler<ChatFunction::ModerationAction> moderationAction_;
    ChatInterfaceFunctionHandler<ChatFunction::Logout> logout_;
    ChatInterfaceFunctionHandler<ChatFunction::ClearChatAuth> clearChatAuth_;
    ChatInterfaceFunctionHandler<ChatFunction::PollEvents> pollEvents_;
};

void SetChatInterfaceSession(ChatSession* session) noexcept;
[[nodiscard]] bool IsChatInterfaceAvailable() noexcept;
[[nodiscard]] ChatInterfaceFunctions& GlobalChatInterfaceFunctions() noexcept;

}  // namespace chat
}  // namespace sf
