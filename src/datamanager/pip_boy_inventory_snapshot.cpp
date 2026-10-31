#include "datamanager/pip_boy_inventory_snapshot.hpp"

#include "common/json.hpp"
#include "datamanager/game_read_lock.hpp"
#include "datamanager/local_player.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>

namespace sf::datamanager {
namespace {

// A BGSInventoryItem keeps its TESForm pointer at +0x00.
constexpr std::size_t kItemFormOffset = 0x0;
// Its instance data control block follows the form; together they are the
// {form, instance data} pair the item card getters take.
constexpr std::size_t kItemInstanceDataOffset = 0x8;
// The favorite slot byte holds 0xFE when the item is not favorited.
constexpr std::uint8_t kNotFavorite = 0xFE;
constexpr std::size_t kMaximumItems = 8192;
constexpr std::size_t kMaximumStacksPerItem = 4096;
constexpr std::int32_t kMaximumStackCount = 100000000;
constexpr std::size_t kMaximumNameLength = 256;
constexpr std::size_t kMaximumInventoryJsonBytes = 4 * 1024 * 1024;

// One BSFixedString: a pointer to a pooled entry. Zero is a valid empty
// string, and the release function accepts it.
struct FixedString final {
    void* entry{};
};
static_assert(sizeof(FixedString) == sizeof(void*));

using GetItemDisplayName = const char*(__fastcall*)(
    void* item,
    FixedString* name);
using ReleaseFixedString = void(__fastcall*)(FixedString* name);
using GetItemUnitWeight = float(__fastcall*)(void* item);
using GetItemValue = std::int32_t(__fastcall*)(
    void* item,
    bool multiplyByCount);

template <typename T>
[[nodiscard]] bool ReadField(
    std::uintptr_t object,
    std::size_t offset,
    T& value) noexcept {
    std::uintptr_t address{};
    return platform::TryAddAddress(object, offset, address) &&
        platform::TryCopyBytes(
            reinterpret_cast<const void*>(address), &value, sizeof(value));
}

[[nodiscard]] bool CallGetItemDisplayName(
    std::uintptr_t function,
    std::uintptr_t item,
    FixedString& name,
    const char*& text) noexcept {
    __try {
        text = reinterpret_cast<GetItemDisplayName>(function)(
            reinterpret_cast<void*>(item), &name);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        text = nullptr;
        return false;
    }
    return true;
}

// Releases the BSFixedString reference the name getter added.
class FixedStringOwner final {
public:
    FixedStringOwner(std::uintptr_t release, FixedString& name) noexcept
        : release_(release), name_(name) {}

    FixedStringOwner(const FixedStringOwner&) = delete;
    FixedStringOwner& operator=(const FixedStringOwner&) = delete;

