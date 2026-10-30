#include "input/hid_gamepad.hpp"

#include "platform/diagnostics.hpp"

#include <Windows.h>
#include <SetupAPI.h>
#include <hidsdi.h>
#include <Xinput.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sf {
namespace {

constexpr USHORT kSonyVendorId = 0x054C;
constexpr std::array<USHORT, 2> kDualSenseProductIds{
    0x0CE6,  // DualSense
    0x0DF2,  // DualSense Edge
};
constexpr USHORT kDualShock4WirelessAdapterProductId = 0x0BA0;
constexpr std::array<USHORT, 3> kDualShock4ProductIds{
    0x05C4,
    0x09CC,
    kDualShock4WirelessAdapterProductId,
};

constexpr std::uint8_t kInputReportId = 0x01;
constexpr std::uint8_t kDualSenseBluetoothReportId = 0x31;
constexpr std::uint8_t kDualShock4BluetoothReportId = 0x11;
// Sticks, triggers and the first two button bytes. Both PlayStation report
// layouts keep them in the first nine bytes after the report header.
constexpr std::size_t kPlayStationDataSize = 9;
// Second status byte of DualShock 4 report 0x01. Bit 2 is set while the
// wireless adapter has no controller.
constexpr std::size_t kAdapterStatusOffset = 31;
constexpr std::uint8_t kAdapterDisconnectedBit = 0x04;

// The USB report descriptors declare only the 64-byte report 0x01. Over
// Bluetooth they also declare longer reports, so the longest input report
// tells the two connections apart. A Sony controller whose descriptor
// matches neither, such as one Wine presents without its hidraw node, is
// read as a generic gamepad.
constexpr USHORT kUsbInputReportLength = 64;
constexpr USHORT kDualSenseBluetoothInputReportLength = 78;

constexpr std::size_t kMaxGamepads = 4;
// Device scans run on the reader thread while fewer than kMaxGamepads
// gamepads are open.
constexpr ULONGLONG kScanIntervalMs = 2'000;

// DirectInput gamepads have 10 to 16 buttons. Flight sticks, throttles and
// wheels usually have more.
constexpr std::size_t kMaxJoystickButtons = 16;

// Hat values 0 to 7 run clockwise from up. Other values mean released.
constexpr std::array<std::uint16_t, 8> kHatButtons{
    XINPUT_GAMEPAD_DPAD_UP,
    XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_DPAD_DOWN,
    XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
    XINPUT_GAMEPAD_DPAD_LEFT,
    XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT,
};

// HID buttons 1 to 12 of a generic gamepad. Buttons 7 and 8 are the
// triggers and have no XInput button.
constexpr std::array<std::uint16_t, 12> kGenericButtons{
    XINPUT_GAMEPAD_X,
    XINPUT_GAMEPAD_A,
    XINPUT_GAMEPAD_B,
    XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER,
    XINPUT_GAMEPAD_RIGHT_SHOULDER,
    0,
    0,
    XINPUT_GAMEPAD_BACK,
    XINPUT_GAMEPAD_START,
    XINPUT_GAMEPAD_LEFT_THUMB,
    XINPUT_GAMEPAD_RIGHT_THUMB,
};
constexpr std::uint32_t kGenericLeftTriggerButton = 1u << 6;
constexpr std::uint32_t kGenericRightTriggerButton = 1u << 7;

[[nodiscard]] std::uint16_t ConvertHat(std::uint8_t hat) noexcept {
    return hat < kHatButtons.size() ? kHatButtons[hat] : 0;
}

// The PS, touchpad and mute buttons are in the third button byte and have
// no XInput bit, so they are not read. DualSense and DualShock 4 use the
// same bits.
[[nodiscard]] std::uint16_t ConvertPlayStationButtons(
    std::uint8_t faceButtons,
    std::uint8_t shoulderButtons) noexcept {
    std::uint16_t buttons = ConvertHat(faceButtons & 0x0F);
    if ((faceButtons & 0x10) != 0) buttons |= XINPUT_GAMEPAD_X;  // Square
    if ((faceButtons & 0x20) != 0) buttons |= XINPUT_GAMEPAD_A;  // Cross
    if ((faceButtons & 0x40) != 0) buttons |= XINPUT_GAMEPAD_B;  // Circle
    if ((faceButtons & 0x80) != 0) buttons |= XINPUT_GAMEPAD_Y;  // Triangle
    if ((shoulderButtons & 0x01) != 0) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if ((shoulderButtons & 0x02) != 0) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if ((shoulderButtons & 0x10) != 0) buttons |= XINPUT_GAMEPAD_BACK;  // Create
    if ((shoulderButtons & 0x20) != 0) buttons |= XINPUT_GAMEPAD_START;  // Options
    if ((shoulderButtons & 0x40) != 0) buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if ((shoulderButtons & 0x80) != 0) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    return buttons;
}

// centered is -128 to 128 with 0 at rest. XInput axes use the full int16
// range with positive Y pointing up.
[[nodiscard]] std::int16_t ScalePlayStationStick(int centered) noexcept {
    return static_cast<std::int16_t>(
        std::clamp(centered * 32767 / 127, -32768, 32767));
}

// The full layout is sticks, triggers, a counter and the buttons. The short
// layout is sticks, buttons and triggers.
[[nodiscard]] bool ReadPlayStationData(
    std::span<const std::uint8_t> report,
    std::size_t dataOffset,
    bool isShortLayout,
    InputState::GamepadSnapshot& snapshot) noexcept {
    if (report.size() < dataOffset + kPlayStationDataSize) {
        return false;
    }
    const auto data = report.subspan(dataOffset);
    const std::size_t buttonsOffset = isShortLayout ? 4 : 7;
    const std::size_t triggersOffset = isShortLayout ? 7 : 4;

    snapshot = {};
    snapshot.buttons = ConvertPlayStationButtons(
        data[buttonsOffset], data[buttonsOffset + 1]);
    snapshot.leftTrigger = data[triggersOffset];
    snapshot.rightTrigger = data[triggersOffset + 1];
    // Stick bytes rest at 128 and grow right and down.
    snapshot.leftX = ScalePlayStationStick(data[0] - 128);
    snapshot.leftY = ScalePlayStationStick(128 - data[1]);
    snapshot.rightX = ScalePlayStationStick(data[2] - 128);
    snapshot.rightY = ScalePlayStationStick(128 - data[3]);
    snapshot.connected = true;
    return true;
}

[[nodiscard]] std::int16_t ScaleGenericStick(
    const GenericGamepadValue& axis,
    bool pointsDown) noexcept {
    if (!axis.present || axis.maximum <= axis.minimum) {
        return 0;
    }
    const std::int64_t range = std::int64_t{axis.maximum} - axis.minimum;
    const std::int64_t position =
        std::clamp(axis.value, axis.minimum, axis.maximum) -
        std::int64_t{axis.minimum};
    std::int64_t centered = position * 2 - range;
    if (pointsDown) {
        centered = -centered;
    }
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(
        centered * 32767 / range, -32768, 32767));
}

