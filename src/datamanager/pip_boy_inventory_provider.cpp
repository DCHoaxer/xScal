#include "datamanager/pip_boy_inventory_provider.hpp"

#include <Windows.h>

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {
namespace {

[[nodiscard]] std::uint64_t SystemTick() noexcept {
    return static_cast<std::uint64_t>(::GetTickCount64());
}

}

PipBoyInventoryProvider::PipBoyInventoryProvider(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery),
      tick_(cacheTick != nullptr ? cacheTick : &SystemTick) {}

bool PipBoyInventoryProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedPipBoyInventoryRuntime resolvedRuntime;
    if (!ResolvePipBoyInventoryRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.pipBoyInventory,
            resolvedRuntime,
            profile.dataManager.itemClass)) {
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

bool PipBoyInventoryProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    runtime_ = {};
    std::scoped_lock lastJsonLock{lastJsonMutex_};
    lastJson_.clear();
    lastJson_.shrink_to_fit();
    lastFingerprint_ = 0;
    lastBuildTick_ = 0;
    return true;
}

bool PipBoyInventoryProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PipBoyInventoryRequestStatus PipBoyInventoryProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PipBoyInventoryRequestStatus::Inactive;
    }

    std::scoped_lock lastJsonLock{lastJsonMutex_};
    const auto now = tick_();
    const bool canReuse = !lastJson_.empty() &&
        now - lastBuildTick_ < kMaximumReuseMilliseconds;
    std::string captured;
    std::uint64_t fingerprint{};
    const auto capture = BuildPipBoyInventoryJson(
        runtime_,
        captured,
        &fingerprint,
        canReuse ? &lastFingerprint_ : nullptr);
    try {
        switch (capture) {
            case PipBoyInventoryCaptureResult::Captured:
                lastJson_ = std::move(captured);
                lastFingerprint_ = fingerprint;
                lastBuildTick_ = now;
                json = lastJson_;
                return PipBoyInventoryRequestStatus::Delivered;
            case PipBoyInventoryCaptureResult::Unchanged:
            case PipBoyInventoryCaptureResult::LockBusy:
                if (lastJson_.empty()) {
                    return PipBoyInventoryRequestStatus::SnapshotUnavailable;
                }
                json = lastJson_;
                return PipBoyInventoryRequestStatus::Delivered;
            case PipBoyInventoryCaptureResult::Unavailable:
                break;
        }
    } catch (...) {
        json.clear();
    }
    lastJson_.clear();
    return PipBoyInventoryRequestStatus::SnapshotUnavailable;
}

}