    ~FixedStringOwner() {
        reinterpret_cast<ReleaseFixedString>(release_)(&name_);
    }

private:
    std::uintptr_t release_{};
    FixedString& name_;
};

[[nodiscard]] bool CallGetItemUnitWeight(
    std::uintptr_t function,
    std::uintptr_t item,
    float& weight) noexcept {
    __try {
        weight = reinterpret_cast<GetItemUnitWeight>(function)(
            reinterpret_cast<void*>(item));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        weight = 0.0F;
        return false;
    }
    return std::isfinite(weight);
}

[[nodiscard]] bool CallGetItemValue(
    std::uintptr_t function,
    std::uintptr_t item,
    std::int32_t& value) noexcept {
    __try {
        value = reinterpret_cast<GetItemValue>(function)(
            reinterpret_cast<void*>(item), false);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        value = 0;
        return false;
    }
    return true;
}

// Calls a one-argument item or form getter.
template <typename Result>
[[nodiscard]] bool CallGetter(
    std::uintptr_t function,
    std::uintptr_t argument,
    Result& result) noexcept {
    using Getter = Result(__fastcall*)(void* argument);
    __try {
        result = reinterpret_cast<Getter>(function)(
            reinterpret_cast<void*>(argument));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        result = {};
        return false;
    }
    return true;
}

// The game's drop and destroy check; the item entry asks with mode 1 for
// canDrop and mode 0 for canDestroy and passes the owning inventory list.
constexpr std::uint32_t kCanDropMode = 1;
constexpr std::uint32_t kCanDestroyMode = 0;

using CanDropOrDestroyItem =
    bool(__fastcall*)(void* item, void* inventoryList, std::uint32_t mode);

[[nodiscard]] bool CallCanDropOrDestroyItem(
    std::uintptr_t function,
    std::uintptr_t item,
    std::uintptr_t inventoryList,
    std::uint32_t mode,
    bool& allowed) noexcept {
    __try {
        allowed = reinterpret_cast<CanDropOrDestroyItem>(function)(
            reinterpret_cast<void*>(item),
            reinterpret_cast<void*>(inventoryList),
            mode);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        allowed = false;
        return false;
    }
    return true;
}

// The game's BSTArray: allocator, buffer, capacity and size. The damage list
// builder appends to a zeroed array; the caller frees the buffer.
struct GameArray final {
    std::uintptr_t allocator{};
    std::uintptr_t data{};
    std::uint32_t capacity{};
    std::uint32_t capacityPadding{};
    std::uint32_t size{};
    std::uint32_t sizePadding{};
};
static_assert(sizeof(GameArray) == 0x20);

// One row of the game's damage list: slot, damage type id, value and the
// duration of a timed effect.
struct GameDamageRow final {
    std::uint32_t slot;
    std::uint32_t type;
    float value;
    std::int32_t duration;
};
static_assert(sizeof(GameDamageRow) == 0x10);

// The builder writes 12 rows per weapon, the armor builder 6.
constexpr std::uint32_t kMaximumDamageRows = 64;
// The armor builder scales each resistance; the item card passes 1.0.
constexpr float kArmorResistanceScale = 1.0F;

using BuildDamageList = void(__fastcall*)(void* item, GameArray* rows);
using BuildResistanceList =
    void(__fastcall*)(void* item, GameArray* rows, float scale);
using ReleaseGameArray = void(__fastcall*)(GameArray* rows);

[[nodiscard]] bool CallBuildDamageList(
    std::uintptr_t function,
    std::uintptr_t item,
    bool armor,
    GameArray& rows) noexcept {
    __try {
        if (armor) {
            reinterpret_cast<BuildResistanceList>(function)(
                reinterpret_cast<void*>(item), &rows, kArmorResistanceScale);
        } else {
            reinterpret_cast<BuildDamageList>(function)(
                reinterpret_cast<void*>(item), &rows);
        }
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

void CallReleaseGameArray(
    std::uintptr_t function,
    GameArray& rows) noexcept {
    __try {
        reinterpret_cast<ReleaseGameArray>(function)(&rows);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
    }
}

// Appends the non-zero rows the game builds for a weapon (damage) or an
// armor item (resistances).
[[nodiscard]] bool ReadDamageRows(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t item,
    bool armor,
    std::vector<PipBoyDamageRow>& damage) {
    GameArray array{};
    std::array<GameDamageRow, kMaximumDamageRows> rows{};
    bool read = CallBuildDamageList(
                    armor ? runtime.buildArmorResistanceList
                          : runtime.buildWeaponDamageList,
                    item,
                    armor,
                    array) &&
        array.size <= rows.size() &&
        (array.size == 0 ||
         platform::TryCopyBytes(
             reinterpret_cast<const void*>(array.data),
             rows.data(),
             array.size * sizeof(GameDamageRow)));
    if (array.data != 0) {
        CallReleaseGameArray(runtime.releaseGameArray, array);
    }
    if (!read) {
        return false;
    }
    for (std::uint32_t index = 0; index < array.size; ++index) {
        const auto& row = rows[index];
        if (std::isfinite(row.value) && row.value != 0.0F) {
            damage.push_back({row.type, row.value, row.duration});
        }
    }
    return true;
}

[[nodiscard]] bool IsWeaponForm(
    const config::NativePipBoyInventoryProfile& layout,
    std::uintptr_t form,
    bool& weapon) noexcept {
    std::uint8_t formType{};
    weapon = false;
    if (!ReadField(form, layout.formTypeOffset, formType)) {
        return false;
    }
    weapon = formType == layout.weaponFormType;
    return true;
}

[[nodiscard]] bool ReadItemKind(
    const config::NativePipBoyInventoryProfile& layout,
    std::uintptr_t form,
    bool& weapon,
    bool& armor) noexcept {
    std::uint8_t formType{};
    weapon = false;
    armor = false;
    if (!ReadField(form, layout.formTypeOffset, formType)) {
        return false;
    }
    weapon = formType == layout.weaponFormType;
    armor = formType == layout.armorFormType;
    return true;
}

// The {form, instance data} pair the item card getters take. The control
// block is reference counted: a dword at +0x8, destroyed by vtable slot 0.
struct ObjectInstance final {
    std::uintptr_t form{};
    std::uintptr_t control{};
};

using ResolveObjectInstance = ObjectInstance*(__fastcall*)(
    ObjectInstance* instance,
    void* form,
    const std::uintptr_t* control);
using DestroyInstanceControl =
    void(__fastcall*)(void* control, std::uint32_t free);
using GetInstanceFloat = float(__fastcall*)(const ObjectInstance* instance);
using GetActorInstanceFloat =
    float(__fastcall*)(void* actor, const ObjectInstance* instance);
using GetInstanceActorFloat =
    float(__fastcall*)(const ObjectInstance* instance, void* actor);

// A field getter and index for the weapon instance value reader.
struct InstanceValueField final {
    std::uintptr_t getter;
    std::uint32_t index;
    std::uint32_t padding;
};
using GetInstanceValue = float(__fastcall*)(
    const ObjectInstance* instance,
    const InstanceValueField* field);

[[nodiscard]] bool CallResolveObjectInstance(
    std::uintptr_t function,
    std::uintptr_t item,
    std::uintptr_t form,
    ObjectInstance& instance) noexcept {
    __try {
        reinterpret_cast<ResolveObjectInstance>(function)(
            &instance,
            reinterpret_cast<void*>(form),
            reinterpret_cast<const std::uintptr_t*>(
                item + kItemInstanceDataOffset));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

// Drops the reference the resolver added, as the item card does after it.
void ReleaseObjectInstance(ObjectInstance& instance) noexcept {
    const auto control = instance.control;
    instance = {};
    if (control == 0) {
        return;
    }
    __try {
        auto* const count = reinterpret_cast<volatile LONG*>(control + 0x8);
        if (::InterlockedDecrement(count) == 0) {
            const auto vtable = *reinterpret_cast<const std::uintptr_t*>(control);
            reinterpret_cast<DestroyInstanceControl>(
                *reinterpret_cast<const std::uintptr_t*>(vtable))(
                reinterpret_cast<void*>(control), 1);
        }
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
    }
}

[[nodiscard]] bool CallWeaponStats(
    const ResolvedPipBoyInventoryRuntime& runtime,
    const ObjectInstance& instance,
    std::uintptr_t player,
    bool withRangedStats,
    PipBoyInventoryItem& entry) noexcept {
    const InstanceValueField apCost{runtime.getWeaponApCostField, 0, 0};
    auto* const actor = reinterpret_cast<void*>(player);
    __try {
        if (withRangedStats) {
            entry.rateOfFire = reinterpret_cast<GetInstanceFloat>(
                runtime.getWeaponRateOfFire)(&instance);
            entry.range = reinterpret_cast<GetActorInstanceFloat>(
                runtime.getWeaponRange)(actor, &instance);
            entry.accuracy = reinterpret_cast<GetInstanceActorFloat>(
                runtime.getWeaponAccuracy)(&instance, actor);
        }
        entry.apCost = reinterpret_cast<GetInstanceValue>(
            runtime.getWeaponInstanceValue)(&instance, &apCost);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

// Reads the item card weapon stats in the order the game's inventory entry
// builder reads them.
[[nodiscard]] bool ReadWeaponStats(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t player,
    std::uintptr_t item,
    std::uintptr_t form,
    PipBoyInventoryItem& entry) noexcept {
    std::uint8_t weaponType{};
    if (!ReadField(form, runtime.layout.weaponTypeOffset, weaponType)) {
        return false;
    }
    entry.weaponType = weaponType;
    entry.hasWeaponStats = weaponType != 10 && weaponType != 11;
    ObjectInstance instance;
    if (!CallResolveObjectInstance(
            runtime.resolveObjectInstance, item, form, instance)) {
        return false;
    }
    const bool read = CallWeaponStats(
        runtime, instance, player, entry.hasWeaponStats, entry);
    ReleaseObjectInstance(instance);
    return read &&
        std::isfinite(entry.rateOfFire) && std::isfinite(entry.range) &&
        std::isfinite(entry.accuracy) && std::isfinite(entry.apCost);
}

// Stop a parent chain or keyword array that points into unrelated memory.
constexpr int kMaximumKeywordParentDepth = 4;
constexpr std::size_t kMaximumKeywords = 512;

// Appends the keyword form IDs of a BGSKeywordForm and its parents: each
// component's read lock, its BGSKeyword* range, then the parent component.
// A lock that a writer holds ends the walk with the keywords read so far,
// because waiting could stall the frame.
[[nodiscard]] bool ReadKeywordIds(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t component,
    std::vector<std::uint32_t>& keywords) {
    const auto& layout = runtime.itemClass;
    for (int depth = 0; component != 0 && depth <= kMaximumKeywordParentDepth; ++depth) {
        std::uintptr_t lock{};
        bool acquired{};
        if (!platform::TryAddAddress(component, layout.keywordLockOffset, lock) ||
            !TryAcquireGameReadLock(runtime.tryAcquireReadLock, lock, acquired)) {
            return false;
        }
        if (!acquired) {
            return true;
        }
        const GameReadLockGuard readLock{runtime.releaseReadLock, lock};
        std::uintptr_t begin{};
        std::uintptr_t end{};
        std::uintptr_t parent{};
        if (!ReadField(component, layout.keywordsBeginOffset, begin) ||
            !ReadField(component, layout.keywordsEndOffset, end) ||
            !ReadField(component, layout.keywordParentOffset, parent) ||
            end < begin || (end - begin) % sizeof(std::uintptr_t) != 0 ||
            (end - begin) / sizeof(std::uintptr_t) > kMaximumKeywords) {
            return false;
        }
        for (auto slot = begin; slot != end && keywords.size() < kMaximumKeywords;
             slot += sizeof(std::uintptr_t)) {
            std::uintptr_t keyword{};
            std::uint32_t formId{};
            if (!ReadField(slot, 0, keyword)) {
                return false;
            }
            if (keyword != 0 && ReadField(keyword, layout.formIdOffset, formId)) {
                keywords.push_back(formId);
            }
        }
        component = parent;
    }
    return true;
}

// Weapon and armor items get their base form's keywords; armor also gets its
// biped slot mask. Both come from the base form, so modifications do not
// change them.
[[nodiscard]] bool ReadItemClass(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t form,
    PipBoyInventoryItem& entry) {
    if (!runtime.hasItemClass || (!entry.weapon && !entry.armor)) {
        return true;
    }
    const auto& layout = runtime.itemClass;
    if (entry.armor && !ReadField(form, layout.armorBipedSlotsOffset, entry.armorSlots)) {
        return false;
    }
    std::uintptr_t component{};
    if (!platform::TryAddAddress(
            form,
            entry.weapon ? layout.weaponKeywordFormOffset : layout.armorKeywordFormOffset,
            component) ||
        !ReadKeywordIds(runtime, component, entry.keywords)) {
        return false;
    }
    entry.hasClass = true;
    return true;
}

using IsFormTaggedForSearch = bool(__fastcall*)(void* tagList, void* form);

[[nodiscard]] bool CallIsFormTaggedForSearch(
    std::uintptr_t function,
    std::uintptr_t tagList,
    std::uintptr_t form,
    bool& tagged) noexcept {
    __try {
        tagged = reinterpret_cast<IsFormTaggedForSearch>(function)(
            reinterpret_cast<void*>(tagList), reinterpret_cast<void*>(form));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        tagged = false;
        return false;
    }
    return true;
}

// The item card writes the condition floats with cvttss2si.
[[nodiscard]] bool TruncateCondition(
    float value,
    std::int32_t& converted) noexcept {
    converted = 0;
    if (!std::isfinite(value) || value <= -2147483648.0F ||
        value >= 2147483648.0F) {
        return false;
    }
    converted = static_cast<std::int32_t>(value);
    return true;
}

[[nodiscard]] bool ReadItemDetails(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t tagList,
    std::uintptr_t inventoryList,
    std::uintptr_t item,
    std::uintptr_t form,
    PipBoyInventoryItem& entry) noexcept {
    std::uintptr_t legendaryData{};
    float currentCondition{};
    float maximumCondition{};
    if (!ReadField(item, runtime.layout.itemHandleOffset, entry.handle) ||
        !CallGetter(runtime.getItemEquipState, item, entry.equipState) ||
        !CallGetter(runtime.getItemFilterFlags, item, entry.filterFlags) ||
        !CallGetter(runtime.getItemLegendaryData, item, legendaryData) ||
        !CallGetter(runtime.isItemTransferLocked, item, entry.transferLocked) ||
        !CallGetter(
            runtime.canItemBeTransferLocked, item, entry.canBeTransferLocked) ||
        !CallGetter(runtime.isQuestItem, item, entry.questItem) ||
        !CallCanDropOrDestroyItem(
            runtime.canDropOrDestroyItem,
            item,
            inventoryList,
            kCanDropMode,
            entry.canDrop) ||
        !CallCanDropOrDestroyItem(
            runtime.canDropOrDestroyItem,
            item,
            inventoryList,
            kCanDestroyMode,
            entry.canDestroy) ||
        !CallGetter(runtime.hasItemUniqueEffect, item, entry.hasUniqueEffect) ||
        !CallGetter(runtime.getItemLevel, item, entry.itemLevel) ||
        !CallGetter(runtime.getItemDurability, item, entry.durability) ||
        !CallGetter(runtime.getItemCurrentCondition, item, currentCondition) ||
        !CallGetter(runtime.getItemMaximumCondition, item, maximumCondition) ||
        !TruncateCondition(currentCondition, entry.currentCondition) ||
        !TruncateCondition(maximumCondition, entry.maximumCondition) ||
        !CallGetter(runtime.canFavoriteForm, form, entry.canFavorite)) {
        return false;
    }
    entry.legendary = legendaryData != 0;
    // The item card asks for stars only for a legendary item.
    if (entry.legendary &&
        !CallGetter(
            runtime.getItemLegendaryStars, item, entry.legendaryStars)) {
        return false;
    }
    if (entry.currentCondition < 0) {
        entry.currentCondition = 0;
    }
    if (entry.maximumCondition == 0) {
        entry.maximumCondition = 100;
    }
    // The tag getter does not check its list pointer, which is null until
    // the game creates the list.
    return tagList == 0 ||
        CallIsFormTaggedForSearch(
            runtime.isFormTaggedForSearch, tagList, form, entry.taggedForSearch);
}

[[nodiscard]] bool ReadItemCount(
    const config::NativePipBoyInventoryProfile& layout,
    std::uintptr_t item,
    std::int64_t& count) noexcept {
    count = 0;
    std::uintptr_t begin{};
    std::uintptr_t end{};
    if (!ReadField(item, layout.itemStacksBeginOffset, begin) ||
        !ReadField(
            item, layout.itemStacksBeginOffset + sizeof(std::uintptr_t), end) ||
        end < begin || (end - begin) % layout.stackSize != 0 ||
        (end - begin) / layout.stackSize > kMaximumStacksPerItem) {
        return false;
    }
    for (auto stack = begin; stack != end; stack += layout.stackSize) {
        std::int32_t stackCount{};
        if (!ReadField(stack, layout.stackCountOffset, stackCount) ||
            stackCount < 0 || stackCount > kMaximumStackCount) {
            return false;
        }
        count += stackCount;
    }
    return true;
}

[[nodiscard]] bool ReadItemName(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t item,
    std::string& name) noexcept {
    FixedString fixedName;
    const FixedStringOwner owner{runtime.releaseFixedString, fixedName};
    const char* text{};
    return CallGetItemDisplayName(
               runtime.getItemDisplayName, item, fixedName, text) &&
        (text == nullptr ||
         platform::CopyBoundedCString(text, kMaximumNameLength, true, name));
}

// Reads the player's inventory list and its item range. A player without a
// list yet has an empty range.
[[nodiscard]] bool ReadItemRange(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t player,
    std::uintptr_t& list,
    std::uintptr_t& begin,
    std::uintptr_t& end) noexcept {
    const auto& layout = runtime.layout;
    list = 0;
    begin = 0;
    end = 0;
    if (!ReadField(player, layout.inventoryListOffset, list)) {
        return false;
    }
    // The list is created with the first item.
    if (list == 0) {
        return true;
    }
    std::uintptr_t listVtable{};
    return list % alignof(std::uintptr_t) == 0 &&
        ReadField(list, 0, listVtable) &&
        listVtable == runtime.inventoryListVtable &&
        ReadField(list, layout.itemsBeginOffset, begin) &&
        ReadField(
            list, layout.itemsBeginOffset + sizeof(std::uintptr_t), end) &&
        end >= begin && (end - begin) % layout.itemSize == 0 &&
        (end - begin) / layout.itemSize <= kMaximumItems;
}

// FNV-1a over the item list as stored: the item range, each item's bytes
// (form, instance data, stack range, flags, handle, favorite slot) and each
// stack's pointer and count. Values behind those pointers, such as
// condition or perk-adjusted stats, are not part of it.
[[nodiscard]] bool HashItemList(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t player,
    std::uint64_t& hash) noexcept {
    constexpr std::uint64_t kOffsetBasis = 0xCBF29CE484222325ULL;
    constexpr std::uint64_t kPrime = 0x100000001B3ULL;
    constexpr std::size_t kMaximumHashedBytes = 0x40;
    const auto& layout = runtime.layout;
    hash = kOffsetBasis;
    const auto mix = [&hash](const std::byte* bytes, std::size_t size) {
        for (std::size_t index = 0; index < size; ++index) {
            hash = (hash ^ static_cast<std::uint8_t>(bytes[index])) * kPrime;
        }
    };
    std::uintptr_t list{};
    std::uintptr_t begin{};
    std::uintptr_t end{};
    if (!ReadItemRange(runtime, player, list, begin, end)) {
        return false;
    }
    const std::uintptr_t range[]{list, begin, end};
    mix(reinterpret_cast<const std::byte*>(range), sizeof(range));

    const auto itemBytes = (std::min)(layout.itemSize, kMaximumHashedBytes);
    const auto stackBytes = (std::min)(layout.stackSize, kMaximumHashedBytes);
    std::array<std::byte, kMaximumHashedBytes> buffer{};
    for (auto item = begin; item != end; item += layout.itemSize) {
        std::uintptr_t stacksBegin{};
        std::uintptr_t stacksEnd{};
        if (!platform::TryCopyBytes(
                reinterpret_cast<const void*>(item), buffer.data(), itemBytes) ||
            !ReadField(item, layout.itemStacksBeginOffset, stacksBegin) ||
            !ReadField(
                item,
                layout.itemStacksBeginOffset + sizeof(std::uintptr_t),
                stacksEnd) ||
            stacksEnd < stacksBegin ||
            (stacksEnd - stacksBegin) / layout.stackSize >
                kMaximumStacksPerItem) {
            return false;
        }
        mix(buffer.data(), itemBytes);
        for (auto stack = stacksBegin; stack + layout.stackSize <= stacksEnd;
             stack += layout.stackSize) {
            if (!platform::TryCopyBytes(
                    reinterpret_cast<const void*>(stack),
                    buffer.data(),
                    stackBytes)) {
                return false;
            }
            mix(buffer.data(), stackBytes);
        }
    }
    return true;
}

// Reads the items while the caller holds the inventory read lock.
[[nodiscard]] bool ReadLockedItems(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::uintptr_t player,
    std::vector<PipBoyInventoryItem>& items) noexcept {
    const auto& layout = runtime.layout;
    std::uintptr_t list{};
    std::uintptr_t begin{};
    std::uintptr_t end{};
    if (!ReadItemRange(runtime, player, list, begin, end)) {
        return false;
    }
    if (list == 0) {
        return true;
    }

    std::uintptr_t tagList{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.searchTagListGlobal),
            &tagList,
            sizeof(tagList))) {
        return false;
    }

    try {
        items.reserve((end - begin) / layout.itemSize);
    } catch (...) {
        return false;
    }
    for (auto item = begin; item != end; item += layout.itemSize) {
        std::uintptr_t form{};
        PipBoyInventoryItem entry;
        std::uint8_t favoriteSlot{};
        if (!ReadField(item, kItemFormOffset, form) ||
            !ReadItemCount(layout, item, entry.count)) {
            return false;
        }
        if (form == 0 || entry.count == 0) {
            continue;
        }
        if (!ReadField(item, layout.itemFavoriteOffset, favoriteSlot) ||
            !ReadItemName(runtime, item, entry.name) ||
            !CallGetItemUnitWeight(runtime.getItemUnitWeight, item, entry.weight) ||
            !CallGetItemValue(runtime.getItemValue, item, entry.value) ||
            !ReadItemDetails(runtime, tagList, list, item, form, entry) ||
            !ReadItemKind(layout, form, entry.weapon, entry.armor) ||
            (entry.weapon &&
             !ReadWeaponStats(runtime, player, item, form, entry))) {
            return false;
        }
        if (entry.weapon || entry.armor) {
            try {
                if (!ReadDamageRows(
                        runtime,
                        item,
                        entry.armor,
                        entry.armor ? entry.resistances : entry.damage)) {
                    return false;
                }
            } catch (...) {
                return false;
            }
        }
        // The class fields are optional: an item whose keywords cannot be
        // read is listed without them.
        bool classRead{};
        try {
            classRead = ReadItemClass(runtime, form, entry);
        } catch (...) {
            classRead = false;
        }
        if (!classRead) {
            entry.hasClass = false;
            entry.armorSlots = 0;
            entry.keywords.clear();
        }
        entry.favorite = favoriteSlot != kNotFavorite;
        items.push_back(std::move(entry));
    }
    return true;
}

[[nodiscard]] bool AppendInventoryItem(
    const PipBoyInventoryItem& item,
    std::string& json) {
    const auto appendInteger =
        [&json](std::string_view prefix, std::int64_t value) {
            json += prefix;
            return json::AppendInteger(value, json, kMaximumInventoryJsonBytes);
        };
    const auto appendBool = [&json](std::string_view prefix, bool value) {
        json += prefix;
        json += value ? "true" : "false";
    };
    json += R"({"name":)";
    if (!json::AppendString(item.name, json, kMaximumInventoryJsonBytes) ||
        !appendInteger(R"(,"handle":)", item.handle) ||
        !appendInteger(R"(,"count":)", item.count)) {
        return false;
    }
    json += R"(,"weight":)";
    if (!json::AppendFloat(item.weight, json) ||
        !appendInteger(R"(,"value":)", item.value) ||
        !appendInteger(R"(,"equipState":)", item.equipState) ||
        !appendInteger(R"(,"filterFlag":)", item.filterFlags) ||
        !appendInteger(R"(,"legendaryStars":)", item.legendaryStars) ||
        !appendInteger(R"(,"itemLevel":)", item.itemLevel) ||
        !appendInteger(R"(,"durability":)", item.durability) ||
        !appendInteger(R"(,"currentHealth":)", item.currentCondition) ||
        !appendInteger(R"(,"maximumHealth":)", item.maximumCondition)) {
        return false;
    }
    appendBool(R"(,"favorite":)", item.favorite);
    appendBool(R"(,"legendary":)", item.legendary);
    appendBool(R"(,"transferLocked":)", item.transferLocked);
    appendBool(R"(,"canBeTransferLocked":)", item.canBeTransferLocked);
    appendBool(R"(,"questItem":)", item.questItem);
    appendBool(R"(,"canFavorite":)", item.canFavorite);
    appendBool(R"(,"taggedForSearch":)", item.taggedForSearch);
    appendBool(R"(,"canDrop":)", item.canDrop);
    appendBool(R"(,"canDestroy":)", item.canDestroy);
    appendBool(R"(,"hasUniqueEffect":)", item.hasUniqueEffect);
    const auto appendStat = [&json, &item](std::string_view prefix, float value) {
        json += prefix;
        if (!item.hasWeaponStats) {
            json += "null";
            return true;
        }
        return json::AppendFloat(value, json);
    };
    if (item.weapon) {
        json += R"(,"damage":)";
        if (!AppendDamageRowsJson(
                item.damage, kMaximumInventoryJsonBytes, json) ||
            !appendStat(R"(,"rateOfFire":)", item.rateOfFire) ||
            !appendStat(R"(,"range":)", item.range) ||
            !appendStat(R"(,"accuracy":)", item.accuracy)) {
            return false;
        }
        json += R"(,"apCost":)";
        if (!json::AppendFloat(item.apCost, json) ||
            !appendInteger(R"(,"weaponType":)", item.weaponType)) {
            return false;
        }
    }
    if (item.armor) {
        json += R"(,"resist":)";
        if (!AppendDamageRowsJson(
                item.resistances, kMaximumInventoryJsonBytes, json)) {
            return false;
        }
        if (item.hasClass &&
            !appendInteger(R"(,"armorSlots":)", item.armorSlots)) {
            return false;
        }
    }
    if (item.hasClass) {
        json += R"(,"keywords":[)";
        for (std::size_t index = 0; index < item.keywords.size(); ++index) {
            if (index != 0) {
                json.push_back(',');
            }
            if (!json::AppendInteger(
                    item.keywords[index], json, kMaximumInventoryJsonBytes)) {
                return false;
            }
        }
        json.push_back(']');
    }
    json.push_back('}');
    return true;
}

}

constexpr std::array kPipBoyInventoryRuntimeBindings{
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, localPlayerGlobal),
        offsetof(ResolvedPipBoyInventoryRuntime, localPlayerGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, localPlayerPrimaryVtable),
        offsetof(ResolvedPipBoyInventoryRuntime, localPlayerPrimaryVtable),
        platform::AddressCheck::None),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, inventoryListVtable),
        offsetof(ResolvedPipBoyInventoryRuntime, inventoryListVtable),
        platform::AddressCheck::None),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, tryAcquireReadLock),
        offsetof(ResolvedPipBoyInventoryRuntime, tryAcquireReadLock),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, releaseReadLock),
        offsetof(ResolvedPipBoyInventoryRuntime, releaseReadLock),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemDisplayName),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemDisplayName),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, releaseFixedString),
        offsetof(ResolvedPipBoyInventoryRuntime, releaseFixedString),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemUnitWeight),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemUnitWeight),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemValue),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemValue),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemEquipState),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemEquipState),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemFilterFlags),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemFilterFlags),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemLegendaryData),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemLegendaryData),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemLegendaryStars),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemLegendaryStars),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, isItemTransferLocked),
        offsetof(ResolvedPipBoyInventoryRuntime, isItemTransferLocked),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, canItemBeTransferLocked),
        offsetof(ResolvedPipBoyInventoryRuntime, canItemBeTransferLocked),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, isQuestItem),
        offsetof(ResolvedPipBoyInventoryRuntime, isQuestItem),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, canDropOrDestroyItem),
        offsetof(ResolvedPipBoyInventoryRuntime, canDropOrDestroyItem),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, hasItemUniqueEffect),
        offsetof(ResolvedPipBoyInventoryRuntime, hasItemUniqueEffect),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, buildWeaponDamageList),
        offsetof(ResolvedPipBoyInventoryRuntime, buildWeaponDamageList),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, releaseGameArray),
        offsetof(ResolvedPipBoyInventoryRuntime, releaseGameArray),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, resolveObjectInstance),
        offsetof(ResolvedPipBoyInventoryRuntime, resolveObjectInstance),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getWeaponRateOfFire),
        offsetof(ResolvedPipBoyInventoryRuntime, getWeaponRateOfFire),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getWeaponRange),
        offsetof(ResolvedPipBoyInventoryRuntime, getWeaponRange),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getWeaponAccuracy),
        offsetof(ResolvedPipBoyInventoryRuntime, getWeaponAccuracy),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getWeaponInstanceValue),
        offsetof(ResolvedPipBoyInventoryRuntime, getWeaponInstanceValue),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getWeaponApCostField),
        offsetof(ResolvedPipBoyInventoryRuntime, getWeaponApCostField),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, buildArmorResistanceList),
        offsetof(ResolvedPipBoyInventoryRuntime, buildArmorResistanceList),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemLevel),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemLevel),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemDurability),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemDurability),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemCurrentCondition),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemCurrentCondition),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, getItemMaximumCondition),
        offsetof(ResolvedPipBoyInventoryRuntime, getItemMaximumCondition),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, canFavoriteForm),
        offsetof(ResolvedPipBoyInventoryRuntime, canFavoriteForm),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, isFormTaggedForSearch),
        offsetof(ResolvedPipBoyInventoryRuntime, isFormTaggedForSearch),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyInventoryProfile, searchTagListGlobal),
        offsetof(ResolvedPipBoyInventoryRuntime, searchTagListGlobal),
        platform::AddressCheck::Readable),
};

bool ResolvePipBoyInventoryRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyInventoryProfile& inventory,
    ResolvedPipBoyInventoryRuntime& runtime,
    const config::NativeItemClassProfile& itemClass) noexcept {
    runtime = {};
    if (virtualQuery == nullptr || moduleBase == 0 ||
        inventory.inventoryListOffset == 0 ||
        inventory.inventoryLockOffset == 0 ||
        inventory.itemsBeginOffset == 0 ||
        inventory.itemSize == 0 ||
        inventory.itemStacksBeginOffset == 0 ||
        inventory.stackSize == 0 ||
        inventory.stackCountOffset == 0 ||
        inventory.itemFavoriteOffset == 0 ||
        inventory.itemHandleOffset == 0 ||
        inventory.formTypeOffset == 0 ||
        inventory.weaponFormType == 0 ||
        inventory.armorFormType == 0 ||
        inventory.weaponTypeOffset == 0 ||
        !platform::ValidateVtables(
            virtualQuery,
            moduleBase,
            {inventory.localPlayerPrimaryVtable, inventory.inventoryListVtable})) {
        return false;
    }

    ResolvedPipBoyInventoryRuntime candidate{
        .layout = inventory,
    };
    if (!platform::ResolveAddressBindings(
            virtualQuery,
            moduleBase,
            &inventory,
            &candidate,
            kPipBoyInventoryRuntimeBindings)) {
        return false;
    }
    candidate.hasItemClass = itemClass.armorBipedSlotsOffset != 0 &&
        itemClass.weaponKeywordFormOffset != 0 &&
        itemClass.armorKeywordFormOffset != 0 &&
        itemClass.keywordsBeginOffset != 0 &&
        itemClass.keywordsEndOffset != 0 &&
        itemClass.keywordLockOffset != 0 &&
        itemClass.keywordParentOffset != 0 &&
        itemClass.formIdOffset != 0;
    if (candidate.hasItemClass) {
        candidate.itemClass = itemClass;
    }

    runtime = candidate;
    return true;
}

