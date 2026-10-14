#include "datamanager/active_effect_snapshot.hpp"

#include "common/ascii_case.hpp"
#include "common/json.hpp"
#include "platform/diagnostics.hpp"
#include "platform/native_access.hpp"
#include "scaleform/scaleform_layout.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace sf::datamanager {
namespace {

constexpr std::size_t kMaximumEffects = 4096;
constexpr std::size_t kMaximumStringLength = 0x10000;
constexpr std::size_t kMaximumSnapshotLength = layout::kMaxReturnStringLength;

struct GameString final {
    char* data{};
    std::uint16_t length{};
    std::uint16_t capacity{};
};
static_assert(sizeof(GameString) == 0x10);

struct MagnitudeDisplayInfo final {
    std::uint8_t hasMagnitude{};
    std::uint8_t isPercentage{};
    std::uint16_t padding{};
    float magnitude{};
};
static_assert(sizeof(MagnitudeDisplayInfo) == 0x08);
static_assert(offsetof(MagnitudeDisplayInfo, magnitude) == 0x04);

struct EffectEntrySnapshot final {
    std::string text;
    float duration{};
    float elapsed{};
    float remaining{};
    float magnitude{};
    bool hasDuration{};
    bool hasMagnitude{};
    bool isPercentage{};
    bool isStacked{};
};

struct EffectGroupSnapshot final {
    std::uintptr_t spell{};
    std::string name;
    std::vector<EffectEntrySnapshot> entries;
};

struct EffectPointerArray final {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uintptr_t capacity{};
};

// Failure reasons are logged where they occur. The caller only needs to know
// whether the active-effect lock was busy, because a busy lock is retried
// later through the cache.
enum class LocalPlayerEffectsStatus {
    Success,
    LockBusy,
    Failed,
};

using EffectPredicate = bool(__fastcall*)(void* const* effect);
using EffectDisplayPredicate = bool(__fastcall*)(
    void* const* effect,
    float displayTime);
using GetMagnitudeDisplayInfo = MagnitudeDisplayInfo*(__fastcall*)(
    MagnitudeDisplayInfo* result,
    void* const* effect);
using FormatEffectText = void(__fastcall*)(
    void* const* effect,
    GameString* text,
    std::uint8_t* usedFallback);
using Deallocate = void(__fastcall*)(void* memory);
using ReleaseEffect = void(__fastcall*)(void* effect, std::uint8_t release);
using GetActiveEffects = void*(__fastcall*)(void* magicTarget);
using ActiveEffectsLockOperation = void(__fastcall*)(void* lock);
using GetFullName = const char*(__fastcall*)(const void* fullNameComponent);

class GameAllocationGuard final {
public:
    GameAllocationGuard(Deallocate deallocate, void* memory) noexcept
        : deallocate_(deallocate), memory_(memory) {}

    GameAllocationGuard(const GameAllocationGuard&) = delete;
    GameAllocationGuard& operator=(const GameAllocationGuard&) = delete;

