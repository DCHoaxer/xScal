#include "datamanager/player_map_position_provider.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

PlayerMapPositionProvider::PlayerMapPositionProvider(
    platform::VirtualQueryFn virtualQuery) noexcept
    : virtualQuery_(virtualQuery) {}

bool PlayerMapPositionProvider::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    const auto& playerMapPosition = profile.dataManager.playerMapPosition;
    ResolvedPlayerMapPositionRuntime resolvedRuntime;
    if (!ResolvePlayerMapPositionRuntime(
            virtualQuery_,
            moduleBase,
            playerMapPosition,
            resolvedRuntime)) {
        return false;
    }

    std::unique_lock lock{lifecycleMutex_};
    if (installed_) {
        return false;
    }

    runtime_ = resolvedRuntime;
    installed_ = true;
    return true;
}

bool PlayerMapPositionProvider::Restore() noexcept {
    std::unique_lock lock{lifecycleMutex_};
    if (!installed_) {
        return false;
    }

    installed_ = false;
    runtime_ = {};
    return true;
}

bool PlayerMapPositionProvider::IsActive() const noexcept {
    std::shared_lock lock{lifecycleMutex_};
    return installed_;
}

PlayerMapPositionRequestStatus PlayerMapPositionProvider::Request(
    std::string& json) noexcept {
    json.clear();

    std::shared_lock lifecycleLock{lifecycleMutex_};
    if (!installed_) {
        return PlayerMapPositionRequestStatus::Inactive;
    }

    if (!BuildPlayerMapPositionJson(runtime_, json)) {
        return PlayerMapPositionRequestStatus::SnapshotUnavailable;
    }
    return PlayerMapPositionRequestStatus::Delivered;
}

}