PipBoyInventoryCaptureResult ReadPipBoyInventory(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::vector<PipBoyInventoryItem>& items,
    std::uint64_t* fingerprint,
    const std::uint64_t* unchangedFingerprint) noexcept {
    items.clear();
    std::uintptr_t player{};
    std::uintptr_t lock{};
    bool acquired{};
    if (!ReadLocalPlayer(
            runtime.localPlayerGlobal,
            runtime.localPlayerPrimaryVtable,
            player) ||
        !platform::TryAddAddress(
            player, runtime.layout.inventoryLockOffset, lock) ||
        !TryAcquireGameReadLock(runtime.tryAcquireReadLock, lock, acquired)) {
        return PipBoyInventoryCaptureResult::Unavailable;
    }
    // Waiting for a writer on the game thread could stall the frame, so a
    // busy lock is reported instead.
    if (!acquired) {
        return PipBoyInventoryCaptureResult::LockBusy;
    }

    std::vector<PipBoyInventoryItem> result;
    {
        const GameReadLockGuard readLock{runtime.releaseReadLock, lock};
        if (fingerprint != nullptr) {
            if (!HashItemList(runtime, player, *fingerprint)) {
                return PipBoyInventoryCaptureResult::Unavailable;
            }
            if (unchangedFingerprint != nullptr &&
                *unchangedFingerprint == *fingerprint) {
                return PipBoyInventoryCaptureResult::Unchanged;
            }
        }
        bool read{};
        try {
            read = ReadLockedItems(runtime, player, result);
        } catch (...) {
            read = false;
        }
        if (!read) {
            return PipBoyInventoryCaptureResult::Unavailable;
        }
    }

    items = std::move(result);
    return PipBoyInventoryCaptureResult::Captured;
}

