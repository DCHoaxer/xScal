#pragma once

#include "xscal/plugin_api.h"

#include <array>
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

// Exact-build data for the libdeflate archive decompression. All values or
// none.
struct ArchiveStreamProfile final {
    std::uintptr_t readerStreamVtable;
    std::uintptr_t read;
    std::uintptr_t seek;
    std::uintptr_t close;
    std::size_t readSlot;
    std::size_t seekSlot;
    std::size_t closeSlot;
    std::size_t backingOffset;
    std::size_t stateOffset;
    std::size_t entryOffsetOffset;
    std::size_t packedSizeOffset;
    std::size_t unpackedSizeOffset;
    std::size_t flagsOffset;
    std::size_t zlibFlag;
    std::size_t unsupportedFlags;
    std::size_t backingErrorOffset;
    std::size_t backingReadSlot;
    std::size_t decodedWaitingOffset;
    std::size_t packedUnfedOffset;
    std::size_t packedUnrequestedOffset;
    std::size_t issueIndexOffset;
    std::size_t waitIndexOffset;
    std::size_t deliveredOffset;
    std::size_t unsupportedResult;

    [[nodiscard]] constexpr bool operator==(
        const ArchiveStreamProfile&) const noexcept = default;
};

// Exact-build data needed to read ActorValues from the local player.
struct NativeActorValueProfile final {
    std::uintptr_t localPlayerGlobal;
    std::uintptr_t localPlayerPrimaryVtable;
    // Found through RTTI by PipBoyProviders::Install; not in the Address Library.
    std::uintptr_t localPlayerActorValueOwnerVtable;
    std::uintptr_t actorValueRegistry;
    // Found through RTTI by PipBoyProviders::Install; not in the Address Library.
    std::uintptr_t actorValueInfoVtable;

    std::size_t actorValueOwnerOffset;
    std::size_t getCurrentActorValueVtableOffset;
    std::size_t formIdOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativeActorValueProfile&) const noexcept = default;
};

// Registry slots of the SPECIAL ActorValueInfo pointers, in Strength,
// Perception, Endurance, Charisma, Intelligence, Agility, Luck order, and the
// data needed for the per-SPECIAL name, description and bonus.
struct NativePipBoySpecialProfile final {
    std::array<std::size_t, 7> registryOffsets;

    std::uintptr_t getSpecialBonusValue;
    std::uintptr_t deallocate;
    std::size_t actorValueFullNameOffset;
    std::size_t getFullNameVtableOffset;
    std::size_t actorValueDescriptionOffset;
    std::size_t getDescriptionVtableOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativePipBoySpecialProfile&) const noexcept = default;
};

// Exact-build data needed to read the local player's level and experience.
// Uses NativeActorValueProfile for the player and the ActorValue getter.
struct NativePipBoyPlayerInfoProfile final {
    std::uintptr_t getTotalExperienceForLevel;

    std::size_t playerLevelRegistryOffset;
    std::size_t experienceRegistryOffset;
    std::size_t getDisplayNameVtableOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativePipBoyPlayerInfoProfile&) const noexcept = default;
};

// Exact-build data needed to read the local player's perk cards.
struct NativePipBoyPerksProfile final {
    std::uintptr_t localPlayerGlobal;
    std::uintptr_t localPlayerPrimaryVtable;
    std::uintptr_t getPlayerGameModeIndex;
    std::uintptr_t getPerkCardRankPerk;
    std::uintptr_t getPerkDescriptionText;
    std::uintptr_t deallocate;

    std::size_t playerPerkCardTreeOffset;
    std::size_t nodeCardOffset;
    std::size_t nodeUniqueIdOffset;
    std::size_t nodeRankOffset;
    std::size_t nodeEquippedOffset;
    std::size_t nodeAnimatedOffset;
    std::size_t cardDescriptionOffset;
    std::size_t cardRankCountOffset;
    std::size_t cardLevelRequirementOffset;
    std::size_t cardSpecialOffset;
    std::size_t cardRaceRestrictionOffset;
    std::size_t cardGameModeOffset;
    std::size_t perkFullNameOffset;
    std::size_t getFullNameVtableOffset;
    std::size_t getDescriptionVtableOffset;
    std::size_t formIdOffset;
    std::size_t formTypeOffset;
    std::size_t perkCardFormType;
    // Rank point cost: the card's rank pointer array, then per rank a
    // TESGlobal pointer or, when it is null, a cost byte.
    std::size_t cardRanksOffset;
    std::size_t rankCostGlobalOffset;
    std::size_t rankCostOffset;
    std::size_t globalValueOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativePipBoyPerksProfile&) const noexcept = default;
};