    ~GameAllocationGuard() {
        if (memory_ != nullptr) {
            deallocate_(memory_);
        }
    }

private:
    Deallocate deallocate_{};
    void* memory_{};
};

[[nodiscard]] int MemoryAccessExceptionFilter(
    unsigned long exceptionCode) noexcept {
    return exceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            exceptionCode == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH;
}

[[nodiscard]] bool TryReadEffectSpell(
    const config::NativeActiveEffectProfile& profile,
    std::uintptr_t effect,
    std::uintptr_t& spell) noexcept {
    spell = 0;
    if (effect > (std::numeric_limits<std::uintptr_t>::max)() -
            profile.effectSpellOffset) {
        return false;
    }
    __try {
        std::memcpy(
            &spell,
            reinterpret_cast<const void*>(effect + profile.effectSpellOffset),
            sizeof(spell));
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        spell = 0;
        return false;
    }
    return spell != 0 && spell % alignof(std::uintptr_t) == 0 &&
        spell <= (std::numeric_limits<std::uintptr_t>::max)() -
            profile.spellFullNameOffset;
}

[[nodiscard]] bool TryGetSpellFullName(
    const config::NativeActiveEffectProfile& profile,
    std::uintptr_t spell,
    const char*& value) noexcept {
    value = nullptr;
    const auto fullName = spell + profile.spellFullNameOffset;
    __try {
        std::uintptr_t vtable{};
        std::memcpy(
            &vtable,
            reinterpret_cast<const void*>(fullName),
            sizeof(vtable));
        if (vtable == 0 ||
            vtable > (std::numeric_limits<std::uintptr_t>::max)() -
                profile.getFullNameVtableOffset) {
            return false;
        }

        std::uintptr_t getFullName{};
        std::memcpy(
            &getFullName,
            reinterpret_cast<const void*>(
                vtable + profile.getFullNameVtableOffset),
            sizeof(getFullName));
        if (getFullName == 0) {
            return false;
        }
        value = reinterpret_cast<GetFullName>(getFullName)(
            reinterpret_cast<const void*>(fullName));
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        value = nullptr;
        return false;
    }
    return value != nullptr;
}

[[nodiscard]] bool ReadSpellName(
    const config::NativeActiveEffectProfile& profile,
    std::uintptr_t spell,
    std::string& name) {
    name.clear();
    const char* value{};
    return TryGetSpellFullName(profile, spell, value) &&
        platform::CopyBoundedCString(
            value, kMaximumStringLength, false, name);
}

[[nodiscard]] bool IsConfigured(
    const config::NativeActiveEffectProfile& profile) noexcept {
    return profile.localPlayerGlobal != 0 &&
        profile.localPlayerPrimaryVtable != 0 &&
        profile.localPlayerMagicTargetVtable != 0 &&
        profile.getActiveEffectsVtableOffset % sizeof(std::uintptr_t) == 0 &&
        profile.releaseActiveEffectsReadLock != 0 &&
        profile.isEligible != 0 &&
        profile.isDisplayable != 0 &&
        profile.getMagnitudeDisplayInfo != 0 &&
        profile.formatEffectText != 0 &&
        profile.deallocate != 0 &&
        profile.localPlayerMagicTargetOffset <= profile.localPlayerSize &&
        profile.localPlayerSize - profile.localPlayerMagicTargetOffset >=
            sizeof(std::uintptr_t) &&
        profile.effectArrayElementSize == sizeof(std::uintptr_t) &&
        profile.getFullNameVtableOffset % sizeof(std::uintptr_t) == 0;
}

void ReleaseStrongEffect(
    platform::VirtualQueryFn virtualQuery,
    void* effect) noexcept {
    if (effect == nullptr) {
        return;
    }
    auto* const referenceCount = reinterpret_cast<volatile LONG*>(
        reinterpret_cast<std::uintptr_t>(effect) + sizeof(std::uintptr_t));
    if (::InterlockedDecrement(referenceCount) != 0) {
        return;
    }

    // The last verified vtable and release pair. Only a pair not seen before
    // is checked with VirtualQuery.
    static std::atomic<std::uintptr_t> verifiedVtable{};
    static std::atomic<std::uintptr_t> verifiedRelease{};

    std::uintptr_t vtable{};
    std::uintptr_t release{};
    if (!platform::TryCopyBytes(effect, &vtable, sizeof(vtable)) ||
        vtable == 0 ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(vtable),
            &release,
            sizeof(release))) {
        return;
    }
    if (vtable != verifiedVtable.load(std::memory_order_acquire) ||
        release != verifiedRelease.load(std::memory_order_acquire)) {
        if (!platform::IsExecutableAddress(
                virtualQuery,
                reinterpret_cast<const void*>(release))) {
            return;
        }
        verifiedRelease.store(release, std::memory_order_release);
        verifiedVtable.store(vtable, std::memory_order_release);
    }
    reinterpret_cast<ReleaseEffect>(release)(effect, 1);
}

