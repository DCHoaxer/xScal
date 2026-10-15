#include "datamanager/player_map_position_snapshot.hpp"

#include "common/json.hpp"
#include "platform/native_access.hpp"

#include <cmath>
#include <cstddef>

namespace sf::datamanager {
namespace {

constexpr std::size_t kMaximumPlayerMapPositionJsonBytes = 96;
constexpr std::uint8_t kCellInteriorFlag = 0x01;

[[nodiscard]] bool HasCompletePlayerMapPositionProfile(
    const config::NativePlayerMapPositionProfile& profile) noexcept {
    return profile.localPlayerGlobal != 0 &&
        profile.localPlayerPrimaryVtable != 0 &&
        profile.mapWorldBoundsGlobal != 0 &&
        profile.referencePositionOffset != 0 &&
        profile.referenceParentCellOffset != 0 &&
        profile.cellFlagsOffset != 0 &&
        profile.cellWorldSpaceOffset != 0 &&
        profile.worldSpaceMapScaleOffset != 0 &&
        profile.worldSpaceMapOffsetOffset != 0 &&
        profile.worldSpaceMapBoundsOffset != 0;
}

[[nodiscard]] bool CopyAtOffset(
    std::uintptr_t base,
    std::size_t offset,
    void* destination,
    std::size_t size) noexcept {
    std::uintptr_t address{};
    return platform::TryAddAddress(base, offset, address) &&
        platform::TryCopyBytes(
            reinterpret_cast<const void*>(address), destination, size);
}

[[nodiscard]] bool AllFinite(const float* values, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index])) {
            return false;
        }
    }
    return true;
}

}

bool ResolvePlayerMapPositionRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePlayerMapPositionProfile& profile,
    ResolvedPlayerMapPositionRuntime& runtime) noexcept {
    runtime = {};
    if (virtualQuery == nullptr ||
        !HasCompletePlayerMapPositionProfile(profile)) {
        return false;
    }

    ResolvedPlayerMapPositionRuntime resolved;
    resolved.layout = profile;
    if (!platform::ResolveRequiredRvas(
            moduleBase,
            {
                {profile.localPlayerGlobal, &resolved.localPlayerGlobal},
                {profile.localPlayerPrimaryVtable,
                 &resolved.localPlayerPrimaryVtable},
                {profile.mapWorldBoundsGlobal, &resolved.mapWorldBoundsGlobal},
            }) ||
        !platform::AreReadableAddresses(
            virtualQuery,
            {resolved.localPlayerGlobal, resolved.mapWorldBoundsGlobal})) {
        return false;
    }

    runtime = resolved;
    return true;
}