[[nodiscard]] std::uint8_t ScaleGenericTrigger(
    const GenericGamepadValue& axis,
    bool buttonPressed) noexcept {
    if (!axis.present) {
        return buttonPressed ? 255 : 0;
    }
    if (axis.maximum <= axis.minimum) {
        return 0;
    }
    const std::int64_t range = std::int64_t{axis.maximum} - axis.minimum;
    const std::int64_t position =
        std::clamp(axis.value, axis.minimum, axis.maximum) -
        std::int64_t{axis.minimum};
    return static_cast<std::uint8_t>(position * 255 / range);
}

// Hat switches report 8 or 4 directions. Values outside the logical range
// mean released.
[[nodiscard]] std::uint16_t ConvertGenericHat(
    const GenericGamepadValue& hat) noexcept {
    if (!hat.present || hat.value < hat.minimum || hat.value > hat.maximum) {
        return 0;
    }
    const std::int64_t positions =
        std::int64_t{hat.maximum} - hat.minimum + 1;
    const auto index = static_cast<std::uint8_t>(hat.value - hat.minimum);
    if (positions == 8) {
        return ConvertHat(index);
    }
    if (positions == 4) {
        return ConvertHat(static_cast<std::uint8_t>(index * 2));
    }
    return 0;
}

enum class GamepadKind { DualSense, DualShock4, Generic };

