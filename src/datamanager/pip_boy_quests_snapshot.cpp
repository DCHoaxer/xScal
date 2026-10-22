#include "datamanager/pip_boy_quests_snapshot.hpp"

#include "common/json.hpp"
#include "datamanager/game_string.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <utility>

namespace sf::datamanager {
namespace {

// Objective state values the Pip-Boy uses: 1 displayed, 3 displayed and
// completed, 5 displayed and failed. 7 is never listed.
constexpr std::uint32_t kDisplayedState = 1;
constexpr std::uint32_t kCompletedState = 3;
constexpr std::uint32_t kFailedState = 5;
constexpr std::uint32_t kEmptyChainIndex = 0xFFFFFFFF;
// A quest objective keeps its text BSFixedString at +0x00.
constexpr std::size_t kObjectiveTextOffset = 0x0;
// Bit 2 of the objective flags hides an open objective; bit 0 marks an
// objective that is an alternative to the previous one.
constexpr std::uint32_t kHiddenObjectiveFlag = 0x4;
constexpr std::uint32_t kOrWithPreviousObjectiveFlag = 0x1;
// Quest flags bit 24: the player can reject the quest.
constexpr std::uint32_t kCanBeRejectedQuestFlag = 1U << 24;
// Quest state record flags read by AddPipBoyQuestEntry.
constexpr std::uint64_t kQuestStateActiveFlag = 1ULL << 11;
constexpr std::uint64_t kQuestStateFinishedFlag = 1ULL << 1;
// Quest-level state entries use this objective id in their key.
constexpr std::uint16_t kQuestLevelObjectiveId = 0xFFFF;

constexpr std::uint64_t kMaximumMapCapacity = 1ULL << 20;
constexpr std::size_t kMaximumQuests = 256;
constexpr std::size_t kMaximumObjectives = 2048;
constexpr std::size_t kMaximumNameLength = 256;
constexpr std::size_t kMaximumTextLength = 4096;
constexpr std::size_t kMaximumQuestsJsonBytes = 2 * 1024 * 1024;

// Key of an objective state entry, also the handle the game passes to its
// quest lookup and text formatting: quest form ID, an owner/context value,
// and the objective id.
struct QuestObjectiveHandle final {
    std::uint32_t questFormId{};
    std::uint32_t context{};
    std::uint16_t objectiveId{};
    std::uint16_t padding{};
};
static_assert(sizeof(QuestObjectiveHandle) == 0xC);
constexpr std::size_t kHandleBytes = 0xA;

using GetQuestFromHandle = void*(__fastcall*)(
    const QuestObjectiveHandle* handle);
using GetQuestObjectiveById = void*(__fastcall*)(
    void* quest,
    std::uint32_t objectiveId);
using GetFixedStringText = const char*(__fastcall*)(const void* fixedString);
using GetFixedStringCharacters =
    const char*(__fastcall*)(const void* fixedString);
using AssignGameString = void(__fastcall*)(
    GameString* text,
    const char* value,
    std::uint32_t length);
using FormatQuestText = bool(__fastcall*)(
    const QuestObjectiveHandle* handle,
    GameString* text);

template <typename T>
[[nodiscard]] bool ReadField(
    std::uintptr_t object,
    std::size_t offset,
    T& value) noexcept {
    std::uintptr_t address{};
    return platform::TryAddAddress(object, offset, address) &&
        platform::TryCopyBytes(
            reinterpret_cast<const void*>(address), &value, sizeof(value));
}

[[nodiscard]] bool CallGetQuestFromHandle(
    std::uintptr_t function,
    const QuestObjectiveHandle& handle,
    std::uintptr_t& quest) noexcept {
    __try {
        quest = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<GetQuestFromHandle>(function)(&handle));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        quest = 0;
        return false;
    }
    return true;
}

[[nodiscard]] bool CallGetQuestObjectiveById(
    std::uintptr_t function,
    std::uintptr_t quest,
    std::uint16_t objectiveId,
    std::uintptr_t& objective) noexcept {
    __try {
        objective = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<GetQuestObjectiveById>(function)(
                reinterpret_cast<void*>(quest), objectiveId));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        objective = 0;
        return false;
    }
    return true;
}

[[nodiscard]] bool CallGetFixedStringText(
    std::uintptr_t function,
    std::uintptr_t fixedString,
    const char*& text) noexcept {
    __try {
        text = reinterpret_cast<GetFixedStringText>(function)(
            reinterpret_cast<const void*>(fixedString));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        text = nullptr;
        return false;
    }
    return text != nullptr;
}

