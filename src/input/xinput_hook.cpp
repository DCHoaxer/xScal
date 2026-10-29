#include "input/xinput_hook.hpp"

#include "common/ascii_case.hpp"
#include "input/gamepad_suppression.hpp"
#include "platform/diagnostics.hpp"
#include "platform/native_access.hpp"
#include "platform/windows_memory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <type_traits>

namespace sf {
namespace {

template <typename T>
[[nodiscard]] bool ReadRva(
    VtableHookPlatform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    std::uint32_t imageSize,
    std::uint64_t rva,
    T& value) noexcept {
    if (rva > imageSize || sizeof(T) > imageSize - rva) {
        return false;
    }
    std::uintptr_t address{};
    return platform::TryAddAddress(moduleBase, rva, address) &&
        platform::ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(address),
            &value,
            sizeof(T));
}

// Compares the NUL-terminated import name at rva with expected. The name and
// its terminator are read with one range check.
[[nodiscard]] bool RvaStringEquals(
    VtableHookPlatform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    std::uint32_t imageSize,
    std::uint32_t rva,
    std::string_view expected,
    bool ignoreCase) noexcept {
    constexpr std::size_t kMaximumNameLength = 64;
    const std::size_t length = expected.size() + 1;
    if (length > kMaximumNameLength || rva > imageSize ||
        length > imageSize - rva) {
        return false;
    }

    std::uintptr_t address{};
    std::array<char, kMaximumNameLength> current{};
    if (!platform::TryAddAddress(moduleBase, rva, address) ||
        !platform::ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(address),
            current.data(),
            length)) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const char wanted = ignoreCase
            ? AsciiLower(expected[index])
            : expected[index];
        const char actual = ignoreCase
            ? AsciiLower(current[index])
            : current[index];
        if (actual != wanted) {
            return false;
        }
    }
    return current[expected.size()] == '\0';
}

[[nodiscard]] std::string_view XInputFunctionName(
    XInputFunction function) noexcept {
    switch (function) {
    case XInputFunction::GetState: return "XInputGetState";
    case XInputFunction::SetState: return "XInputSetState";
    case XInputFunction::GetCapabilities: return "XInputGetCapabilities";
    }
    return {};
}

[[nodiscard]] bool IsXInputImportModule(
    VtableHookPlatform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    std::uint32_t imageSize,
    std::uint32_t nameRva) noexcept {
    constexpr std::array names{
        std::string_view{"xinput1_4.dll"},
        std::string_view{"xinput1_3.dll"},
        std::string_view{"xinput9_1_0.dll"},
        std::string_view{"xinputuap.dll"},
    };
    return std::any_of(
        names.begin(),
        names.end(),
        [&](std::string_view name) {
            return RvaStringEquals(
                virtualQuery,
                moduleBase,
                imageSize,
                nameRva,
                name,
                true);
        });
}

GamepadSuppressionState activeSuppression;
// Hold the game's original XInput functions after install. They are kept
// after Restore so a game thread that loaded them before the restore can
// still call them.
std::atomic<XInputGetStateFn> activeXInputGetState;
std::atomic<XInputGetCapabilitiesFn> activeXInputGetCapabilities;
std::atomic<XInputSetStateFn> activeXInputSetState;
std::atomic<bool> activeFilterInstalled;
std::mutex activeFilterLifecycleMutex;
std::atomic<ForwardedGamepadQuery> activeForwardedGamepadQuery;
std::atomic<std::uint32_t*> activeScePadBluetoothFlag;

// Whether the game's XInput reported a controller at each index in the most
// recent XInputGetState or XInputGetCapabilities call. Fallout checks the
// indices in order, so the lower indices are current when a higher one is
// checked.
std::array<std::atomic<bool>, XUSER_MAX_COUNT> gameXInputConnected{};

// Packet numbers of forwarded gamepads. XInput changes the packet number
// only when the state changes.
struct ForwardedPacket final {
    XINPUT_GAMEPAD gamepad{};
    DWORD number{};
};
std::mutex forwardedPacketMutex;
std::array<ForwardedPacket, XUSER_MAX_COUNT> forwardedPackets{};

