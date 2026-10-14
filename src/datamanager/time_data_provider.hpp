#pragma once

#include "config/target_profile.hpp"
#include "datamanager/time_data_snapshot.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

enum class TimeDataRequestStatus {
    Delivered,
    SnapshotUnavailable,
    Inactive,
};

class TimeDataProvider final {
public:
    explicit TimeDataProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery) noexcept;

    TimeDataProvider(const TimeDataProvider&) = delete;
    TimeDataProvider& operator=(const TimeDataProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] TimeDataRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedNativeTimeDataRuntime runtime_{};
    bool installed_{};

};

}
