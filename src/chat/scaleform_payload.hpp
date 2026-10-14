#pragma once

#include "scaleform/native_handler.hpp"
#include "common/json.hpp"
#include "scaleform/abi.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sf::chat {

enum class PayloadMemberStatus {
    Found,
    Missing,
    WrongType,
};

class alignas(16) ScaleformPayload final {
public:
    explicit ScaleformPayload(
        const ScaleformCall& call,
        std::size_t argumentIndex = 1) noexcept;

    [[nodiscard]] bool IsObject() const noexcept;
    [[nodiscard]] PayloadMemberStatus ReadString(
        std::string_view name,
        std::string& value) const noexcept;
    [[nodiscard]] PayloadMemberStatus ReadBoolean(
        std::string_view name,
        bool& value) const noexcept;
    [[nodiscard]] PayloadMemberStatus ReadInteger(
        std::string_view name,
        std::int64_t& value) const noexcept;

private:
    enum class Source {
        None,
        Json,
        ScaleformObject,
    };

    const ScaleformCall* call_{};
    Source source_{Source::None};
    json::Document json_;
    std::byte objectAlignmentPadding_[8]{};
    ScaleformValue object_{};
};

}  // namespace sf::chat