[[nodiscard]] bool IsWritableDword(
    VtableHookPlatform::VirtualQueryFn virtualQuery,
    std::uintptr_t address) noexcept {
    if (address == 0 || address % alignof(std::uint32_t) != 0) {
        return false;
    }
    const auto* pointer = reinterpret_cast<const void*>(address);
    MEMORY_BASIC_INFORMATION region{};
    if (virtualQuery(pointer, &region, sizeof(region)) == 0 ||
        region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        !platform::RegionContains(region, pointer, sizeof(std::uint32_t))) {
        return false;
    }
    switch (region.Protect & 0xFFU) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

// Returns the function in an import slot, or nullptr when the module does not
// import it or the slot cannot be read.
[[nodiscard]] void* ReadImportSlot(
    VtableHookPlatform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    std::uintptr_t slotOffset) noexcept {
    std::uintptr_t slotAddress{};
    if (slotOffset == 0 ||
        !platform::TryAddAddress(moduleBase, slotOffset, slotAddress)) {
        return nullptr;
    }
    auto** slot = reinterpret_cast<void**>(slotAddress);
    if (!platform::IsReadableRange(virtualQuery, slot, sizeof(*slot))) {
        return nullptr;
    }
    return *slot;
}

// libScePad accepts Bluetooth controllers only while the flag is 1. The game
// calls XInputGetCapabilities for a controller index right before it opens
// a ScePad device for that index, so the flag is set before every open.
void SetScePadBluetoothFlag() noexcept {
    std::uint32_t* const flag =
        activeScePadBluetoothFlag.load(std::memory_order_acquire);
    if (flag == nullptr) {
        return;
    }
    std::atomic_ref<std::uint32_t> value{*flag};
    if (value.load(std::memory_order_relaxed) != 1) {
        value.store(1, std::memory_order_relaxed);
    }
}

void RecordGameXInputResult(DWORD userIndex, DWORD status) noexcept {
    if (userIndex < gameXInputConnected.size()) {
        gameXInputConnected[userIndex].store(
            status == ERROR_SUCCESS, std::memory_order_relaxed);
    }
}

// Forwarded gamepads take the indices the game's XInput leaves empty, in
// order.
[[nodiscard]] bool ReadForwardedGamepad(
    DWORD userIndex,
    XINPUT_GAMEPAD& gamepad) noexcept {
    const ForwardedGamepadQuery query =
        activeForwardedGamepadQuery.load(std::memory_order_acquire);
    if (query == nullptr || userIndex >= gameXInputConnected.size()) {
        return false;
    }
    std::size_t padIndex{};
    for (DWORD lowerIndex = 0; lowerIndex < userIndex; ++lowerIndex) {
        if (!gameXInputConnected[lowerIndex].load(std::memory_order_relaxed)) {
            ++padIndex;
        }
    }
    InputState::GamepadSnapshot snapshot;
    if (!query(padIndex, snapshot)) {
        return false;
    }
    gamepad = {};
    gamepad.wButtons = snapshot.buttons;
    gamepad.bLeftTrigger = snapshot.leftTrigger;
    gamepad.bRightTrigger = snapshot.rightTrigger;
    gamepad.sThumbLX = snapshot.leftX;
    gamepad.sThumbLY = snapshot.leftY;
    gamepad.sThumbRX = snapshot.rightX;
    gamepad.sThumbRY = snapshot.rightY;
    return true;
}

[[nodiscard]] DWORD NextForwardedPacketNumber(
    DWORD userIndex,
    const XINPUT_GAMEPAD& gamepad) noexcept {
    std::scoped_lock lock{forwardedPacketMutex};
    ForwardedPacket& packet = forwardedPackets[userIndex];
    if (std::memcmp(&packet.gamepad, &gamepad, sizeof(gamepad)) != 0) {
        packet.gamepad = gamepad;
        ++packet.number;
    }
    return packet.number;
}

void ClearForwardingState() noexcept {
    activeForwardedGamepadQuery.store(nullptr, std::memory_order_release);
    activeScePadBluetoothFlag.store(nullptr, std::memory_order_release);
    for (auto& connected : gameXInputConnected) {
        connected.store(false, std::memory_order_relaxed);
    }
}

}  // namespace

