#include "hook/vtable_hook.hpp"

#include "platform/diagnostics.hpp"
#include "platform/native_access.hpp"
#include "platform/windows_memory.hpp"

namespace sf {
namespace {

HANDLE WINAPI CurrentProcess() noexcept {
    return ::GetCurrentProcess();
}

PVOID WINAPI ExchangePointer(PVOID volatile* target, PVOID value) noexcept {
    return ::InterlockedExchangePointer(target, value);
}

PVOID WINAPI CompareExchangePointer(
    PVOID volatile* target, PVOID exchange, PVOID comparand) noexcept {
    return ::InterlockedCompareExchangePointer(target, exchange, comparand);
}

}

VtableHookPlatform SystemVtableHookPlatform() noexcept {
    return {
        &::VirtualQuery,
        &::VirtualProtect,
        &ExchangePointer,
        &CompareExchangePointer,
        &::FlushInstructionCache,
        &CurrentProcess,
    };
}

VtableHook::VtableHook(VtableHookPlatform platform) noexcept : platform_(platform) {}

bool VtableHook::HasCompletePlatform() const noexcept {
    return platform_.virtualQuery != nullptr
        && platform_.virtualProtect != nullptr
        && platform_.exchangePointer != nullptr
        && platform_.compareExchangePointer != nullptr
        && platform_.flushInstructionCache != nullptr
        && platform_.getCurrentProcess != nullptr;
}

bool VtableHook::IsReadableSlot(void* slot) const noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    return platform_.virtualQuery(slot, &memory, sizeof(memory)) != 0
        && memory.State == MEM_COMMIT
        && platform::RegionContains(memory, slot, sizeof(void*))
        && platform::IsReadableProtection(memory.Protect);
}

bool VtableHook::IsExecutableTarget(void* target) const noexcept {
    if (target == nullptr) {
        return false;
    }

    MEMORY_BASIC_INFORMATION memory{};
    return platform_.virtualQuery(target, &memory, sizeof(memory)) != 0
        && memory.State == MEM_COMMIT
        && platform::IsExecutableProtection(memory.Protect);
}

bool VtableHook::RestoreProtection(void* slot, DWORD oldProtection) const noexcept {
    DWORD ignored{};
    return platform_.virtualProtect(slot, sizeof(void*), oldProtection, &ignored) != FALSE;
}

void VtableHook::BeginProtectionRecovery(
    void** slot, DWORD oldProtection, VtableHookStatus terminalResult) noexcept {
    DiagnosticLogFormat(
        "xScal: vtable slot %p protection restore failed; retrying on the next restore",
        static_cast<void*>(slot));
    state_ = State::ProtectionRecovery;
    slot_ = slot;
    original_ = nullptr;
    hook_ = nullptr;
    recoveryOldProtection_ = oldProtection;
    recoveryResult_ = terminalResult;
}

void VtableHook::Clear() noexcept {
    state_ = State::Inactive;
    slot_ = nullptr;
    original_ = nullptr;
    hook_ = nullptr;
    recoveryOldProtection_ = 0;
    recoveryResult_ = VtableHookStatus::NotActive;
}

VtableHookStatus VtableHook::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase,
    MovieRootGetVariable hook) noexcept {
    return InstallSlot(
        profile.primaryGetVariableSlotOffset,
        moduleBase,
        reinterpret_cast<void*>(hook));
}

