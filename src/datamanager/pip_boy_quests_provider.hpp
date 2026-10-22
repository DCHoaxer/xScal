#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_quests_snapshot.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

using PipBoyQuestsRequestStatus = GameProviderRequestStatus;

class PipBoyQuestsProvider final {
public:
    explicit PipBoyQuestsProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoyQuestsProvider(const PipBoyQuestsProvider&) = delete;
    PipBoyQuestsProvider& operator=(const PipBoyQuestsProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] PipBoyQuestsRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPipBoyQuestsRuntime runtime_{};
    bool installed_{};
    JsonResultCache cache_;
};

}
