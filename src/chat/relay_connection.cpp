#include "chat/relay_connection.hpp"

#include "config/config.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <limits>

namespace sf::chat {
namespace {

constexpr int kConnectTimeoutMs = 5'000;
constexpr int kIoTimeoutMs = 15'000;
constexpr std::size_t kReceiveChunkSize = 4'096;

[[nodiscard]] RelayResult Result(
    RelayStatus status,
    unsigned long systemError = 0) noexcept {
    RelayResult result;
    result.status = status;
    result.systemError = systemError;
    return result;
}

void CloseHandle(std::atomic<void*>& slot) noexcept {
    void* const handle = slot.exchange(nullptr, std::memory_order_acq_rel);
    if (handle != nullptr) {
        (void)::WinHttpCloseHandle(static_cast<HINTERNET>(handle));
    }
}

[[nodiscard]] bool IsAscii(std::string_view text) noexcept {
    for (const unsigned char character : text) {
        if (character > 0x7F) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ToWide(
    std::string_view text,
    std::wstring& wide) noexcept {
    if (!IsAscii(text)) {
        return false;
    }
    try {
        wide.assign(text.begin(), text.end());
        return true;
    } catch (...) {
        wide.clear();
        return false;
    }
}

[[nodiscard]] bool ParsePort(
    std::string_view text,
    std::uint16_t& port) noexcept {
    if (text.empty()) {
        return false;
    }
    unsigned int value{};
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        value = value * 10U + static_cast<unsigned int>(character - '0');
        if (value > (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace

bool ParseRelayEndpoint(
    std::string_view endpoint,
    RelayEndpoint& parsed) noexcept {
    parsed = {};
    constexpr std::string_view prefix = "wss://";
    if (!endpoint.starts_with(prefix)) {
        return false;
    }
    endpoint.remove_prefix(prefix.size());
    if (endpoint.empty() || !IsAscii(endpoint)) {
        return false;
    }

    const auto slash = endpoint.find('/');
    const std::string_view authority = endpoint.substr(0, slash);
    const std::string_view target = slash == std::string_view::npos
        ? std::string_view{"/"}
        : endpoint.substr(slash);
    if (authority.empty() || target.empty()) {
        return false;
    }

    std::string_view host = authority;
    std::string_view portText;
    bool hasExplicitPort{};
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1) {
            return false;
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return false;
            }
            hasExplicitPort = true;
            portText = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) {
                return false;
            }
            hasExplicitPort = true;
            host = authority.substr(0, colon);
            portText = authority.substr(colon + 1);
        }
    }
    if (host.empty() ||
        (hasExplicitPort && !ParsePort(portText, parsed.port)) ||
        !ToWide(host, parsed.host) || !ToWide(target, parsed.target)) {
        parsed = {};
        return false;
    }
    return true;
}

RelayConnection::~RelayConnection() {
    Close();
}

RelayResult RelayConnection::Open(
    const RelayEndpoint& endpoint,
    const std::atomic_bool& stopRequested) noexcept {
    Close();
    if (stopRequested.load(std::memory_order_acquire)) {
        return Result(RelayStatus::Stopped);
    }

    HINTERNET session = ::WinHttpOpen(
        L"ZFE",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (session == nullptr) {
        return Result(RelayStatus::ConnectFailed, ::GetLastError());
    }
    session_.store(session, std::memory_order_release);
    if (::WinHttpSetTimeouts(
            session,
            kConnectTimeoutMs,
            kConnectTimeoutMs,
            kIoTimeoutMs,
            kIoTimeoutMs) == FALSE) {
        const auto error = ::GetLastError();
        Close();
        return Result(RelayStatus::ConnectFailed, error);
    }

    HINTERNET connection = ::WinHttpConnect(
        session,
        endpoint.host.c_str(),
        endpoint.port,
        0);
    if (connection == nullptr) {
        const auto error = ::GetLastError();
        Close();
        return Result(RelayStatus::ConnectFailed, error);
    }
    connection_.store(connection, std::memory_order_release);
    if (stopRequested.load(std::memory_order_acquire)) {
        Close();
        return Result(RelayStatus::Stopped);
    }

    HINTERNET request = ::WinHttpOpenRequest(
        connection,
        L"GET",
        endpoint.target.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        const auto error = ::GetLastError();
        Close();
        return Result(RelayStatus::ConnectFailed, error);
    }
    request_.store(request, std::memory_order_release);
    if (::WinHttpSetOption(
            request,
            WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
            nullptr,
            0) == FALSE ||
        ::WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) == FALSE ||
        ::WinHttpReceiveResponse(request, nullptr) == FALSE) {
        const auto error = ::GetLastError();
        Close();
        return Result(RelayStatus::ConnectFailed, error);
    }

    unsigned long status{};
    unsigned long statusSize = sizeof(status);
    if (::WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) == FALSE ||
        status != HTTP_STATUS_SWITCH_PROTOCOLS) {
        const auto error = ::GetLastError();
        Close();
        return Result(RelayStatus::ProtocolError, error);
    }

    HINTERNET socket = ::WinHttpWebSocketCompleteUpgrade(request, 0);
    if (socket == nullptr) {
        const auto error = ::GetLastError();
        Close();
        return Result(RelayStatus::ConnectFailed, error);
    }
    socket_.store(socket, std::memory_order_release);
    CloseHandle(request_);
    if (stopRequested.load(std::memory_order_acquire)) {
        Close();
        return Result(RelayStatus::Stopped);
    }
    return Result(RelayStatus::Ok);
}

RelayResult RelayConnection::SendText(
    std::string_view message,
    const std::atomic_bool& stopRequested) noexcept {
    if (stopRequested.load(std::memory_order_acquire)) {
        return Result(RelayStatus::Stopped);
    }
    if (message.size() > (std::numeric_limits<unsigned long>::max)()) {
        return Result(RelayStatus::MessageTooLarge);
    }
    auto* const socket = socket_.load(std::memory_order_acquire);
    if (socket == nullptr) {
        return Result(RelayStatus::Closed);
    }
    const auto error = ::WinHttpWebSocketSend(
        static_cast<HINTERNET>(socket),
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message.data()),
        static_cast<unsigned long>(message.size()));
    return error == ERROR_SUCCESS
        ? Result(RelayStatus::Ok)
        : Result(stopRequested.load(std::memory_order_acquire)
            ? RelayStatus::Stopped
            : RelayStatus::WriteFailed,
            error);
}

RelayResult RelayConnection::ReceiveText(
    std::size_t maximumBytes,
    const std::atomic_bool& stopRequested) noexcept {
    if (maximumBytes == 0) {
        return Result(RelayStatus::MessageTooLarge);
    }
    RelayResult result = Result(RelayStatus::ReadFailed);
    auto* const socket = socket_.load(std::memory_order_acquire);
    if (socket == nullptr) {
        return Result(RelayStatus::Closed);
    }
    try {
        result.message.reserve((std::min)(maximumBytes, kReceiveChunkSize));
    } catch (...) {
        return Result(RelayStatus::MessageTooLarge);
    }

    std::array<char, kReceiveChunkSize> buffer{};
    for (;;) {
        if (stopRequested.load(std::memory_order_acquire)) {
            return Result(RelayStatus::Stopped);
        }
        unsigned long received{};
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const auto error = ::WinHttpWebSocketReceive(
            static_cast<HINTERNET>(socket),
            buffer.data(),
            static_cast<unsigned long>(buffer.size()),
            &received,
            &type);
        if (error != ERROR_SUCCESS) {
            return Result(stopRequested.load(std::memory_order_acquire)
                ? RelayStatus::Stopped
                : RelayStatus::ReadFailed,
                error);
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            result.status = RelayStatus::Closed;
            result.message.clear();
            unsigned short closeCode{};
            unsigned long reasonLength{};
            const auto closeError = ::WinHttpWebSocketQueryCloseStatus(
                static_cast<HINTERNET>(socket),
                &closeCode,
                buffer.data(),
                static_cast<unsigned long>(buffer.size()),
                &reasonLength);
            if (closeError == ERROR_SUCCESS) {
                result.webSocketCloseCode = closeCode;
                try {
                    result.webSocketCloseReason.assign(
                        buffer.data(), reasonLength);
                } catch (...) {
                    result.webSocketCloseReason.clear();
                }
            } else {
                result.systemError = closeError;
            }
            return result;
        }
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
            type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            return Result(RelayStatus::ProtocolError);
        }
        if (received > maximumBytes - result.message.size()) {
            return Result(RelayStatus::MessageTooLarge);
        }
        try {
            result.message.append(buffer.data(), received);
        } catch (...) {
            return Result(RelayStatus::MessageTooLarge);
        }
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            result.status = RelayStatus::Ok;
            result.systemError = 0;
            return result;
        }
    }
}

