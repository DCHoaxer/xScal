#pragma once

#include <Windows.h>
#include <Xinput.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace sf {

class GamepadSuppressionState final {
public:
    static constexpr std::size_t kGamepadCount = XUSER_MAX_COUNT;

    [[nodiscard]] bool Suppress(
        std::uint16_t buttons,
        std::uint32_t userIndex) noexcept;
    [[nodiscard]] bool Unsuppress(
        std::uint16_t buttons,
        std::uint32_t userIndex) noexcept;
    void Clear() noexcept;
    void Reset() noexcept;
    void Filter(std::uint32_t userIndex, XINPUT_STATE& state) noexcept;

    [[nodiscard]] std::uint16_t ActiveMask(
        std::uint32_t userIndex) const noexcept;
    [[nodiscard]] std::uint16_t ReleaseMask(
        std::uint32_t userIndex) const noexcept;

private:
    [[nodiscard]] static constexpr std::uint32_t Pack(
        std::uint16_t active,
        std::uint16_t release) noexcept {
        return static_cast<std::uint32_t>(active) |
            (static_cast<std::uint32_t>(release) << 16U);
    }
    [[nodiscard]] static constexpr std::uint16_t Active(
        std::uint32_t packed) noexcept {
        return static_cast<std::uint16_t>(packed);
    }
    [[nodiscard]] static constexpr std::uint16_t Release(
        std::uint32_t packed) noexcept {
        return static_cast<std::uint16_t>(packed >> 16U);
    }

    std::array<std::atomic<std::uint32_t>, kGamepadCount> masks_{};
};

}
