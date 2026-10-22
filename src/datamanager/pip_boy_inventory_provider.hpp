#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_inventory_snapshot.hpp"

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

using PipBoyInventoryRequestStatus = GameProviderRequestStatus;

class PipBoyInventoryProvider final {
public:
    // A result is reused while the item list hashes the same, for at most
    // this long; values the hash does not cover (condition, perk-adjusted
    // stats) can be this old.
    static constexpr std::uint64_t kMaximumReuseMilliseconds = 1000;

    explicit PipBoyInventoryProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoyInventoryProvider(const PipBoyInventoryProvider&) = delete;
    PipBoyInventoryProvider& operator=(const PipBoyInventoryProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    // Returns the last captured inventory while the game holds the
    // inventory lock for writing or the item list is unchanged.
    [[nodiscard]] PipBoyInventoryRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPipBoyInventoryRuntime runtime_{};
    bool installed_{};
    JsonResultCache::TickFn tick_{};
    std::mutex lastJsonMutex_;
    std::string lastJson_;
    std::uint64_t lastFingerprint_{};
    std::uint64_t lastBuildTick_{};
};

}
