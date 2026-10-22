#include "datamanager/pip_boy_player_info_snapshot.hpp"

#include "common/json.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sf::datamanager {
namespace {

// Fallout76.esm form IDs of the PlayerLevel and Experience ActorValueInfo
// forms (0x2BC + ActorValue index 112 and 13).
constexpr std::uint32_t kPlayerLevelFormId = 0x32C;
constexpr std::uint32_t kExperienceFormId = 0x2C9;
// Rejects a level that would make the experience curve lookup grow its cache
// far beyond any reachable level.
constexpr std::uint32_t kMaximumPlayerLevel = 10000;
constexpr std::size_t kMaximumNameLength = 256;
constexpr std::size_t kMaximumPlayerInfoJsonBytes = 1024;

using GetTotalExperienceForLevel = float(__fastcall*)(std::uint32_t level);
using GetDisplayName = const char*(__fastcall*)(const void* form);

[[nodiscard]] bool CallGetTotalExperienceForLevel(
    std::uintptr_t function,
    std::uint32_t level,
    float& experience) noexcept {
    __try {
        experience =
            reinterpret_cast<GetTotalExperienceForLevel>(function)(level);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        experience = 0.0F;
        return false;
    }
    return std::isfinite(experience) && experience >= 0.0F;
}

[[nodiscard]] bool CallGetDisplayName(
    std::uintptr_t function,
    std::uintptr_t player,
    const char*& name) noexcept {
    __try {
        name = reinterpret_cast<GetDisplayName>(function)(
            reinterpret_cast<const void*>(player));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        name = nullptr;
        return false;
    }
    return name != nullptr;
}

[[nodiscard]] bool ReadCurrentRegisteredValue(
    const ResolvedActorValueReader& reader,
    std::uintptr_t owner,
    std::size_t registryOffset,
    std::uint32_t formId,
    float& value) noexcept {
    std::uintptr_t actorValueInfo{};
    return ReadRegisteredActorValueInfo(
               reader, registryOffset, formId, actorValueInfo) &&
        ReadCurrentActorValue(reader, owner, actorValueInfo, value);
}

}

bool ResolvePipBoyPlayerInfoRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActorValueProfile& actorValues,
    const config::NativePipBoyPlayerInfoProfile& playerInfo,
    ResolvedPipBoyPlayerInfoRuntime& runtime) noexcept {
    runtime = {};
    if (playerInfo.getTotalExperienceForLevel == 0 ||
        playerInfo.playerLevelRegistryOffset == 0 ||
        playerInfo.experienceRegistryOffset == 0 ||
        playerInfo.getDisplayNameVtableOffset == 0) {
        return false;
    }

    ResolvedPipBoyPlayerInfoRuntime candidate{
        .layout = playerInfo,
    };
    if (!ResolveActorValueReader(
            virtualQuery, moduleBase, actorValues, candidate.reader) ||
        !platform::ResolveRequiredRvas(moduleBase, {
            {playerInfo.getTotalExperienceForLevel,
             &candidate.getTotalExperienceForLevel}})) {
        return false;
    }

    // The display-name getter is taken from the exact player vtable, which
    // ReadLocalPlayer checks on every request.
    std::uintptr_t displayNameSlot{};
    std::uintptr_t levelSlot{};
    std::uintptr_t experienceSlot{};
    if (!platform::TryAddAddress(
            candidate.reader.localPlayerPrimaryVtable,
            playerInfo.getDisplayNameVtableOffset,
            displayNameSlot) ||
        !platform::ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(displayNameSlot),
            &candidate.getDisplayName,
            sizeof(candidate.getDisplayName)) ||
        !platform::AreExecutableAddresses(
            virtualQuery,
            {candidate.getDisplayName,
             candidate.getTotalExperienceForLevel}) ||
        !platform::TryAddAddress(
            candidate.reader.actorValueRegistry,
            playerInfo.playerLevelRegistryOffset,
            levelSlot) ||
        !platform::TryAddAddress(
            candidate.reader.actorValueRegistry,
            playerInfo.experienceRegistryOffset,
            experienceSlot) ||
        !platform::AreReadableAddresses(
            virtualQuery, {levelSlot, experienceSlot})) {
        return false;
    }

    runtime = candidate;
    return true;
}

