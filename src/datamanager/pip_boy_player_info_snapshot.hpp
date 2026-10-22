#pragma once

#include "config/target_profile.hpp"
#include "datamanager/actor_value_reader.hpp"
#include "platform/windows_memory.hpp"

#include <cstdint>
#include <string>

namespace sf::datamanager {

struct ResolvedPipBoyPlayerInfoRuntime final {
    ResolvedActorValueReader reader{};
    config::NativePipBoyPlayerInfoProfile layout{};
    std::uintptr_t getTotalExperienceForLevel{};
    std::uintptr_t getDisplayName{};
};

struct PipBoyPlayerInfo final {
    std::string name;
    std::uint32_t level{};
    // Experience is cumulative; the next two values are relative to the
    // start of the current level, as the HUD experience bar uses them.
    double totalExperience{};
    double levelExperience{};
    double levelExperienceSpan{};
    float levelProgress{};
};

[[nodiscard]] bool ResolvePipBoyPlayerInfoRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActorValueProfile& actorValues,
    const config::NativePipBoyPlayerInfoProfile& playerInfo,
    ResolvedPipBoyPlayerInfoRuntime& runtime) noexcept;

[[nodiscard]] bool ReadPipBoyPlayerInfo(
    const ResolvedPipBoyPlayerInfoRuntime& runtime,
    PipBoyPlayerInfo& info) noexcept;

[[nodiscard]] bool BuildPipBoyPlayerInfoJson(
    const ResolvedPipBoyPlayerInfoRuntime& runtime,
    std::string& json) noexcept;

}