[[nodiscard]] GamepadKind FindGamepadKind(
    const HIDD_ATTRIBUTES& attributes,
    USHORT inputReportLength) noexcept {
    if (attributes.VendorID != kSonyVendorId) {
        return GamepadKind::Generic;
    }
    if (std::ranges::find(kDualSenseProductIds, attributes.ProductID) !=
            kDualSenseProductIds.end() &&
        (inputReportLength == kUsbInputReportLength ||
            inputReportLength >= kDualSenseBluetoothInputReportLength)) {
        return GamepadKind::DualSense;
    }
    if (std::ranges::find(kDualShock4ProductIds, attributes.ProductID) !=
            kDualShock4ProductIds.end() &&
        inputReportLength >= kUsbInputReportLength) {
        return GamepadKind::DualShock4;
    }
    return GamepadKind::Generic;
}

// XInput reads controllers whose interface path contains "IG_", so they
// already have an XInput index.
[[nodiscard]] bool IsXInputDevicePath(const std::wstring& path) {
    std::wstring upperPath = path;
    std::ranges::transform(upperPath, upperPath.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    return upperPath.find(L"IG_") != std::wstring::npos;
}

// Where a generic HID value is found in the device's reports.
struct GenericField final {
    USAGE usage{};
    USHORT linkCollection{};
    USHORT bitSize{};
};

[[nodiscard]] std::int32_t ReadLogicalValue(
    ULONG raw,
    USHORT bitSize,
    std::int32_t minimum) noexcept {
    if (minimum >= 0 || bitSize == 0 || bitSize >= 32 ||
        (raw & (1UL << (bitSize - 1))) == 0) {
        return static_cast<std::int32_t>(raw);
    }
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(raw) - (std::int64_t{1} << bitSize));
}

struct Gamepad final {
    Gamepad() = default;
    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;
    // Gamepads are destroyed only when no read is pending, so the buffer
    // and OVERLAPPED are no longer used by the HID driver.
    ~Gamepad() {
        if (preparsedData != nullptr) ::HidD_FreePreparsedData(preparsedData);
        if (readDone != nullptr) ::CloseHandle(readDone);
        if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
    }

    std::wstring path;
    GamepadKind kind{GamepadKind::Generic};
    bool bluetooth{};
    bool wirelessAdapter{};
    HANDLE file{INVALID_HANDLE_VALUE};
    HANDLE readDone{};
    OVERLAPPED overlapped{};
    std::vector<std::uint8_t> report;
    InputState::GamepadSnapshot state;

    // Generic gamepads only.
    PHIDP_PREPARSED_DATA preparsedData{};
    std::vector<USAGE> pressedButtons;
    std::array<GenericField, GenericGamepadReport::AxisCount> axisFields{};
    GenericField hatField;
    GenericGamepadReport generic;
    bool hasSliderDialOrWheel{};
};

