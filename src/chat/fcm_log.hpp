#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sf::chat {

inline constexpr std::size_t kMaximumFcmLogLineBytes = 960;

// Prefixes one message from a chat mod and replaces control characters with spaces so
// DiagnosticLog writes exactly one timestamped line.
[[nodiscard]] bool BuildChatLogLine(
    std::string_view prefix,
    std::string_view message,
    std::string& line) noexcept;

}  // namespace sf::chat
