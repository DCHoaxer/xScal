#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_collections_snapshot.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

using PipBoyCollectionsRequestStatus = GameProviderRequestStatus;

class PipBoyCollectionsProvider final {
public:
    explicit PipBoyCollectionsProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoyCollectionsProvider(const PipBoyCollectionsProvider&) = delete;
    PipBoyCollectionsProvider& operator=(const PipBoyCollectionsProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] PipBoyCollectionsRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPipBoyCollectionsRuntime runtime_{};
    bool installed_{};
    JsonResultCache cache_;
};

}