std::uintptr_t FindXInputImport(
    std::uintptr_t moduleBase,
    VtableHookPlatform::VirtualQueryFn virtualQuery,
    XInputFunction function) noexcept {
    if (moduleBase == 0 || virtualQuery == nullptr) {
        return 0;
    }
    const std::string_view functionName = XInputFunctionName(function);

    IMAGE_DOS_HEADER dos{};
    if (!platform::ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(moduleBase),
            &dos,
            sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0) {
        return 0;
    }

    std::uintptr_t ntAddress{};
    IMAGE_NT_HEADERS64 nt{};
    if (!platform::TryAddAddress(
            moduleBase,
            static_cast<std::uint32_t>(dos.e_lfanew),
            ntAddress) ||
        !platform::ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(ntAddress),
            &nt,
            sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage == 0 ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return 0;
    }

    const auto imageSize = nt.OptionalHeader.SizeOfImage;
    const auto directory =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 ||
        directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        directory.VirtualAddress > imageSize ||
        directory.Size > imageSize - directory.VirtualAddress) {
        return 0;
    }

    const auto descriptorCount = std::min<std::size_t>(
        directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR),
        1024);
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < descriptorCount;
         ++descriptorIndex) {
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        const auto descriptorRva =
            static_cast<std::uint64_t>(directory.VirtualAddress) +
            descriptorIndex * sizeof(descriptor);
        if (!ReadRva(
                virtualQuery,
                moduleBase,
                imageSize,
                descriptorRva,
                descriptor)) {
            return 0;
        }
        if (descriptor.Name == 0 && descriptor.FirstThunk == 0) {
            break;
        }
        if (descriptor.Name == 0 || descriptor.FirstThunk == 0 ||
            descriptor.OriginalFirstThunk == 0 ||
            !IsXInputImportModule(
                virtualQuery,
                moduleBase,
                imageSize,
                descriptor.Name)) {
            continue;
        }

        constexpr std::size_t kMaximumImportsPerModule = 4096;
        for (std::size_t thunkIndex = 0;
             thunkIndex < kMaximumImportsPerModule;
             ++thunkIndex) {
            IMAGE_THUNK_DATA64 originalThunk{};
            const auto thunkByteOffset =
                static_cast<std::uint64_t>(thunkIndex) * sizeof(originalThunk);
            if (!ReadRva(
                    virtualQuery,
                    moduleBase,
                    imageSize,
                    static_cast<std::uint64_t>(
                        descriptor.OriginalFirstThunk) + thunkByteOffset,
                    originalThunk)) {
                break;
            }
            if (originalThunk.u1.AddressOfData == 0) {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL64(originalThunk.u1.Ordinal)) {
                if (IMAGE_ORDINAL64(originalThunk.u1.Ordinal) !=
                    static_cast<WORD>(function)) {
                    continue;
                }
            } else {
                if (originalThunk.u1.AddressOfData >
                    (std::numeric_limits<std::uint32_t>::max)()) {
                    continue;
                }

                const auto nameRva = static_cast<std::uint64_t>(
                    originalThunk.u1.AddressOfData) +
                    offsetof(IMAGE_IMPORT_BY_NAME, Name);
                if (nameRva > (std::numeric_limits<std::uint32_t>::max)() ||
                    !RvaStringEquals(
                        virtualQuery,
                        moduleBase,
                        imageSize,
                        static_cast<std::uint32_t>(nameRva),
                        functionName,
                        false)) {
                    continue;
                }
            }

            const auto slotRva =
                static_cast<std::uint64_t>(descriptor.FirstThunk) +
                thunkByteOffset;
            void* slotValue{};
            if (!ReadRva(
                    virtualQuery,
                    moduleBase,
                    imageSize,
                    slotRva,
                    slotValue)) {
                return 0;
            }
            return static_cast<std::uintptr_t>(slotRva);
        }
    }
    return 0;
}

GamepadInputFilterController::GamepadInputFilterController(
    VtableHookPlatform platform,
    XInputImportResolver resolver,
    ForwardedGamepadQuery forwardedGamepadQuery) noexcept
    : platform_{platform},
      resolver_{resolver},
      forwardedGamepadQuery_{forwardedGamepadQuery},
      getStateHook_{platform},
      getCapabilitiesHook_{platform},
      setStateHook_{platform} {}