// Finds the axes and hat switch in the device's value capabilities.
[[nodiscard]] bool ReadGenericLayout(
    Gamepad& gamepad,
    const HIDP_CAPS& caps) noexcept {
    USHORT valueCapCount = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> valueCaps(valueCapCount);
    if (valueCapCount > 0 &&
        ::HidP_GetValueCaps(
            HidP_Input, valueCaps.data(), &valueCapCount,
            gamepad.preparsedData) != HIDP_STATUS_SUCCESS) {
        return false;
    }
    for (USHORT capIndex = 0; capIndex < valueCapCount; ++capIndex) {
        const HIDP_VALUE_CAPS& cap = valueCaps[capIndex];
        if (cap.UsagePage != HID_USAGE_PAGE_GENERIC) {
            continue;
        }
        const int firstUsage = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage;
        const int lastUsage = cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage;
        for (int usage = firstUsage; usage <= lastUsage; ++usage) {
            GenericGamepadValue* value{};
            GenericField* field{};
            if (usage >= HID_USAGE_GENERIC_X && usage <= HID_USAGE_GENERIC_RZ) {
                const std::size_t axis = usage - HID_USAGE_GENERIC_X;
                value = &gamepad.generic.axes[axis];
                field = &gamepad.axisFields[axis];
            } else if (usage == HID_USAGE_GENERIC_HATSWITCH) {
                value = &gamepad.generic.hat;
                field = &gamepad.hatField;
            } else if (usage >= HID_USAGE_GENERIC_SLIDER &&
                       usage <= HID_USAGE_GENERIC_WHEEL) {
                gamepad.hasSliderDialOrWheel = true;
                continue;
            } else {
                continue;
            }
            if (value->present) {
                continue;
            }
            value->present = true;
            value->minimum = cap.LogicalMin;
            value->maximum = cap.LogicalMax;
            // Some descriptors write an unsigned maximum such as 255 in a
            // one-byte item, which the HID parser reads as -1.
            if (value->maximum < value->minimum && cap.BitSize < 32) {
                value->maximum = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(value->maximum) &
                    ((1u << cap.BitSize) - 1));
            }
            field->usage = static_cast<USAGE>(usage);
            field->linkCollection = cap.LinkCollection;
            field->bitSize = cap.BitSize;
        }
    }
    gamepad.pressedButtons.resize(::HidP_MaxUsageListLength(
        HidP_Input, HID_USAGE_PAGE_BUTTON, gamepad.preparsedData));
    return true;
}

// Logs the IDs and value ranges of a generic device, so a device that is
// read as a gamepad, or skipped, can be checked against its layout.
void LogGenericLayout(
    const Gamepad& gamepad,
    const HIDD_ATTRIBUTES& attributes,
    bool joystickUsage,
    bool read) noexcept {
    constexpr std::array<const char*, GenericGamepadReport::AxisCount>
        kAxisNames{"X", "Y", "Z", "Rx", "Ry", "Rz"};
    std::string ranges;
    char range[64]{};
    const auto appendRange = [&](const char* name,
                                 const GenericGamepadValue& value) {
        if (!value.present) {
            return;
        }
        std::snprintf(
            range, sizeof(range), " %s %ld..%ld", name,
            static_cast<long>(value.minimum), static_cast<long>(value.maximum));
        ranges += range;
    };
    for (std::size_t axis = 0; axis < kAxisNames.size(); ++axis) {
        appendRange(kAxisNames[axis], gamepad.generic.axes[axis]);
    }
    appendRange("hat", gamepad.generic.hat);
    DiagnosticLogFormat(
        "xScal HID gamepad: %s %04X:%04X, %s usage, %u buttons,%s%s",
        read ? "reading" : "skipped",
        static_cast<unsigned int>(attributes.VendorID),
        static_cast<unsigned int>(attributes.ProductID),
        joystickUsage ? "joystick" : "gamepad",
        static_cast<unsigned int>(gamepad.pressedButtons.size()),
        ranges.empty() ? " no axes" : ranges.c_str(),
        gamepad.hasSliderDialOrWheel ? ", slider, dial or wheel" : "");
}

void ReadGenericValue(
    const Gamepad& gamepad,
    const GenericField& field,
    GenericGamepadValue& value,
    PCHAR report,
    ULONG reportLength) noexcept {
    ULONG raw{};
    // A device with several input reports leaves values that are not in
    // this report unchanged.
    if (!value.present ||
        ::HidP_GetUsageValue(
            HidP_Input, HID_USAGE_PAGE_GENERIC, field.linkCollection,
            field.usage, &raw, gamepad.preparsedData, report,
            reportLength) != HIDP_STATUS_SUCCESS) {
        return;
    }
    value.value = ReadLogicalValue(raw, field.bitSize, value.minimum);
}

