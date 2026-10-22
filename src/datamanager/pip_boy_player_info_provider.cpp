#include "datamanager/pip_boy_player_info_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

PipBoyPlayerInfoProvider::PipBoyPlayerInfoProvider(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery), cache_(cacheTick) {}

bool PipBoyPlayerInfoProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedPipBoyPlayerInfoRuntime resolvedRuntime;
    if (!ResolvePipBoyPlayerInfoRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.actorValues,
            profile.dataManager.pipBoyPlayerInfo,
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

bool PipBoyPlayerInfoProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    cache_.Clear();
    runtime_ = {};
    return true;
}

bool PipBoyPlayerInfoProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PipBoyPlayerInfoRequestStatus PipBoyPlayerInfoProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PipBoyPlayerInfoRequestStatus::Inactive;
    }
    if (cache_.TryCopyFresh(json)) {
        return PipBoyPlayerInfoRequestStatus::Delivered;
    }

    if (!BuildPipBoyPlayerInfoJson(runtime_, json)) {
        return PipBoyPlayerInfoRequestStatus::SnapshotUnavailable;
    }
    cache_.Store(json);
    return PipBoyPlayerInfoRequestStatus::Delivered;
}

}
