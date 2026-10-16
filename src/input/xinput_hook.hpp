#pragma once

#include "hook/vtable_hook.hpp"

#include <Windows.h>
#include <Xinput.h>

#include <cstdint>
#include <mutex>

namespace sf {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputImportResolver = std::uintptr_t(*)(
    std::uintptr_t moduleBase,
    VtableHookPlatform::VirtualQueryFn virtualQuery) noexcept;

[[nodiscard]] std::uintptr_t FindXInputGetStateImport(
    std::uintptr_t moduleBase,
    VtableHookPlatform::VirtualQueryFn virtualQuery) noexcept;

class GamepadInputFilterController final {
public:
    explicit GamepadInputFilterController(
        VtableHookPlatform platform = SystemVtableHookPlatform(),
        XInputImportResolver resolver = &FindXInputGetStateImport) noexcept;

    GamepadInputFilterController(const GamepadInputFilterController&) = delete;
    GamepadInputFilterController& operator=(
        const GamepadInputFilterController&) = delete;

    [[nodiscard]] VtableHookStatus Install(std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] VtableHookStatus Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

private:
    VtableHookPlatform platform_;
    XInputImportResolver resolver_{};
    VtableHook hook_;
    mutable std::mutex lifecycleMutex_;
};

[[nodiscard]] bool SuppressGamepadButtonsForGame(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept;
[[nodiscard]] bool UnsuppressGamepadButtonsForGame(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept;
[[nodiscard]] bool SuppressGamepadAnalogForGame(
    std::uint32_t userIndex) noexcept;
[[nodiscard]] bool UnsuppressGamepadAnalogForGame(
    std::uint32_t userIndex) noexcept;
[[nodiscard]] bool ClearGamepadSuppressionsForGame() noexcept;

[[nodiscard]] DWORD WINAPI HookedXInputGetState(
    DWORD userIndex,
    XINPUT_STATE* state) noexcept;

}
