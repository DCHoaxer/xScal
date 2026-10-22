#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_perks_snapshot.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

using PipBoyPerksRequestStatus = GameProviderRequestStatus;

class PipBoyPerksProvider final {
public:
    explicit PipBoyPerksProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoyPerksProvider(const PipBoyPerksProvider&) = delete;
    PipBoyPerksProvider& operator=(const PipBoyPerksProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] PipBoyPerksRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPipBoyPerksRuntime runtime_{};
    bool installed_{};
    JsonResultCache cache_;
};

}
