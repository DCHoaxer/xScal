#include "datamanager/pip_boy_perks_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

PipBoyPerksProvider::PipBoyPerksProvider(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery), cache_(cacheTick) {}

bool PipBoyPerksProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedPipBoyPerksRuntime resolvedRuntime;
    if (!ResolvePipBoyPerksRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.pipBoyPerks,
            resolvedRuntime)) {
        return false;
    }

    std::unique_lock lock{lifecycleMutex_};
    if (installed_) {
        return false;
    }

    runtime_ = resolvedRuntime;
    installed_ = true;
    return true;
}

bool PipBoyPerksProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    cache_.Clear();
    runtime_ = {};
    return true;
}

bool PipBoyPerksProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PipBoyPerksRequestStatus PipBoyPerksProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PipBoyPerksRequestStatus::Inactive;
    }
    if (cache_.TryCopyFresh(json)) {
        return PipBoyPerksRequestStatus::Delivered;
    }

    if (!BuildPipBoyPerksJson(runtime_, json)) {
        return PipBoyPerksRequestStatus::SnapshotUnavailable;
    }
    cache_.Store(json);
    return PipBoyPerksRequestStatus::Delivered;
}

}
