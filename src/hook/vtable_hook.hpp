#pragma once

#include "scaleform/abi.hpp"
#include "config/target_profile.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace sf {

// All platform calls are explicit so lifecycle tests can exercise the patcher
// against a fake address space. Production uses SystemVtableHookPlatform().
struct VtableHookPlatform final {
    using VirtualQueryFn = SIZE_T(WINAPI*)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
    using VirtualProtectFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
    using ExchangePointerFn = PVOID(WINAPI*)(PVOID volatile*, PVOID);
    using CompareExchangePointerFn = PVOID(WINAPI*)(PVOID volatile*, PVOID, PVOID);
    using FlushInstructionCacheFn = BOOL(WINAPI*)(HANDLE, LPCVOID, SIZE_T);
    using GetCurrentProcessFn = HANDLE(WINAPI*)();

    VirtualQueryFn virtualQuery{};
    VirtualProtectFn virtualProtect{};
    ExchangePointerFn exchangePointer{};
    CompareExchangePointerFn compareExchangePointer{};
    FlushInstructionCacheFn flushInstructionCache{};
    GetCurrentProcessFn getCurrentProcess{};
};

[[nodiscard]] VtableHookPlatform SystemVtableHookPlatform() noexcept;

enum class VtableHookStatus {
    Installed,
    Restored,
    AlreadyActive,
    NotActive,
    InvalidArgument,
    InvalidPlatform,
    RuntimeVersionUnavailable,
    RuntimeVersionMismatch,
    AddressLibraryMissing,
    AddressLibraryRestartRequired,
    AddressLibraryRequiresNewerXScal,
    AddressLibraryInvalid,
    AddressLibraryIncomplete,
    AddressOverflow,
    SlotUnaligned,
    SlotNotReadable,
    OriginalNotExecutable,
    TargetRoutineNotExecutable,
    ProtectFailed,
    FlushFailed,
    ProtectionRestoreFailed,
    ReplacedByAnotherWriter,
    AllocationFailed,
};

class VtableHook final {
public:
    explicit VtableHook(VtableHookPlatform platform = SystemVtableHookPlatform()) noexcept;

    VtableHook(const VtableHook&) = delete;
    VtableHook& operator=(const VtableHook&) = delete;

    [[nodiscard]] VtableHookStatus Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase,
        MovieRootGetVariable hook) noexcept;
    [[nodiscard]] VtableHookStatus InstallSlot(
        std::uintptr_t slotOffset,
        std::uintptr_t moduleBase,
        void* hook) noexcept;
    [[nodiscard]] VtableHookStatus Restore() noexcept;

    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] void** SlotAddress() const noexcept;
    [[nodiscard]] MovieRootGetVariable Original() const noexcept;

private:
    enum class State {
        Inactive,
        Active,
        ProtectionRecovery,
    };

    [[nodiscard]] bool HasCompletePlatform() const noexcept;
    [[nodiscard]] bool IsReadableSlot(void* slot) const noexcept;
    [[nodiscard]] bool IsExecutableTarget(void* target) const noexcept;
    [[nodiscard]] bool RestoreProtection(void* slot, DWORD oldProtection) const noexcept;
    void BeginProtectionRecovery(
        void** slot, DWORD oldProtection, VtableHookStatus terminalResult) noexcept;
    void Clear() noexcept;

    VtableHookPlatform platform_;
    mutable std::mutex mutex_;
    State state_{State::Inactive};
    void** slot_{};
    void* original_{};
    void* hook_{};
    DWORD recoveryOldProtection_{};
    VtableHookStatus recoveryResult_{VtableHookStatus::NotActive};
};

}
