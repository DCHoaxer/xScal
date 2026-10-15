#pragma once

#include "xscal/plugin_api.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sf::config {

struct RuntimeVersion final {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t build;
    std::uint16_t revision;

    [[nodiscard]] constexpr bool operator==(
        const RuntimeVersion&) const noexcept = default;
};

// Exact-build data needed to read Pip-Boy Stats Effects.
struct NativeActiveEffectProfile final {
    std::uintptr_t localPlayerGlobal;
    std::uintptr_t localPlayerPrimaryVtable;
    std::uintptr_t localPlayerMagicTargetVtable;
    std::size_t getActiveEffectsVtableOffset;
    // Never read from the Address Library. The acquire is repeated inline (see
    // TryAcquireActiveEffectsReadLockUnchecked), so this stays zero.
    std::uintptr_t acquireActiveEffectsReadLock;
    std::uintptr_t releaseActiveEffectsReadLock;

    std::uintptr_t isEligible;
    std::uintptr_t isDisplayable;
    std::uintptr_t getMagnitudeDisplayInfo;
    std::uintptr_t formatEffectText;
    std::uintptr_t deallocate;

    std::size_t localPlayerMagicTargetOffset;
    std::size_t localPlayerSize;
    std::size_t activeEffectsLockOffset;
    std::size_t effectArrayElementSize;

    std::size_t effectSpellOffset;
    std::size_t effectTargetOffset;
    std::size_t effectElapsedTimeOffset;
    std::size_t effectDurationOffset;

    std::size_t spellFullNameOffset;
    std::size_t getFullNameVtableOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativeActiveEffectProfile&) const noexcept = default;
};

// Exact-build data needed to read enemy-meter resistance values.
struct NativeEnemyMeterProfile final {
    std::uintptr_t targetEventSourceGlobal;
    std::uintptr_t targetModelVtable;
    std::uintptr_t resolveObjectHandle;
    std::uintptr_t vatsCollectResistances;

    std::size_t targetEventSourceSinksOffset;
    std::size_t targetModelHandleOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativeEnemyMeterProfile&) const noexcept = default;
};

// Exact-build data needed to read the Pip-Boy game clock.
struct NativeTimeDataProfile final {
    std::uintptr_t calendarSingletonGlobal;
    std::uintptr_t tesGlobalVtable;
    // Never read from the Address Library; the game hour is read from memory
    // directly. This stays zero.
    std::uintptr_t gameHourGetter;

    std::size_t calendarGameHourOffset;
    std::size_t tesGlobalValueOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativeTimeDataProfile&) const noexcept = default;
};

// Exact-build data needed to read the local player's position in the
// worldspace map frame.
struct NativePlayerMapPositionProfile final {
    std::uintptr_t localPlayerGlobal;
    std::uintptr_t localPlayerPrimaryVtable;
    std::uintptr_t mapWorldBoundsGlobal;

    std::size_t referencePositionOffset;
    std::size_t referenceParentCellOffset;
    std::size_t cellFlagsOffset;
    std::size_t cellWorldSpaceOffset;
    std::size_t worldSpaceMapScaleOffset;
    std::size_t worldSpaceMapOffsetOffset;
    std::size_t worldSpaceMapBoundsOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativePlayerMapPositionProfile&) const noexcept = default;
};

struct DataManagerProfile final {
    NativeActiveEffectProfile pipBoyStatsEffects;
    NativeEnemyMeterProfile enemyMeter;
    NativeTimeDataProfile timeData;
    NativePlayerMapPositionProfile playerMapPosition;

    [[nodiscard]] constexpr bool operator==(
        const DataManagerProfile&) const noexcept = default;
};

}

namespace sf {

enum class TargetKind {
    Steam,
    GamePass,
};

[[nodiscard]] constexpr const char* TargetKindName(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::Steam: return "steam";
        case TargetKind::GamePass: return "gamepass";
        default: return "unknown";
    }
}

struct TargetDefinition final {
    TargetKind kind;
    std::wstring_view executableName;
};

// Address Library RVAs are copied into this runtime profile before any game
// address is read. Consumers continue to add the executable module base.
struct TargetProfile final {
    TargetKind kind;
    std::wstring_view executableName;
    config::RuntimeVersion runtimeVersion;
    std::uintptr_t primaryGetVariableSlotOffset;
    std::uintptr_t getMemberOffset;
    std::uintptr_t setMemberOffset;
    std::uintptr_t releaseValueOffset;
    // ObjectInterface::Invoke. Zero when the Address Library has no record.
    std::uintptr_t invokeOffset;
    config::DataManagerProfile dataManager;
    bool pipBoyStatsEffectsEnabled;

    [[nodiscard]] constexpr bool operator==(
        const TargetProfile&) const noexcept = default;
};

[[nodiscard]] const TargetDefinition* SelectTarget(
    std::wstring_view executableName) noexcept;
[[nodiscard]] bool ReadTargetProfile(
    const TargetDefinition& target,
    const config::RuntimeVersion& runtime,
    const XScalAddressLibraryV1& addresses,
    TargetProfile& profile) noexcept;

}
