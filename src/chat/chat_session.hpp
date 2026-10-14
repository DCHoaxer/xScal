#pragma once

#include "chat/chat_auth_file.hpp"
#include "chat/chat_types.hpp"
#include "chat/relay_connection.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace sf::chat {

class ChatSession final {
public:
    ChatSession() noexcept = default;
    ~ChatSession();

    ChatSession(const ChatSession&) = delete;
    ChatSession& operator=(const ChatSession&) = delete;

    [[nodiscard]] bool Start() noexcept;
    void Stop() noexcept;

    [[nodiscard]] std::string RuntimeInfo() const noexcept;
    [[nodiscard]] std::string Connect(const ConnectOptions& options) noexcept;
    [[nodiscard]] std::string State() const noexcept;
    [[nodiscard]] std::string SendChatMessage(
        const ChatMessageRequest& message) noexcept;
    [[nodiscard]] std::string ReportMessage(
        const ChatReportRequest& report) noexcept;
    [[nodiscard]] std::string SubmitModeration(
        const ModerationRequest& moderation) noexcept;
    [[nodiscard]] std::string PollEvents(
        std::uint32_t maximum,
        std::uint64_t cursor) noexcept;
    [[nodiscard]] std::string Disconnect() noexcept;
    [[nodiscard]] std::string Logout() noexcept;

private:
    friend struct ChatSessionTestAccess;

    struct TransportCounts final {
        std::uint64_t requestsSent{};
        std::uint64_t requestsSucceeded{};
        std::uint64_t requestsFailed{};
        std::string lastError;
        unsigned long lastSystemError{};
        std::uint16_t lastWebSocketCloseCode{};
        std::string lastWebSocketCloseReason;
    };

    struct SubscriberState final {
        bool active{};
        std::uint64_t reconnectAttempts{};
        std::string lastError;
        std::uint32_t nextReconnectDelayMs{};
        std::uint64_t messagesReceived{};
        std::uint64_t pingsReceived{};
        std::uint64_t pongsReceived{};
        std::uint64_t pongsSent{};
    };

    enum class RelayOperationKind {
        Send,
        Report,
        Moderation,
    };

    struct PendingOperation final {
        RelayOperationKind kind{RelayOperationKind::Send};
        std::uint64_t requestId{};
        std::string request;
        std::string channel;
    };

    struct QueuedEvent final {
        std::uint64_t id{};
        std::shared_ptr<const std::string> json;
    };

    void ConnectionWorker(ConnectOptions options) noexcept;
    void OperationWorker() noexcept;
    // registered selects the auth.registered event; saveCredential writes the
    // token to chat-auth.bin.
    [[nodiscard]] bool AcceptAuthentication(
        std::string_view userId,
        std::string_view token,
        std::string_view role,
        bool registered,
        bool saveCredential,
        std::string_view createdAt) noexcept;
    void RejectAuth(std::string_view code, std::string_view message) noexcept;
    void RunSubscriber(
        std::string token,
        std::string clientVersion) noexcept;
    [[nodiscard]] RelayResult Request(std::string_view request) noexcept;
    // resultFields is appended to the queued result object, after requestId.
    [[nodiscard]] std::string QueueOperation(
        PendingOperation operation,
        std::string_view resultFields = {}) noexcept;
    void PushOperationResult(
        PendingOperation operation,
        RelayResult relay) noexcept;
    void InterruptConnection() noexcept;
    // The caller holds connectionMutex_ through stopping and any credential deletion.
    void StopConnection(std::string_view eventKind) noexcept;
    void PushEvent(ChatEvent event) noexcept;

    mutable std::mutex stateMutex_;
    std::mutex eventMutex_;
    std::mutex connectionMutex_;
    std::mutex requestMutex_;
    std::mutex operationMutex_;
    std::condition_variable operationReady_;
    std::thread connectionThread_;
    std::thread operationThread_;
    RelayConnection subscriberConnection_;
    RelayConnection requestConnection_;
    std::atomic_bool started_{};
    std::atomic_bool connectionStop_{};
    std::atomic_bool connectionWorkerRunning_{};
    std::atomic_bool operationStop_{};

    AuthState authState_{AuthState::Unauthenticated};
    bool connected_{};
    std::string userId_;
    std::string displayName_;
    std::string clientVersion_;
    std::string token_;
    std::string role_{"user"};
    ChatPermissions permissions_{};
    TransportCounts transport_{};
    SubscriberState subscriber_{};
    std::deque<QueuedEvent> events_;
    std::deque<PendingOperation> pendingOperations_;
    std::uint64_t nextEventId_{};
    std::uint64_t nextRequestId_{};
    std::uint64_t pendingDroppedEvents_{};
    // Highest event ID returned by PollEvents. Evicted events at or below this ID
    // were already seen by the caller and are not reported as dropped.
    std::uint64_t polledEventId_{};
    std::atomic_uint64_t droppedEvents_{};
    std::uint64_t relayCursor_{};
    std::unique_ptr<AuthFile> authFile_;
};

}  // namespace sf::chat
