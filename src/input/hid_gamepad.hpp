#pragma once

#include "input/input_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sf {

// Converts one DualSense input report, starting with its report ID, to the
// XInput layout. Bluetooth sends report 0x01 in a shorter layout than USB,
// so the connection type is needed to read it.
[[nodiscard]] bool ReadDualSenseReport(
    std::span<const std::uint8_t> report,
    bool bluetooth,
    InputState::GamepadSnapshot& snapshot) noexcept;

// Converts DualShock 4 report 0x01 (USB, or Bluetooth before the full
// report is requested) or Bluetooth report 0x11. The Sony wireless adapter
// sends report 0x01 while no controller is connected to it. That report
// gives a snapshot with connected set to false.
[[nodiscard]] bool ReadDualShock4Report(
    std::span<const std::uint8_t> report,
    bool wirelessAdapter,
    InputState::GamepadSnapshot& snapshot) noexcept;

// Logical value range and the latest value of one generic HID usage.
struct GenericGamepadValue final {
    bool present{};
    std::int32_t minimum{};
    std::int32_t maximum{};
    std::int32_t value{};
};

// Values read with the HidP functions from a gamepad that has no
// dedicated report reader.
struct GenericGamepadReport final {
    enum Axis : std::size_t { X, Y, Z, Rx, Ry, Rz, AxisCount };

    // Bit n is HID button n + 1.
    std::uint32_t buttons{};
    std::array<GenericGamepadValue, AxisCount> axes{};
    GenericGamepadValue hat;
};

// Maps HID buttons 1 to 12 in the order DirectInput gamepads commonly use:
// X, A, B, Y, LB, RB, LT, RT, Back, Start, LS, RS. X and Y are the left
// stick. The right stick is Z and Rz with the triggers on Rx and Ry when
// the device has Z and Rz, and Rx and Ry with the triggers on Z and Rz
// otherwise. Buttons 7 and 8 set a trigger that has no axis.
[[nodiscard]] InputState::GamepadSnapshot ConvertGenericGamepadReport(
    const GenericGamepadReport& report) noexcept;

// Number of HID gamepads the reader has open. The first call starts the
// reader thread, which runs until the process exits.
[[nodiscard]] std::size_t CountHidGamepads() noexcept;

// Latest state of the gamepad at padIndex, in the order the gamepads were
// opened. Returns false until the gamepad has sent a report.
[[nodiscard]] bool ReadHidGamepadState(
    std::size_t padIndex,
    InputState::GamepadSnapshot& snapshot) noexcept;

// Like ReadHidGamepadState, but counts only DualShock 4 controllers connected
// over Bluetooth. Fallout's libScePad rejects them, so the XInput hook passes
// them to the game. The first call starts the reader thread.
[[nodiscard]] bool ReadBluetoothDualShock4State(
    std::size_t padIndex,
    InputState::GamepadSnapshot& snapshot) noexcept;

}
