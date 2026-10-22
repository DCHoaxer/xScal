#include "datamanager/pip_boy_status_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace sf::datamanager {

PipBoyStatusProvider::PipBoyStatusProvider(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery), cache_(cacheTick) {}

bool PipBoyStatusProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedPipBoyStatusRuntime resolvedRuntime;
    if (!ResolvePipBoyStatusRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.actorValues,
            profile.dataManager.pipBoyStatus,
            resolvedRuntime)) {
        return false;
    }
    ResolvedPipBoyInventoryRuntime resolvedInventory;
    const bool inventoryResolved = ResolvePipBoyInventoryRuntime(
        virtualQuery_,
        moduleBase,
        profile.dataManager.pipBoyInventory,
        resolvedInventory);

    std::unique_lock lock{lifecycleMutex_};
    if (installed_) {
        return false;
    }

    runtime_ = resolvedRuntime;
    inventory_ = resolvedInventory;
    inventoryResolved_ = inventoryResolved;
    installed_ = true;
    return true;
}

bool PipBoyStatusProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    cache_.Clear();
    runtime_ = {};
    inventory_ = {};
    inventoryResolved_ = false;
    return true;
}

bool PipBoyStatusProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PipBoyStatusRequestStatus PipBoyStatusProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PipBoyStatusRequestStatus::Inactive;
    }
    if (cache_.TryCopyFresh(json)) {
        return PipBoyStatusRequestStatus::Delivered;
    }

    // A busy or unreadable inventory leaves weaponDamage null.
    std::vector<PipBoyDamageRow> weaponDamage;
    const bool hasWeaponDamage = inventoryResolved_ &&
        ReadEquippedWeaponDamage(inventory_, weaponDamage) ==
            PipBoyInventoryCaptureResult::Captured;
    if (!BuildPipBoyStatusJson(
            runtime_, json, hasWeaponDamage ? &weaponDamage : nullptr)) {
        return PipBoyStatusRequestStatus::SnapshotUnavailable;
    }
    cache_.Store(json);
    return PipBoyStatusRequestStatus::Delivered;
}

}