// Exact-build data needed to read the local player's vitals, carry weight
// and caps. Uses NativeActorValueProfile for the player and the ActorValue
// getter.
struct NativePipBoyStatusProfile final {
    std::uintptr_t getActorValueModifierByType;
    std::uintptr_t getActorCarryWeightValues;
    std::uintptr_t getPlayerCapsCount;
    std::uintptr_t isActorGhoulRace;
    std::uintptr_t gameCalendarGlobal;
    std::uintptr_t gameYearSettingValue;
    std::uintptr_t lethalRadsSettingValue;

    std::size_t healthRegistryOffset;
    std::size_t radHealthMaxRegistryOffset;
    std::size_t actionPointsRegistryOffset;
    std::size_t radsRegistryOffset;
    std::size_t foodRegistryOffset;
    std::size_t waterRegistryOffset;
    std::size_t damageModifierType;
    // Head, left arm, right arm, left leg, right leg condition.
    std::array<std::size_t, 5> limbConditionRegistryOffsets;
    // DamageResist, PoisonResist, FireResist, ElectricResist, FrostResist,
    // MagicResist, RadResistIngestion, RadResistExposure, EnergyResist.
    std::array<std::size_t, 9> resistRegistryOffsets;
    // Calendar slots holding the month, day and hour TESGlobals, and the
    // value offset inside a TESGlobal.
    std::size_t calendarMonthOffset;
    std::size_t calendarDayOffset;
    std::size_t calendarHourOffset;
    std::size_t globalValueOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativePipBoyStatusProfile&) const noexcept = default;
};

// Exact-build data needed to read the quest objectives the Pip-Boy lists.
struct NativePipBoyQuestsProfile final {
    std::uintptr_t objectiveStateEntriesGlobal;
    // Entries global plus stateTableCapacityOffset.
    std::uintptr_t objectiveStateCapacityGlobal;
    std::uintptr_t getQuestFromHandle;
    std::uintptr_t getQuestObjectiveById;
    std::uintptr_t getFixedStringText;
    std::uintptr_t assignGameString;
    std::uintptr_t formatQuestText;
    std::uintptr_t deallocate;
    std::uintptr_t questStateEntriesGlobal;
    // Entries global plus stateTableCapacityOffset.
    std::uintptr_t questStateCapacityGlobal;
    std::uintptr_t getFixedStringCharacters;

    std::size_t objectiveStateEntrySize;
    std::size_t objectiveStateChainOffset;
    std::size_t objectiveStateValueOffset;
    std::size_t questNameOffset;
    std::size_t questTypeOffset;
    std::size_t hiddenQuestType;
    std::size_t questFlagsOffset;
    std::size_t objectiveFlagsOffset;
    std::size_t questStateEntrySize;
    std::size_t questStateChainOffset;
    std::size_t questStateFlagsOffset;
    std::size_t questSwfFileOffset;
    // Distance from a state table's entries pointer to its capacity.
    std::size_t stateTableCapacityOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativePipBoyQuestsProfile&) const noexcept = default;
};

