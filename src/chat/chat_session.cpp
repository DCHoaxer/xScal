#include "chat/chat_session.hpp"

#include "chat/relay_protocol.hpp"
#include "common/json.hpp"
#include "config/config.h"
#include "platform/diagnostics.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace sf::chat {
namespace {

constexpr std::size_t kMaximumResultBytes = 40 * 1024;
constexpr std::size_t kMaximumPendingOperations = 16;

[[nodiscard]] bool AddString(
    std::string_view text,
    std::string& output) noexcept {
    return json::AppendString(text, output, kMaximumResultBytes);
}

[[nodiscard]] std::string ErrorResult(
    std::string_view code,
    std::string_view message) noexcept {
    std::string output;
    if (!AppendLiteral("{\"success\":false,\"code\":", kMaximumResultBytes, output) ||
        !AddString(code, output) ||
        !AppendLiteral(",\"message\":", kMaximumResultBytes, output) ||
        !AddString(message, output) ||
        !AppendLiteral("}", kMaximumResultBytes, output)) {
        return R"({"success":false,"code":"allocation_failure","message":"Could not construct result"})";
    }
    return output;
}

[[nodiscard]] const char* RelayStatusCode(RelayStatus status) noexcept {
    switch (status) {
    case RelayStatus::Stopped:
        return "stopped";
    case RelayStatus::InvalidEndpoint:
        return "invalid_endpoint";
    case RelayStatus::ConnectFailed:
        return "connect_failed";
    case RelayStatus::WriteFailed:
        return "write_failed";
    case RelayStatus::ReadFailed:
        return "read_failed";
    case RelayStatus::ProtocolError:
        return "protocol_error";
    case RelayStatus::MessageTooLarge:
        return "message_too_large";
    case RelayStatus::Closed:
        return "connection_closed";
    case RelayStatus::Ok:
        return "ok";
    }
    return "transport_error";
}

void LogRelayFailure(
    std::string_view operation,
    const RelayResult& relay) noexcept {
    if (relay.status == RelayStatus::Stopped) {
        return;
    }
    // The detail is "websocketCloseCode=N[, reason=...]" or "systemError=N".
    // It is empty when neither error field is set.
    std::string detail;
    try {
        if (relay.webSocketCloseCode != 0) {
            detail = "websocketCloseCode=" +
                std::to_string(relay.webSocketCloseCode);
            std::string quotedReason;
            if (!relay.webSocketCloseReason.empty() &&
                AddString(relay.webSocketCloseReason, quotedReason)) {
                detail += ", reason=" + quotedReason;
            }
        } else if (relay.systemError != 0) {
            detail = "systemError=" + std::to_string(relay.systemError);
        }
    } catch (...) {
        detail.clear();
    }
    DiagnosticLogFormat(
        "xScal chat: %.*s failed (status=%s%s%s)",
        static_cast<int>(operation.size()),
        operation.data(),
        RelayStatusCode(relay.status),
        detail.empty() ? "" : ", ",
        detail.c_str());
}

[[nodiscard]] bool IsAsciiWhitespace(char character) noexcept {
    return character == ' ' || character == '\t' ||
        character == '\r' || character == '\n';
}

[[nodiscard]] std::string_view TrimAscii(std::string_view text) noexcept {
    while (!text.empty() && IsAsciiWhitespace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsAsciiWhitespace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Counts code points in text that has already passed json::IsValidUtf8.
// Every code point has exactly one non-continuation byte.
[[nodiscard]] std::size_t CountUtf8Characters(std::string_view text) noexcept {
    std::size_t count{};
    for (const char byte : text) {
        if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] ChatPermissions PermissionsForRole(
    std::string_view role) noexcept {
    if (role == "moderator") {
        return {true, true, true, true, true};
    }
    ChatPermissions permissions;
    permissions.canReport = true;
    return permissions;
}

[[nodiscard]] bool AppendEvent(
    const ChatEvent& event,
    std::string& output) noexcept {
    const auto addLiteral = [&](std::string_view text) {
        return AppendLiteral(text, kMaximumResultBytes, output);
    };
    if (!addLiteral("{\"id\":") ||
        !json::AppendInteger(
            static_cast<std::int64_t>(event.id),
            output,
            kMaximumResultBytes) ||
        !addLiteral(",\"kind\":") ||
        !AddString(event.kind, output)) {
        return false;
    }
    if (event.requestId != 0 &&
        (!addLiteral(",\"requestId\":") ||
         !json::AppendInteger(
             static_cast<std::int64_t>(event.requestId),
             output,
             kMaximumResultBytes))) {
        return false;
    }
    const auto addOptional = [&](std::string_view name, std::string_view value) {
        return value.empty() ||
            (addLiteral(",\"") && addLiteral(name) &&
             addLiteral("\":") && AddString(value, output));
    };
    if (!addOptional("status", event.status) ||
        !addOptional("channel", event.channel) ||
        !addOptional("senderUserId", event.senderUserId) ||
        !addOptional("senderDisplayName", event.senderDisplayName) ||
        !addOptional("nameColor", event.nameColor) ||
        !addOptional("body", event.body) ||
        !addOptional("messageId", event.messageId) ||
        !addOptional("createdAt", event.createdAt) ||
        !addOptional("targetUserId", event.targetUserId) ||
        !addOptional("code", event.code) ||
        !addOptional("message", event.message)) {
        return false;
    }
    if ((!event.errorCode.empty() || !event.errorMessage.empty()) &&
        (!addLiteral(",\"error\":{\"code\":") ||
         !AddString(event.errorCode, output) ||
         !addLiteral(",\"message\":") ||
         !AddString(event.errorMessage, output) ||
         !addLiteral("}"))) {
        return false;
    }
    if (event.dropped != 0 &&
        (!addLiteral(",\"queue\":\"chatInterface\",\"dropped\":") ||
         !json::AppendInteger(
             static_cast<std::int64_t>(event.dropped),
             output,
             kMaximumResultBytes))) {
        return false;
    }
    return addLiteral("}");
}

[[nodiscard]] bool IsKnownModerationAction(
    std::string_view action) noexcept {
    constexpr std::array<std::string_view, 6> actions{
        "deleteMessage", "muteUser", "unmuteUser", "banUser",
        "unbanUser", "setSlowMode"};
    return std::find(actions.begin(), actions.end(), action) != actions.end();
}

// Extra result fields for moderation requests whose duration or category
// was supplied by FCM. The relay never receives these two values.
[[nodiscard]] std::string_view ModerationAppliedFields(
    const ModerationRequest& moderation) noexcept {
    if (moderation.durationSupplied) {
        return moderation.categorySupplied
            ? R"(,"durationApplied":false,"categoryApplied":false)"
            : R"(,"durationApplied":false)";
    }
    return moderation.categorySupplied ? R"(,"categoryApplied":false)" : "";
}

class ConnectionWorkerLifetime final {
public:
    explicit ConnectionWorkerLifetime(std::atomic_bool& running) noexcept
        : running_{running} {}

    ~ConnectionWorkerLifetime() {
        running_.store(false, std::memory_order_release);
    }

    ConnectionWorkerLifetime(const ConnectionWorkerLifetime&) = delete;
    ConnectionWorkerLifetime& operator=(
        const ConnectionWorkerLifetime&) = delete;

private:
    std::atomic_bool& running_;
};

}  // namespace

ChatSession::~ChatSession() {
    Stop();
}

bool ChatSession::Start() noexcept {
    std::scoped_lock connectionLock{connectionMutex_};
    bool expected = false;
    if (!started_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return true;
    }
    connectionStop_.store(false, std::memory_order_release);
    operationStop_.store(false, std::memory_order_release);
    try {
        std::wstring path;
        if (FindProductionAuthFilePath(path)) {
            authFile_ = std::make_unique<AuthFile>(
                std::move(path), WindowsAuthFilePlatform());
        }
        return true;
    } catch (...) {
        started_.store(false, std::memory_order_release);
        authFile_.reset();
        return false;
    }
}

void ChatSession::Stop() noexcept {
    InterruptConnection();
    std::scoped_lock connectionLock{connectionMutex_};
    if (!started_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    StopConnection({});
    std::scoped_lock lock{stateMutex_};
    WipeString(token_);
    userId_.clear();
    displayName_.clear();
    clientVersion_.clear();
    role_ = "user";
    permissions_ = {};
    authState_ = AuthState::Unauthenticated;
    connected_ = false;
    authFile_.reset();
}

std::string ChatSession::RuntimeInfo() const noexcept {
    std::string output{
        R"({"success":true,"runtime":"xScal Chat","version":")"};
    const auto addLiteral = [&](std::string_view text) {
        return AppendLiteral(text, kMaximumResultBytes, output);
    };
    if (!addLiteral(std::to_string(config::kXScalVersion.major)) ||
        !addLiteral(".") ||
        !addLiteral(std::to_string(config::kXScalVersion.minor)) ||
        !addLiteral(".") ||
        !addLiteral(std::to_string(config::kXScalVersion.patch)) ||
        !addLiteral(
            R"(","protocol":1,"mode":"online","capabilities":["xscal-chat-interface","xscal-chat-async-control-v1"]})")) {
        return ErrorResult("allocation_failure", "Could not construct runtime info");
    }
    return output;
}

std::string ChatSession::Connect(const ConnectOptions& options) noexcept {
    std::scoped_lock connectionLock{connectionMutex_};
    if (!started_.load(std::memory_order_acquire)) {
        return ErrorResult("not_started", "Chat runtime is not active");
    }
    const auto displayName = TrimAscii(options.displayName);
    if (displayName.empty()) {
        return ErrorResult("invalid_display_name", "displayName is required");
    }
    if (options.clientVersion.size() > kMaximumClientVersionBytes) {
        return ErrorResult("invalid_client_version", "clientVersion is too long");
    }

    {
        std::scoped_lock stateLock{stateMutex_};
        if (authState_ == AuthState::Authenticated && connected_) {
            return R"({"success":true,"status":"accepted"})";
        }
        if (connectionWorkerRunning_.load(std::memory_order_acquire)) {
            return R"({"success":true,"status":"connecting"})";
        }
    }

    StopConnection({});
    connectionStop_.store(false, std::memory_order_release);
    operationStop_.store(false, std::memory_order_release);

    ConnectOptions copied = options;
    copied.displayName.assign(displayName);
    {
        std::scoped_lock stateLock{stateMutex_};
        authState_ = AuthState::Connecting;
        connected_ = false;
        displayName_ = copied.displayName;
        clientVersion_ = copied.clientVersion;
    }
    PushEvent(ChatEvent{.kind = "auth.connecting"});
    try {
        operationThread_ = std::thread{&ChatSession::OperationWorker, this};
        connectionWorkerRunning_.store(true, std::memory_order_release);
        connectionThread_ = std::thread{
            &ChatSession::ConnectionWorker, this, std::move(copied)};
    } catch (...) {
        StopConnection({});
        RejectAuth("thread_start_failed", "Could not start chat connection");
        return ErrorResult("thread_start_failed", "Could not start chat connection");
    }
    return R"({"success":true,"status":"connecting"})";
}

RelayResult ChatSession::Request(std::string_view request) noexcept {
    std::scoped_lock requestLock{requestMutex_};
    {
        std::scoped_lock stateLock{stateMutex_};
        ++transport_.requestsSent;
    }
    auto result = SendRelayRequest(
        requestConnection_,
        request,
        kMaximumRelayMessageBytes,
        connectionStop_);
    {
        std::scoped_lock stateLock{stateMutex_};
        if (result.Succeeded()) {
            ++transport_.requestsSucceeded;
            transport_.lastError.clear();
            transport_.lastSystemError = 0;
            transport_.lastWebSocketCloseCode = 0;
            transport_.lastWebSocketCloseReason.clear();
        } else {
            ++transport_.requestsFailed;
            try {
                transport_.lastError.assign(RelayStatusCode(result.status));
                transport_.lastWebSocketCloseReason.assign(
                    result.webSocketCloseReason);
            } catch (...) {
                transport_.lastError.clear();
                transport_.lastWebSocketCloseReason.clear();
            }
            transport_.lastSystemError = result.systemError;
            transport_.lastWebSocketCloseCode = result.webSocketCloseCode;
        }
    }
    return result;
}

std::string ChatSession::QueueOperation(
    PendingOperation operation,
    std::string_view resultFields) noexcept {
    std::string result;
    {
        std::scoped_lock lock{operationMutex_};
        if (operationStop_.load(std::memory_order_acquire)) {
            WipeString(operation.request);
            return ErrorResult("not_started", "Chat runtime is stopping");
        }
        if (pendingOperations_.size() == kMaximumPendingOperations) {
            WipeString(operation.request);
            return ErrorResult(
                "operation_queue_full",
                "Chat operation queue is full");
        }
        if (nextRequestId_ == static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            WipeString(operation.request);
            return ErrorResult(
                "request_id_exhausted",
                "Chat request IDs are exhausted");
        }
        const auto requestId = nextRequestId_ + 1;
        if (!AppendLiteral(
                "{\"success\":true,\"status\":\"queued\",\"requestId\":",
                kMaximumResultBytes,
                result) ||
            !json::AppendInteger(
                static_cast<std::int64_t>(requestId),
                result,
                kMaximumResultBytes) ||
            !AppendLiteral(resultFields, kMaximumResultBytes, result) ||
            !AppendLiteral("}", kMaximumResultBytes, result)) {
            WipeString(operation.request);
            return ErrorResult(
                "allocation_failure",
                "Could not construct queued operation result");
        }
        operation.requestId = requestId;
        try {
            pendingOperations_.push_back(std::move(operation));
        } catch (...) {
            WipeString(operation.request);
            return ErrorResult(
                "allocation_failure",
                "Could not queue chat operation");
        }
        nextRequestId_ = requestId;
    }
    operationReady_.notify_one();
    return result;
}

void ChatSession::PushOperationResult(
    PendingOperation operation,
    RelayResult relay) noexcept {
    // Indexed by RelayOperationKind: Send, Report, Moderation.
    struct OperationText final {
        std::string_view acceptedKind;
        std::string_view failedKind;
        std::string_view acceptedStatus;
        std::string_view rejectedMessage;
        std::string_view transportMessage;
    };
    constexpr std::array<OperationText, 3> operationText{{
        {"chat.send.accepted", "chat.send.failed", "accepted",
         "The relay rejected the chat message", "Chat transport failed"},
        {"chat.report.accepted", "chat.report.failed", "reported",
         "The relay rejected the report", "Report transport failed"},
        {"chat.moderation.accepted", "chat.moderation.failed", "submitted",
         "The relay rejected the moderation request",
         "Moderation transport failed"},
    }};
    try {
        auto reply = relay.Succeeded()
            ? ParseRelayOperationReply(relay.message)
            : RelayOperationReply{};
        const auto& text = operationText[static_cast<std::size_t>(operation.kind)];
        const bool accepted = reply.parsed && reply.success;
        ChatEvent event{
            .requestId = operation.requestId,
            .channel = std::move(operation.channel)};
        event.kind.assign(accepted ? text.acceptedKind : text.failedKind);
        event.status.assign(accepted ? text.acceptedStatus : "failed");
        if (operation.kind == RelayOperationKind::Send) {
            event.messageId = std::move(reply.messageId);
        }
        if (!accepted) {
            if (!relay.Succeeded()) {
                event.errorCode = RelayStatusCode(relay.status);
                event.errorMessage.assign(text.transportMessage);
            } else if (!reply.parsed) {
                event.errorCode = "protocol_error";
                event.errorMessage = "Relay returned an invalid response";
            } else {
                event.errorCode = reply.code.empty()
                    ? "relay_rejected"
                    : std::move(reply.code);
                event.errorMessage = reply.message.empty()
                    ? std::string{text.rejectedMessage}
                    : std::move(reply.message);
            }
        }
        WipeString(relay.message);
        PushEvent(std::move(event));
    } catch (...) {
        WipeString(relay.message);
        droppedEvents_.fetch_add(1, std::memory_order_relaxed);
        try {
            std::scoped_lock lock{eventMutex_};
            ++pendingDroppedEvents_;
        } catch (...) {
        }
    }
}

void ChatSession::OperationWorker() noexcept {
    for (;;) {
        PendingOperation operation;
        {
            std::unique_lock lock{operationMutex_};
            operationReady_.wait(lock, [this] {
                return operationStop_.load(std::memory_order_acquire) ||
                    !pendingOperations_.empty();
            });
            if (operationStop_.load(std::memory_order_acquire)) {
                return;
            }
            operation = std::move(pendingOperations_.front());
            pendingOperations_.pop_front();
        }

        auto relay = Request(operation.request);
        WipeString(operation.request);
        if (relay.status == RelayStatus::Stopped ||
            operationStop_.load(std::memory_order_acquire)) {
            WipeString(relay.message);
            continue;
        }
        PushOperationResult(std::move(operation), std::move(relay));
    }
}

void ChatSession::ConnectionWorker(ConnectOptions options) noexcept {
    ConnectionWorkerLifetime workerLifetime{connectionWorkerRunning_};
    AuthCredential saved;
    bool triedSavedToken{};
    if (authFile_ != nullptr &&
        authFile_->Load(saved) == AuthFileLoadResult::Loaded &&
        saved.server == config::GetRuntimeConfig().chat.Endpoint() &&
        !saved.token.empty()) {
        triedSavedToken = true;
        std::string request;
        if (!BuildHelloRequest(
                saved.token,
                options.displayName,
                options.clientVersion,
                request)) {
            WipeString(request);
            ClearAuthCredential(saved);
            RejectAuth("invalid_request", "Could not construct hello request");
            return;
        }
        auto relay = Request(request);
        WipeString(request);
        if (connectionStop_.load(std::memory_order_acquire)) {
            WipeString(relay.message);
            ClearAuthCredential(saved);
            return;
        }
        // Keep the saved credential on transport failures and unparseable
        // replies. Only a parsed rejection falls through to registration.
        if (!relay.Succeeded()) {
            LogRelayFailure("hello", relay);
            WipeString(relay.message);
            ClearAuthCredential(saved);
            RejectAuth("relay_unavailable", "Chat relay did not answer the saved token");
            return;
        }
        auto reply = ParseAuthReply(relay.message);
        WipeString(relay.message);
        if (!reply.parsed) {
            WipeString(reply.token);
            ClearAuthCredential(saved);
            RejectAuth("relay_unavailable", "Chat relay returned an invalid response");
            return;
        }
        if (reply.success) {
            std::string acceptedToken = reply.token.empty()
                ? saved.token
                : reply.token;
            const std::string acceptedUserId = reply.userId.empty()
                ? saved.userId
                : reply.userId;
            // A reply can carry a new token. Save it when it differs from the
            // saved one so the next start presents the token from this reply.
            const bool tokenRotated = acceptedToken != saved.token;
            const bool accepted = AcceptAuthentication(
                acceptedUserId,
                acceptedToken,
                reply.role,
                false,
                tokenRotated,
                saved.createdAt);
            WipeString(reply.token);
            ClearAuthCredential(saved);
            if (accepted) {
                RunSubscriber(std::move(acceptedToken), options.clientVersion);
            }
            WipeString(acceptedToken);
            return;
        }
        WipeString(reply.token);
    }
    ClearAuthCredential(saved);
    if (connectionStop_.load(std::memory_order_acquire)) {
        return;
    }
    if (!options.autoRegister) {
        RejectAuth(
            triedSavedToken ? "auth_token_invalid" : "no_saved_token",
            "No accepted saved chat token");
        return;
    }

    std::string request;
    if (!BuildRegisterRequest(
            options.displayName, options.clientVersion, request)) {
        WipeString(request);
        RejectAuth("invalid_request", "Could not construct registration request");
        return;
    }
    auto relay = Request(request);
    WipeString(request);
    if (!relay.Succeeded()) {
        LogRelayFailure("register", relay);
        WipeString(relay.message);
        RejectAuth(RelayStatusCode(relay.status), "Registration transport failed");
        return;
    }
    auto reply = ParseAuthReply(relay.message);
    WipeString(relay.message);
    if (!reply.parsed || !reply.success || reply.token.empty() ||
        reply.userId.empty()) {
        const std::string code = reply.code.empty()
            ? "registration_rejected"
            : reply.code;
        WipeString(reply.token);
        RejectAuth(code, reply.message);
        return;
    }
    std::string acceptedToken = std::move(reply.token);
    const bool accepted = AcceptAuthentication(
        reply.userId,
        acceptedToken,
        reply.role,
        true,
        true,
        reply.createdAt);
    if (accepted) {
        RunSubscriber(std::move(acceptedToken), options.clientVersion);
    }
    WipeString(acceptedToken);
}

void ChatSession::RejectAuth(
    std::string_view code,
    std::string_view message) noexcept {
    {
        std::scoped_lock lock{stateMutex_};
        authState_ = AuthState::Rejected;
        connected_ = false;
    }
    PushEvent(ChatEvent{
        .kind = "auth.rejected",
        .code = std::string{code},
        .message = std::string{message}});
}

bool ChatSession::AcceptAuthentication(
    std::string_view userId,
    std::string_view token,
    std::string_view role,
    bool registered,
    bool saveCredential,
    std::string_view createdAt) noexcept {
    if (connectionStop_.load(std::memory_order_acquire) ||
        userId.empty() || token.empty()) {
        return false;
    }
    try {
        {
            std::scoped_lock lock{stateMutex_};
            userId_.assign(userId);
            token_.assign(token);
            role_.assign(role.empty() ? "user" : role);
            permissions_ = PermissionsForRole(role_);
            authState_ = AuthState::Authenticated;
            connected_ = true;
            // Publish the authentication event before another thread can
            // observe connected_ and enqueue an operation completion.
            PushEvent(ChatEvent{
                .kind = registered ? "auth.registered" : "auth.accepted"});
        }
        if (saveCredential && authFile_ != nullptr) {
            AuthCredential credential{
                std::string{config::GetRuntimeConfig().chat.Endpoint()},
                std::string{userId},
                std::string{token},
                std::string{createdAt}};
            (void)authFile_->Save(credential);
            ClearAuthCredential(credential);
        }
        return true;
    } catch (...) {
        RejectAuth("allocation_failure", "Could not retain authentication state");
        return false;
    }
}

void ChatSession::RunSubscriber(
    std::string token,
    std::string clientVersion) noexcept {
    constexpr std::array<std::uint32_t, 6> reconnectDelays{
        250, 500, 1'000, 2'000, 4'000, 5'000};
    std::size_t delayIndex{};
    while (!connectionStop_.load(std::memory_order_acquire)) {
        RelayEndpoint endpoint;
        if (!ParseRelayEndpoint(
                config::GetRuntimeConfig().chat.Endpoint(), endpoint)) {
            {
                std::scoped_lock lock{stateMutex_};
                connected_ = false;
            }
            PushEvent(ChatEvent{
                .kind = "chat.error",
                .code = "invalid_endpoint",
                .message = "Chat relay endpoint is invalid"});
            break;
        }
        auto result = subscriberConnection_.Open(endpoint, connectionStop_);
        if (result.Succeeded()) {
            std::string subscribe;
            std::uint64_t cursor{};
            {
                std::scoped_lock lock{stateMutex_};
                cursor = relayCursor_;
            }
            if (!BuildSubscribeRequest(
                    token, clientVersion, cursor, subscribe)) {
                result.status = RelayStatus::ProtocolError;
            } else {
                result = subscriberConnection_.SendText(
                    subscribe, connectionStop_);
            }
            WipeString(subscribe);
        }
        if (result.Succeeded()) {
            std::scoped_lock lock{stateMutex_};
            subscriber_.active = true;
            subscriber_.lastError.clear();
            subscriber_.nextReconnectDelayMs = 0;
            delayIndex = 0;
        }
        bool subscriptionRejected{};
        while (result.Succeeded() &&
               !connectionStop_.load(std::memory_order_acquire)) {
            result = subscriberConnection_.ReceiveText(
                kMaximumRelayMessageBytes, connectionStop_);
            if (!result.Succeeded()) {
                break;
            }
            auto reply = ParseSubscriberReply(result.message);
            if (reply.rejected) {
                RejectAuth(
                    reply.code.empty() ? "subscribe_rejected" : reply.code,
                    reply.message.empty()
                        ? "The relay rejected the chat subscription"
                        : reply.message);
                subscriptionRejected = true;
                break;
            }
            if (!reply.parsed) {
                result.status = RelayStatus::ProtocolError;
                break;
            }
            {
                std::scoped_lock lock{stateMutex_};
                ++subscriber_.messagesReceived;
                relayCursor_ = (std::max)(relayCursor_, reply.cursor);
            }
            for (auto& event : reply.events) {
                PushEvent(std::move(event));
            }
        }
        subscriberConnection_.Close(connectionStop_.load(std::memory_order_acquire));
        if (connectionStop_.load(std::memory_order_acquire) ||
            subscriptionRejected) {
            break;
        }
        const auto delay = reconnectDelays[
            (std::min)(delayIndex, reconnectDelays.size() - 1)];
        if (delayIndex + 1 < reconnectDelays.size()) {
            ++delayIndex;
        }
        {
            std::scoped_lock lock{stateMutex_};
            subscriber_.active = false;
            ++subscriber_.reconnectAttempts;
            subscriber_.lastError = RelayStatusCode(result.status);
            subscriber_.nextReconnectDelayMs = delay;
        }
        std::uint32_t waited{};
        while (waited < delay &&
               !connectionStop_.load(std::memory_order_acquire)) {
            const auto slice = (std::min)(100U, delay - waited);
            std::this_thread::sleep_for(std::chrono::milliseconds{slice});
            waited += slice;
        }
    }
    subscriberConnection_.Close(connectionStop_.load(std::memory_order_acquire));
    WipeString(token);
    WipeString(clientVersion);
    std::scoped_lock lock{stateMutex_};
    subscriber_.active = false;
    subscriber_.nextReconnectDelayMs = 0;
}

std::string ChatSession::State() const noexcept {
    std::scoped_lock lock{stateMutex_};
    std::string output;
    const auto addLiteral = [&](std::string_view text) {
        return AppendLiteral(text, kMaximumResultBytes, output);
    };
    const auto addBoolean = [&](std::string_view name, bool value) {
        return addLiteral(",\"") && addLiteral(name) &&
            addLiteral(value ? "\":true" : "\":false");
    };
    const auto addUnsigned = [&](std::string_view name, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            return false;
        }
        return addLiteral(",\"") && addLiteral(name) &&
            addLiteral("\":") &&
            json::AppendInteger(
                static_cast<std::int64_t>(value), output, kMaximumResultBytes);
    };
    if (!addLiteral("{\"success\":true,\"state\":") ||
        !AddString(AuthStateName(authState_), output) ||
        !addBoolean("connected", connected_) ||
        !addLiteral(",\"endpoint\":") ||
        !AddString(config::GetRuntimeConfig().chat.Endpoint(), output) ||
        !addLiteral(",\"userId\":") || !AddString(userId_, output) ||
        !addLiteral(",\"displayName\":") ||
        !AddString(displayName_, output) ||
        !addLiteral(",\"clientVersion\":") ||
        !AddString(clientVersion_, output) ||
        !addLiteral(",\"roles\":[") || !AddString(role_, output) ||
        !addLiteral("],\"permissions\":{\"canReport\":") ||
        !addLiteral(permissions_.canReport ? "true" : "false") ||
        !addBoolean("canDeleteMessage", permissions_.canDeleteMessage) ||
        !addBoolean("canKickUser", false) ||
        !addBoolean("canMuteUser", permissions_.canMuteUser) ||
        !addBoolean("canUnmuteUser", permissions_.canMuteUser) ||
        !addBoolean("canBanUser", permissions_.canBanUser) ||
        !addBoolean("canUnbanUser", permissions_.canBanUser) ||
        !addBoolean("canSetSlowMode", permissions_.canSetSlowMode) ||
        !addLiteral("},\"transport\":{\"wssBackend\":\"winhttp\"") ||
        !addUnsigned("requestsSent", transport_.requestsSent) ||
        !addUnsigned("requestsSucceeded", transport_.requestsSucceeded) ||
        !addUnsigned("requestsFailed", transport_.requestsFailed) ||
        !addLiteral(",\"lastError\":") ||
        !AddString(transport_.lastError, output) ||
        !addUnsigned("lastSystemError", transport_.lastSystemError) ||
        !addUnsigned(
            "lastWebSocketCloseCode",
            transport_.lastWebSocketCloseCode) ||
        !addLiteral(",\"lastWebSocketCloseReason\":") ||
        !AddString(transport_.lastWebSocketCloseReason, output) ||
        !addLiteral("},\"liveSubscriber\":{\"active\":") ||
        !addLiteral(subscriber_.active ? "true" : "false") ||
        !addUnsigned("reconnectAttempts", subscriber_.reconnectAttempts) ||
        !addLiteral(",\"lastError\":") ||
        !AddString(subscriber_.lastError, output) ||
        !addUnsigned(
            "nextReconnectDelayMs", subscriber_.nextReconnectDelayMs) ||
        !addLiteral(",\"stats\":{\"messagesReceived\":") ||
        !json::AppendInteger(
            static_cast<std::int64_t>(subscriber_.messagesReceived),
            output,
            kMaximumResultBytes) ||
        !addUnsigned("pingsReceived", subscriber_.pingsReceived) ||
        !addUnsigned("pongsReceived", subscriber_.pongsReceived) ||
        !addUnsigned("pongsSent", subscriber_.pongsSent) ||
        !addUnsigned(
            "droppedEvents",
            droppedEvents_.load(std::memory_order_relaxed)) ||
        !addLiteral("}}}")) {
        return ErrorResult("allocation_failure", "Could not construct state");
    }
    return output;
}

std::string ChatSession::SendChatMessage(
    const ChatMessageRequest& message) noexcept {
    std::scoped_lock connectionLock{connectionMutex_};
    if (!IsSendableChannel(message.channel)) {
        return ErrorResult("invalid_channel", "Channel is not sendable");
    }
    if (message.body.empty() || !json::IsValidUtf8(message.body)) {
        return ErrorResult("invalid_message", "Message body is not valid UTF-8");
    }
    if (CountUtf8Characters(message.body) > kMaximumChatBodyCharacters) {
        return ErrorResult("message_too_long", "Message body exceeds 512 characters");
    }
    std::string token;
    {
        std::scoped_lock lock{stateMutex_};
        if (!connected_ || authState_ != AuthState::Authenticated) {
            return ErrorResult("not_connected", "Chat is not authenticated");
        }
        token = token_;
    }
    std::string request;
    if (!BuildSendMessageRequest(token, message, request)) {
        WipeString(token);
        return ErrorResult("invalid_request", "Could not construct send request");
    }
    WipeString(token);

    return QueueOperation(PendingOperation{
        .kind = RelayOperationKind::Send,
        .request = std::move(request),
        .channel = message.channel});
}

std::string ChatSession::ReportMessage(
    const ChatReportRequest& report) noexcept {
    std::scoped_lock connectionLock{connectionMutex_};
    std::string token;
    {
        std::scoped_lock lock{stateMutex_};
        if (!connected_) {
            return ErrorResult("not_connected", "Chat is not authenticated");
        }
        if (!permissions_.canReport) {
            return ErrorResult("permission_denied", "Report permission is absent");
        }
        token = token_;
    }
    std::string request;
    if (report.messageId.empty() ||
        !BuildReportMessageRequest(token, report, request)) {
        WipeString(token);
        return ErrorResult("invalid_request", "messageId is required");
    }
    WipeString(token);
    return QueueOperation(PendingOperation{
        .kind = RelayOperationKind::Report,
        .request = std::move(request)});
}

std::string ChatSession::SubmitModeration(
    const ModerationRequest& moderation) noexcept {
    std::scoped_lock connectionLock{connectionMutex_};
    if (!IsKnownModerationAction(moderation.action)) {
        return ErrorResult("invalid_action", "Moderation action is not supported");
    }
    if (moderation.action == "deleteMessage" &&
        moderation.messageId.empty()) {
        return ErrorResult("invalid_request", "messageId is required");
    }
    if (moderation.action != "deleteMessage" &&
        moderation.action != "setSlowMode" &&
        moderation.targetUserId.empty()) {
        return ErrorResult("invalid_request", "targetUserId is required");
    }
    std::string token;
    {
        std::scoped_lock lock{stateMutex_};
        if (!connected_) {
            return ErrorResult("not_connected", "Chat is not authenticated");
        }
        const bool permitted = moderation.action == "deleteMessage"
            ? permissions_.canDeleteMessage
            : moderation.action == "setSlowMode"
                ? permissions_.canSetSlowMode
                : moderation.action == "muteUser" ||
                      moderation.action == "unmuteUser"
                    ? permissions_.canMuteUser
                    : permissions_.canBanUser;
        if (!permitted) {
            return ErrorResult("permission_denied", "Moderation permission is absent");
        }
        token = token_;
    }
    std::string request;
    if (!BuildModerationRequest(token, moderation, request)) {
        WipeString(token);
        return ErrorResult("invalid_request", "Could not construct moderation request");
    }
    WipeString(token);
    return QueueOperation(
        PendingOperation{
            .kind = RelayOperationKind::Moderation,
            .request = std::move(request)},
        ModerationAppliedFields(moderation));
}

void ChatSession::PushEvent(ChatEvent event) noexcept {
    std::scoped_lock lock{eventMutex_};
    event.id = ++nextEventId_;
    std::string serialized;
    if (!AppendEvent(event, serialized)) {
        ++pendingDroppedEvents_;
        droppedEvents_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        auto json = std::make_shared<const std::string>(
            std::move(serialized));
        if (events_.size() == kMaximumQueuedEvents) {
            const std::uint64_t evictedId = events_.front().id;
            events_.pop_front();
            if (evictedId > polledEventId_) {
                ++pendingDroppedEvents_;
                droppedEvents_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        events_.push_back(QueuedEvent{event.id, std::move(json)});
    } catch (...) {
        ++pendingDroppedEvents_;
        droppedEvents_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::string ChatSession::PollEvents(
    std::uint32_t maximum,
    std::uint64_t cursor) noexcept {
    maximum = (std::min)(maximum == 0 ? 64U : maximum, kMaximumPollEvents);
    std::array<QueuedEvent, kMaximumPollEvents> selected;
    std::uint32_t selectedCount{};
    std::uint64_t droppedToReport{};
    {
        std::scoped_lock lock{eventMutex_};
        for (const auto& event : events_) {
            if (event.id <= cursor || selectedCount == maximum) {
                continue;
            }
            selected[selectedCount++] = event;
        }
        if (pendingDroppedEvents_ != 0 && selectedCount < maximum) {
            droppedToReport = pendingDroppedEvents_;
            ChatEvent dropped{
                .id = ++nextEventId_,
                .kind = "events.dropped",
                .dropped = droppedToReport};
            std::string serialized;
            if (!AppendEvent(dropped, serialized)) {
                return ErrorResult(
                    "allocation_failure",
                    "Could not construct events");
            }
            try {
                selected[selectedCount++] = QueuedEvent{
                    dropped.id,
                    std::make_shared<const std::string>(std::move(serialized))};
            } catch (...) {
                return ErrorResult(
                    "allocation_failure",
                    "Could not construct events");
            }
            pendingDroppedEvents_ = 0;
        }
    }

    // The dropped notice is always the last selected entry. If it is not
    // returned, its count goes back to pendingDroppedEvents_.
    const auto restoreDroppedNotice = [&]() noexcept {
        if (droppedToReport == 0) {
            return;
        }
        try {
            std::scoped_lock lock{eventMutex_};
            pendingDroppedEvents_ += droppedToReport;
        } catch (...) {
        }
    };
    std::string output;
    if (!AppendLiteral(
            "{\"success\":true,\"events\":[", kMaximumResultBytes, output)) {
        restoreDroppedNotice();
        return ErrorResult("allocation_failure", "Could not construct events");
    }
    std::uint32_t emitted{};
    for (; emitted < selectedCount; ++emitted) {
        const std::string& json = *selected[emitted].json;
        const std::size_t separatorBytes = emitted == 0 ? 0 : 1;
        // Reserve two bytes for the closing "]}".
        if (json.size() + separatorBytes + 2 > kMaximumResultBytes - output.size()) {
            break;
        }
        if ((emitted != 0 &&
             !AppendLiteral(",", kMaximumResultBytes, output)) ||
            !AppendLiteral(json, kMaximumResultBytes, output)) {
            restoreDroppedNotice();
            return ErrorResult("allocation_failure", "Could not construct events");
        }
    }
    if (!AppendLiteral("]}", kMaximumResultBytes, output)) {
        restoreDroppedNotice();
        return ErrorResult("allocation_failure", "Could not construct events");
    }
    if (emitted < selectedCount) {
        restoreDroppedNotice();
    }
    if (emitted != 0) {
        try {
            std::scoped_lock lock{eventMutex_};
            polledEventId_ = (std::max)(polledEventId_, selected[emitted - 1].id);
        } catch (...) {
        }
    }
    return output;
}

void ChatSession::InterruptConnection() noexcept {
    connectionStop_.store(true, std::memory_order_release);
    subscriberConnection_.Interrupt();
    requestConnection_.Interrupt();
}

void ChatSession::StopConnection(std::string_view eventKind) noexcept {
    InterruptConnection();
    {
        std::scoped_lock lock{operationMutex_};
        operationStop_.store(true, std::memory_order_release);
        for (auto& operation : pendingOperations_) {
            WipeString(operation.request);
        }
        pendingOperations_.clear();
    }
    operationReady_.notify_all();
    if (connectionThread_.joinable()) {
        connectionThread_.join();
    }
    // A dequeued operation may still be waiting for requestMutex_. Join it before
    // Connect resets cancellation and starts another authenticated session.
    if (operationThread_.joinable()) {
        operationThread_.join();
    }
    connectionWorkerRunning_.store(false, std::memory_order_release);
    {
        std::scoped_lock stateLock{stateMutex_};
        WipeString(token_);
        userId_.clear();
        role_ = "user";
        permissions_ = {};
        connected_ = false;
        authState_ = AuthState::Unauthenticated;
        subscriber_.active = false;
    }
    if (!eventKind.empty()) {
        PushEvent(ChatEvent{.kind = std::string{eventKind}});
    }
}

std::string ChatSession::Disconnect() noexcept {
    // Publish cancellation before waiting for a concurrent lifecycle call.
    InterruptConnection();
    std::scoped_lock connectionLock{connectionMutex_};
    StopConnection("auth.disconnected");
    return State();
}

std::string ChatSession::Logout() noexcept {
    InterruptConnection();
    std::scoped_lock connectionLock{connectionMutex_};
    StopConnection("auth.logout");
    if (authFile_ != nullptr && !authFile_->Logout()) {
        return ErrorResult("credential_delete_failed", "Could not delete chat-auth.bin");
    }
    return State();
}

}  // namespace sf::chat