PipBoyInventoryCaptureResult ReadEquippedWeaponDamage(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::vector<PipBoyDamageRow>& damage) noexcept {
    damage.clear();
    std::uintptr_t player{};
    std::uintptr_t lock{};
    bool acquired{};
    if (!ReadLocalPlayer(
            runtime.localPlayerGlobal,
            runtime.localPlayerPrimaryVtable,
            player) ||
        !platform::TryAddAddress(
            player, runtime.layout.inventoryLockOffset, lock) ||
        !TryAcquireGameReadLock(runtime.tryAcquireReadLock, lock, acquired)) {
        return PipBoyInventoryCaptureResult::Unavailable;
    }
    if (!acquired) {
        return PipBoyInventoryCaptureResult::LockBusy;
    }

    const auto& layout = runtime.layout;
    const GameReadLockGuard readLock{runtime.releaseReadLock, lock};
    std::uintptr_t list{};
    std::uintptr_t begin{};
    std::uintptr_t end{};
    if (!ReadItemRange(runtime, player, list, begin, end)) {
        return PipBoyInventoryCaptureResult::Unavailable;
    }

    // The STAT page lists the rows of each weapon whose equip state is set.
    std::vector<PipBoyDamageRow> rows;
    try {
        for (auto item = begin; item != end; item += layout.itemSize) {
            std::uintptr_t form{};
            bool weapon{};
            std::int32_t equipState{};
            if (!ReadField(item, kItemFormOffset, form)) {
                return PipBoyInventoryCaptureResult::Unavailable;
            }
            if (form == 0) {
                continue;
            }
            if (!IsWeaponForm(layout, form, weapon) ||
                (weapon &&
                 !CallGetter(runtime.getItemEquipState, item, equipState))) {
                return PipBoyInventoryCaptureResult::Unavailable;
            }
            if (weapon && equipState != 0 &&
                !ReadDamageRows(runtime, item, false, rows)) {
                return PipBoyInventoryCaptureResult::Unavailable;
            }
        }
    } catch (...) {
        return PipBoyInventoryCaptureResult::Unavailable;
    }
    damage = std::move(rows);
    return PipBoyInventoryCaptureResult::Captured;
}