void ReadGenericReport(Gamepad& gamepad, ULONG reportLength) noexcept {
    auto* report = reinterpret_cast<PCHAR>(gamepad.report.data());
    auto buttonCount = static_cast<ULONG>(gamepad.pressedButtons.size());
    if (buttonCount > 0 &&
        ::HidP_GetUsages(
            HidP_Input, HID_USAGE_PAGE_BUTTON, 0,
            gamepad.pressedButtons.data(), &buttonCount,
            gamepad.preparsedData, report,
            reportLength) == HIDP_STATUS_SUCCESS) {
        gamepad.generic.buttons = 0;
        for (ULONG index = 0; index < buttonCount; ++index) {
            const USAGE button = gamepad.pressedButtons[index];
            if (button >= 1 && button <= 32) {
                gamepad.generic.buttons |= 1u << (button - 1);
            }
        }
    }
    for (std::size_t axis = 0; axis < gamepad.axisFields.size(); ++axis) {
        ReadGenericValue(
            gamepad, gamepad.axisFields[axis], gamepad.generic.axes[axis],
            report, reportLength);
    }
    ReadGenericValue(
        gamepad, gamepad.hatField, gamepad.generic.hat, report, reportLength);
    gamepad.state = ConvertGenericGamepadReport(gamepad.generic);
}

class HidGamepadReader final {
public:
    [[nodiscard]] std::size_t CountGamepads() noexcept {
        StartIfStopped();
        std::scoped_lock lock{mutex_};
        return stateCount_;
    }

    [[nodiscard]] bool ReadState(
        std::size_t padIndex,
        InputState::GamepadSnapshot& snapshot) noexcept {
        StartIfStopped();
        std::scoped_lock lock{mutex_};
        if (padIndex >= stateCount_ || !states_[padIndex].connected) {
            return false;
        }
        snapshot = states_[padIndex];
        return true;
    }

    [[nodiscard]] bool ReadBluetoothDualShock4State(
        std::size_t padIndex,
        InputState::GamepadSnapshot& snapshot) noexcept {
        StartIfStopped();
        std::scoped_lock lock{mutex_};
        for (std::size_t index = 0; index < stateCount_; ++index) {
            if (!isBluetoothDualShock4_[index]) {
                continue;
            }
            if (padIndex == 0) {
                if (!states_[index].connected) {
                    return false;
                }
                snapshot = states_[index];
                return true;
            }
            --padIndex;
        }
        return false;
    }

private:
    void StartIfStopped() noexcept {
        std::call_once(startOnce_, [this] {
            try {
                std::thread{&HidGamepadReader::Run, this}.detach();
            } catch (...) {
                DiagnosticLog("xScal HID gamepad: reader thread start failed");
            }
        });
    }

    void Run() noexcept {
        ULONGLONG nextScan{};
        for (;;) {
            if (::GetTickCount64() >= nextScan) {
                if (gamepads_.size() < kMaxGamepads) {
                    OpenNewGamepads();
                }
                nextScan = ::GetTickCount64() + kScanIntervalMs;
            }
            const ULONGLONG now = ::GetTickCount64();
            const auto timeout = static_cast<DWORD>(
                now < nextScan ? nextScan - now : 0);
            if (gamepads_.empty()) {
                ::Sleep(timeout);
                continue;
            }

            std::array<HANDLE, kMaxGamepads> readEvents{};
            for (std::size_t index = 0; index < gamepads_.size(); ++index) {
                readEvents[index] = gamepads_[index]->readDone;
            }
            ::WaitForMultipleObjects(
                static_cast<DWORD>(gamepads_.size()),
                readEvents.data(),
                FALSE,
                timeout);
            ReadCompletedReports();
        }
    }