class StrongEffect final {
public:
    StrongEffect(
        platform::VirtualQueryFn virtualQuery,
        void* effect) noexcept
        : virtualQuery_(virtualQuery), effect_(effect) {}

    StrongEffect(const StrongEffect&) = delete;
    StrongEffect& operator=(const StrongEffect&) = delete;

    StrongEffect(StrongEffect&& other) noexcept
        : virtualQuery_(other.virtualQuery_), effect_(other.effect_) {
        other.effect_ = nullptr;
    }

    StrongEffect& operator=(StrongEffect&& other) noexcept {
        if (this != &other) {
            ReleaseStrongEffect(virtualQuery_, effect_);
            virtualQuery_ = other.virtualQuery_;
            effect_ = other.effect_;
            other.effect_ = nullptr;
        }
        return *this;
    }

    ~StrongEffect() {
        ReleaseStrongEffect(virtualQuery_, effect_);
    }

    [[nodiscard]] void* get() const noexcept {
        return effect_;
    }

private:
    platform::VirtualQueryFn virtualQuery_{};
    void* effect_{};
};

enum class ActiveEffectsReadLockStatus {
    Acquired,
    Busy,
    Unavailable,
};

// The game inlines the read-lock acquire, so this CAS repeats it on the reader
// count at lock+4 (the writer flag is in the high 16 bits) instead of calling a
// game routine. The release calls the game's function found in the Address
// Library.
[[nodiscard]] ActiveEffectsReadLockStatus
TryAcquireActiveEffectsReadLockUnchecked(
    void* lock) noexcept {
    constexpr LONG kWriterMask = static_cast<LONG>(0xFFFF0000U);
    constexpr LONG kReaderMask = 0x0000FFFF;
    auto* const state = reinterpret_cast<volatile LONG*>(
        reinterpret_cast<std::uintptr_t>(lock) + sizeof(std::uint32_t));

    LONG current = ::InterlockedCompareExchange(state, 0, 0);
    for (unsigned int attempt = 0; attempt < 4; ++attempt) {
        if ((current & kWriterMask) != 0 ||
            (current & kReaderMask) == kReaderMask) {
            return ActiveEffectsReadLockStatus::Busy;
        }
        const LONG previous = ::InterlockedCompareExchange(
            state,
            current + 1,
            current);
        if (previous == current) {
            return ActiveEffectsReadLockStatus::Acquired;
        }
        current = previous;
    }
    return ActiveEffectsReadLockStatus::Busy;
}

[[nodiscard]] ActiveEffectsReadLockStatus TryAcquireActiveEffectsReadLock(
    void* lock) noexcept {
    if (lock == nullptr) {
        return ActiveEffectsReadLockStatus::Unavailable;
    }
    const auto lockAddress = reinterpret_cast<std::uintptr_t>(lock);
    if (lockAddress > (std::numeric_limits<std::uintptr_t>::max)() -
            sizeof(std::uint32_t) ||
        (lockAddress + sizeof(std::uint32_t)) % alignof(LONG) != 0) {
        return ActiveEffectsReadLockStatus::Unavailable;
    }
    __try {
        return TryAcquireActiveEffectsReadLockUnchecked(lock);
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        return ActiveEffectsReadLockStatus::Unavailable;
    }
}

class ActiveEffectsTryReadGuard final {
public:
    ActiveEffectsTryReadGuard(
        ActiveEffectsLockOperation release,
        void* lock) noexcept
        : release_(release), lock_(lock) {
        status_ = TryAcquireActiveEffectsReadLock(lock_);
    }

    ActiveEffectsTryReadGuard(const ActiveEffectsTryReadGuard&) = delete;
    ActiveEffectsTryReadGuard& operator=(
        const ActiveEffectsTryReadGuard&) = delete;

