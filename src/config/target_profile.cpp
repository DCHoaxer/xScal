#include "config/target_profile.hpp"

#include "common/ascii_case.hpp"
#include "xscal/address_ids.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

// One Address Library value of a profile struct: its ID, the byte offset of
// the member, and how it is read. Databases for builds without a verified
// layout carry RequiredConstant values as 0, which disables the profile.
enum class FieldKind : std::uint8_t {
    Rva,
    Constant,
    RequiredConstant,
};

struct ProfileField final {
    std::uint32_t id;
    std::uint16_t offset;
    FieldKind kind;
};

[[nodiscard]] constexpr ProfileField Field(
    std::uint32_t id,
    std::size_t offset,
    FieldKind kind) noexcept {
    return {id, static_cast<std::uint16_t>(offset), kind};
}

static_assert(sizeof(std::uintptr_t) == sizeof(std::size_t));

// Profile members are uintptr_t or size_t, so every field is written as one
// pointer-sized value.
[[nodiscard]] bool ReadFields(
    const XScalAddressLibraryV1& addresses,
    const ProfileField* fields,
    std::size_t count,
    std::byte* profile) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        const auto& field = fields[index];
        std::size_t value{};
        if (field.kind == FieldKind::Rva) {
            std::uintptr_t rva{};
            if (!ReadRva(addresses, field.id, rva)) {
                return false;
            }
            value = rva;
        } else if (!ReadConstant(addresses, field.id, value) ||
                   (field.kind == FieldKind::RequiredConstant && value == 0)) {
            return false;
        }
        std::memcpy(profile + field.offset, &value, sizeof(value));
    }
    return true;
}

// Stores the profile only when every field resolves.
template <typename Profile, std::size_t Count>
void ReadProfileFields(
    const XScalAddressLibraryV1& addresses,
    const std::array<ProfileField, Count>& fields,
    Profile& profile) noexcept {
    profile = {};
    Profile result{};
    if (ReadFields(
            addresses,
            fields.data(),
            Count,
            reinterpret_cast<std::byte*>(&result))) {
        profile = result;
    }
}

constexpr std::array kActorValueFields{
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_GLOBAL,
        offsetof(config::NativeActorValueProfile, localPlayerGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_PRIMARY_VTABLE,
        offsetof(config::NativeActorValueProfile, localPlayerPrimaryVtable),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_ACTOR_VALUE_OWNER_VTABLE,
        offsetof(config::NativeActorValueProfile, localPlayerActorValueOwnerVtable),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_ACTOR_VALUE_REGISTRY,
        offsetof(config::NativeActorValueProfile, actorValueRegistry),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_ACTOR_VALUE_INFO_VTABLE,
        offsetof(config::NativeActorValueProfile, actorValueInfoVtable),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_ACTOR_VALUE_OWNER_OFFSET,
        offsetof(config::NativeActorValueProfile, actorValueOwnerOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_GET_CURRENT_ACTOR_VALUE_VTABLE_OFFSET,
        offsetof(config::NativeActorValueProfile, getCurrentActorValueVtableOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FORM_ID_OFFSET,
        offsetof(config::NativeActorValueProfile, formIdOffset),
        FieldKind::RequiredConstant),
};

void ReadActorValueProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativeActorValueProfile& actorValues) noexcept {
    ReadProfileFields(addresses, kActorValueFields, actorValues);
}

constexpr std::array kPipBoySpecialFields{
    Field(
        XSCAL_CONSTANT_STRENGTH_REGISTRY_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, registryOffsets),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERCEPTION_REGISTRY_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, registryOffsets) + 1 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_ENDURANCE_REGISTRY_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, registryOffsets) + 2 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_CHARISMA_REGISTRY_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, registryOffsets) + 3 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_INTELLIGENCE_REGISTRY_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, registryOffsets) + 4 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_AGILITY_REGISTRY_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, registryOffsets) + 5 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_LUCK_REGISTRY_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, registryOffsets) + 6 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_ADDRESS_GET_SPECIAL_BONUS_VALUE,
        offsetof(config::NativePipBoySpecialProfile, getSpecialBonusValue),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GAME_DEALLOCATE,
        offsetof(config::NativePipBoySpecialProfile, deallocate),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_ACTOR_VALUE_FULL_NAME_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, actorValueFullNameOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_GET_FULL_NAME_VTABLE_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, getFullNameVtableOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_ACTOR_VALUE_DESCRIPTION_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, actorValueDescriptionOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_GET_DESCRIPTION_VTABLE_OFFSET,
        offsetof(config::NativePipBoySpecialProfile, getDescriptionVtableOffset),
        FieldKind::RequiredConstant),
};

void ReadPipBoySpecialProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePipBoySpecialProfile& special) noexcept {
    ReadProfileFields(addresses, kPipBoySpecialFields, special);
}

constexpr std::array kPipBoyPlayerInfoFields{
    Field(
        XSCAL_ADDRESS_GET_TOTAL_EXPERIENCE_FOR_LEVEL,
        offsetof(config::NativePipBoyPlayerInfoProfile, getTotalExperienceForLevel),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_PLAYER_LEVEL_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyPlayerInfoProfile, playerLevelRegistryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_EXPERIENCE_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyPlayerInfoProfile, experienceRegistryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_GET_DISPLAY_NAME_VTABLE_OFFSET,
        offsetof(config::NativePipBoyPlayerInfoProfile, getDisplayNameVtableOffset),
        FieldKind::RequiredConstant),
};

void ReadPipBoyPlayerInfoProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePipBoyPlayerInfoProfile& playerInfo) noexcept {
    ReadProfileFields(addresses, kPipBoyPlayerInfoFields, playerInfo);
}

constexpr std::array kPipBoyPerksFields{
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_GLOBAL,
        offsetof(config::NativePipBoyPerksProfile, localPlayerGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_PRIMARY_VTABLE,
        offsetof(config::NativePipBoyPerksProfile, localPlayerPrimaryVtable),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_PLAYER_GAME_MODE_INDEX,
        offsetof(config::NativePipBoyPerksProfile, getPlayerGameModeIndex),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_PERK_CARD_RANK_PERK,
        offsetof(config::NativePipBoyPerksProfile, getPerkCardRankPerk),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_PERK_DESCRIPTION_TEXT,
        offsetof(config::NativePipBoyPerksProfile, getPerkDescriptionText),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_PERK_CARD_RANK_POINT_COST,
        offsetof(config::NativePipBoyPerksProfile, getPerkCardRankPointCost),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GAME_DEALLOCATE,
        offsetof(config::NativePipBoyPerksProfile, deallocate),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_PLAYER_PERK_CARD_TREE_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, playerPerkCardTreeOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_NODE_CARD_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, nodeCardOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_NODE_UNIQUE_ID_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, nodeUniqueIdOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_NODE_RANK_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, nodeRankOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_NODE_EQUIPPED_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, nodeEquippedOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_NODE_ANIMATED_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, nodeAnimatedOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_DESCRIPTION_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, cardDescriptionOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_RANK_COUNT_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, cardRankCountOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_LEVEL_REQUIREMENT_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, cardLevelRequirementOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_SPECIAL_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, cardSpecialOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_RACE_RESTRICTION_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, cardRaceRestrictionOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_GAME_MODE_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, cardGameModeOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_FULL_NAME_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, perkFullNameOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_GET_FULL_NAME_VTABLE_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, getFullNameVtableOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_GET_DESCRIPTION_VTABLE_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, getDescriptionVtableOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FORM_ID_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, formIdOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FORM_TYPE_OFFSET,
        offsetof(config::NativePipBoyPerksProfile, formTypeOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_PERK_CARD_FORM_TYPE,
        offsetof(config::NativePipBoyPerksProfile, perkCardFormType),
        FieldKind::RequiredConstant),
};

void ReadPipBoyPerksProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePipBoyPerksProfile& perks) noexcept {
    ReadProfileFields(addresses, kPipBoyPerksFields, perks);
}

