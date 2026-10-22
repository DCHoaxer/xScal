#pragma once

#include "config/target_profile.hpp"
#include "datamanager/actor_value_reader.hpp"
#include "platform/windows_memory.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace sf::datamanager {

struct ResolvedPipBoySpecialRuntime final {
    ResolvedActorValueReader reader{};
    config::NativePipBoySpecialProfile layout{};
    std::uintptr_t getSpecialBonusValue{};
    std::uintptr_t deallocate{};
};

// One SPECIAL as the Pip-Boy SPECIAL tab shows it.
struct PipBoySpecialStat final {
    std::string name;
    std::string description;
    std::int32_t value{};
    // The 0..255 bonus the Pip-Boy reads from a separate bonus value.
    std::uint8_t bonus{};
};

// Stats in Strength, Perception, Endurance, Charisma, Intelligence, Agility,
// Luck order.
using PipBoySpecialStats = std::array<PipBoySpecialStat, 7>;

[[nodiscard]] bool ResolvePipBoySpecialRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActorValueProfile& actorValues,
    const config::NativePipBoySpecialProfile& special,
    ResolvedPipBoySpecialRuntime& runtime) noexcept;

[[nodiscard]] bool ReadPipBoySpecialStats(
    const ResolvedPipBoySpecialRuntime& runtime,
    PipBoySpecialStats& stats) noexcept;

[[nodiscard]] bool BuildPipBoySpecialJson(
    const ResolvedPipBoySpecialRuntime& runtime,
    std::string& json) noexcept;

}
