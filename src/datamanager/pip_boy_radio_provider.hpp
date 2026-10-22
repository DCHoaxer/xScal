#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_radio_snapshot.hpp"

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

using PipBoyRadioRequestStatus = GameProviderRequestStatus;

class PipBoyRadioProvider final {
public:
    explicit PipBoyRadioProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoyRadioProvider(const PipBoyRadioProvider&) = delete;
    PipBoyRadioProvider& operator=(const PipBoyRadioProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    // Returns the last captured radio state while the game holds the
    // radio lock for writing.
    [[nodiscard]] PipBoyRadioRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPipBoyRadioRuntime runtime_{};
    bool installed_{};
    JsonResultCache cache_;
    std::mutex lastJsonMutex_;
    std::string lastJson_;
};

}
