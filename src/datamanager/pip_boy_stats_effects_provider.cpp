#include "datamanager/pip_boy_stats_effects_provider.hpp"

#include "datamanager/active_effect_snapshot.hpp"

#include <Windows.h>

#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

namespace sf::datamanager {
namespace {

[[nodiscard]] std::uint64_t SystemCacheTick() noexcept {
    return static_cast<std::uint64_t>(::GetTickCount64());
}

}

PipBoyStatsEffectsProvider::PipBoyStatsEffectsProvider(
    platform::VirtualQueryFn virtualQuery,
    CacheTickFn cacheTick) noexcept
    : virtualQuery_(virtualQuery),
      cacheTick_(cacheTick != nullptr ? cacheTick : &SystemCacheTick) {}

bool PipBoyStatsEffectsProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    ResolvedNativeActiveEffectRuntime resolvedRuntime;
    if (virtualQuery_ == nullptr ||
        !ResolveNativeActiveEffectRuntime(
            virtualQuery_,
            moduleBase,
            profile.dataManager.pipBoyStatsEffects,
            resolvedRuntime)) {
        return false;
    }

    std::unique_lock lock{lifecycleMutex_};
    if (installed_) {
        return false;
    }

    runtime_ = resolvedRuntime;
    snapshot_.store(nullptr);
    installed_ = true;
    return true;
}

bool PipBoyStatsEffectsProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    runtime_ = {};
    snapshot_.store(nullptr);
    return true;
}

bool PipBoyStatsEffectsProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

StatsEffectsRequestStatus PipBoyStatsEffectsProvider::Request(
    std::shared_ptr<const std::string>& json) noexcept {
    json.reset();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return StatsEffectsRequestStatus::Inactive;
    }

    const auto deliverCached = [&json, this](
        std::uint64_t lifetimeMilliseconds) noexcept {
        const auto cached = snapshot_.load();
        const auto currentTick = cacheTick_();
        if (cached == nullptr || cached->json == nullptr ||
            cached->sourcePlayer == 0 ||
            currentTick < cached->capturedTick ||
            currentTick - cached->capturedTick >= lifetimeMilliseconds) {
            return StatsEffectsRequestStatus::SnapshotUnavailable;
        }

        std::uintptr_t currentSource{};
        if (!ReadNativeActiveEffectSourcePlayer(runtime_, currentSource) ||
            currentSource != cached->sourcePlayer) {
            return StatsEffectsRequestStatus::SnapshotUnavailable;
        }
        json = cached->json;
        return StatsEffectsRequestStatus::Delivered;
    };

    std::unique_lock snapshotLock{snapshotMutex_, std::try_to_lock};
    if (!snapshotLock.owns_lock()) {
        return deliverCached(kCacheLifetimeMilliseconds);
    }
    if (deliverCached(kFreshnessMilliseconds) ==
        StatsEffectsRequestStatus::Delivered) {
        return StatsEffectsRequestStatus::Delivered;
    }

    std::string snapshotJson;
    bool lockBusy = false;
    std::uintptr_t sourcePlayer{};
    if (!BuildNativeActiveEffectJsonSnapshot(
            virtualQuery_,
            runtime_,
            snapshotJson,
            &lockBusy,
            &sourcePlayer)) {
        if (!lockBusy) {
            return StatsEffectsRequestStatus::SnapshotUnavailable;
        }
        return deliverCached(kCacheLifetimeMilliseconds);
    }

    try {
        auto published =
            std::make_shared<const std::string>(std::move(snapshotJson));
        snapshot_.store(std::make_shared<const CachedStatsSnapshot>(
            CachedStatsSnapshot{published, sourcePlayer, cacheTick_()}));
        json = std::move(published);
    } catch (...) {
        return StatsEffectsRequestStatus::SnapshotUnavailable;
    }
    return StatsEffectsRequestStatus::Delivered;
}

}