// Exact-build data needed to read the local player's inventory items.
struct NativePipBoyInventoryProfile final {
    std::uintptr_t localPlayerGlobal;
    std::uintptr_t localPlayerPrimaryVtable;
    // Found through RTTI by PipBoyProviders::Install; not in the Address Library.
    std::uintptr_t inventoryListVtable;
    std::uintptr_t tryAcquireReadLock;
    std::uintptr_t releaseReadLock;
    std::uintptr_t getItemDisplayName;
    std::uintptr_t releaseFixedString;
    std::uintptr_t getItemUnitWeight;
    std::uintptr_t getItemValue;
    std::uintptr_t getItemEquipState;
    std::uintptr_t getItemFilterFlags;
    std::uintptr_t getItemLegendaryData;
    std::uintptr_t getItemLegendaryStars;
    std::uintptr_t isItemTransferLocked;
    std::uintptr_t canItemBeTransferLocked;
    std::uintptr_t isQuestItem;
    std::uintptr_t canDropOrDestroyItem;
    std::uintptr_t hasItemUniqueEffect;
    std::uintptr_t buildWeaponDamageList;
    std::uintptr_t releaseGameArray;
    std::uintptr_t resolveObjectInstance;
    std::uintptr_t getWeaponRateOfFire;
    std::uintptr_t getWeaponRange;
    std::uintptr_t getWeaponAccuracy;
    std::uintptr_t getWeaponInstanceValue;
    std::uintptr_t getWeaponApCostField;
    std::uintptr_t buildArmorResistanceList;
    std::uintptr_t getItemLevel;
    std::uintptr_t getItemDurability;
    std::uintptr_t getItemCurrentCondition;
    std::uintptr_t getItemMaximumCondition;
    std::uintptr_t canFavoriteForm;
    std::uintptr_t isFormTaggedForSearch;
    std::uintptr_t searchTagListGlobal;

    std::size_t inventoryListOffset;
    std::size_t inventoryLockOffset;
    std::size_t itemsBeginOffset;
    std::size_t itemSize;
    std::size_t itemStacksBeginOffset;
    std::size_t stackSize;
    std::size_t stackCountOffset;
    std::size_t itemFavoriteOffset;
    std::size_t itemHandleOffset;
    std::size_t formTypeOffset;
    std::size_t weaponFormType;
    std::size_t armorFormType;
    std::size_t weaponTypeOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativePipBoyInventoryProfile&) const noexcept = default;
};

// Exact-build data needed to read the collectibles the Pip-Boy lists.
struct NativePipBoyCollectionsProfile final {
    std::uintptr_t localPlayerGlobal;
    std::uintptr_t localPlayerPrimaryVtable;
    std::uintptr_t collectionManagerGlobal;
    std::uintptr_t getFormComponent;
    std::uintptr_t getCollectionOwnedCount;
    std::uintptr_t isCollectionGateOpen;
    std::uintptr_t collectionGateObject;
    std::uintptr_t getFixedStringText;
    std::uintptr_t deallocate;

    std::size_t collectionFormsOffset;
    std::size_t collectionFormCountOffset;
    std::size_t collectionFormType;
    std::size_t collectionNameOffset;
    std::size_t collectionCapacityOffset;
    std::size_t collectionCategoryOffset;
    std::size_t collectionGatedCategory;
    std::size_t collectionDescriptionSignature;
    std::size_t formIdOffset;
    std::size_t formTypeOffset;
    std::size_t getDescriptionVtableOffset;

    [[nodiscard]] constexpr bool operator==(
        const NativePipBoyCollectionsProfile&) const noexcept = default;
};

struct DataManagerProfile final {
    NativeActiveEffectProfile pipBoyStatsEffects;
    NativeEnemyMeterProfile enemyMeter;
    NativeTimeDataProfile timeData;
    NativePlayerMapPositionProfile playerMapPosition;
    NativeActorValueProfile actorValues;
    NativePipBoySpecialProfile pipBoySpecial;
    NativePipBoyPlayerInfoProfile pipBoyPlayerInfo;
    NativePipBoyPerksProfile pipBoyPerks;
    NativePipBoyStatusProfile pipBoyStatus;
    NativePipBoyQuestsProfile pipBoyQuests;
    NativePipBoyInventoryProfile pipBoyInventory;
    NativePipBoyCollectionsProfile pipBoyCollections;

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
    // Zero when the Address Library has no archive stream records.
    config::ArchiveStreamProfile archiveStream;
    bool pipBoyStatsEffectsEnabled;
    bool pipBoySpecialEnabled;
    bool pipBoyPlayerInfoEnabled;
    bool pipBoyPerksEnabled;
    bool pipBoyStatusEnabled;
    bool pipBoyQuestsEnabled;
    bool pipBoyInventoryEnabled;
    bool pipBoyCollectionsEnabled;
    // libScePad Bluetooth enable flag. Zero when the Address Library has no
    // record.
    std::uintptr_t scePadBluetoothFlagOffset;

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
