#include "chat/chat_callbacks.hpp"

#include "chat/chat_session.hpp"
#include "chat/scaleform_payload.hpp"

#include <algorithm>
#include <limits>

namespace sf::chat {
namespace {

// Chat commands read their arguments from the first ActionScript argument.
constexpr std::size_t kCommandArgumentIndex = 0;

[[nodiscard]] bool SetResult(
    const ScaleformCall& call,
    std::string result) noexcept {
    return call.result->SetString(result);
}

// Returns a fixed literal when the field name cannot be copied, because
// callers are noexcept.
[[nodiscard]] std::string InvalidPayload(std::string_view field) noexcept {
    try {
        std::string output{
            R"({"success":false,"code":"invalid_payload","message":"Invalid or missing field: )"};
        output.append(field);
        output.append(R"("})");
        return output;
    } catch (...) {
        return R"({"success":false,"code":"invalid_payload","message":"Invalid or missing field"})";
    }
}

[[nodiscard]] bool GetRuntimeInfo(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    return SetResult(call, session.RuntimeInfo());
}

[[nodiscard]] bool Connect(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    ScaleformPayload payload{call, kCommandArgumentIndex};
    ConnectOptions options;
    if (!payload.IsObject() ||
        payload.ReadString("displayName", options.displayName) !=
            PayloadMemberStatus::Found) {
        return SetResult(call, InvalidPayload("displayName"));
    }
    const auto versionStatus =
        payload.ReadString("clientVersion", options.clientVersion);
    if (versionStatus == PayloadMemberStatus::WrongType) {
        return SetResult(call, InvalidPayload("clientVersion"));
    }
    bool autoRegister{};
    const auto autoStatus =
        payload.ReadBoolean("autoRegister", autoRegister);
    if (autoStatus == PayloadMemberStatus::WrongType) {
        return SetResult(call, InvalidPayload("autoRegister"));
    }
    if (autoStatus == PayloadMemberStatus::Found) {
        options.autoRegister = autoRegister;
    }
    return SetResult(call, session.Connect(options));
}

[[nodiscard]] bool GetState(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    return SetResult(call, session.State());
}

[[nodiscard]] bool SendMessage(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    ScaleformPayload payload{call, kCommandArgumentIndex};
    ChatMessageRequest message;
    if (!payload.IsObject() ||
        payload.ReadString("channel", message.channel) !=
            PayloadMemberStatus::Found) {
        return SetResult(call, InvalidPayload("channel"));
    }
    if (payload.ReadString("body", message.body) !=
        PayloadMemberStatus::Found) {
        return SetResult(call, InvalidPayload("body"));
    }
    const auto targetStatus =
        payload.ReadString("targetUserId", message.targetUserId);
    if (targetStatus == PayloadMemberStatus::WrongType) {
        return SetResult(call, InvalidPayload("targetUserId"));
    }
    return SetResult(call, session.SendChatMessage(message));
}

[[nodiscard]] bool Disconnect(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    return SetResult(call, session.Disconnect());
}

[[nodiscard]] bool ReportMessage(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    ScaleformPayload payload{call, kCommandArgumentIndex};
    ChatReportRequest report;
    if (!payload.IsObject() ||
        payload.ReadString("messageId", report.messageId) !=
            PayloadMemberStatus::Found) {
        return SetResult(call, InvalidPayload("messageId"));
    }
    if (payload.ReadString("reason", report.reason) ==
        PayloadMemberStatus::WrongType) {
        return SetResult(call, InvalidPayload("reason"));
    }
    return SetResult(call, session.ReportMessage(report));
}

[[nodiscard]] bool ModerationAction(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    ScaleformPayload payload{call, kCommandArgumentIndex};
    ModerationRequest moderation;
    if (!payload.IsObject() ||
        payload.ReadString("action", moderation.action) !=
            PayloadMemberStatus::Found) {
        return SetResult(call, InvalidPayload("action"));
    }
    const auto readOptionalString = [&](std::string_view name, std::string& value) {
        return payload.ReadString(name, value) != PayloadMemberStatus::WrongType;
    };
    if (!readOptionalString("messageId", moderation.messageId) ||
        !readOptionalString("targetUserId", moderation.targetUserId) ||
        !readOptionalString("reason", moderation.reason)) {
        return SetResult(call, InvalidPayload("moderation field"));
    }
    const auto categoryStatus =
        payload.ReadString("category", moderation.category);
    if (categoryStatus == PayloadMemberStatus::WrongType) {
        return SetResult(call, InvalidPayload("moderation field"));
    }
    moderation.categorySupplied = categoryStatus == PayloadMemberStatus::Found;
    const auto durationStatus =
        payload.ReadInteger("durationMinutes", moderation.durationMinutes);
    if (durationStatus == PayloadMemberStatus::WrongType ||
        moderation.durationMinutes < 0) {
        return SetResult(call, InvalidPayload("durationMinutes"));
    }
    moderation.durationSupplied = durationStatus == PayloadMemberStatus::Found;
    return SetResult(call, session.SubmitModeration(moderation));
}

[[nodiscard]] bool Logout(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    return SetResult(call, session.Logout());
}

[[nodiscard]] bool PollEvents(
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    ScaleformPayload payload{call, kCommandArgumentIndex};
    std::int64_t maximum = 64;
    std::int64_t cursor{};
    const auto maximumStatus = payload.ReadInteger("max", maximum);
    const auto cursorStatus = payload.ReadInteger("cursor", cursor);
    if (maximumStatus == PayloadMemberStatus::WrongType ||
        cursorStatus == PayloadMemberStatus::WrongType ||
        maximum < 0 || cursor < 0) {
        return SetResult(call, InvalidPayload("max/cursor"));
    }
    const auto boundedMaximum = static_cast<std::uint32_t>((std::min)(
        maximum,
        static_cast<std::int64_t>(kMaximumPollEvents)));
    return SetResult(
        call,
        session.PollEvents(
            boundedMaximum,
            static_cast<std::uint64_t>(cursor)));
}

}  // namespace

bool InvokeChatFunction(
    ChatFunction function,
    const ScaleformCall& call,
    ChatSession& session) noexcept {
    switch (function) {
    case ChatFunction::GetRuntimeInfo:
        return GetRuntimeInfo(call, session);
    case ChatFunction::Connect:
        return Connect(call, session);
    case ChatFunction::GetAuthState:
    case ChatFunction::GetConnectionState:
        return GetState(call, session);
    case ChatFunction::SendChatMessage:
        return SendMessage(call, session);
    case ChatFunction::Disconnect:
        return Disconnect(call, session);
    case ChatFunction::ReportMessage:
        return ReportMessage(call, session);
    case ChatFunction::ModerationAction:
        return ModerationAction(call, session);
    case ChatFunction::Logout:
    case ChatFunction::ClearChatAuth:
        return Logout(call, session);
    case ChatFunction::PollEvents:
        return PollEvents(call, session);
    }
    return false;
}

}  // namespace sf::chat