bool ReadPipBoyPlayerInfo(
    const ResolvedPipBoyPlayerInfoRuntime& runtime,
    PipBoyPlayerInfo& info) noexcept {
    info = {};
    std::uintptr_t owner{};
    float levelValue{};
    float experience{};
    if (!ReadLocalPlayerActorValueOwner(runtime.reader, owner) ||
        !ReadCurrentRegisteredValue(
            runtime.reader,
            owner,
            runtime.layout.playerLevelRegistryOffset,
            kPlayerLevelFormId,
            levelValue) ||
        !ReadCurrentRegisteredValue(
            runtime.reader,
            owner,
            runtime.layout.experienceRegistryOffset,
            kExperienceFormId,
            experience) ||
        levelValue < 1.0F ||
        levelValue >= static_cast<float>(kMaximumPlayerLevel) ||
        experience < 0.0F) {
        return false;
    }

    // GetPlayerCharacterLevel truncates the PlayerLevel value the same way.
    const auto level = static_cast<std::uint32_t>(levelValue);
    float levelStart{};
    float levelEnd{};
    if (!CallGetTotalExperienceForLevel(
            runtime.getTotalExperienceForLevel, level, levelStart) ||
        !CallGetTotalExperienceForLevel(
            runtime.getTotalExperienceForLevel, level + 1, levelEnd)) {
        return false;
    }

    PipBoyPlayerInfo result;
    result.level = level;
    result.totalExperience = experience;
    result.levelExperience = static_cast<double>(experience) - levelStart;
    result.levelExperienceSpan = static_cast<double>(levelEnd) - levelStart;
    // Same rule as the HUD experience bar: zero for an empty span, otherwise
    // the fraction clamped to 0..1.
    if (result.levelExperienceSpan > 0.0) {
        result.levelProgress = static_cast<float>(std::clamp(
            result.levelExperience / result.levelExperienceSpan, 0.0, 1.0));
    }

    // The character name is optional: a missing or unreadable name leaves the
    // level and experience values usable.
    const auto player = owner - runtime.reader.layout.actorValueOwnerOffset;
    const char* name{};
    if (CallGetDisplayName(runtime.getDisplayName, player, name) &&
        !platform::CopyBoundedCString(
            name, kMaximumNameLength, true, result.name)) {
        result.name.clear();
    }

    try {
        info = std::move(result);
    } catch (...) {
        info = {};
        return false;
    }
    return true;
}

bool BuildPipBoyPlayerInfoJson(
    const ResolvedPipBoyPlayerInfoRuntime& runtime,
    std::string& json) noexcept {
    json.clear();
    PipBoyPlayerInfo info;
    if (!ReadPipBoyPlayerInfo(runtime, info)) {
        return false;
    }

    const auto appendInteger = [&json](double value) {
        return json::AppendInteger(
            static_cast<std::int64_t>(std::trunc(value)),
            json,
            kMaximumPlayerInfoJsonBytes);
    };
    try {
        json.reserve(160);
        json += R"({"name":)";
        if (!json::AppendString(info.name, json, kMaximumPlayerInfoJsonBytes)) {
            json.clear();
            return false;
        }
        json += R"(,"level":)";
        if (!appendInteger(info.level)) {
            json.clear();
            return false;
        }
        json += R"(,"xp":{"total":)";
        if (!appendInteger(info.totalExperience)) {
            json.clear();
            return false;
        }
        json += R"(,"current":)";
        if (!appendInteger(info.levelExperience)) {
            json.clear();
            return false;
        }
        json += R"(,"next":)";
        if (!appendInteger(info.levelExperienceSpan)) {
            json.clear();
            return false;
        }
        json += R"(},"progress":)";
        if (!json::AppendFloat(info.levelProgress, json)) {
            json.clear();
            return false;
        }
        json.push_back('}');
    } catch (...) {
        json.clear();
        return false;
    }
    if (json.size() > kMaximumPlayerInfoJsonBytes) {
        json.clear();
        return false;
    }
    return true;
}

}
