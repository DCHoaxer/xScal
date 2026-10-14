#include "config/target_profile.hpp"

#include "common/ascii_case.hpp"
#include "xscal/address_ids.h"

#include <limits>

namespace sf {
namespace {

constexpr TargetDefinition kSteamTarget{
    .kind = TargetKind::Steam,
    .executableName = L"Fallout76.exe",
};

constexpr TargetDefinition kGamePassTarget{
    .kind = TargetKind::GamePass,
    .executableName = L"Project76_GamePass.exe",
};

[[nodiscard]] bool ReadRva(
    const XScalAddressLibraryV1& addresses,
    std::uint64_t id,
    std::uintptr_t& value) noexcept {
    if (addresses.lookupRva == nullptr) {
        return false;
    }
    std::uint64_t raw{};
    if (!addresses.lookupRva(addresses.context, id, &raw) ||
        raw > (std::numeric_limits<std::uintptr_t>::max)()) {
        return false;
    }
    value = static_cast<std::uintptr_t>(raw);
    return true;
}

[[nodiscard]] bool ReadConstant(
    const XScalAddressLibraryV1& addresses,
    std::uint64_t id,
    std::size_t& value) noexcept {
    if (addresses.lookupConstant == nullptr) {
        return false;
    }
    std::uint64_t raw{};
    if (!addresses.lookupConstant(addresses.context, id, &raw) ||
        raw > (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }
    value = static_cast<std::size_t>(raw);
    return true;
}

// Each optional feature reader leaves its profile zeroed unless every ID it
// needs is present. A zeroed profile makes that feature's Install() fail.
void ReadStatsEffectsProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativeActiveEffectProfile& effects) noexcept {
    effects = {};
    config::NativeActiveEffectProfile result{};
    const bool complete =
        ReadRva(addresses, XSCAL_ADDRESS_LOCAL_PLAYER_GLOBAL, result.localPlayerGlobal) &&
        ReadRva(addresses, XSCAL_ADDRESS_LOCAL_PLAYER_PRIMARY_VTABLE, result.localPlayerPrimaryVtable) &&
        ReadRva(addresses, XSCAL_ADDRESS_LOCAL_PLAYER_MAGIC_TARGET_VTABLE, result.localPlayerMagicTargetVtable) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ACTIVE_EFFECTS_VTABLE_OFFSET, result.getActiveEffectsVtableOffset) &&
        ReadRva(addresses, XSCAL_ADDRESS_ACTIVE_EFFECTS_RELEASE_LOCK, result.releaseActiveEffectsReadLock) &&
        ReadRva(addresses, XSCAL_ADDRESS_ACTIVE_EFFECT_IS_ELIGIBLE, result.isEligible) &&
        ReadRva(addresses, XSCAL_ADDRESS_ACTIVE_EFFECT_IS_DISPLAYABLE, result.isDisplayable) &&
        ReadRva(addresses, XSCAL_ADDRESS_ACTIVE_EFFECT_MAGNITUDE_INFO, result.getMagnitudeDisplayInfo) &&
        ReadRva(addresses, XSCAL_ADDRESS_ACTIVE_EFFECT_FORMAT_TEXT, result.formatEffectText) &&
        ReadRva(addresses, XSCAL_ADDRESS_GAME_DEALLOCATE, result.deallocate) &&
        ReadConstant(addresses, XSCAL_CONSTANT_LOCAL_PLAYER_MAGIC_TARGET_OFFSET, result.localPlayerMagicTargetOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_LOCAL_PLAYER_SIZE, result.localPlayerSize) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ACTIVE_EFFECTS_LOCK_OFFSET, result.activeEffectsLockOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ACTIVE_EFFECT_ARRAY_ELEMENT_SIZE, result.effectArrayElementSize) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ACTIVE_EFFECT_SPELL_OFFSET, result.effectSpellOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ACTIVE_EFFECT_TARGET_OFFSET, result.effectTargetOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ACTIVE_EFFECT_ELAPSED_TIME_OFFSET, result.effectElapsedTimeOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ACTIVE_EFFECT_DURATION_OFFSET, result.effectDurationOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_SPELL_FULL_NAME_OFFSET, result.spellFullNameOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_GET_FULL_NAME_VTABLE_OFFSET, result.getFullNameVtableOffset);
    if (complete) {
        effects = result;
    }
}

void ReadEnemyMeterProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativeEnemyMeterProfile& enemy) noexcept {
    enemy = {};
    config::NativeEnemyMeterProfile result{};
    const bool complete =
        ReadRva(addresses, XSCAL_ADDRESS_TARGET_EVENT_SOURCE_GLOBAL, result.targetEventSourceGlobal) &&
        ReadRva(addresses, XSCAL_ADDRESS_TARGET_MODEL_VTABLE, result.targetModelVtable) &&
        ReadRva(addresses, XSCAL_ADDRESS_RESOLVE_OBJECT_HANDLE, result.resolveObjectHandle) &&
        ReadRva(addresses, XSCAL_ADDRESS_VATS_COLLECT_RESISTANCES, result.vatsCollectResistances) &&
        ReadConstant(addresses, XSCAL_CONSTANT_TARGET_EVENT_SOURCE_SINKS_OFFSET, result.targetEventSourceSinksOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_TARGET_MODEL_HANDLE_OFFSET, result.targetModelHandleOffset);
    if (complete) {
        enemy = result;
    }
}

void ReadTimeDataProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativeTimeDataProfile& time) noexcept {
    time = {};
    config::NativeTimeDataProfile result{};
    const bool complete =
        ReadRva(addresses, XSCAL_ADDRESS_CALENDAR_SINGLETON_GLOBAL, result.calendarSingletonGlobal) &&
        ReadRva(addresses, XSCAL_ADDRESS_TES_GLOBAL_VTABLE, result.tesGlobalVtable) &&
        ReadConstant(addresses, XSCAL_CONSTANT_CALENDAR_GAME_HOUR_OFFSET, result.calendarGameHourOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_TES_GLOBAL_VALUE_OFFSET, result.tesGlobalValueOffset);
    if (complete) {
        time = result;
    }
}

}

const TargetDefinition* SelectTarget(
    std::wstring_view executableName) noexcept {
    if (EqualsAsciiIgnoreCase(executableName, kSteamTarget.executableName)) {
        return &kSteamTarget;
    }
    if (EqualsAsciiIgnoreCase(executableName, kGamePassTarget.executableName)) {
        return &kGamePassTarget;
    }
    return nullptr;
}

bool ReadTargetProfile(
    const TargetDefinition& target,
    const config::RuntimeVersion& runtime,
    const XScalAddressLibraryV1& addresses,
    TargetProfile& profile) noexcept {
    if (addresses.size < sizeof(XScalAddressLibraryV1) ||
        addresses.version != XSCAL_ADDRESS_LIBRARY_API_VERSION) {
        return false;
    }

    // Core IDs are needed by the Scaleform bridge and the MovieRoot hook.
    TargetProfile result{};
    result.kind = target.kind;
    result.executableName = target.executableName;
    result.runtimeVersion = runtime;
    std::uintptr_t primaryVtable{};
    std::size_t getVariableSlot{};
    const bool core =
        ReadRva(addresses, XSCAL_ADDRESS_MOVIE_ROOT_PRIMARY_VTABLE, primaryVtable) &&
        ReadConstant(addresses, XSCAL_CONSTANT_MOVIE_ROOT_GET_VARIABLE_SLOT, getVariableSlot) &&
        primaryVtable <= (std::numeric_limits<std::uintptr_t>::max)() - getVariableSlot &&
        ReadRva(addresses, XSCAL_ADDRESS_SCALEFORM_GET_MEMBER, result.getMemberOffset) &&
        ReadRva(addresses, XSCAL_ADDRESS_SCALEFORM_SET_MEMBER, result.setMemberOffset) &&
        ReadRva(addresses, XSCAL_ADDRESS_SCALEFORM_RELEASE_VALUE, result.releaseValueOffset);
    if (!core) {
        return false;
    }
    result.primaryGetVariableSlotOffset = primaryVtable + getVariableSlot;

    // Optional: without it XScalScaleformMovieV2::invokeMember fails.
    if (!ReadRva(addresses, XSCAL_ADDRESS_SCALEFORM_INVOKE, result.invokeOffset)) {
        result.invokeOffset = 0;
    }

    ReadStatsEffectsProfile(addresses, result.dataManager.pipBoyStatsEffects);
    ReadEnemyMeterProfile(addresses, result.dataManager.enemyMeter);
    ReadTimeDataProfile(addresses, result.dataManager.timeData);

    std::size_t statsEffectsEnabled{};
    result.pipBoyStatsEffectsEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_STATS_EFFECTS_ENABLED, statsEffectsEnabled) &&
        statsEffectsEnabled != 0;
    profile = result;
    return true;
}

}
