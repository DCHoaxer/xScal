#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sf::chat {

struct RelayEndpoint final {
    std::wstring host;
    std::wstring target;
    std::uint16_t port{443};
};

enum class RelayStatus {
    Ok,
    Stopped,
    InvalidEndpoint,
    ConnectFailed,
    WriteFailed,
    ReadFailed,
    ProtocolError,
    MessageTooLarge,
    Closed,
};

struct RelayResult final {
    RelayStatus status{RelayStatus::ProtocolError};
    unsigned long systemError{};
    std::uint16_t webSocketCloseCode{};
    std::string webSocketCloseReason;
    std::string message;

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == RelayStatus::Ok;
    }
};

[[nodiscard]] bool ParseRelayEndpoint(
    std::string_view endpoint,
    RelayEndpoint& parsed) noexcept;

class RelayConnection final {
public:
    RelayConnection() noexcept = default;
    ~RelayConnection();

    RelayConnection(const RelayConnection&) = delete;
    RelayConnection& operator=(const RelayConnection&) = delete;

    [[nodiscard]] RelayResult Open(
        const RelayEndpoint& endpoint,
        const std::atomic_bool& stopRequested) noexcept;
    [[nodiscard]] RelayResult SendText(
        std::string_view message,
        const std::atomic_bool& stopRequested) noexcept;
    [[nodiscard]] RelayResult ReceiveText(
        std::size_t maximumBytes,
        const std::atomic_bool& stopRequested) noexcept;

    void Interrupt() noexcept;
    // Pass stopRequested as true to skip the WebSocket close frame. The frame
    // wait can last until the I/O timeout, and a stop must not wait for it.
    void Close(bool stopRequested = false) noexcept;

private:
    std::atomic<void*> session_{};
    std::atomic<void*> connection_{};
    std::atomic<void*> request_{};
    std::atomic<void*> socket_{};
};

[[nodiscard]] RelayResult SendRelayRequest(
    RelayConnection& connection,
    std::string_view request,
    std::size_t maximumResponseBytes,
    const std::atomic_bool& stopRequested) noexcept;

}  // namespace sf::chat
