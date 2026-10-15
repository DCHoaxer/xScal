#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstdint>
#include <string>

namespace sf::datamanager {

struct ResolvedPlayerMapPositionRuntime final {
    config::NativePlayerMapPositionProfile layout{};
    std::uintptr_t localPlayerGlobal{};
    std::uintptr_t localPlayerPrimaryVtable{};
    std::uintptr_t mapWorldBoundsGlobal{};
};

// Normalized Pip-Boy map coordinates, the same as the MapMenuData
// marker x/y (PlayerLocal and location markers). required because ugh, bethesda...
struct PlayerMapPosition final {
    float x{};
    float y{};
};

[[nodiscard]] bool ResolvePlayerMapPositionRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePlayerMapPositionProfile& profile,
    ResolvedPlayerMapPositionRuntime& runtime) noexcept;

[[nodiscard]] bool ReadPlayerMapPosition(
    const ResolvedPlayerMapPositionRuntime& runtime,
    PlayerMapPosition& position) noexcept;

[[nodiscard]] bool BuildPlayerMapPositionJson(
    const ResolvedPlayerMapPositionRuntime& runtime,
    std::string& json) noexcept;

}