void RelayConnection::Interrupt() noexcept {
    CloseHandle(socket_);
    CloseHandle(request_);
    CloseHandle(connection_);
    CloseHandle(session_);
}

void RelayConnection::Close(bool stopRequested) noexcept {
    void* const socket = socket_.exchange(nullptr, std::memory_order_acq_rel);
    if (socket != nullptr) {
        if (!stopRequested) {
            (void)::WinHttpWebSocketClose(
                static_cast<HINTERNET>(socket),
                WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                nullptr,
                0);
        }
        (void)::WinHttpCloseHandle(static_cast<HINTERNET>(socket));
    }
    CloseHandle(request_);
    CloseHandle(connection_);
    CloseHandle(session_);
}

RelayResult SendRelayRequest(
    RelayConnection& connection,
    std::string_view request,
    std::size_t maximumResponseBytes,
    const std::atomic_bool& stopRequested) noexcept {
    RelayEndpoint endpoint;
    if (!ParseRelayEndpoint(
            config::GetRuntimeConfig().chat.Endpoint(), endpoint)) {
        return Result(RelayStatus::InvalidEndpoint);
    }
    auto result = connection.Open(endpoint, stopRequested);
    if (!result.Succeeded()) {
        connection.Close(stopRequested.load(std::memory_order_acquire));
        return result;
    }
    result = connection.SendText(request, stopRequested);
    if (!result.Succeeded()) {
        connection.Close(stopRequested.load(std::memory_order_acquire));
        return result;
    }
    result = connection.ReceiveText(maximumResponseBytes, stopRequested);
    connection.Close(stopRequested.load(std::memory_order_acquire));
    return result;
}

}  // namespace sf::chat
