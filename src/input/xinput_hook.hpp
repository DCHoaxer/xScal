#pragma once

#include "hook/vtable_hook.hpp"
#include "input/hid_gamepad.hpp"
#include "input/input_state.hpp"

#include <Windows.h>
#include <Xinput.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace sf {

// Export ordinals shared by the XInput DLLs. Fallout imports all three from
// xinput1_3.dll by ordinal.
enum class XInputFunction : WORD {
    GetState = 2,
    SetState = 3,
    GetCapabilities = 4,
};

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using XInputGetCapabilitiesFn =
    DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using XInputImportResolver = std::uintptr_t(*)(
    std::uintptr_t moduleBase,
    VtableHookPlatform::VirtualQueryFn virtualQuery,
    XInputFunction function) noexcept;
// Gamepads the game cannot read itself. Returns false when no gamepad has
// padIndex.
using ForwardedGamepadQuery = bool(*)(
    std::size_t padIndex,
    InputState::GamepadSnapshot& snapshot) noexcept;

// Returns the RVA of the import slot for function, or 0 when the module does
// not import it from an XInput DLL.
[[nodiscard]] std::uintptr_t FindXInputImport(
    std::uintptr_t moduleBase,
    VtableHookPlatform::VirtualQueryFn virtualQuery,
    XInputFunction function) noexcept;

class GamepadInputFilterController final {
public:
    explicit GamepadInputFilterController(
        VtableHookPlatform platform = SystemVtableHookPlatform(),
        XInputImportResolver resolver = &FindXInputImport,
        ForwardedGamepadQuery forwardedGamepadQuery =
            &ReadBluetoothDualShock4State) noexcept;

    GamepadInputFilterController(const GamepadInputFilterController&) = delete;
    GamepadInputFilterController& operator=(
        const GamepadInputFilterController&) = delete;

    // Replaces the XInputGetState import slot, and the XInputGetCapabilities
    // and XInputSetState slots when the module imports them.
    [[nodiscard]] VtableHookStatus Install(std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] VtableHookStatus Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    // Returns the gamepads from forwardedGamepadQuery to the game at the
    // indices its XInput leaves empty. Fallout finds a controller with
    // XInputGetCapabilities, so this needs that slot.
    [[nodiscard]] bool ForwardGamepads() noexcept;
    // Writes 1 to the libScePad Bluetooth flag at flagAddress whenever the
    // game calls XInputGetCapabilities or XInputGetState and the flag is not
    // 1. ScePadInit sets it to 0.
    [[nodiscard]] bool EnableScePadBluetooth(std::uintptr_t flagAddress) noexcept;

private:
    VtableHookPlatform platform_;
    XInputImportResolver resolver_{};
    ForwardedGamepadQuery forwardedGamepadQuery_{};
    VtableHook getStateHook_;
    VtableHook getCapabilitiesHook_;
    VtableHook setStateHook_;
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
[[nodiscard]] DWORD WINAPI HookedXInputGetCapabilities(
    DWORD userIndex,
    DWORD flags,
    XINPUT_CAPABILITIES* capabilities) noexcept;
[[nodiscard]] DWORD WINAPI HookedXInputSetState(
    DWORD userIndex,
    XINPUT_VIBRATION* vibration) noexcept;

}