    ~ActiveEffectsTryReadGuard() {
        if (status_ == ActiveEffectsReadLockStatus::Acquired) {
            release_(lock_);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return status_ == ActiveEffectsReadLockStatus::Acquired;
    }

    [[nodiscard]] ActiveEffectsReadLockStatus Status() const noexcept {
        return status_;
    }

private:
    ActiveEffectsLockOperation release_{};
    void* lock_{};
    ActiveEffectsReadLockStatus status_{
        ActiveEffectsReadLockStatus::Unavailable};
};

// The active-effects view owns every listed effect while its read lock is
// held. Compare the exact target and retain matching ownership directly in the
// critical section. Fixed-layout reads then use that strong reference after the
// lock is released. Performing VirtualQuery for every field here made the game
// lock scheduler-sensitive on native Windows.
[[nodiscard]] bool TryRetainListedEffect(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t effect,
    StrongEffect& retained) noexcept {
    if (effect == 0 || effect % alignof(std::uintptr_t) != 0 ||
        effect > (std::numeric_limits<std::uintptr_t>::max)() -
            sizeof(std::uintptr_t)) {
        return false;
    }

    const auto referenceCountAddress = effect + sizeof(std::uintptr_t);
    auto* const referenceCount = reinterpret_cast<volatile LONG*>(
        referenceCountAddress);
    LONG current = ::InterlockedCompareExchange(referenceCount, 0, 0);
    while (current > 0 &&
           current < (std::numeric_limits<LONG>::max)()) {
        const LONG previous = ::InterlockedCompareExchange(
            referenceCount,
            current + 1,
            current);
        if (previous == current) {
            retained = StrongEffect{
                virtualQuery,
                reinterpret_cast<void*>(effect)};
            return true;
        }
        current = previous;
    }
    return false;
}

// Logs the reason for a failed enumeration and returns Failed.
[[nodiscard]] LocalPlayerEffectsStatus FailLocalPlayerEffects(
    const char* reason) noexcept {
    DiagnosticLogFormat(
        "xScal: StatsEffects local-player snapshot failed: %s",
        reason);
    return LocalPlayerEffectsStatus::Failed;
}

[[nodiscard]] LocalPlayerEffectsStatus EnumerateLocalPlayerEffects(
    platform::VirtualQueryFn virtualQuery,
    const ResolvedNativeActiveEffectRuntime& runtime,
    std::vector<StrongEffect>& effects,
    std::uintptr_t& sourcePlayer) {
    sourcePlayer = 0;
    const auto& profile = runtime.layout;

    std::uintptr_t player{};
    if (!ReadNativeActiveEffectSourcePlayer(runtime, player)) {
        return FailLocalPlayerEffects("player_unavailable");
    }

    if (player > (std::numeric_limits<std::uintptr_t>::max)() -
            profile.localPlayerMagicTargetOffset) {
        return FailLocalPlayerEffects("address_unavailable");
    }
    const auto localTarget =
        player + profile.localPlayerMagicTargetOffset;

    // Exact guarded reads establish the supported player types without a
    // separate whole-allocation query. A prior range query cannot prevent a
    // concurrent lifetime change between validation and access.
    std::uintptr_t primaryVtable{};
    std::uintptr_t magicTargetVtable{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(player),
            &primaryVtable,
            sizeof(primaryVtable)) ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(localTarget),
            &magicTargetVtable,
            sizeof(magicTargetVtable))) {
        return FailLocalPlayerEffects("player_unavailable");
    }
    if (primaryVtable != runtime.localPlayerPrimaryVtable ||
        magicTargetVtable != runtime.localPlayerMagicTargetVtable) {
        return FailLocalPlayerEffects("player_type_mismatch");
    }

    const auto getActiveEffects =
        reinterpret_cast<GetActiveEffects>(runtime.getActiveEffects);
    const auto releaseReadLock =
        reinterpret_cast<ActiveEffectsLockOperation>(
            runtime.releaseActiveEffectsReadLock);
    void* const activeEffects = getActiveEffects(
        reinterpret_cast<void*>(localTarget));

    effects.clear();
    if (activeEffects != nullptr) {
        const auto activeEffectsAddress =
            reinterpret_cast<std::uintptr_t>(activeEffects);
        if (activeEffectsAddress >
                (std::numeric_limits<std::uintptr_t>::max)() -
                    profile.activeEffectsLockOffset) {
            return FailLocalPlayerEffects("array_unavailable");
        }

        // Reserve before entering the game lock so vector growth cannot extend
        // the critical section. The fixed pointer buffer lets one guarded copy
        // contain faults without querying every element.
        effects.reserve(kMaximumEffects);
        std::array<std::uintptr_t, kMaximumEffects> effectPointers{};
        ActiveEffectsTryReadGuard guard{
            releaseReadLock,
            reinterpret_cast<void*>(
                activeEffectsAddress + profile.activeEffectsLockOffset)};
        if (!guard) {
            if (guard.Status() == ActiveEffectsReadLockStatus::Busy) {
                return LocalPlayerEffectsStatus::LockBusy;
            }
            return FailLocalPlayerEffects("array_unavailable");
        }

        EffectPointerArray effectArray{};
        if (!platform::TryCopyBytes(
                activeEffects,
                &effectArray,
                sizeof(effectArray))) {
            return FailLocalPlayerEffects("array_unavailable");
        }
        if (effectArray.begin == 0 &&
            (effectArray.end != 0 || effectArray.capacity != 0)) {
            return FailLocalPlayerEffects("invalid_array");
        }

        const auto elementSize = profile.effectArrayElementSize;
        if (effectArray.begin % alignof(std::uintptr_t) != 0 ||
            effectArray.end < effectArray.begin ||
            effectArray.capacity < effectArray.end ||
            (effectArray.end - effectArray.begin) % elementSize != 0 ||
            (effectArray.capacity - effectArray.begin) % elementSize != 0) {
            return FailLocalPlayerEffects("invalid_array");
        }

        const auto count =
            (effectArray.end - effectArray.begin) / elementSize;
        const auto capacity =
            (effectArray.capacity - effectArray.begin) / elementSize;
        if (count > kMaximumEffects || capacity > kMaximumEffects) {
            return FailLocalPlayerEffects("invalid_array");
        }

        const auto pointerBytes = count * elementSize;
        if (pointerBytes != 0 &&
            !platform::TryCopyBytes(
                reinterpret_cast<const void*>(effectArray.begin),
                effectPointers.data(),
                pointerBytes)) {
            return FailLocalPlayerEffects("array_storage_unavailable");
        }

        for (std::size_t index = 0; index < count; ++index) {
            const auto effect = effectPointers[index];
            if (effect != 0) {
                if (effect >
                    (std::numeric_limits<std::uintptr_t>::max)() -
                        profile.effectTargetOffset) {
                    return FailLocalPlayerEffects("effect_retain_failed");
                }
                std::uintptr_t target{};
                if (!platform::TryCopyBytes(
                        reinterpret_cast<const void*>(
                            effect + profile.effectTargetOffset),
                        &target,
                        sizeof(target))) {
                    return FailLocalPlayerEffects("effect_retain_failed");
                }
                if (target != localTarget) {
                    continue;
                }

                StrongEffect retained{virtualQuery, nullptr};
                if (!TryRetainListedEffect(
                        virtualQuery,
                        effect,
                        retained)) {
                    return FailLocalPlayerEffects("effect_retain_failed");
                }
                effects.push_back(std::move(retained));
            }
        }
    }

    std::uintptr_t currentPlayer{};
    if (!ReadNativeActiveEffectSourcePlayer(runtime, currentPlayer) ||
        currentPlayer != player) {
        return FailLocalPlayerEffects("source_changed");
    }
    sourcePlayer = player;
    return LocalPlayerEffectsStatus::Success;
}

}

bool ResolveNativeActiveEffectRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActiveEffectProfile& profile,
    ResolvedNativeActiveEffectRuntime& runtime) noexcept {
    runtime = {};
    if (virtualQuery == nullptr || moduleBase == 0 ||
        !IsConfigured(profile) ||
        !platform::ValidateVtables(
            virtualQuery,
            moduleBase,
            {profile.localPlayerMagicTargetVtable,
             profile.localPlayerPrimaryVtable})) {
        return false;
    }

    ResolvedNativeActiveEffectRuntime candidate{
        .layout = profile,
    };
    if (!platform::ResolveRequiredRvas(moduleBase, {
            {profile.localPlayerGlobal, &candidate.localPlayerGlobal},
            {profile.localPlayerPrimaryVtable,
             &candidate.localPlayerPrimaryVtable},
            {profile.localPlayerMagicTargetVtable,
             &candidate.localPlayerMagicTargetVtable},
            {profile.releaseActiveEffectsReadLock,
             &candidate.releaseActiveEffectsReadLock},
            {profile.isEligible, &candidate.isEligible},
            {profile.isDisplayable, &candidate.isDisplayable},
            {profile.getMagnitudeDisplayInfo,
             &candidate.getMagnitudeDisplayInfo},
            {profile.formatEffectText, &candidate.formatEffectText},
            {profile.deallocate, &candidate.deallocate}})) {
        return false;
    }

    std::uintptr_t getActiveEffectsSlot{};
    if (!platform::TryAddAddress(
            candidate.localPlayerMagicTargetVtable,
            profile.getActiveEffectsVtableOffset,
            getActiveEffectsSlot) ||
        !platform::ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(getActiveEffectsSlot),
            &candidate.getActiveEffects,
            sizeof(candidate.getActiveEffects))) {
        return false;
    }

    if (!platform::AreExecutableAddresses(virtualQuery, {
            candidate.getActiveEffects,
            candidate.releaseActiveEffectsReadLock,
            candidate.isEligible,
            candidate.isDisplayable,
            candidate.getMagnitudeDisplayInfo,
            candidate.formatEffectText,
            candidate.deallocate}) ||
        !platform::AreReadableAddresses(
            virtualQuery, {candidate.localPlayerGlobal})) {
        return false;
    }

    runtime = candidate;
    return true;
}

