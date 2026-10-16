#include "input/gamepad_suppression.hpp"

namespace sf {
namespace {

// Applies update to the packed mask until the compare-exchange succeeds.
// Returns the mask value that update was applied to.
template <typename Update>
std::uint32_t UpdateMask(
    std::atomic<std::uint32_t>& mask,
    Update update) noexcept {
    auto current = mask.load(std::memory_order_acquire);
    for (;;) {
        if (mask.compare_exchange_weak(
                current,
                update(current),
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return current;
        }
    }
}

}

bool GamepadSuppressionState::Suppress(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    if (buttons == 0 || userIndex >= masks_.size()) {
        return false;
    }

    UpdateMask(masks_[userIndex], [buttons](std::uint32_t current) {
        return Pack(
            static_cast<std::uint16_t>(Active(current) | buttons),
            Release(current));
    });
    return true;
}

bool GamepadSuppressionState::Unsuppress(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    if (buttons == 0 || userIndex >= masks_.size()) {
        return false;
    }

    UpdateMask(masks_[userIndex], [buttons](std::uint32_t current) {
        const auto active = Active(current);
        return Pack(
            static_cast<std::uint16_t>(active & ~buttons),
            static_cast<std::uint16_t>(Release(current) | (active & buttons)));
    });
    return true;
}

bool GamepadSuppressionState::SuppressAnalog(
    std::uint32_t userIndex) noexcept {
    if (userIndex >= analogSuppressed_.size()) {
        return false;
    }
    analogSuppressed_[userIndex].store(true, std::memory_order_release);
    return true;
}

bool GamepadSuppressionState::UnsuppressAnalog(
    std::uint32_t userIndex) noexcept {
    if (userIndex >= analogSuppressed_.size()) {
        return false;
    }
    analogSuppressed_[userIndex].store(false, std::memory_order_release);
    return true;
}

bool GamepadSuppressionState::IsAnalogSuppressed(
    std::uint32_t userIndex) const noexcept {
    return userIndex < analogSuppressed_.size() &&
        analogSuppressed_[userIndex].load(std::memory_order_acquire);
}

void GamepadSuppressionState::Clear() noexcept {
    for (auto& analog : analogSuppressed_) {
        analog.store(false, std::memory_order_release);
    }
    for (auto& mask : masks_) {
        UpdateMask(mask, [](std::uint32_t current) {
            return Pack(
                0,
                static_cast<std::uint16_t>(Release(current) | Active(current)));
        });
    }
}

void GamepadSuppressionState::Reset() noexcept {
    for (auto& analog : analogSuppressed_) {
        analog.store(false, std::memory_order_release);
    }
    for (auto& masks : masks_) {
        masks.store(0, std::memory_order_release);
    }
}

void GamepadSuppressionState::Filter(
    std::uint32_t userIndex,
    XINPUT_STATE& state) noexcept {
    if (userIndex >= masks_.size()) {
        return;
    }

    // Held release bits are consumed by this report. Buttons that are active
    // or still held from a release request are removed from the reported state.
    const auto rawButtons = state.Gamepad.wButtons;
    const auto previous = UpdateMask(
        masks_[userIndex],
        [rawButtons](std::uint32_t current) {
            return Pack(
                Active(current),
                static_cast<std::uint16_t>(Release(current) & rawButtons));
        });
    const auto suppressed = static_cast<std::uint16_t>(
        Active(previous) | (Release(previous) & rawButtons));
    state.Gamepad.wButtons = static_cast<WORD>(
        rawButtons & static_cast<std::uint16_t>(~suppressed));
    if (analogSuppressed_[userIndex].load(std::memory_order_acquire)) {
        state.Gamepad.bLeftTrigger = 0;
        state.Gamepad.bRightTrigger = 0;
        state.Gamepad.sThumbLX = 0;
        state.Gamepad.sThumbLY = 0;
        state.Gamepad.sThumbRX = 0;
        state.Gamepad.sThumbRY = 0;
    }
}

std::uint16_t GamepadSuppressionState::ActiveMask(
    std::uint32_t userIndex) const noexcept {
    return userIndex < masks_.size()
        ? Active(masks_[userIndex].load(std::memory_order_acquire))
        : 0;
}

std::uint16_t GamepadSuppressionState::ReleaseMask(
    std::uint32_t userIndex) const noexcept {
    return userIndex < masks_.size()
        ? Release(masks_[userIndex].load(std::memory_order_acquire))
        : 0;
}

}