VtableHookStatus GamepadInputFilterController::Install(
    std::uintptr_t moduleBase) noexcept {
    std::scoped_lock lock{lifecycleMutex_};
    std::scoped_lock activeLock{activeFilterLifecycleMutex};
    if (getStateHook_.IsActive() ||
        activeFilterInstalled.load(std::memory_order_acquire)) {
        return VtableHookStatus::AlreadyActive;
    }
    if (moduleBase == 0 || resolver_ == nullptr ||
        platform_.virtualQuery == nullptr) {
        return VtableHookStatus::InvalidArgument;
    }

    const auto slotOffset =
        resolver_(moduleBase, platform_.virtualQuery, XInputFunction::GetState);
    std::uintptr_t slotAddress{};
    if (slotOffset == 0 || !platform::TryAddAddress(
            moduleBase, slotOffset, slotAddress)) {
        return VtableHookStatus::InvalidArgument;
    }
    auto** slot = reinterpret_cast<void**>(slotAddress);
    if (!platform::IsReadableRange(
            platform_.virtualQuery, slot, sizeof(*slot))) {
        return VtableHookStatus::SlotNotReadable;
    }

    const auto original = reinterpret_cast<XInputGetStateFn>(*slot);
    if (original == nullptr) {
        return VtableHookStatus::InvalidArgument;
    }
    activeSuppression.Reset();
    ClearForwardingState();
    activeXInputGetState.store(original, std::memory_order_release);
    activeFilterInstalled.store(true, std::memory_order_release);

    const auto status = getStateHook_.InstallSlot(
        slotOffset,
        moduleBase,
        reinterpret_cast<void*>(&HookedXInputGetState));
    if (status != VtableHookStatus::Installed) {
        activeFilterInstalled.store(false, std::memory_order_release);
        activeSuppression.Reset();
        return status;
    }

    // The game's view of these two functions only changes for forwarded
    // gamepads, so the filter works without them.
    const auto installOptionalSlot = [&](
        VtableHook& hook,
        XInputFunction function,
        auto& activeOriginal,
        void* replacement) {
        const auto optionalSlotOffset =
            resolver_(moduleBase, platform_.virtualQuery, function);
        void* const optionalOriginal = ReadImportSlot(
            platform_.virtualQuery, moduleBase, optionalSlotOffset);
        if (optionalOriginal == nullptr) {
            return;
        }
        using Function = std::remove_reference_t<decltype(activeOriginal)>::value_type;
        activeOriginal.store(
            reinterpret_cast<Function>(optionalOriginal),
            std::memory_order_release);
        const auto optionalStatus =
            hook.InstallSlot(optionalSlotOffset, moduleBase, replacement);
        if (optionalStatus != VtableHookStatus::Installed) {
            DiagnosticLogFormat(
                "xScal GamepadInputFilter: %s slot %s",
                XInputFunctionName(function).data(),
                VtableHookStatusName(optionalStatus));
        }
    };
    installOptionalSlot(
        getCapabilitiesHook_,
        XInputFunction::GetCapabilities,
        activeXInputGetCapabilities,
        reinterpret_cast<void*>(&HookedXInputGetCapabilities));
    installOptionalSlot(
        setStateHook_,
        XInputFunction::SetState,
        activeXInputSetState,
        reinterpret_cast<void*>(&HookedXInputSetState));
    return status;
}

VtableHookStatus GamepadInputFilterController::Restore() noexcept {
    std::scoped_lock lock{lifecycleMutex_};
    std::scoped_lock activeLock{activeFilterLifecycleMutex};
    ClearForwardingState();
    if (getCapabilitiesHook_.IsActive()) {
        (void)getCapabilitiesHook_.Restore();
    }
    if (setStateHook_.IsActive()) {
        (void)setStateHook_.Restore();
    }
    const auto status = getStateHook_.Restore();
    if (!getStateHook_.IsActive() &&
        activeFilterInstalled.load(std::memory_order_acquire)) {
        activeFilterInstalled.store(false, std::memory_order_release);
        activeSuppression.Reset();
    }
    return status;
}

bool GamepadInputFilterController::IsActive() const noexcept {
    return getStateHook_.IsActive();
}

bool GamepadInputFilterController::ForwardGamepads() noexcept {
    std::scoped_lock lock{lifecycleMutex_};
    if (!getStateHook_.IsActive() || !getCapabilitiesHook_.IsActive() ||
        forwardedGamepadQuery_ == nullptr) {
        return false;
    }
    activeForwardedGamepadQuery.store(
        forwardedGamepadQuery_, std::memory_order_release);
    return true;
}