bool ReadNativeActiveEffectSourcePlayer(
    const ResolvedNativeActiveEffectRuntime& runtime,
    std::uintptr_t& sourcePlayer) noexcept {
    sourcePlayer = 0;
    std::uintptr_t player{};
    if (runtime.localPlayerGlobal == 0) {
        return false;
    }

    // ResolveNativeActiveEffectRuntime validates this module-owned slot once
    // during installation. Its contents can change, so callers still read it
    // before and after capture, but re-querying the same module page does not
    // add lifetime or consistency protection.
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.localPlayerGlobal),
            &player,
            sizeof(player))) {
        return false;
    }
    if (player == 0 || player % alignof(std::uintptr_t) != 0) {
        return false;
    }
    sourcePlayer = player;
    return true;
}

bool BuildNativeActiveEffectJsonSnapshot(
    platform::VirtualQueryFn virtualQuery,
    const ResolvedNativeActiveEffectRuntime& runtime,
    std::string& json,
    bool* lockBusy,
    std::uintptr_t* sourcePlayer) noexcept {
    try {
        json.clear();
        if (lockBusy != nullptr) {
            *lockBusy = false;
        }
        if (sourcePlayer != nullptr) {
            *sourcePlayer = 0;
        }
        if (virtualQuery == nullptr ||
            !IsConfigured(runtime.layout) || runtime.getActiveEffects == 0) {
            return false;
        }
        const auto& profile = runtime.layout;

        const auto isEligible =
            reinterpret_cast<EffectPredicate>(runtime.isEligible);
        const auto isDisplayable =
            reinterpret_cast<EffectDisplayPredicate>(runtime.isDisplayable);
        const auto getMagnitudeDisplayInfo =
            reinterpret_cast<GetMagnitudeDisplayInfo>(
                runtime.getMagnitudeDisplayInfo);
        const auto format =
            reinterpret_cast<FormatEffectText>(runtime.formatEffectText);
        const auto deallocate =
            reinterpret_cast<Deallocate>(runtime.deallocate);

        std::vector<StrongEffect> retainedEffects;
        std::uintptr_t verifiedSourcePlayer{};
        const auto localPlayerStatus = EnumerateLocalPlayerEffects(
            virtualQuery,
            runtime,
            retainedEffects,
            verifiedSourcePlayer);
        if (localPlayerStatus == LocalPlayerEffectsStatus::LockBusy) {
            if (lockBusy != nullptr) {
                *lockBusy = true;
            }
            return false;
        }
        if (localPlayerStatus != LocalPlayerEffectsStatus::Success) {
            return false;
        }
        std::vector<EffectGroupSnapshot> effectGroups;
        effectGroups.reserve(retainedEffects.size());
        for (const auto& retainedEffect : retainedEffects) {
            void* const effect = retainedEffect.get();
            void* effectArgument = effect;
            const auto effectAddress =
                reinterpret_cast<std::uintptr_t>(effect);
            std::uintptr_t spell{};
            if (!TryReadEffectSpell(profile, effectAddress, spell)) {
                continue;
            }
            float elapsed{};
            float duration{};
            if (effectAddress >
                    (std::numeric_limits<std::uintptr_t>::max)() -
                        profile.effectElapsedTimeOffset ||
                effectAddress >
                    (std::numeric_limits<std::uintptr_t>::max)() -
                        profile.effectDurationOffset) {
                continue;
            }
            if (!platform::TryCopyBytes(
                    reinterpret_cast<const void*>(
                        effectAddress + profile.effectElapsedTimeOffset),
                    &elapsed,
                    sizeof(elapsed)) ||
                !platform::TryCopyBytes(
                    reinterpret_cast<const void*>(
                        effectAddress + profile.effectDurationOffset),
                    &duration,
                    sizeof(duration))) {
                continue;
            }

            const bool eligible = isEligible(&effectArgument);
            const bool displayable = eligible &&
                isDisplayable(&effectArgument, duration);
            if (!displayable) {
                continue;
            }

            const float remaining = duration - elapsed;
            const bool hasDuration =
                std::isfinite(duration) &&
                std::isfinite(elapsed) &&
                std::isfinite(remaining) &&
                remaining > 0.0F &&
                remaining <= 2147483600.0F;

            MagnitudeDisplayInfo magnitudeInfo{};
            const auto* const magnitudeResult = getMagnitudeDisplayInfo(
                &magnitudeInfo,
                &effectArgument);
            if (magnitudeResult != &magnitudeInfo ||
                (magnitudeInfo.hasMagnitude != 0 &&
                    !std::isfinite(magnitudeInfo.magnitude))) {
                DiagnosticLog(
                    "xScal: StatsEffects snapshot failed: magnitude_info");
                return false;
            }

            GameString formatted{};
            std::uint8_t usedFallback{};
            format(&effectArgument, &formatted, &usedFallback);
            const GameAllocationGuard formattedOwner{
                deallocate,
                formatted.data};
            if (formatted.data == nullptr || formatted.length == 0) {
                continue;
            }
            EffectEntrySnapshot entrySnapshot{
                .duration = duration,
                .elapsed = elapsed,
                .remaining = remaining,
                .magnitude = magnitudeInfo.magnitude,
                .hasDuration = hasDuration,
                .hasMagnitude = magnitudeInfo.hasMagnitude != 0,
                .isPercentage = magnitudeInfo.isPercentage != 0,
            };
            entrySnapshot.text.resize(formatted.length);
            if (!platform::TryCopyBytes(
                    formatted.data,
                    entrySnapshot.text.data(),
                    formatted.length)) {
                DiagnosticLog(
                    "xScal: StatsEffects snapshot failed: formatted_text");
                return false;
            }

            auto group = std::find_if(
                effectGroups.begin(),
                effectGroups.end(),
                [spell](const EffectGroupSnapshot& candidate) {
                    return candidate.spell == spell;
                });
            if (group == effectGroups.end()) {
                std::string effectName;
                if (!ReadSpellName(profile, spell, effectName)) {
                    continue;
                }
                effectGroups.push_back({
                    .spell = spell,
                    .name = std::move(effectName),
                });
                group = std::prev(effectGroups.end());
                group->entries.reserve(1);
            }
            const auto duplicate = std::find_if(
                group->entries.begin(),
                group->entries.end(),
                [&entrySnapshot](const EffectEntrySnapshot& existing) {
                    return EqualsAsciiIgnoreCase(
                        std::string_view{existing.text},
                        std::string_view{entrySnapshot.text});
                });
            if (duplicate == group->entries.end()) {
                group->entries.push_back(std::move(entrySnapshot));
                continue;
            }

            if (!duplicate->hasDuration && entrySnapshot.hasDuration) {
                duplicate->duration = entrySnapshot.duration;
                duplicate->elapsed = entrySnapshot.elapsed;
                duplicate->remaining = entrySnapshot.remaining;
                duplicate->hasDuration = true;
            }
            if (entrySnapshot.hasMagnitude) {
                if (duplicate->hasMagnitude) {
                    duplicate->magnitude += entrySnapshot.magnitude;
                    if (!std::isfinite(duplicate->magnitude)) {
                        DiagnosticLog(
                            "xScal: StatsEffects snapshot failed: "
                            "stacked_magnitude");
                        return false;
                    }
                } else {
                    duplicate->magnitude = entrySnapshot.magnitude;
                    duplicate->hasMagnitude = true;
                }
                duplicate->isPercentage =
                    duplicate->isPercentage || entrySnapshot.isPercentage;
                duplicate->isStacked = true;
            }
        }
        std::string snapshot;
        snapshot.reserve(32 + retainedEffects.size() * 64);
        snapshot.append(R"({"EffectsA":[)");
        for (std::size_t groupIndex = 0;
             groupIndex < effectGroups.size();
             ++groupIndex) {
            if (groupIndex != 0) {
                snapshot.push_back(',');
            }
            const auto& group = effectGroups[groupIndex];
            snapshot.append("{\"Name\":");
            json::AppendString(group.name, snapshot);
            snapshot.append(",\"EffectEntriesA\":[");
            for (std::size_t entryIndex = 0;
                 entryIndex < group.entries.size();
                 ++entryIndex) {
                if (entryIndex != 0) {
                    snapshot.push_back(',');
                }
                const auto& entry = group.entries[entryIndex];
                snapshot.append("{\"text\":");
                json::AppendString(entry.text, snapshot);
                // Float fields are appended only when their values were checked
                // finite while the entry was built, so AppendFloat cannot fail.
                if (entry.hasDuration) {
                    snapshot.append(",\"duration\":");
                    (void)json::AppendFloat(entry.duration, snapshot);
                    snapshot.append(",\"elapsed\":");
                    (void)json::AppendFloat(entry.elapsed, snapshot);
                    snapshot.append(",\"remaining\":");
                    (void)json::AppendFloat(entry.remaining, snapshot);
                }
                if (entry.hasMagnitude) {
                    snapshot.append(",\"magnitude\":");
                    (void)json::AppendFloat(entry.magnitude, snapshot);
                    snapshot.append(
                        entry.isPercentage
                            ? ",\"isPercentage\":true"
                            : ",\"isPercentage\":false");
                    snapshot.append(
                        entry.isStacked
                            ? ",\"isStacked\":true"
                            : ",\"isStacked\":false");
                }
                snapshot.push_back('}');
            }
            snapshot.append("]}");
        }
        snapshot.append("]}");
        if (snapshot.size() > kMaximumSnapshotLength) {
            DiagnosticLog(
                "xScal: StatsEffects snapshot failed: maximum_length");
            return false;
        }
        json = std::move(snapshot);
        if (sourcePlayer != nullptr) {
            *sourcePlayer = verifiedSourcePlayer;
        }
        return true;
    } catch (...) {
        json.clear();
        DiagnosticLog("xScal: StatsEffects snapshot failed: exception");
        return false;
    }
}

}
