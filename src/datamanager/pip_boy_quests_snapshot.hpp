#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sf::datamanager {

struct ResolvedPipBoyQuestsRuntime final {
    config::NativePipBoyQuestsProfile layout{};
    std::uintptr_t objectiveStateEntriesGlobal{};
    std::uintptr_t objectiveStateCapacityGlobal{};
    std::uintptr_t getQuestFromHandle{};
    std::uintptr_t getQuestObjectiveById{};
    std::uintptr_t getFixedStringText{};
    std::uintptr_t assignGameString{};
    std::uintptr_t formatQuestText{};
    std::uintptr_t deallocate{};
    std::uintptr_t questStateEntriesGlobal{};
    std::uintptr_t questStateCapacityGlobal{};
    std::uintptr_t getFixedStringCharacters{};
};

struct PipBoyQuestObjective final {
    std::string text;
    std::uint16_t index{};
    bool completed{};
    bool failed{};
    // The objective is an alternative to the one listed before it.
    bool orWithPrevious{};
};

struct PipBoyQuest final {
    std::uint32_t formId{};
    std::string name;
    // Custom Pip-Boy SWF the quest page loads; empty for none.
    std::string swfFile;
    // The quest type byte the Pip-Boy uses to pick a quest tab.
    std::uint8_t type{};
    // Quest state record flags, as AddPipBoyQuestEntry publishes them:
    // IsActive is bit 11, IsEnabled is bit 1 clear, IsCompleted is "no
    // record" or bit 1 set.
    bool active{};
    bool enabled{};
    bool completed{};
    bool canBeRejected{};
    std::vector<PipBoyQuestObjective> objectives;
};

[[nodiscard]] bool ResolvePipBoyQuestsRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyQuestsProfile& quests,
    ResolvedPipBoyQuestsRuntime& runtime) noexcept;

// Reads the objectives the Pip-Boy lists: displayed objectives, grouped by
// quest in the order the quests are first found, objectives sorted by index.
[[nodiscard]] bool ReadPipBoyQuests(
    const ResolvedPipBoyQuestsRuntime& runtime,
    std::vector<PipBoyQuest>& quests) noexcept;

[[nodiscard]] bool BuildPipBoyQuestsJson(
    const ResolvedPipBoyQuestsRuntime& runtime,
    std::string& json) noexcept;

}
