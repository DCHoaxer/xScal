#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_special_snapshot.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

using PipBoySpecialRequestStatus = GameProviderRequestStatus;

class PipBoySpecialProvider final {
public:
    explicit PipBoySpecialProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoySpecialProvider(const PipBoySpecialProvider&) = delete;
    PipBoySpecialProvider& operator=(const PipBoySpecialProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] PipBoySpecialRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPipBoySpecialRuntime runtime_{};
    bool installed_{};
    JsonResultCache cache_;
};

}
