#include "input/xinput_hook.hpp"

#include "common/ascii_case.hpp"
#include "input/gamepad_suppression.hpp"
#include "platform/native_access.hpp"
#include "platform/windows_memory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <string_view>

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
// Holds the game's original XInputGetState after install. It is kept after
// Restore so a game thread that loaded it before the restore can still call it.
std::atomic<XInputGetStateFn> activeXInputGetState;
std::atomic<bool> activeFilterInstalled;
std::mutex activeFilterLifecycleMutex;

}  // namespace

std::uintptr_t FindXInputGetStateImport(
    std::uintptr_t moduleBase,
    VtableHookPlatform::VirtualQueryFn virtualQuery) noexcept {
    if (moduleBase == 0 || virtualQuery == nullptr) {
        return 0;
    }

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
                constexpr WORD kXInputGetStateOrdinal = 2;
                if (IMAGE_ORDINAL64(originalThunk.u1.Ordinal) !=
                    kXInputGetStateOrdinal) {
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
                        "XInputGetState",
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
    XInputImportResolver resolver) noexcept
    : platform_{platform},
      resolver_{resolver},
      hook_{platform} {}

VtableHookStatus GamepadInputFilterController::Install(
    std::uintptr_t moduleBase) noexcept {
    std::scoped_lock lock{lifecycleMutex_};
    std::scoped_lock activeLock{activeFilterLifecycleMutex};
    if (hook_.IsActive() || activeFilterInstalled.load(std::memory_order_acquire)) {
        return VtableHookStatus::AlreadyActive;
    }
    if (moduleBase == 0 || resolver_ == nullptr ||
        platform_.virtualQuery == nullptr) {
        return VtableHookStatus::InvalidArgument;
    }

    const auto slotOffset = resolver_(moduleBase, platform_.virtualQuery);
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
    activeXInputGetState.store(original, std::memory_order_release);
    activeFilterInstalled.store(true, std::memory_order_release);

    const auto status = hook_.InstallSlot(
        slotOffset,
        moduleBase,
        reinterpret_cast<void*>(&HookedXInputGetState));
    if (status != VtableHookStatus::Installed) {
        activeFilterInstalled.store(false, std::memory_order_release);
        activeSuppression.Reset();
    }
    return status;
}

VtableHookStatus GamepadInputFilterController::Restore() noexcept {
    std::scoped_lock lock{lifecycleMutex_};
    std::scoped_lock activeLock{activeFilterLifecycleMutex};
    const auto status = hook_.Restore();
    if (!hook_.IsActive() &&
        activeFilterInstalled.load(std::memory_order_acquire)) {
        activeFilterInstalled.store(false, std::memory_order_release);
        activeSuppression.Reset();
    }
    return status;
}

bool GamepadInputFilterController::IsActive() const noexcept {
    return hook_.IsActive();
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
    const DWORD status = original(userIndex, state);
    if (status == ERROR_SUCCESS && state != nullptr) {
        activeSuppression.Filter(userIndex, *state);
    }
    return status;
}

}
