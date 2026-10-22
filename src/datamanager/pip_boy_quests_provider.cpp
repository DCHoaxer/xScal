#include "datamanager/pip_boy_quests_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

PipBoyQuestsProvider::PipBoyQuestsProvider(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery), cache_(cacheTick) {}

bool PipBoyQuestsProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedPipBoyQuestsRuntime resolvedRuntime;
    if (!ResolvePipBoyQuestsRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.pipBoyQuests,
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

bool PipBoyQuestsProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    cache_.Clear();
    runtime_ = {};
    return true;
}

bool PipBoyQuestsProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PipBoyQuestsRequestStatus PipBoyQuestsProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PipBoyQuestsRequestStatus::Inactive;
    }
    if (cache_.TryCopyFresh(json)) {
        return PipBoyQuestsRequestStatus::Delivered;
    }

    if (!BuildPipBoyQuestsJson(runtime_, json)) {
        return PipBoyQuestsRequestStatus::SnapshotUnavailable;
    }
    cache_.Store(json);
    return PipBoyQuestsRequestStatus::Delivered;
}

}