// Returns an empty string for a null BSFixedString.
[[nodiscard]] bool CallGetFixedStringCharacters(
    std::uintptr_t function,
    std::uintptr_t fixedString,
    const char*& text) noexcept {
    __try {
        text = reinterpret_cast<GetFixedStringCharacters>(function)(
            reinterpret_cast<const void*>(fixedString));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        text = nullptr;
        return false;
    }
    if (text == nullptr) {
        text = "";
    }
    return true;
}

[[nodiscard]] bool CallFormatObjectiveText(
    std::uintptr_t assign,
    std::uintptr_t format,
    const char* rawText,
    const QuestObjectiveHandle& handle,
    GameString& text) noexcept {
    __try {
        reinterpret_cast<AssignGameString>(assign)(&text, rawText, 0);
        static_cast<void>(
            reinterpret_cast<FormatQuestText>(format)(&handle, &text));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

// Copies the objective text with its tags expanded, as the Pip-Boy shows it.
[[nodiscard]] bool ReadObjectiveText(
    const ResolvedPipBoyQuestsRuntime& runtime,
    std::uintptr_t objective,
    const QuestObjectiveHandle& handle,
    std::string& text) noexcept {
    std::uintptr_t fixedString{};
    const char* rawText{};
    if (!platform::TryAddAddress(objective, kObjectiveTextOffset, fixedString) ||
        !CallGetFixedStringText(
            runtime.getFixedStringText, fixedString, rawText)) {
        return false;
    }

    GameString formatted;
    const GameStringOwner owner{
        reinterpret_cast<GameDeallocate>(runtime.deallocate), formatted};
    return CallFormatObjectiveText(
               runtime.assignGameString,
               runtime.formatQuestText,
               rawText,
               handle,
               formatted) &&
        CopyGameString(formatted, kMaximumTextLength, text);
}

// The Pip-Boy does not list an open objective of a hidden quest type, of a
// quest with the high flag bit set, or with the hidden objective flag.
[[nodiscard]] bool IsHiddenOpenObjective(
    const config::NativePipBoyQuestsProfile& layout,
    std::uintptr_t quest,
    std::uintptr_t objective,
    bool& hidden) noexcept {
    std::uint8_t questType{};
    std::int32_t questFlags{};
    std::uint32_t objectiveFlags{};
    if (!ReadField(quest, layout.questTypeOffset, questType) ||
        !ReadField(quest, layout.questFlagsOffset, questFlags) ||
        !ReadField(objective, layout.objectiveFlagsOffset, objectiveFlags)) {
        return false;
    }
    hidden = questType == layout.hiddenQuestType || questFlags < 0 ||
        (objectiveFlags & kHiddenObjectiveFlag) != 0;
    return true;
}

struct HashMapView final {
    std::uintptr_t entries{};
    std::uint64_t capacity{};
};

// Reads an open-addressing map's entry array and capacity globals. A zero
// capacity means the game has not allocated the map yet.
[[nodiscard]] bool ReadHashMapView(
    std::uintptr_t entriesGlobal,
    std::uintptr_t capacityGlobal,
    std::size_t entrySize,
    HashMapView& view) noexcept {
    view = {};
    HashMapView result;
    if (entriesGlobal == 0 ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(entriesGlobal),
            &result.entries,
            sizeof(result.entries)) ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(capacityGlobal),
            &result.capacity,
            sizeof(result.capacity))) {
        return false;
    }
    if (result.capacity == 0) {
        return true;
    }
    if (result.entries == 0 || result.capacity > kMaximumMapCapacity ||
        (result.capacity & (result.capacity - 1)) != 0 ||
        result.capacity > (std::numeric_limits<std::uintptr_t>::max)() /
            entrySize) {
        return false;
    }
    view = result;
    return true;
}

