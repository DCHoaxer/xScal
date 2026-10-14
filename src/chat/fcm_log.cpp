#include "chat/fcm_log.hpp"

#include "common/json.hpp"

#include <algorithm>

namespace sf::chat {
namespace {

[[nodiscard]] bool IsContinuationByte(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

}  // namespace

bool BuildFcmLogLine(
    std::string_view message,
    std::string& line) noexcept {
    line.clear();
    if (!json::IsValidUtf8(message)) {
        return false;
    }
    try {
        constexpr std::string_view prefix{"FCM: "};
        line.reserve((std::min)(
            kMaximumFcmLogLineBytes,
            prefix.size() + message.size()));
        line.append(prefix);

        bool previousWasSpace{};
        for (std::size_t index = 0; index < message.size();) {
            const auto first = static_cast<unsigned char>(message[index]);
            if (first < 0x20 || first == 0x7F) {
                if (!previousWasSpace &&
                    line.size() < kMaximumFcmLogLineBytes) {
                    line.push_back(' ');
                }
                previousWasSpace = true;
                ++index;
                continue;
            }
            // A character is its lead byte plus the continuation bytes that
            // follow it. The message is already valid UTF-8.
            std::size_t end = index + 1;
            while (end < message.size() &&
                   IsContinuationByte(message[end])) {
                ++end;
            }
            if (end - index > kMaximumFcmLogLineBytes - line.size()) {
                break;
            }
            line.append(message.substr(index, end - index));
            previousWasSpace = first == ' ';
            index = end;
        }
        while (line.size() > prefix.size() && line.back() == ' ') {
            line.pop_back();
        }
        if (line.size() == prefix.size()) {
            line.pop_back();
        }
        return true;
    } catch (...) {
        line.clear();
        return false;
    }
}

}  // namespace sf::chat
