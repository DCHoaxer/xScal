#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sf::datamanager {

struct ResolvedPipBoyInventoryRuntime final {
    config::NativePipBoyInventoryProfile layout{};
    std::uintptr_t localPlayerGlobal{};
    std::uintptr_t localPlayerPrimaryVtable{};
    std::uintptr_t inventoryListVtable{};
    std::uintptr_t tryAcquireReadLock{};
    std::uintptr_t releaseReadLock{};
    std::uintptr_t getItemDisplayName{};
    std::uintptr_t releaseFixedString{};
    std::uintptr_t getItemUnitWeight{};
    std::uintptr_t getItemValue{};
    std::uintptr_t getItemEquipState{};
    std::uintptr_t getItemFilterFlags{};
    std::uintptr_t getItemLegendaryData{};
    std::uintptr_t getItemLegendaryStars{};
    std::uintptr_t isItemTransferLocked{};
    std::uintptr_t canItemBeTransferLocked{};
    std::uintptr_t isQuestItem{};
    std::uintptr_t canDropOrDestroyItem{};
    std::uintptr_t hasItemUniqueEffect{};
    std::uintptr_t buildWeaponDamageList{};
    std::uintptr_t releaseGameArray{};
    std::uintptr_t resolveObjectInstance{};
    std::uintptr_t getWeaponRateOfFire{};
    std::uintptr_t getWeaponRange{};
    std::uintptr_t getWeaponAccuracy{};
    std::uintptr_t getWeaponInstanceValue{};
    std::uintptr_t getWeaponApCostField{};
    std::uintptr_t buildArmorResistanceList{};
    std::uintptr_t getItemLevel{};
    std::uintptr_t getItemDurability{};
    std::uintptr_t getItemCurrentCondition{};
    std::uintptr_t getItemMaximumCondition{};
    std::uintptr_t canFavoriteForm{};
    std::uintptr_t isFormTaggedForSearch{};
    std::uintptr_t searchTagListGlobal{};
    config::NativeItemClassProfile itemClass{};
    bool hasItemClass{};
};

// One damage row the game builds for a weapon. type is the game's damage
// type id; a row from a timed effect also has its duration in seconds.
struct PipBoyDamageRow final {
    std::uint32_t type{};
    float value{};
    std::int32_t duration{};
};

struct PipBoyInventoryItem final {
    std::string name;
    // The server item handle the game uses to address the item.
    std::uint32_t handle{};
    std::int64_t count{};
    // Per-unit values, as the Pip-Boy item card shows them.
    float weight{};
    std::int32_t value{};
    // 0 not equipped, 1 equipped, 2 equipped armor with item flag bit 4.
    std::int32_t equipState{};
    // Pip-Boy filter bitmask chosen by form type.
    std::uint32_t filterFlags{};
    std::int32_t legendaryStars{};
    // Condition as the item card shows it: current clamped at 0, maximum 100
    // when the item reports none.
    std::int32_t currentCondition{};
    std::int32_t maximumCondition{};
    std::uint16_t itemLevel{};
    std::uint8_t durability{};
    bool favorite{};
    bool legendary{};
    bool transferLocked{};
    bool canBeTransferLocked{};
    bool questItem{};
    bool canFavorite{};
    bool taggedForSearch{};
    bool canDrop{};
    bool canDestroy{};
    bool hasUniqueEffect{};
    bool weapon{};
    bool armor{};
    // The item card shows no rate of fire, range or accuracy for some
    // weapon types.
    bool hasWeaponStats{};
    float rateOfFire{};
    float range{};
    float accuracy{};
    float apCost{};
    // Non-zero damage rows of a weapon, as the item card lists them.
    std::vector<PipBoyDamageRow> damage;
    // Non-zero resistance rows of an armor item.
    std::vector<PipBoyDamageRow> resistances;
    // Read only when the runtime has an item class profile.
    bool hasClass{};
    // Weapon type byte: 0xFF and 0 to 6 melee, 7 to 9 ranged, 10 grenade or
    // thrown, 11 mine.
    std::uint8_t weaponType{};
    // Armor biped slot mask of the base form; bit n is slot n+30.
    std::uint32_t armorSlots{};
    // Form IDs of the base form's keywords, its parent keyword forms
    // included. Keywords that modifications add are not listed.
    std::vector<std::uint32_t> keywords;
};

enum class PipBoyInventoryCaptureResult {
    Captured,
    // The game held the inventory lock for writing; nothing was read.
    LockBusy,
    Unavailable,
    // The item list hashed to the fingerprint the caller passed; nothing
    // else was read.
    Unchanged,
};

// An itemClass profile with a zero value leaves weaponType, armorSlots and
// keywords out of every item; the rest is read as before.
[[nodiscard]] bool ResolvePipBoyInventoryRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyInventoryProfile& inventory,
    ResolvedPipBoyInventoryRuntime& runtime,
    const config::NativeItemClassProfile& itemClass = {}) noexcept;

// fingerprint receives a hash of the item list as stored: each item's bytes
// and each stack's pointer and count. When unchangedFingerprint matches it,
// the items are not read and Unchanged is returned.
[[nodiscard]] PipBoyInventoryCaptureResult ReadPipBoyInventory(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::vector<PipBoyInventoryItem>& items,
    std::uint64_t* fingerprint = nullptr,
    const std::uint64_t* unchangedFingerprint = nullptr) noexcept;

// Reads the damage rows of every equipped weapon, the rows the STAT page
// shows.
[[nodiscard]] PipBoyInventoryCaptureResult ReadEquippedWeaponDamage(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::vector<PipBoyDamageRow>& damage) noexcept;

// Appends damage rows as a JSON array of {"type","value","duration"}.
[[nodiscard]] bool AppendDamageRowsJson(
    const std::vector<PipBoyDamageRow>& damage,
    std::size_t maximumBytes,
    std::string& json);

[[nodiscard]] PipBoyInventoryCaptureResult BuildPipBoyInventoryJson(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::string& json,
    std::uint64_t* fingerprint = nullptr,
    const std::uint64_t* unchangedFingerprint = nullptr) noexcept;

}