// Looks up the quest-level state entry the way GetQuestStateRecord does:
// bucket (formID + context) & (capacity - 1), then the chain of next-entry
// indexes until one equals the capacity.
[[nodiscard]] bool ReadQuestStateFlags(
    const config::NativePipBoyQuestsProfile& layout,
    const HashMapView& map,
    std::uint32_t questFormId,
    std::uint32_t context,
    bool& found,
    std::uint64_t& flags) noexcept {
    found = false;
    flags = 0;
    if (map.capacity == 0) {
        return true;
    }
    auto index = (static_cast<std::uint64_t>(
                      static_cast<std::uint32_t>(questFormId + context))) &
        (map.capacity - 1);
    for (std::uint64_t step = 0; step < map.capacity; ++step) {
        std::uintptr_t entry{};
        QuestObjectiveHandle key;
        std::uint32_t next{};
        if (!platform::TryAddAddress(
                map.entries,
                static_cast<std::size_t>(index) * layout.questStateEntrySize,
                entry) ||
            !platform::TryCopyBytes(
                reinterpret_cast<const void*>(entry), &key, kHandleBytes) ||
            !ReadField(entry, layout.questStateChainOffset, next)) {
            return false;
        }
        if (step == 0 && next == kEmptyChainIndex) {
            return true;
        }
        if (key.questFormId == questFormId && key.context == context &&
            key.objectiveId == kQuestLevelObjectiveId) {
            found = true;
            return ReadField(entry, layout.questStateFlagsOffset, flags);
        }
        if (next == map.capacity) {
            return true;
        }
        if (next > map.capacity) {
            return false;
        }
        index = next;
    }
    return false;
}

[[nodiscard]] PipBoyQuest* FindOrAddQuest(
    const ResolvedPipBoyQuestsRuntime& runtime,
    const HashMapView& questStates,
    std::vector<PipBoyQuest>& quests,
    const QuestObjectiveHandle& handle,
    std::uintptr_t quest) noexcept {
    const auto questFormId = handle.questFormId;
    const auto found = std::find_if(
        quests.begin(), quests.end(), [questFormId](const PipBoyQuest& entry) {
            return entry.formId == questFormId;
        });
    if (found != quests.end()) {
        return &*found;
    }
    if (quests.size() >= kMaximumQuests) {
        return nullptr;
    }

    const auto& layout = runtime.layout;
    PipBoyQuest entry;
    entry.formId = questFormId;
    std::uintptr_t nameString{};
    std::uintptr_t swfString{};
    const char* name{};
    const char* swfFile{};
    std::uint32_t questFlags{};
    bool hasState{};
    std::uint64_t stateFlags{};
    if (!platform::TryAddAddress(quest, layout.questNameOffset, nameString) ||
        !CallGetFixedStringText(runtime.getFixedStringText, nameString, name) ||
        !platform::CopyBoundedCString(name, kMaximumNameLength, true, entry.name) ||
        !platform::TryAddAddress(quest, layout.questSwfFileOffset, swfString) ||
        !CallGetFixedStringCharacters(
            runtime.getFixedStringCharacters, swfString, swfFile) ||
        !platform::CopyBoundedCString(
            swfFile, kMaximumNameLength, true, entry.swfFile) ||
        !ReadField(quest, layout.questTypeOffset, entry.type) ||
        !ReadField(quest, layout.questFlagsOffset, questFlags) ||
        !ReadQuestStateFlags(
            layout, questStates, questFormId, handle.context, hasState,
            stateFlags)) {
        return nullptr;
    }
    entry.canBeRejected = (questFlags & kCanBeRejectedQuestFlag) != 0;
    entry.active = hasState && (stateFlags & kQuestStateActiveFlag) != 0;
    entry.enabled = !hasState || (stateFlags & kQuestStateFinishedFlag) == 0;
    entry.completed = !hasState || (stateFlags & kQuestStateFinishedFlag) != 0;
    try {
        quests.push_back(std::move(entry));
    } catch (...) {
        return nullptr;
    }
    return &quests.back();
}

}

constexpr std::array kPipBoyQuestsRuntimeBindings{
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, objectiveStateEntriesGlobal),
        offsetof(ResolvedPipBoyQuestsRuntime, objectiveStateEntriesGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, objectiveStateCapacityGlobal),
        offsetof(ResolvedPipBoyQuestsRuntime, objectiveStateCapacityGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, getQuestFromHandle),
        offsetof(ResolvedPipBoyQuestsRuntime, getQuestFromHandle),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, getQuestObjectiveById),
        offsetof(ResolvedPipBoyQuestsRuntime, getQuestObjectiveById),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, getFixedStringText),
        offsetof(ResolvedPipBoyQuestsRuntime, getFixedStringText),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, assignGameString),
        offsetof(ResolvedPipBoyQuestsRuntime, assignGameString),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, formatQuestText),
        offsetof(ResolvedPipBoyQuestsRuntime, formatQuestText),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, deallocate),
        offsetof(ResolvedPipBoyQuestsRuntime, deallocate),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, questStateEntriesGlobal),
        offsetof(ResolvedPipBoyQuestsRuntime, questStateEntriesGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, questStateCapacityGlobal),
        offsetof(ResolvedPipBoyQuestsRuntime, questStateCapacityGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyQuestsProfile, getFixedStringCharacters),
        offsetof(ResolvedPipBoyQuestsRuntime, getFixedStringCharacters),
        platform::AddressCheck::Executable),
};

bool ResolvePipBoyQuestsRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyQuestsProfile& quests,
    ResolvedPipBoyQuestsRuntime& runtime) noexcept {
    runtime = {};
    if (virtualQuery == nullptr || moduleBase == 0 ||
        quests.objectiveStateEntrySize < kHandleBytes ||
        quests.objectiveStateChainOffset == 0 ||
        quests.objectiveStateChainOffset + sizeof(std::uint32_t) >
            quests.objectiveStateEntrySize ||
        quests.objectiveStateValueOffset == 0 ||
        quests.objectiveStateValueOffset + sizeof(std::uint32_t) >
            quests.objectiveStateEntrySize ||
        quests.questNameOffset == 0 ||
        quests.questTypeOffset == 0 ||
        quests.hiddenQuestType == 0 ||
        quests.hiddenQuestType > 0xFF ||
        quests.questFlagsOffset == 0 ||
        quests.objectiveFlagsOffset == 0 ||
        quests.questStateEntrySize < kHandleBytes ||
        quests.questStateChainOffset == 0 ||
        quests.questStateChainOffset + sizeof(std::uint32_t) >
            quests.questStateEntrySize ||
        quests.questStateFlagsOffset == 0 ||
        quests.questStateFlagsOffset + sizeof(std::uint64_t) >
            quests.questStateEntrySize ||
        quests.questSwfFileOffset == 0) {
        return false;
    }

    ResolvedPipBoyQuestsRuntime candidate{
        .layout = quests,
    };
    if (!platform::ResolveAddressBindings(
            virtualQuery,
            moduleBase,
            &quests,
            &candidate,
            kPipBoyQuestsRuntimeBindings)) {
        return false;
    }

    runtime = candidate;
    return true;
}

bool ReadPipBoyQuests(
    const ResolvedPipBoyQuestsRuntime& runtime,
    std::vector<PipBoyQuest>& quests) noexcept {
    quests.clear();
    const auto& layout = runtime.layout;
    HashMapView objectiveStates;
    HashMapView questStates;
    if (!ReadHashMapView(
            runtime.objectiveStateEntriesGlobal,
            runtime.objectiveStateCapacityGlobal,
            layout.objectiveStateEntrySize,
            objectiveStates) ||
        !ReadHashMapView(
            runtime.questStateEntriesGlobal,
            runtime.questStateCapacityGlobal,
            layout.questStateEntrySize,
            questStates)) {
        return false;
    }
    // The map is allocated with the first objective state, so a game without
    // one has no quests to list.
    if (objectiveStates.capacity == 0) {
        return true;
    }
    const auto entries = objectiveStates.entries;
    const auto capacity = objectiveStates.capacity;

    std::vector<PipBoyQuest> result;
    std::size_t objectiveCount{};
    for (std::uint64_t index = 0; index < capacity; ++index) {
        std::uintptr_t entry{};
        std::uint32_t chainIndex{};
        std::uint32_t state{};
        if (!platform::TryAddAddress(
                entries,
                static_cast<std::size_t>(index) * layout.objectiveStateEntrySize,
                entry) ||
            !ReadField(entry, layout.objectiveStateChainOffset, chainIndex) ||
            !ReadField(entry, layout.objectiveStateValueOffset, state)) {
            return false;
        }
        if (chainIndex == kEmptyChainIndex ||
            (state != kDisplayedState && state != kCompletedState &&
             state != kFailedState)) {
            continue;
        }

        QuestObjectiveHandle handle;
        std::uintptr_t quest{};
        std::uintptr_t objective{};
        if (!platform::TryCopyBytes(
                reinterpret_cast<const void*>(entry), &handle, kHandleBytes) ||
            !CallGetQuestFromHandle(runtime.getQuestFromHandle, handle, quest) ||
            !CallGetQuestObjectiveById(
                runtime.getQuestObjectiveById,
                quest,
                handle.objectiveId,
                objective)) {
            return false;
        }
        // An objective state can outlive its quest form or objective.
        if (quest == 0 || objective == 0) {
            continue;
        }

        const bool completed = state == kCompletedState;
        bool hidden{};
        if (!IsHiddenOpenObjective(layout, quest, objective, hidden)) {
            return false;
        }
        if (hidden && !completed) {
            continue;
        }

        PipBoyQuestObjective objectiveEntry;
        std::uint32_t objectiveFlags{};
        objectiveEntry.index = handle.objectiveId;
        objectiveEntry.completed = completed;
        objectiveEntry.failed = state == kFailedState;
        if (++objectiveCount > kMaximumObjectives ||
            !ReadField(objective, layout.objectiveFlagsOffset, objectiveFlags) ||
            !ReadObjectiveText(runtime, objective, handle, objectiveEntry.text)) {
            return false;
        }
        objectiveEntry.orWithPrevious =
            (objectiveFlags & kOrWithPreviousObjectiveFlag) != 0;

        auto* const questEntry =
            FindOrAddQuest(runtime, questStates, result, handle, quest);
        if (questEntry == nullptr) {
            return false;
        }
        try {
            questEntry->objectives.push_back(std::move(objectiveEntry));
        } catch (...) {
            return false;
        }
    }

    for (auto& quest : result) {
        std::sort(
            quest.objectives.begin(),
            quest.objectives.end(),
            [](const PipBoyQuestObjective& left, const PipBoyQuestObjective& right) {
                return left.index < right.index;
            });
    }
    quests = std::move(result);
    return true;
}

