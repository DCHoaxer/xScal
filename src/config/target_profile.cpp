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

void ReadArchiveStreamProfile(
    const XScalAddressLibraryV1& addresses,
    config::ArchiveStreamProfile& archiveStream) noexcept {
    archiveStream = {};
    config::ArchiveStreamProfile result{};
    const bool complete =
        ReadRva(addresses, XSCAL_ADDRESS_ARCHIVE_READER_STREAM_VTABLE, result.readerStreamVtable) &&
        ReadRva(addresses, XSCAL_ADDRESS_ARCHIVE_READER_STREAM_READ, result.read) &&
        ReadRva(addresses, XSCAL_ADDRESS_ARCHIVE_READER_STREAM_SEEK, result.seek) &&
        ReadRva(addresses, XSCAL_ADDRESS_ARCHIVE_READER_STREAM_CLOSE, result.close) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_READ_SLOT, result.readSlot) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_SEEK_SLOT, result.seekSlot) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_CLOSE_SLOT, result.closeSlot) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_BACKING_OFFSET, result.backingOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_STATE_OFFSET, result.stateOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_ENTRY_OFFSET_OFFSET, result.entryOffsetOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_PACKED_SIZE_OFFSET, result.packedSizeOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_UNPACKED_SIZE_OFFSET, result.unpackedSizeOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_FLAGS_OFFSET, result.flagsOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_ZLIB_FLAG, result.zlibFlag) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_UNSUPPORTED_FLAGS, result.unsupportedFlags) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_BACKING_ERROR_OFFSET, result.backingErrorOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_BACKING_READ_SLOT, result.backingReadSlot) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STATE_DECODED_WAITING_OFFSET, result.decodedWaitingOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STATE_PACKED_UNFED_OFFSET, result.packedUnfedOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STATE_PACKED_UNREQUESTED_OFFSET, result.packedUnrequestedOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STATE_ISSUE_INDEX_OFFSET, result.issueIndexOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STATE_WAIT_INDEX_OFFSET, result.waitIndexOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STATE_DELIVERED_OFFSET, result.deliveredOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_ARCHIVE_STREAM_UNSUPPORTED_RESULT, result.unsupportedResult);
    // A database for a build whose layout was not verified carries the
    // constants with value 0.
    if (complete &&
        result.readerStreamVtable != 0 &&
        result.read != 0 &&
        result.seek != 0 &&
        result.close != 0 &&
        result.readSlot != 0 &&
        result.seekSlot != 0 &&
        result.closeSlot != 0 &&
        result.backingOffset != 0 &&
        result.stateOffset != 0 &&
        result.entryOffsetOffset != 0 &&
        result.packedSizeOffset != 0 &&
        result.unpackedSizeOffset != 0 &&
        result.flagsOffset != 0 &&
        result.zlibFlag != 0 &&
        result.unsupportedFlags != 0 &&
        result.backingErrorOffset != 0 &&
        result.backingReadSlot != 0 &&
        result.decodedWaitingOffset != 0 &&
        result.packedUnfedOffset != 0 &&
        result.packedUnrequestedOffset != 0 &&
        result.issueIndexOffset != 0 &&
        result.waitIndexOffset != 0 &&
        result.deliveredOffset != 0 &&
        result.unsupportedResult != 0) {
        archiveStream = result;
    }
}

void ReadPlayerMapPositionProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePlayerMapPositionProfile& position) noexcept {
    position = {};
    config::NativePlayerMapPositionProfile result{};
    const bool complete =
        ReadRva(addresses, XSCAL_ADDRESS_LOCAL_PLAYER_GLOBAL, result.localPlayerGlobal) &&
        ReadRva(addresses, XSCAL_ADDRESS_LOCAL_PLAYER_PRIMARY_VTABLE, result.localPlayerPrimaryVtable) &&
        ReadRva(addresses, XSCAL_ADDRESS_MAP_WORLD_BOUNDS_GLOBAL, result.mapWorldBoundsGlobal) &&
        ReadConstant(addresses, XSCAL_CONSTANT_REFERENCE_POSITION_OFFSET, result.referencePositionOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_REFERENCE_PARENT_CELL_OFFSET, result.referenceParentCellOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_CELL_FLAGS_OFFSET, result.cellFlagsOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_CELL_WORLD_SPACE_OFFSET, result.cellWorldSpaceOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_WORLD_SPACE_MAP_SCALE_OFFSET, result.worldSpaceMapScaleOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_WORLD_SPACE_MAP_OFFSET_OFFSET, result.worldSpaceMapOffsetOffset) &&
        ReadConstant(addresses, XSCAL_CONSTANT_WORLD_SPACE_MAP_BOUNDS_OFFSET, result.worldSpaceMapBoundsOffset);
    // A database for a build whose layout was not verified carries these IDs
    // with value 0, so a zero constant means unavailable.
    if (complete &&
        result.referencePositionOffset != 0 &&
        result.referenceParentCellOffset != 0 &&
        result.cellFlagsOffset != 0 &&
        result.cellWorldSpaceOffset != 0 &&
        result.worldSpaceMapScaleOffset != 0 &&
        result.worldSpaceMapOffsetOffset != 0 &&
        result.worldSpaceMapBoundsOffset != 0) {
        position = result;
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
    ReadPlayerMapPositionProfile(addresses, result.dataManager.playerMapPosition);
    ReadArchiveStreamProfile(addresses, result.archiveStream);

    std::size_t statsEffectsEnabled{};
    result.pipBoyStatsEffectsEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_STATS_EFFECTS_ENABLED, statsEffectsEnabled) &&
        statsEffectsEnabled != 0;
    profile = result;
    return true;
}

}
