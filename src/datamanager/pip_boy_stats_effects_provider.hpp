#pragma once

#include "config/target_profile.hpp"
#include "datamanager/active_effect_snapshot.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

enum class StatsEffectsRequestStatus {
    Delivered,
    SnapshotUnavailable,
    Inactive,
};

// The last published snapshot. A record is replaced as a whole, so readers that
// do not hold snapshotMutex_ see either the old or the new record.
struct CachedStatsSnapshot final {
    std::shared_ptr<const std::string> json;
    std::uintptr_t sourcePlayer{};
    std::uint64_t capturedTick{};
};

class PipBoyStatsEffectsProvider final {
public:
    using CacheTickFn = std::uint64_t(*)() noexcept;
    // Nearby HUD requests share a snapshot; lock contention uses the longer limit.
    static constexpr std::uint64_t kFreshnessMilliseconds = 50;
    static constexpr std::uint64_t kCacheLifetimeMilliseconds = 10000;

    explicit PipBoyStatsEffectsProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        CacheTickFn cacheTick = nullptr) noexcept;

    PipBoyStatsEffectsProvider(const PipBoyStatsEffectsProvider&) = delete;
    PipBoyStatsEffectsProvider& operator=(
        const PipBoyStatsEffectsProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] StatsEffectsRequestStatus Request(
        std::shared_ptr<const std::string>& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    CacheTickFn cacheTick_{};
    mutable std::shared_mutex lifecycleMutex_;
    // Serializes snapshot builds. A request that cannot take it reads the
    // published record instead of waiting for the build to finish.
    std::mutex snapshotMutex_;
    std::atomic<std::shared_ptr<const CachedStatsSnapshot>> snapshot_;
    ResolvedNativeActiveEffectRuntime runtime_{};
    bool installed_{};
};

}