    void ReadCompletedReports() noexcept {
        bool statesChanged{};
        for (auto it = gamepads_.begin(); it != gamepads_.end();) {
            Gamepad& gamepad = **it;
            if (!HasOverlappedIoCompleted(&gamepad.overlapped)) {
                ++it;
                continue;
            }
            statesChanged = true;
            DWORD bytesRead{};
            const bool readSucceeded = ::GetOverlappedResult(
                gamepad.file, &gamepad.overlapped, &bytesRead, FALSE);
            if (readSucceeded) {
                ReadReport(gamepad, bytesRead);
            }
            if (!readSucceeded || !StartRead(gamepad)) {
                it = gamepads_.erase(it);
                continue;
            }
            ++it;
        }
        if (statesChanged) {
            PublishStates();
        }
    }

    static void ReadReport(Gamepad& gamepad, DWORD bytesRead) noexcept {
        const std::span<const std::uint8_t> report{
            gamepad.report.data(), bytesRead};
        InputState::GamepadSnapshot state;
        switch (gamepad.kind) {
        case GamepadKind::DualSense:
            if (ReadDualSenseReport(report, gamepad.bluetooth, state)) {
                gamepad.state = state;
            }
            break;
        case GamepadKind::DualShock4:
            if (ReadDualShock4Report(report, gamepad.wirelessAdapter, state)) {
                gamepad.state = state;
            }
            break;
        case GamepadKind::Generic:
            ReadGenericReport(gamepad, bytesRead);
            break;
        }
    }

