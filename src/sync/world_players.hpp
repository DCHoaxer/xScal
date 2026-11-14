#pragma once

#include "config/target_profile.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace sf::sync {

struct WorldAddresses final {
    std::uintptr_t playerManagerGlobal{};
    std::uintptr_t recordOwnerGlobal{};
    std::uintptr_t worldDataModelGlobal{};
    std::uintptr_t worldDataModelVtable{};
    std::uintptr_t tryAcquireReadLock{};
    std::uintptr_t releaseReadLock{};
    std::size_t joinedWorldOffset{};
};

[[nodiscard]] bool ReadWorldAddresses(
    const config::ProtoSyncProfile& profile,
    std::uintptr_t moduleBase,
    WorldAddresses& addresses) noexcept;

[[nodiscard]] std::uint64_t AccountIdKey(std::uint64_t accountId) noexcept;
[[nodiscard]] std::uint64_t NameKey(std::string_view name) noexcept;
[[nodiscard]] std::uint64_t WorldKey(std::uint64_t worldId) noexcept;

struct WorldPlayer final {
    std::uint32_t playerId{};
    std::uint64_t accountKey{};
    std::uint64_t nameKey{};
    bool isLocal{};
};

// The game keeps this ID after the player leaves the world.
[[nodiscard]] std::uint64_t ReadJoinedWorldId(const WorldAddresses& addresses) noexcept;

[[nodiscard]] bool ReadWorldPlayers(
    const WorldAddresses& addresses,
    std::vector<WorldPlayer>& players) noexcept;

}  // namespace sf::sync