constexpr std::array kPipBoyStatusFields{
    Field(
        XSCAL_CONSTANT_HEAD_CONDITION_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, limbConditionRegistryOffsets),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_LEFT_ARM_CONDITION_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, limbConditionRegistryOffsets) + 1 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RIGHT_ARM_CONDITION_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, limbConditionRegistryOffsets) + 2 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_LEFT_LEG_CONDITION_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, limbConditionRegistryOffsets) + 3 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RIGHT_LEG_CONDITION_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, limbConditionRegistryOffsets) + 4 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_DAMAGE_RESIST_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_POISON_RESIST_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets) + 1 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FIRE_RESIST_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets) + 2 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_ELECTRIC_RESIST_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets) + 3 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FROST_RESIST_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets) + 4 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_MAGIC_RESIST_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets) + 5 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RAD_RESIST_INGESTION_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets) + 6 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RAD_RESIST_EXPOSURE_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets) + 7 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_ENERGY_RESIST_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, resistRegistryOffsets) + 8 * sizeof(std::size_t),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_ADDRESS_GET_ACTOR_VALUE_MODIFIER_BY_TYPE,
        offsetof(config::NativePipBoyStatusProfile, getActorValueModifierByType),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_ACTOR_CARRY_WEIGHT_VALUES,
        offsetof(config::NativePipBoyStatusProfile, getActorCarryWeightValues),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_PLAYER_CAPS_COUNT,
        offsetof(config::NativePipBoyStatusProfile, getPlayerCapsCount),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_IS_ACTOR_GHOUL_RACE,
        offsetof(config::NativePipBoyStatusProfile, isActorGhoulRace),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GAME_CALENDAR_GLOBAL,
        offsetof(config::NativePipBoyStatusProfile, gameCalendarGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_GAME_MONTH,
        offsetof(config::NativePipBoyStatusProfile, getGameMonth),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_GAME_DAY,
        offsetof(config::NativePipBoyStatusProfile, getGameDay),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_GAME_HOUR,
        offsetof(config::NativePipBoyStatusProfile, getGameHour),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GAME_YEAR_SETTING_VALUE,
        offsetof(config::NativePipBoyStatusProfile, gameYearSettingValue),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_LETHAL_RADS_SETTING_VALUE,
        offsetof(config::NativePipBoyStatusProfile, lethalRadsSettingValue),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_HEALTH_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, healthRegistryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RAD_HEALTH_MAX_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, radHealthMaxRegistryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_ACTION_POINTS_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, actionPointsRegistryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RADS_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, radsRegistryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FOOD_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, foodRegistryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_WATER_REGISTRY_OFFSET,
        offsetof(config::NativePipBoyStatusProfile, waterRegistryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_DAMAGE_MODIFIER_TYPE,
        offsetof(config::NativePipBoyStatusProfile, damageModifierType),
        FieldKind::RequiredConstant),
};

void ReadPipBoyStatusProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePipBoyStatusProfile& status) noexcept {
    ReadProfileFields(addresses, kPipBoyStatusFields, status);
}

