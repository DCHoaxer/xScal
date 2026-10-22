#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_player_info_snapshot.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

using PipBoyPlayerInfoRequestStatus = GameProviderRequestStatus;

class PipBoyPlayerInfoProvider final {
public:
    explicit PipBoyPlayerInfoProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoyPlayerInfoProvider(const PipBoyPlayerInfoProvider&) = delete;
    PipBoyPlayerInfoProvider& operator=(const PipBoyPlayerInfoProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] PipBoyPlayerInfoRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPipBoyPlayerInfoRuntime runtime_{};
    bool installed_{};
    JsonResultCache cache_;
};

}
