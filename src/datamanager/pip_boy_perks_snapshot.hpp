#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sf::datamanager {

struct ResolvedPipBoyPerksRuntime final {
    config::NativePipBoyPerksProfile layout{};
    std::uintptr_t localPlayerGlobal{};
    std::uintptr_t localPlayerPrimaryVtable{};
    std::uintptr_t getPlayerGameModeIndex{};
    std::uintptr_t getPerkCardRankPerk{};
    std::uintptr_t getPerkDescriptionText{};
    std::uintptr_t deallocate{};
};

struct PipBoyPerkCard final {
    std::string name;
    std::string description;
    // The card's own description, shown as flavor text under the rank text.
    std::string flavorText;
    std::uint32_t perkId{};
    // Per-instance id the game uses to tell duplicate cards apart.
    std::uint16_t uniqueId{};
    std::uint32_t rank{};
    std::uint32_t maximumRank{};
    std::int32_t pointCost{};
    std::uint8_t levelRequirement{};
    // 0 any race, 1 non-ghoul only, 2 ghoul only.
    std::uint8_t raceRestriction{};
    // 1 for Nuclear Winter cards.
    std::uint8_t gameMode{};
    // 0..6 for Strength..Luck; 7 when the card has no SPECIAL.
    std::uint8_t special{};
    bool equipped{};
    bool animated{};
};

[[nodiscard]] bool ResolvePipBoyPerksRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyPerksProfile& perks,
    ResolvedPipBoyPerksRuntime& runtime) noexcept;

[[nodiscard]] bool ReadPipBoyPerkCards(
    const ResolvedPipBoyPerksRuntime& runtime,
    std::vector<PipBoyPerkCard>& cards) noexcept;

[[nodiscard]] bool BuildPipBoyPerksJson(
    const ResolvedPipBoyPerksRuntime& runtime,
    std::string& json) noexcept;

}