bool GamepadInputFilterController::EnableScePadBluetooth(
    std::uintptr_t flagAddress) noexcept {
    std::scoped_lock lock{lifecycleMutex_};
    if (!getStateHook_.IsActive() ||
        !IsWritableDword(platform_.virtualQuery, flagAddress)) {
        return false;
    }
    activeScePadBluetoothFlag.store(
        reinterpret_cast<std::uint32_t*>(flagAddress),
        std::memory_order_release);
    return true;
}

bool SuppressGamepadButtonsForGame(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    return activeFilterInstalled.load(std::memory_order_acquire) &&
        activeSuppression.Suppress(buttons, userIndex);
}

bool UnsuppressGamepadButtonsForGame(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    return activeFilterInstalled.load(std::memory_order_acquire) &&
        activeSuppression.Unsuppress(buttons, userIndex);
}

bool SuppressGamepadAnalogForGame(std::uint32_t userIndex) noexcept {
    return activeFilterInstalled.load(std::memory_order_acquire) &&
        activeSuppression.SuppressAnalog(userIndex);
}

bool UnsuppressGamepadAnalogForGame(std::uint32_t userIndex) noexcept {
    return activeFilterInstalled.load(std::memory_order_acquire) &&
        activeSuppression.UnsuppressAnalog(userIndex);
}

bool ClearGamepadSuppressionsForGame() noexcept {
    if (!activeFilterInstalled.load(std::memory_order_acquire)) {
        return false;
    }
    activeSuppression.Clear();
    return true;
}

DWORD WINAPI HookedXInputGetState(
    DWORD userIndex,
    XINPUT_STATE* state) noexcept {
    const auto original =
        activeXInputGetState.load(std::memory_order_acquire);
    if (original == nullptr) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    SetScePadBluetoothFlag();
    DWORD status = original(userIndex, state);
    RecordGameXInputResult(userIndex, status);
    if (status != ERROR_SUCCESS && state != nullptr &&
        ReadForwardedGamepad(userIndex, state->Gamepad)) {
        state->dwPacketNumber =
            NextForwardedPacketNumber(userIndex, state->Gamepad);
        status = ERROR_SUCCESS;
    }
    if (status == ERROR_SUCCESS && state != nullptr) {
        activeSuppression.Filter(userIndex, *state);
    }
    return status;
}

DWORD WINAPI HookedXInputGetCapabilities(
    DWORD userIndex,
    DWORD flags,
    XINPUT_CAPABILITIES* capabilities) noexcept {
    const auto original =
        activeXInputGetCapabilities.load(std::memory_order_acquire);
    if (original == nullptr) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    SetScePadBluetoothFlag();
    const DWORD status = original(userIndex, flags, capabilities);
    RecordGameXInputResult(userIndex, status);
    XINPUT_GAMEPAD gamepad{};
    if (status == ERROR_SUCCESS || capabilities == nullptr ||
        !ReadForwardedGamepad(userIndex, gamepad)) {
        return status;
    }
    // A gamepad with every button and full trigger and stick resolution,
    // and no vibration motors.
    *capabilities = {};
    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    capabilities->Gamepad.wButtons = XINPUT_GAMEPAD_DPAD_UP |
        XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
        XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_START |
        XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_THUMB |
        XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_LEFT_SHOULDER |
        XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B |
        XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y;
    capabilities->Gamepad.bLeftTrigger = 0xFF;
    capabilities->Gamepad.bRightTrigger = 0xFF;
    capabilities->Gamepad.sThumbLX = -1;
    capabilities->Gamepad.sThumbLY = -1;
    capabilities->Gamepad.sThumbRX = -1;
    capabilities->Gamepad.sThumbRY = -1;
    return ERROR_SUCCESS;
}

DWORD WINAPI HookedXInputSetState(
    DWORD userIndex,
    XINPUT_VIBRATION* vibration) noexcept {
    const auto original =
        activeXInputSetState.load(std::memory_order_acquire);
    if (original == nullptr) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    const DWORD status = original(userIndex, vibration);
    XINPUT_GAMEPAD gamepad{};
    // Forwarded gamepads do not vibrate. The call reports success, as XInput
    // does for a connected controller.
    if (status != ERROR_SUCCESS && ReadForwardedGamepad(userIndex, gamepad)) {
        return ERROR_SUCCESS;
    }
    return status;
}

}
