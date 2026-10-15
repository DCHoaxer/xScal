#pragma once

#include "config/target_profile.hpp"
#include "datamanager/player_map_position_snapshot.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

enum class PlayerMapPositionRequestStatus {
    Delivered,
    SnapshotUnavailable,
    Inactive,
};

class PlayerMapPositionProvider final {
public:
    explicit PlayerMapPositionProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery) noexcept;

    PlayerMapPositionProvider(const PlayerMapPositionProvider&) = delete;
    PlayerMapPositionProvider& operator=(const PlayerMapPositionProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

    [[nodiscard]] PlayerMapPositionRequestStatus Request(
        std::string& json) noexcept;

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    ResolvedPlayerMapPositionRuntime runtime_{};
    bool installed_{};

};

}