constexpr std::array kPipBoyQuestsFields{
    Field(
        XSCAL_ADDRESS_OBJECTIVE_STATE_ENTRIES_GLOBAL,
        offsetof(config::NativePipBoyQuestsProfile, objectiveStateEntriesGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_OBJECTIVE_STATE_CAPACITY_GLOBAL,
        offsetof(config::NativePipBoyQuestsProfile, objectiveStateCapacityGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_QUEST_FROM_HANDLE,
        offsetof(config::NativePipBoyQuestsProfile, getQuestFromHandle),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_QUEST_OBJECTIVE_BY_ID,
        offsetof(config::NativePipBoyQuestsProfile, getQuestObjectiveById),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_FIXED_STRING_TEXT,
        offsetof(config::NativePipBoyQuestsProfile, getFixedStringText),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_ASSIGN_GAME_STRING,
        offsetof(config::NativePipBoyQuestsProfile, assignGameString),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_FORMAT_QUEST_TEXT,
        offsetof(config::NativePipBoyQuestsProfile, formatQuestText),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GAME_DEALLOCATE,
        offsetof(config::NativePipBoyQuestsProfile, deallocate),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_QUEST_STATE_ENTRIES_GLOBAL,
        offsetof(config::NativePipBoyQuestsProfile, questStateEntriesGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_QUEST_STATE_CAPACITY_GLOBAL,
        offsetof(config::NativePipBoyQuestsProfile, questStateCapacityGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_FIXED_STRING_CHARACTERS,
        offsetof(config::NativePipBoyQuestsProfile, getFixedStringCharacters),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_OBJECTIVE_STATE_ENTRY_SIZE,
        offsetof(config::NativePipBoyQuestsProfile, objectiveStateEntrySize),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_OBJECTIVE_STATE_CHAIN_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, objectiveStateChainOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_OBJECTIVE_STATE_VALUE_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, objectiveStateValueOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_QUEST_NAME_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, questNameOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_QUEST_TYPE_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, questTypeOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_HIDDEN_QUEST_TYPE,
        offsetof(config::NativePipBoyQuestsProfile, hiddenQuestType),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_QUEST_FLAGS_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, questFlagsOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_OBJECTIVE_FLAGS_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, objectiveFlagsOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_QUEST_STATE_ENTRY_SIZE,
        offsetof(config::NativePipBoyQuestsProfile, questStateEntrySize),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_QUEST_STATE_CHAIN_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, questStateChainOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_QUEST_STATE_FLAGS_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, questStateFlagsOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_QUEST_SWF_FILE_OFFSET,
        offsetof(config::NativePipBoyQuestsProfile, questSwfFileOffset),
        FieldKind::RequiredConstant),
};

void ReadPipBoyQuestsProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePipBoyQuestsProfile& quests) noexcept {
    ReadProfileFields(addresses, kPipBoyQuestsFields, quests);
}

constexpr std::array kPipBoyRadioFields{
    Field(
        XSCAL_ADDRESS_RADIO_LOCK,
        offsetof(config::NativePipBoyRadioProfile, radioLock),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_RADIO_STATION_MAP,
        offsetof(config::NativePipBoyRadioProfile, radioStationMap),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_RADIO_MANAGER_GLOBAL,
        offsetof(config::NativePipBoyRadioProfile, radioManagerGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_TRY_ACQUIRE_READ_LOCK,
        offsetof(config::NativePipBoyRadioProfile, tryAcquireReadLock),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_RELEASE_READ_LOCK,
        offsetof(config::NativePipBoyRadioProfile, releaseReadLock),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_FIXED_STRING_CHARACTERS,
        offsetof(config::NativePipBoyRadioProfile, getFixedStringCharacters),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_RADIO_STATION_MAP_ENTRIES_OFFSET,
        offsetof(config::NativePipBoyRadioProfile, radioStationMapEntriesOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RADIO_STATION_MAP_CAPACITY_OFFSET,
        offsetof(config::NativePipBoyRadioProfile, radioStationMapCapacityOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RADIO_STATION_ENTRY_SIZE,
        offsetof(config::NativePipBoyRadioProfile, radioStationEntrySize),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RADIO_STATION_NAME_OFFSET,
        offsetof(config::NativePipBoyRadioProfile, radioStationNameOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RADIO_STATION_CHAIN_OFFSET,
        offsetof(config::NativePipBoyRadioProfile, radioStationChainOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RADIO_TUNED_FREQUENCY_OFFSET,
        offsetof(config::NativePipBoyRadioProfile, radioTunedFrequencyOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_RADIO_POWERED_OFFSET,
        offsetof(config::NativePipBoyRadioProfile, radioPoweredOffset),
        FieldKind::RequiredConstant),
};

void ReadPipBoyRadioProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePipBoyRadioProfile& radio) noexcept {
    ReadProfileFields(addresses, kPipBoyRadioFields, radio);
}

constexpr std::array kPipBoyCollectionsFields{
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_GLOBAL,
        offsetof(config::NativePipBoyCollectionsProfile, localPlayerGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_PRIMARY_VTABLE,
        offsetof(config::NativePipBoyCollectionsProfile, localPlayerPrimaryVtable),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_COLLECTION_MANAGER_GLOBAL,
        offsetof(config::NativePipBoyCollectionsProfile, collectionManagerGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_FORM_COMPONENT,
        offsetof(config::NativePipBoyCollectionsProfile, getFormComponent),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_COLLECTION_OWNED_COUNT,
        offsetof(config::NativePipBoyCollectionsProfile, getCollectionOwnedCount),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_IS_COLLECTION_GATE_OPEN,
        offsetof(config::NativePipBoyCollectionsProfile, isCollectionGateOpen),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_COLLECTION_GATE_OBJECT,
        offsetof(config::NativePipBoyCollectionsProfile, collectionGateObject),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_FIXED_STRING_TEXT,
        offsetof(config::NativePipBoyCollectionsProfile, getFixedStringText),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GAME_DEALLOCATE,
        offsetof(config::NativePipBoyCollectionsProfile, deallocate),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_COLLECTION_FORMS_OFFSET,
        offsetof(config::NativePipBoyCollectionsProfile, collectionFormsOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_COLLECTION_FORM_COUNT_OFFSET,
        offsetof(config::NativePipBoyCollectionsProfile, collectionFormCountOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_COLLECTION_FORM_TYPE,
        offsetof(config::NativePipBoyCollectionsProfile, collectionFormType),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_COLLECTION_NAME_OFFSET,
        offsetof(config::NativePipBoyCollectionsProfile, collectionNameOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_COLLECTION_CAPACITY_OFFSET,
        offsetof(config::NativePipBoyCollectionsProfile, collectionCapacityOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_COLLECTION_CATEGORY_OFFSET,
        offsetof(config::NativePipBoyCollectionsProfile, collectionCategoryOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_COLLECTION_GATED_CATEGORY,
        offsetof(config::NativePipBoyCollectionsProfile, collectionGatedCategory),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_COLLECTION_DESCRIPTION_SIGNATURE,
        offsetof(config::NativePipBoyCollectionsProfile, collectionDescriptionSignature),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FORM_ID_OFFSET,
        offsetof(config::NativePipBoyCollectionsProfile, formIdOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FORM_TYPE_OFFSET,
        offsetof(config::NativePipBoyCollectionsProfile, formTypeOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_GET_DESCRIPTION_VTABLE_OFFSET,
        offsetof(config::NativePipBoyCollectionsProfile, getDescriptionVtableOffset),
        FieldKind::RequiredConstant),
};

void ReadPipBoyCollectionsProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePipBoyCollectionsProfile& collections) noexcept {
    ReadProfileFields(addresses, kPipBoyCollectionsFields, collections);
}

constexpr std::array kPipBoyInventoryFields{
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_GLOBAL,
        offsetof(config::NativePipBoyInventoryProfile, localPlayerGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_LOCAL_PLAYER_PRIMARY_VTABLE,
        offsetof(config::NativePipBoyInventoryProfile, localPlayerPrimaryVtable),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_INVENTORY_LIST_VTABLE,
        offsetof(config::NativePipBoyInventoryProfile, inventoryListVtable),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_TRY_ACQUIRE_READ_LOCK,
        offsetof(config::NativePipBoyInventoryProfile, tryAcquireReadLock),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_RELEASE_READ_LOCK,
        offsetof(config::NativePipBoyInventoryProfile, releaseReadLock),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_DISPLAY_NAME,
        offsetof(config::NativePipBoyInventoryProfile, getItemDisplayName),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_RELEASE_FIXED_STRING,
        offsetof(config::NativePipBoyInventoryProfile, releaseFixedString),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_UNIT_WEIGHT,
        offsetof(config::NativePipBoyInventoryProfile, getItemUnitWeight),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_VALUE,
        offsetof(config::NativePipBoyInventoryProfile, getItemValue),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_EQUIP_STATE,
        offsetof(config::NativePipBoyInventoryProfile, getItemEquipState),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_FILTER_FLAGS,
        offsetof(config::NativePipBoyInventoryProfile, getItemFilterFlags),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_LEGENDARY_DATA,
        offsetof(config::NativePipBoyInventoryProfile, getItemLegendaryData),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_LEGENDARY_STARS,
        offsetof(config::NativePipBoyInventoryProfile, getItemLegendaryStars),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_IS_INVENTORY_ITEM_TRANSFER_LOCKED,
        offsetof(config::NativePipBoyInventoryProfile, isItemTransferLocked),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_CAN_INVENTORY_ITEM_BE_TRANSFER_LOCKED,
        offsetof(config::NativePipBoyInventoryProfile, canItemBeTransferLocked),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_IS_INVENTORY_ITEM_QUEST_ITEM,
        offsetof(config::NativePipBoyInventoryProfile, isQuestItem),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_CAN_DROP_OR_DESTROY_INVENTORY_ITEM,
        offsetof(config::NativePipBoyInventoryProfile, canDropOrDestroyItem),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_HAS_INVENTORY_ITEM_UNIQUE_EFFECT,
        offsetof(config::NativePipBoyInventoryProfile, hasItemUniqueEffect),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_BUILD_WEAPON_DAMAGE_LIST,
        offsetof(config::NativePipBoyInventoryProfile, buildWeaponDamageList),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_RELEASE_GAME_ARRAY,
        offsetof(config::NativePipBoyInventoryProfile, releaseGameArray),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_RESOLVE_OBJECT_INSTANCE,
        offsetof(config::NativePipBoyInventoryProfile, resolveObjectInstance),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_WEAPON_RATE_OF_FIRE,
        offsetof(config::NativePipBoyInventoryProfile, getWeaponRateOfFire),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_WEAPON_RANGE,
        offsetof(config::NativePipBoyInventoryProfile, getWeaponRange),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_WEAPON_ACCURACY,
        offsetof(config::NativePipBoyInventoryProfile, getWeaponAccuracy),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_WEAPON_INSTANCE_VALUE,
        offsetof(config::NativePipBoyInventoryProfile, getWeaponInstanceValue),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_WEAPON_AP_COST_FIELD,
        offsetof(config::NativePipBoyInventoryProfile, getWeaponApCostField),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_BUILD_ARMOR_RESISTANCE_LIST,
        offsetof(config::NativePipBoyInventoryProfile, buildArmorResistanceList),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_LEVEL,
        offsetof(config::NativePipBoyInventoryProfile, getItemLevel),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_DURABILITY,
        offsetof(config::NativePipBoyInventoryProfile, getItemDurability),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_CURRENT_CONDITION,
        offsetof(config::NativePipBoyInventoryProfile, getItemCurrentCondition),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_GET_INVENTORY_ITEM_MAXIMUM_CONDITION,
        offsetof(config::NativePipBoyInventoryProfile, getItemMaximumCondition),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_CAN_FAVORITE_FORM,
        offsetof(config::NativePipBoyInventoryProfile, canFavoriteForm),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_IS_FORM_TAGGED_FOR_SEARCH,
        offsetof(config::NativePipBoyInventoryProfile, isFormTaggedForSearch),
        FieldKind::Rva),
    Field(
        XSCAL_ADDRESS_SEARCH_TAG_LIST_GLOBAL,
        offsetof(config::NativePipBoyInventoryProfile, searchTagListGlobal),
        FieldKind::Rva),
    Field(
        XSCAL_CONSTANT_REFERENCE_INVENTORY_LIST_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, inventoryListOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_REFERENCE_INVENTORY_LOCK_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, inventoryLockOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_INVENTORY_ITEMS_BEGIN_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, itemsBeginOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_INVENTORY_ITEM_SIZE,
        offsetof(config::NativePipBoyInventoryProfile, itemSize),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_INVENTORY_ITEM_STACKS_BEGIN_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, itemStacksBeginOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_INVENTORY_STACK_SIZE,
        offsetof(config::NativePipBoyInventoryProfile, stackSize),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_INVENTORY_STACK_COUNT_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, stackCountOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_INVENTORY_ITEM_FAVORITE_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, itemFavoriteOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_INVENTORY_ITEM_HANDLE_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, itemHandleOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_FORM_TYPE_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, formTypeOffset),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_WEAPON_FORM_TYPE,
        offsetof(config::NativePipBoyInventoryProfile, weaponFormType),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_ARMOR_FORM_TYPE,
        offsetof(config::NativePipBoyInventoryProfile, armorFormType),
        FieldKind::RequiredConstant),
    Field(
        XSCAL_CONSTANT_WEAPON_TYPE_OFFSET,
        offsetof(config::NativePipBoyInventoryProfile, weaponTypeOffset),
        FieldKind::RequiredConstant),
};

void ReadPipBoyInventoryProfile(
    const XScalAddressLibraryV1& addresses,
    config::NativePipBoyInventoryProfile& inventory) noexcept {
    ReadProfileFields(addresses, kPipBoyInventoryFields, inventory);
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
    ReadActorValueProfile(addresses, result.dataManager.actorValues);
    ReadPipBoySpecialProfile(addresses, result.dataManager.pipBoySpecial);
    ReadPipBoyPlayerInfoProfile(addresses, result.dataManager.pipBoyPlayerInfo);
    ReadPipBoyPerksProfile(addresses, result.dataManager.pipBoyPerks);
    ReadPipBoyStatusProfile(addresses, result.dataManager.pipBoyStatus);
    ReadPipBoyQuestsProfile(addresses, result.dataManager.pipBoyQuests);
    ReadPipBoyInventoryProfile(addresses, result.dataManager.pipBoyInventory);
    ReadPipBoyRadioProfile(addresses, result.dataManager.pipBoyRadio);
    ReadPipBoyCollectionsProfile(addresses, result.dataManager.pipBoyCollections);
    ReadArchiveStreamProfile(addresses, result.archiveStream);

    std::size_t statsEffectsEnabled{};
    result.pipBoyStatsEffectsEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_STATS_EFFECTS_ENABLED, statsEffectsEnabled) &&
        statsEffectsEnabled != 0;
    std::size_t specialEnabled{};
    result.pipBoySpecialEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_SPECIAL_ENABLED, specialEnabled) &&
        specialEnabled != 0;
    std::size_t playerInfoEnabled{};
    result.pipBoyPlayerInfoEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_PLAYER_INFO_ENABLED, playerInfoEnabled) &&
        playerInfoEnabled != 0;
    std::size_t perksEnabled{};
    result.pipBoyPerksEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_PERKS_ENABLED, perksEnabled) &&
        perksEnabled != 0;
    std::size_t statusEnabled{};
    result.pipBoyStatusEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_STATUS_ENABLED, statusEnabled) &&
        statusEnabled != 0;
    std::size_t questsEnabled{};
    result.pipBoyQuestsEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_QUESTS_ENABLED, questsEnabled) &&
        questsEnabled != 0;
    std::size_t inventoryEnabled{};
    result.pipBoyInventoryEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_INVENTORY_ENABLED, inventoryEnabled) &&
        inventoryEnabled != 0;
    std::size_t radioEnabled{};
    result.pipBoyRadioEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_RADIO_ENABLED, radioEnabled) &&
        radioEnabled != 0;
    std::size_t collectionsEnabled{};
    result.pipBoyCollectionsEnabled =
        ReadConstant(addresses, XSCAL_CONSTANT_PIPBOY_COLLECTIONS_ENABLED, collectionsEnabled) &&
        collectionsEnabled != 0;
    profile = result;
    return true;
}

}