    void OpenNewGamepads() noexcept {
        GUID hidGuid{};
        ::HidD_GetHidGuid(&hidGuid);
        const HDEVINFO devices = ::SetupDiGetClassDevsW(
            &hidGuid,
            nullptr,
            nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE) {
            return;
        }
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        for (DWORD index = 0; gamepads_.size() < kMaxGamepads &&
             ::SetupDiEnumDeviceInterfaces(
                 devices, nullptr, &hidGuid, index, &interfaceData);
             ++index) {
            DWORD detailSize{};
            ::SetupDiGetDeviceInterfaceDetailW(
                devices, &interfaceData, nullptr, 0, &detailSize, nullptr);
            if (detailSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
                continue;
            }
            // DWORD elements give the structure its required alignment.
            std::vector<DWORD> detailBuffer(
                (detailSize + sizeof(DWORD) - 1) / sizeof(DWORD));
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
                detailBuffer.data());
            detail->cbSize = sizeof(*detail);
            if (!::SetupDiGetDeviceInterfaceDetailW(
                    devices, &interfaceData, detail, detailSize, nullptr,
                    nullptr)) {
                continue;
            }
            std::wstring path{detail->DevicePath};
            if (!IsKnownPath(path)) {
                OpenGamepad(std::move(path));
            }
        }
        ::SetupDiDestroyDeviceInfoList(devices);
    }

    [[nodiscard]] bool IsKnownPath(const std::wstring& path) const noexcept {
        return std::ranges::find(ignoredPaths_, path) != ignoredPaths_.end() ||
            std::ranges::any_of(gamepads_, [&](const auto& gamepad) {
                return gamepad->path == path;
            });
    }

    void OpenGamepad(std::wstring path) noexcept {
        if (IsXInputDevicePath(path)) {
            ignoredPaths_.push_back(std::move(path));
            return;
        }
        auto gamepad = std::make_unique<Gamepad>();
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        HIDP_CAPS caps{};
        {
            // Opening without access rights reads the attributes of
            // keyboards and mice, which the system holds open exclusively.
            const HANDLE probe = ::CreateFileW(
                path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, 0, nullptr);
            if (probe == INVALID_HANDLE_VALUE) {
                return;
            }
            const bool capsRead =
                ::HidD_GetAttributes(probe, &attributes) &&
                ::HidD_GetPreparsedData(probe, &gamepad->preparsedData) &&
                ::HidP_GetCaps(gamepad->preparsedData, &caps) ==
                    HIDP_STATUS_SUCCESS;
            ::CloseHandle(probe);
            if (!capsRead || caps.UsagePage != HID_USAGE_PAGE_GENERIC ||
                (caps.Usage != HID_USAGE_GENERIC_GAMEPAD &&
                    caps.Usage != HID_USAGE_GENERIC_JOYSTICK)) {
                ignoredPaths_.push_back(std::move(path));
                return;
            }
        }

        gamepad->kind = FindGamepadKind(attributes, caps.InputReportByteLength);
        gamepad->bluetooth =
            caps.InputReportByteLength > kUsbInputReportLength;
        gamepad->wirelessAdapter =
            attributes.VendorID == kSonyVendorId &&
            attributes.ProductID == kDualShock4WirelessAdapterProductId;
        const bool joystickUsage = caps.Usage == HID_USAGE_GENERIC_JOYSTICK;
        if (gamepad->kind == GamepadKind::Generic) {
            if (!ReadGenericLayout(*gamepad, caps)) {
                ignoredPaths_.push_back(std::move(path));
                return;
            }
            if (!IsGenericGamepad(
                    gamepad->generic, joystickUsage,
                    gamepad->pressedButtons.size(),
                    gamepad->hasSliderDialOrWheel)) {
                LogGenericLayout(*gamepad, attributes, joystickUsage, false);
                ignoredPaths_.push_back(std::move(path));
                return;
            }
        }

        gamepad->file = ::CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (gamepad->file == INVALID_HANDLE_VALUE) {
            ignoredPaths_.push_back(std::move(path));
            return;
        }
        gamepad->path = std::move(path);
        gamepad->report.resize(caps.InputReportByteLength);
        gamepad->readDone = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (gamepad->readDone == nullptr || !StartRead(*gamepad)) {
            return;
        }
        if (gamepad->kind == GamepadKind::Generic) {
            LogGenericLayout(*gamepad, attributes, joystickUsage, true);
        }
        gamepads_.push_back(std::move(gamepad));
        PublishStates();
    }

    [[nodiscard]] static bool StartRead(Gamepad& gamepad) noexcept {
        gamepad.overlapped = {};
        gamepad.overlapped.hEvent = gamepad.readDone;
        return ::ReadFile(
                   gamepad.file,
                   gamepad.report.data(),
                   static_cast<DWORD>(gamepad.report.size()),
                   nullptr,
                   &gamepad.overlapped) ||
            ::GetLastError() == ERROR_IO_PENDING;
    }

    void PublishStates() noexcept {
        std::scoped_lock lock{mutex_};
        stateCount_ = gamepads_.size();
        for (std::size_t index = 0; index < stateCount_; ++index) {
            const Gamepad& gamepad = *gamepads_[index];
            states_[index] = gamepad.state;
            isBluetoothDualShock4_[index] =
                gamepad.kind == GamepadKind::DualShock4 && gamepad.bluetooth;
        }
    }

    std::once_flag startOnce_;
    // Used only by the reader thread.
    std::vector<std::unique_ptr<Gamepad>> gamepads_;
    std::vector<std::wstring> ignoredPaths_;
    // Copies of the gamepad states for the polling threads.
    std::mutex mutex_;
    std::array<InputState::GamepadSnapshot, kMaxGamepads> states_{};
    std::array<bool, kMaxGamepads> isBluetoothDualShock4_{};
    std::size_t stateCount_{};
};

// Never destroyed: the detached reader thread uses it until the process
// exits.
HidGamepadReader& Reader() noexcept {
    static auto* const reader = new HidGamepadReader();
    return *reader;
}

}

bool ReadDualSenseReport(
    std::span<const std::uint8_t> report,
    bool bluetooth,
    InputState::GamepadSnapshot& snapshot) noexcept {
    if (report.empty()) {
        return false;
    }
    // USB report 0x01 and Bluetooth report 0x31 use the full layout.
    // Bluetooth report 0x01 is what a controller sends before anything asks
    // it for the full report.
    if (report[0] == kInputReportId) {
        return ReadPlayStationData(report, 1, bluetooth, snapshot);
    }
    if (report[0] == kDualSenseBluetoothReportId && bluetooth) {
        // One byte follows the report ID before the data.
        return ReadPlayStationData(report, 2, false, snapshot);
    }
    return false;
}

