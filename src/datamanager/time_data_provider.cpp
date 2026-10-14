#include "datamanager/time_data_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

TimeDataProvider::TimeDataProvider(
    platform::VirtualQueryFn virtualQuery) noexcept
    : virtualQuery_(virtualQuery) {}

bool TimeDataProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    const auto& timeData = profile.dataManager.timeData;
    ResolvedNativeTimeDataRuntime resolvedRuntime;
    if (!ResolveNativeTimeDataRuntime(
            virtualQuery_,
            moduleBase,
            timeData,
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

bool TimeDataProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    runtime_ = {};
    return true;
}

bool TimeDataProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

TimeDataRequestStatus TimeDataProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return TimeDataRequestStatus::Inactive;
    }

    if (!BuildNativeTimeDataJson(runtime_, json)) {
        return TimeDataRequestStatus::SnapshotUnavailable;
    }
    return TimeDataRequestStatus::Delivered;
}

}