bool BuildPipBoyQuestsJson(
    const ResolvedPipBoyQuestsRuntime& runtime,
    std::string& json) noexcept {
    json.clear();
    std::vector<PipBoyQuest> quests;
    if (!ReadPipBoyQuests(runtime, quests)) {
        return false;
    }

    try {
        json += R"({"quests":[)";
        for (std::size_t questIndex = 0; questIndex < quests.size(); ++questIndex) {
            const auto& quest = quests[questIndex];
            if (questIndex != 0) {
                json.push_back(',');
            }
            json += R"({"name":)";
            if (!json::AppendString(quest.name, json, kMaximumQuestsJsonBytes)) {
                json.clear();
                return false;
            }
            json += R"(,"formId":)";
            if (!json::AppendInteger(quest.formId, json, kMaximumQuestsJsonBytes)) {
                json.clear();
                return false;
            }
            json += R"(,"type":)";
            if (!json::AppendInteger(quest.type, json, kMaximumQuestsJsonBytes)) {
                json.clear();
                return false;
            }
            json += R"(,"swfFile":)";
            if (!json::AppendString(quest.swfFile, json, kMaximumQuestsJsonBytes)) {
                json.clear();
                return false;
            }
            json += quest.active ? R"(,"active":true)" : R"(,"active":false)";
            json += quest.enabled ? R"(,"enabled":true)" : R"(,"enabled":false)";
            json += quest.completed ? R"(,"completed":true)"
                                    : R"(,"completed":false)";
            json += quest.canBeRejected ? R"(,"canBeRejected":true)"
                                        : R"(,"canBeRejected":false)";
            json += R"(,"objectives":[)";
            for (std::size_t index = 0; index < quest.objectives.size(); ++index) {
                const auto& objective = quest.objectives[index];
                if (index != 0) {
                    json.push_back(',');
                }
                json += R"({"index":)";
                if (!json::AppendInteger(
                        objective.index, json, kMaximumQuestsJsonBytes)) {
                    json.clear();
                    return false;
                }
                json += R"(,"text":)";
                if (!json::AppendString(
                        objective.text, json, kMaximumQuestsJsonBytes)) {
                    json.clear();
                    return false;
                }
                json += objective.completed ? R"(,"completed":true)"
                                            : R"(,"completed":false)";
                json += objective.failed ? R"(,"failed":true)"
                                         : R"(,"failed":false)";
                json += objective.orWithPrevious ? R"(,"orWithPrevious":true})"
                                                 : R"(,"orWithPrevious":false})";
            }
            json += "]}";
        }
        json += "]}";
    } catch (...) {
        json.clear();
        return false;
    }
    if (json.size() > kMaximumQuestsJsonBytes) {
        json.clear();
        return false;
    }
    return true;
}

}
