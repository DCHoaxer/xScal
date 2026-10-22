#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_status_snapshot.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

using PipBoyStatusRequestStatus = GameProviderRequestStatus;

class PipBoyStatusProvider final {
public:
    explicit PipBoyStatusProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoyStatusProvider(const PipBoyStatusProvider&) = delete;
    PipBoyStatusProvider& operator=(const PipBoyStatusProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] PipBoyStatusRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPipBoyStatusRuntime runtime_{};
    // The inventory reader for the equipped weapon damage; optional.
    ResolvedPipBoyInventoryRuntime inventory_{};
    bool inventoryResolved_{};
    bool installed_{};
    JsonResultCache cache_;
};

}