bool ReadPlayerMapPosition(
    const ResolvedPlayerMapPositionRuntime& runtime,
    PlayerMapPosition& position) noexcept {
    position = {};
    if (runtime.localPlayerGlobal == 0 ||
        runtime.localPlayerPrimaryVtable == 0 ||
        runtime.mapWorldBoundsGlobal == 0) {
        return false;
    }
    const auto& layout = runtime.layout;

    std::uintptr_t player{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.localPlayerGlobal),
            &player,
            sizeof(player)) ||
        player == 0) {
        return false;
    }

    std::uintptr_t actualVtable{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(player),
            &actualVtable,
            sizeof(actualVtable)) ||
        actualVtable != runtime.localPlayerPrimaryVtable) {
        return false;
    }

    std::uintptr_t cell{};
    if (!CopyAtOffset(
            player, layout.referenceParentCellOffset, &cell, sizeof(cell)) ||
        cell == 0) {
        return false;
    }

    // Interior cells have no worldspace map; the game shows no player marker.
    std::uint8_t cellFlags{};
    if (!CopyAtOffset(
            cell, layout.cellFlagsOffset, &cellFlags, sizeof(cellFlags)) ||
        (cellFlags & kCellInteriorFlag) != 0) {
        return false;
    }

    std::uintptr_t worldSpace{};
    if (!CopyAtOffset(
            cell, layout.cellWorldSpaceOffset, &worldSpace, sizeof(worldSpace)) ||
        worldSpace == 0) {
        return false;
    }

    float scale{};
    float offset[3]{};
    float bounds[4]{};
    float playerPosition[2]{};
    if (!CopyAtOffset(
            worldSpace,
            layout.worldSpaceMapScaleOffset,
            &scale,
            sizeof(scale)) ||
        !CopyAtOffset(
            worldSpace,
            layout.worldSpaceMapOffsetOffset,
            offset,
            sizeof(offset)) ||
        !CopyAtOffset(
            worldSpace,
            layout.worldSpaceMapBoundsOffset,
            bounds,
            sizeof(bounds)) ||
        !CopyAtOffset(
            player,
            layout.referencePositionOffset,
            playerPosition,
            sizeof(playerPosition)) ||
        !std::isfinite(scale) ||
        !AllFinite(offset, 3) ||
        !AllFinite(bounds, 4) ||
        !AllFinite(playerPosition, 2)) {
        return false;
    }

    // Zero offsets with scale -1 is the unset map data; the game does not
    // show the player marker for it.
    if (offset[0] == 0.0F && offset[1] == 0.0F && offset[2] == 0.0F &&
        scale == -1.0F) {
        return false;
    }

    float mapX = playerPosition[0];
    float mapY = playerPosition[1];
    if (scale != 1.0F && scale != 0.0F) {
        const float centerX = (bounds[0] + bounds[2]) * 0.5F;
        const float centerY = (bounds[1] + bounds[3]) * 0.5F;
        mapX = (mapX - centerX) * scale + centerX;
        mapY = (mapY - centerY) * scale + centerY;
    }
    mapX += offset[0];
    mapY += offset[1];

    // The game divides by the map worldspace cell bounds before writing
    // marker x/y. Zero-width bounds mean the global is not set yet.
    float mapBounds[4]{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.mapWorldBoundsGlobal),
            mapBounds,
            sizeof(mapBounds)) ||
        !AllFinite(mapBounds, 4)) {
        return false;
    }
    const float boundsWidth = mapBounds[2] - mapBounds[0];
    const float boundsHeight = mapBounds[3] - mapBounds[1];
    if (boundsWidth == 0.0F || boundsHeight == 0.0F) {
        return false;
    }
    mapX = (mapX - mapBounds[0]) / boundsWidth;
    mapY = (mapY - mapBounds[1]) / boundsHeight;

    std::uintptr_t currentPlayer{};
    std::uintptr_t currentCell{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.localPlayerGlobal),
            &currentPlayer,
            sizeof(currentPlayer)) ||
        currentPlayer != player ||
        !CopyAtOffset(
            player,
            layout.referenceParentCellOffset,
            &currentCell,
            sizeof(currentCell)) ||
        currentCell != cell ||
        !std::isfinite(mapX) ||
        !std::isfinite(mapY)) {
        return false;
    }

    position.x = mapX;
    position.y = mapY;
    return true;
}

bool BuildPlayerMapPositionJson(
    const ResolvedPlayerMapPositionRuntime& runtime,
    std::string& json) noexcept {
    json.clear();
    PlayerMapPosition position;
    if (!ReadPlayerMapPosition(runtime, position)) {
        return false;
    }

    try {
        json.reserve(48);
        json += R"({"MapX":)";
        if (!json::AppendFloat(position.x, json)) {
            json.clear();
            return false;
        }
        json += R"(,"MapY":)";
        if (!json::AppendFloat(position.y, json)) {
            json.clear();
            return false;
        }
        json.push_back('}');
    } catch (...) {
        json.clear();
        return false;
    }
    if (json.size() > kMaximumPlayerMapPositionJsonBytes) {
        json.clear();
        return false;
    }
    return true;
}

}