bool ReadDualShock4Report(
    std::span<const std::uint8_t> report,
    bool wirelessAdapter,
    InputState::GamepadSnapshot& snapshot) noexcept {
    if (report.empty()) {
        return false;
    }
    if (report[0] == kDualShock4BluetoothReportId) {
        // Two bytes follow the report ID before the data.
        return ReadPlayStationData(report, 3, true, snapshot);
    }
    if (report[0] != kInputReportId) {
        return false;
    }
    if (wirelessAdapter) {
        if (report.size() <= kAdapterStatusOffset) {
            return false;
        }
        if ((report[kAdapterStatusOffset] & kAdapterDisconnectedBit) != 0) {
            snapshot = {};
            return true;
        }
    }
    return ReadPlayStationData(report, 1, true, snapshot);
}

InputState::GamepadSnapshot ConvertGenericGamepadReport(
    const GenericGamepadReport& report) noexcept {
    using Axis = GenericGamepadReport::Axis;
    const auto& axes = report.axes;
    InputState::GamepadSnapshot snapshot;
    for (std::size_t button = 0; button < kGenericButtons.size(); ++button) {
        if ((report.buttons & (1u << button)) != 0) {
            snapshot.buttons |= kGenericButtons[button];
        }
    }
    snapshot.buttons |= ConvertGenericHat(report.hat);

    const bool rightStickOnZ = axes[Axis::Z].present && axes[Axis::Rz].present;
    const auto& rightX = rightStickOnZ ? axes[Axis::Z] : axes[Axis::Rx];
    const auto& rightY = rightStickOnZ ? axes[Axis::Rz] : axes[Axis::Ry];
    const auto& leftTrigger = rightStickOnZ ? axes[Axis::Rx] : axes[Axis::Z];
    const auto& rightTrigger = rightStickOnZ ? axes[Axis::Ry] : axes[Axis::Rz];
    snapshot.leftX = ScaleGenericStick(axes[Axis::X], false);
    snapshot.leftY = ScaleGenericStick(axes[Axis::Y], true);
    snapshot.rightX = ScaleGenericStick(rightX, false);
    snapshot.rightY = ScaleGenericStick(rightY, true);
    snapshot.leftTrigger = ScaleGenericTrigger(
        leftTrigger, (report.buttons & kGenericLeftTriggerButton) != 0);
    snapshot.rightTrigger = ScaleGenericTrigger(
        rightTrigger, (report.buttons & kGenericRightTriggerButton) != 0);
    snapshot.connected = true;
    return snapshot;
}

bool IsGenericGamepad(
    const GenericGamepadReport& report,
    bool joystickUsage,
    std::size_t buttonCount,
    bool hasSliderDialOrWheel) noexcept {
    using Axis = GenericGamepadReport::Axis;
    const auto& axes = report.axes;
    const bool hasLeftStick = axes[Axis::X].present && axes[Axis::Y].present;
    if (!joystickUsage) {
        return buttonCount > 0 || hasLeftStick;
    }
    const bool hasRightStick =
        (axes[Axis::Z].present && axes[Axis::Rz].present) ||
        (axes[Axis::Rx].present && axes[Axis::Ry].present);
    return hasLeftStick && hasRightStick && buttonCount > 0 &&
        buttonCount <= kMaxJoystickButtons && !hasSliderDialOrWheel;
}

std::size_t CountHidGamepads() noexcept {
    return Reader().CountGamepads();
}

bool ReadHidGamepadState(
    std::size_t padIndex,
    InputState::GamepadSnapshot& snapshot) noexcept {
    return Reader().ReadState(padIndex, snapshot);
}

bool ReadBluetoothDualShock4State(
    std::size_t padIndex,
    InputState::GamepadSnapshot& snapshot) noexcept {
    return Reader().ReadBluetoothDualShock4State(padIndex, snapshot);
}

}
