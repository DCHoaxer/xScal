#include "datamanager/pip_boy_special_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

PipBoySpecialProvider::PipBoySpecialProvider(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery), cache_(cacheTick) {}

bool PipBoySpecialProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedPipBoySpecialRuntime resolvedRuntime;
    if (!ResolvePipBoySpecialRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.actorValues,
            profile.dataManager.pipBoySpecial,
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

bool PipBoySpecialProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    cache_.Clear();
    runtime_ = {};
    return true;
}

bool PipBoySpecialProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PipBoySpecialRequestStatus PipBoySpecialProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PipBoySpecialRequestStatus::Inactive;
    }
    if (cache_.TryCopyFresh(json)) {
        return PipBoySpecialRequestStatus::Delivered;
    }

    if (!BuildPipBoySpecialJson(runtime_, json)) {
        return PipBoySpecialRequestStatus::SnapshotUnavailable;
    }
    cache_.Store(json);
    return PipBoySpecialRequestStatus::Delivered;
}

}
