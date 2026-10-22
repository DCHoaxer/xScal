#include "datamanager/pip_boy_radio_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

PipBoyRadioProvider::PipBoyRadioProvider(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery), cache_(cacheTick) {}

bool PipBoyRadioProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedPipBoyRadioRuntime resolvedRuntime;
    if (!ResolvePipBoyRadioRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.pipBoyRadio,
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

bool PipBoyRadioProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    cache_.Clear();
    runtime_ = {};
    std::scoped_lock lastJsonLock{lastJsonMutex_};
    lastJson_.clear();
    lastJson_.shrink_to_fit();
    return true;
}

bool PipBoyRadioProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PipBoyRadioRequestStatus PipBoyRadioProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PipBoyRadioRequestStatus::Inactive;
    }
    if (cache_.TryCopyFresh(json)) {
        return PipBoyRadioRequestStatus::Delivered;
    }

    std::string captured;
    const auto capture = BuildPipBoyRadioJson(runtime_, captured);
    std::scoped_lock lastJsonLock{lastJsonMutex_};
    try {
        switch (capture) {
            case PipBoyRadioCaptureResult::Captured:
                lastJson_ = captured;
                json = std::move(captured);
                cache_.Store(json);
                return PipBoyRadioRequestStatus::Delivered;
            case PipBoyRadioCaptureResult::LockBusy:
                if (lastJson_.empty()) {
                    return PipBoyRadioRequestStatus::SnapshotUnavailable;
                }
                json = lastJson_;
                cache_.Store(json);
                return PipBoyRadioRequestStatus::Delivered;
            case PipBoyRadioCaptureResult::Unavailable:
                break;
        }
    } catch (...) {
        json.clear();
    }
    lastJson_.clear();
    return PipBoyRadioRequestStatus::SnapshotUnavailable;
}

}
