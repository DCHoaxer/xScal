#include "chat/relay_protocol.hpp"

#include "common/json.hpp"

#include <limits>

namespace sf::chat {
namespace {

constexpr json::ParseOptions kRelayJsonOptions{
    json::NumberSyntax::SignedInteger,
    kMaximumRelayMessageBytes,
    64,
    true};

[[nodiscard]] bool AddStringMember(
    std::string_view name,
    std::string_view value,
    bool first,
    std::string& output) noexcept {
    return AppendLiteral(first ? "\"" : ",\"", kMaximumRelayMessageBytes, output) &&
        AppendLiteral(name, kMaximumRelayMessageBytes, output) &&
        AppendLiteral("\":", kMaximumRelayMessageBytes, output) &&
        json::AppendString(value, output, kMaximumRelayMessageBytes);
}

[[nodiscard]] bool AddIntegerMember(
    std::string_view name,
    std::int64_t value,
    bool first,
    std::string& output) noexcept {
    return AppendLiteral(first ? "\"" : ",\"", kMaximumRelayMessageBytes, output) &&
        AppendLiteral(name, kMaximumRelayMessageBytes, output) &&
        AppendLiteral("\":", kMaximumRelayMessageBytes, output) &&
        json::AppendInteger(value, output, kMaximumRelayMessageBytes);
}

[[nodiscard]] bool Finish(std::string& output) noexcept {
    if (!AppendLiteral("}", kMaximumRelayMessageBytes, output)) {
        output.clear();
        return false;
    }
    return true;
}

[[nodiscard]] bool BeginOperation(
    std::string_view operation,
    std::string_view token,
    std::string& output) noexcept {
    output.clear();
    return AppendLiteral("{", kMaximumRelayMessageBytes, output) &&
        AddStringMember("op", operation, true, output) &&
        AddStringMember("token", token, false, output);
}

void CopyStringMember(
    const json::Value& object,
    std::string_view name,
    std::string& output) {
    std::string_view value;
    if (object.ReadStringMember(name, value) == json::MemberStatus::Found) {
        output.assign(value);
    }
}

[[nodiscard]] bool ReadUnsignedMember(
    const json::Value& object,
    std::string_view name,
    std::uint64_t& output) noexcept {
    std::int64_t value{};
    if (object.ReadIntegerMember(name, value) != json::MemberStatus::Found ||
        value < 0) {
        return false;
    }
    output = static_cast<std::uint64_t>(value);
    return true;
}

void ReadChatEvent(const json::Value& object, ChatEvent& event) {
    CopyStringMember(object, "kind", event.kind);
    CopyStringMember(object, "channel", event.channel);
    CopyStringMember(object, "senderUserId", event.senderUserId);
    CopyStringMember(object, "senderDisplayName", event.senderDisplayName);
    CopyStringMember(object, "nameColor", event.nameColor);
    CopyStringMember(object, "body", event.body);
    CopyStringMember(object, "messageId", event.messageId);
    CopyStringMember(object, "createdAt", event.createdAt);
    CopyStringMember(object, "targetUserId", event.targetUserId);
}

void FindChatEvents(
    const json::Value& value,
    std::vector<ChatEvent>& events) {
    if (value.GetType() == json::Type::Object) {
        std::string_view kind;
        if (value.ReadStringMember("kind", kind) ==
                json::MemberStatus::Found &&
            kind == "chat.message") {
            ChatEvent event;
            ReadChatEvent(value, event);
            events.push_back(std::move(event));
            return;
        }
        auto members = value.Members();
        std::string_view name;
        json::Value member;
        while (members.Next(name, member)) {
            FindChatEvents(member, events);
        }
        return;
    }
    if (value.GetType() == json::Type::Array) {
        for (std::size_t index = 0; index < value.ArraySize(); ++index) {
            const auto item = value.ArrayItem(index);
            if (item) {
                FindChatEvents(item, events);
            }
        }
    }
}

}  // namespace

bool BuildRegisterRequest(
    std::string_view displayName,
    std::string_view clientVersion,
    std::string& request) noexcept {
    request.clear();
    return AppendLiteral("{", kMaximumRelayMessageBytes, request) &&
        AddStringMember("op", "register", true, request) &&
        AddStringMember("displayName", displayName, false, request) &&
        AddStringMember("clientVersion", clientVersion, false, request) &&
        Finish(request);
}

bool BuildHelloRequest(
    std::string_view token,
    std::string_view displayName,
    std::string_view clientVersion,
    std::string& request) noexcept {
    return BeginOperation("hello", token, request) &&
        AddStringMember("displayName", displayName, false, request) &&
        AddStringMember("clientVersion", clientVersion, false, request) &&
        Finish(request);
}

bool BuildSendMessageRequest(
    std::string_view token,
    const ChatMessageRequest& message,
    std::string& request) noexcept {
    const bool built = BeginOperation("send", token, request) &&
        AddStringMember("channel", message.channel, false, request) &&
        AddStringMember("body", message.body, false, request) &&
        (message.targetUserId.empty() ||
         AddStringMember("targetUserId", message.targetUserId, false, request)) &&
        Finish(request);
    if (!built) {
        request.clear();
    }
    return built;
}

bool BuildReportMessageRequest(
    std::string_view token,
    const ChatReportRequest& report,
    std::string& request) noexcept {
    return BeginOperation("report", token, request) &&
        AddStringMember("messageId", report.messageId, false, request) &&
        AddStringMember("reason", report.reason, false, request) &&
        AddStringMember("details", "", false, request) &&
        Finish(request);
}

bool BuildModerationRequest(
    std::string_view token,
    const ModerationRequest& moderation,
    std::string& request) noexcept {
    const bool built = BeginOperation("moderationAction", token, request) &&
        AddStringMember("action", moderation.action, false, request) &&
        (moderation.messageId.empty() ||
         AddStringMember("messageId", moderation.messageId, false, request)) &&
        (moderation.targetUserId.empty() ||
         AddStringMember("targetUserId", moderation.targetUserId, false, request)) &&
        (moderation.reason.empty() ||
         AddStringMember("reason", moderation.reason, false, request)) &&
        Finish(request);
    if (!built) {
        request.clear();
    }
    return built;
}

bool BuildSubscribeRequest(
    std::string_view token,
    std::string_view clientVersion,
    std::uint64_t cursor,
    std::string& request) noexcept {
    if (cursor > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    return BeginOperation("subscribe", token, request) &&
        AddStringMember("clientVersion", clientVersion, false, request) &&
        AddIntegerMember(
            "cursor", static_cast<std::int64_t>(cursor), false, request) &&
        AddIntegerMember("max", 64, false, request) &&
        Finish(request);
}

AuthReply ParseAuthReply(std::string_view response) noexcept {
    AuthReply reply;
    json::Document document;
    if (!json::ParseDocument(
            response, kRelayJsonOptions, document) ||
        document.Root().GetType() != json::Type::Object) {
        return reply;
    }
    const auto root = document.Root();
    bool success{};
    if (root.ReadBooleanMember("success", success) !=
        json::MemberStatus::Found) {
        return reply;
    }
    reply.parsed = true;
    reply.success = success;
    try {
        CopyStringMember(root, "userId", reply.userId);
        CopyStringMember(root, "token", reply.token);
        CopyStringMember(root, "role", reply.role);
        CopyStringMember(root, "createdAt", reply.createdAt);
        CopyStringMember(root, "code", reply.code);
        CopyStringMember(root, "message", reply.message);
    } catch (...) {
        return {};
    }
    if (reply.role.empty()) {
        reply.role = "user";
    }
    return reply;
}

RelayOperationReply ParseRelayOperationReply(
    std::string_view response) noexcept {
    RelayOperationReply reply;
    json::Document document;
    if (!json::ParseDocument(response, kRelayJsonOptions, document)) {
        return reply;
    }
    const auto root = document.Root();
    if (root.GetType() != json::Type::Object ||
        root.ReadBooleanMember("success", reply.success) !=
            json::MemberStatus::Found) {
        return reply;
    }
    reply.parsed = true;
    try {
        CopyStringMember(root, "messageId", reply.messageId);
        CopyStringMember(root, "code", reply.code);
        CopyStringMember(root, "message", reply.message);
    } catch (...) {
        return {};
    }
    return reply;
}

SubscriberReply ParseSubscriberReply(
    std::string_view response) noexcept {
    SubscriberReply reply;
    json::Document document;
    if (!json::ParseDocument(response, kRelayJsonOptions, document) ||
        document.Root().GetType() != json::Type::Object) {
        return reply;
    }
    const auto root = document.Root();
    reply.parsed = true;
    try {
        std::string_view operation;
        bool success{};
        if (root.ReadStringMember("op", operation) ==
                json::MemberStatus::Found &&
            operation == "subscribed") {
            if (root.ReadBooleanMember("success", success) ==
                    json::MemberStatus::Found) {
                reply.rejected = !success;
            }
        }
        (void)ReadUnsignedMember(root, "cursor", reply.cursor);
        CopyStringMember(root, "code", reply.code);
        CopyStringMember(root, "message", reply.message);
        FindChatEvents(root, reply.events);
    } catch (...) {
        return {};
    }
    return reply;
}

}  // namespace sf::chat
