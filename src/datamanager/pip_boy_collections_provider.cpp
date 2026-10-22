#include "datamanager/pip_boy_collections_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

PipBoyCollectionsProvider::PipBoyCollectionsProvider(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery), cache_(cacheTick) {}

bool PipBoyCollectionsProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedPipBoyCollectionsRuntime resolvedRuntime;
    if (!ResolvePipBoyCollectionsRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.pipBoyCollections,
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

bool PipBoyCollectionsProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    cache_.Clear();
    runtime_ = {};
    return true;
}

bool PipBoyCollectionsProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PipBoyCollectionsRequestStatus PipBoyCollectionsProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PipBoyCollectionsRequestStatus::Inactive;
    }
    if (cache_.TryCopyFresh(json)) {
        return PipBoyCollectionsRequestStatus::Delivered;
    }

    if (!BuildPipBoyCollectionsJson(runtime_, json)) {
        return PipBoyCollectionsRequestStatus::SnapshotUnavailable;
    }
    cache_.Store(json);
    return PipBoyCollectionsRequestStatus::Delivered;
}

}