VtableHookStatus VtableHook::InstallSlot(
    std::uintptr_t slotOffset,
    std::uintptr_t moduleBase,
    void* hook) noexcept {
    std::scoped_lock lock{mutex_};
    if (state_ != State::Inactive) {
        return VtableHookStatus::AlreadyActive;
    }
    if (!HasCompletePlatform() || hook == nullptr) {
        return HasCompletePlatform() ? VtableHookStatus::InvalidArgument
                                     : VtableHookStatus::InvalidPlatform;
    }
    std::uintptr_t slotValue{};
    if (!platform::TryAddAddress(moduleBase, slotOffset, slotValue)) {
        return VtableHookStatus::AddressOverflow;
    }

    if ((slotValue % alignof(void*)) != 0) {
        return VtableHookStatus::SlotUnaligned;
    }
    auto* slot = reinterpret_cast<void**>(slotValue);
    if (!IsReadableSlot(slot)) {
        return VtableHookStatus::SlotNotReadable;
    }

    const auto rawHook = hook;
    void* currentOriginal = *reinterpret_cast<void* volatile*>(slot);
    if (currentOriginal == rawHook) {
        return VtableHookStatus::AlreadyActive;
    }
    if (!IsExecutableTarget(currentOriginal)) {
        return VtableHookStatus::OriginalNotExecutable;
    }

    DWORD oldProtection{};
    if (platform_.virtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection) == FALSE) {
        return VtableHookStatus::ProtectFailed;
    }

    currentOriginal = platform_.exchangePointer(
        reinterpret_cast<PVOID volatile*>(slot), rawHook);
    if (currentOriginal == rawHook) {
        if (!RestoreProtection(slot, oldProtection)) {
            BeginProtectionRecovery(slot, oldProtection, VtableHookStatus::AlreadyActive);
            return VtableHookStatus::ProtectionRestoreFailed;
        }
        return VtableHookStatus::AlreadyActive;
    }
    if (!IsExecutableTarget(currentOriginal)) {
        platform_.compareExchangePointer(
            reinterpret_cast<PVOID volatile*>(slot), currentOriginal, rawHook);
        if (!RestoreProtection(slot, oldProtection)) {
            BeginProtectionRecovery(
                slot, oldProtection, VtableHookStatus::OriginalNotExecutable);
            return VtableHookStatus::ProtectionRestoreFailed;
        }
        return VtableHookStatus::OriginalNotExecutable;
    }

    if (platform_.flushInstructionCache(
            platform_.getCurrentProcess(), slot, sizeof(void*)) == FALSE) {
        platform_.compareExchangePointer(
            reinterpret_cast<PVOID volatile*>(slot), currentOriginal, rawHook);
        if (!RestoreProtection(slot, oldProtection)) {
            BeginProtectionRecovery(slot, oldProtection, VtableHookStatus::FlushFailed);
            return VtableHookStatus::ProtectionRestoreFailed;
        }
        return VtableHookStatus::FlushFailed;
    }

    if (!RestoreProtection(slot, oldProtection)) {
        platform_.compareExchangePointer(
            reinterpret_cast<PVOID volatile*>(slot), currentOriginal, rawHook);
        (void)platform_.flushInstructionCache(
            platform_.getCurrentProcess(), slot, sizeof(void*));
        // The slot holds the original again, so the restore reports Restored.
        BeginProtectionRecovery(slot, oldProtection, VtableHookStatus::Restored);
        return VtableHookStatus::ProtectionRestoreFailed;
    }

    state_ = State::Active;
    slot_ = slot;
    original_ = currentOriginal;
    hook_ = rawHook;
    return VtableHookStatus::Installed;
}

VtableHookStatus VtableHook::Restore() noexcept {
    std::scoped_lock lock{mutex_};
    if (state_ == State::Inactive) {
        return VtableHookStatus::NotActive;
    }
    if (!HasCompletePlatform()) {
        return VtableHookStatus::InvalidPlatform;
    }
    if (state_ == State::ProtectionRecovery) {
        if (!RestoreProtection(slot_, recoveryOldProtection_)) {
            return VtableHookStatus::ProtectionRestoreFailed;
        }
        const auto terminalResult = recoveryResult_;
        Clear();
        return terminalResult;
    }
    if (!IsReadableSlot(slot_)) {
        return VtableHookStatus::SlotNotReadable;
    }

    DWORD oldProtection{};
    if (platform_.virtualProtect(slot_, sizeof(void*), PAGE_READWRITE, &oldProtection) == FALSE) {
        return VtableHookStatus::ProtectFailed;
    }

    const auto current = platform_.compareExchangePointer(
        reinterpret_cast<PVOID volatile*>(slot_), original_, hook_);
    const bool changedByAnotherWriter = current != hook_;
    const bool flushed = changedByAnotherWriter || platform_.flushInstructionCache(
        platform_.getCurrentProcess(), slot_, sizeof(void*)) != FALSE;
    const auto terminalResult = changedByAnotherWriter
        ? VtableHookStatus::ReplacedByAnotherWriter
        : (flushed ? VtableHookStatus::Restored : VtableHookStatus::FlushFailed);
    if (!RestoreProtection(slot_, oldProtection)) {
        BeginProtectionRecovery(slot_, oldProtection, terminalResult);
        return VtableHookStatus::ProtectionRestoreFailed;
    }

    Clear();
    return terminalResult;
}

bool VtableHook::IsActive() const noexcept {
    std::scoped_lock lock{mutex_};
    return state_ == State::Active;
}

void** VtableHook::SlotAddress() const noexcept {
    std::scoped_lock lock{mutex_};
    return state_ == State::Active ? slot_ : nullptr;
}

MovieRootGetVariable VtableHook::Original() const noexcept {
    std::scoped_lock lock{mutex_};
    return state_ == State::Active
        ? reinterpret_cast<MovieRootGetVariable>(original_)
        : nullptr;
}

}