bool AppendDamageRowsJson(
    const std::vector<PipBoyDamageRow>& damage,
    std::size_t maximumBytes,
    std::string& json) {
    json.push_back('[');
    for (std::size_t index = 0; index < damage.size(); ++index) {
        const auto& row = damage[index];
        json += index == 0 ? R"({"type":)" : R"(,{"type":)";
        if (!json::AppendInteger(row.type, json, maximumBytes)) {
            return false;
        }
        json += R"(,"value":)";
        if (!json::AppendFloat(row.value, json)) {
            return false;
        }
        json += R"(,"duration":)";
        if (!json::AppendInteger(row.duration, json, maximumBytes)) {
            return false;
        }
        json.push_back('}');
    }
    json.push_back(']');
    return true;
}

PipBoyInventoryCaptureResult BuildPipBoyInventoryJson(
    const ResolvedPipBoyInventoryRuntime& runtime,
    std::string& json,
    std::uint64_t* fingerprint,
    const std::uint64_t* unchangedFingerprint) noexcept {
    json.clear();
    std::vector<PipBoyInventoryItem> items;
    const auto capture =
        ReadPipBoyInventory(runtime, items, fingerprint, unchangedFingerprint);
    if (capture != PipBoyInventoryCaptureResult::Captured) {
        return capture;
    }

    try {
        json.reserve(16 + items.size() * 384);
        json += R"({"items":[)";
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (index != 0) {
                json.push_back(',');
            }
            if (!AppendInventoryItem(items[index], json)) {
                json.clear();
                return PipBoyInventoryCaptureResult::Unavailable;
            }
        }
        json += "]}";
    } catch (...) {
        json.clear();
        return PipBoyInventoryCaptureResult::Unavailable;
    }
    if (json.size() > kMaximumInventoryJsonBytes) {
        json.clear();
        return PipBoyInventoryCaptureResult::Unavailable;
    }
    return PipBoyInventoryCaptureResult::Captured;
}

}
