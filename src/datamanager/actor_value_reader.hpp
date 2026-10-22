#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstddef>
#include <cstdint>

namespace sf::datamanager {

struct ResolvedActorValueReader final {
    config::NativeActorValueProfile layout{};
    std::uintptr_t localPlayerGlobal{};
    std::uintptr_t localPlayerPrimaryVtable{};
    std::uintptr_t actorValueOwnerVtable{};
    std::uintptr_t getCurrentActorValue{};
    std::uintptr_t actorValueRegistry{};
    std::uintptr_t actorValueInfoVtable{};
};

[[nodiscard]] bool ResolveActorValueReader(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActorValueProfile& profile,
    ResolvedActorValueReader& reader) noexcept;

// Reads the local player's ActorValueOwner subobject after checking the
// player's primary vtable and the subobject's vtable.
[[nodiscard]] bool ReadLocalPlayerActorValueOwner(
    const ResolvedActorValueReader& reader,
    std::uintptr_t& actorValueOwner) noexcept;

// Reads one ActorValue registry slot and accepts it only when it holds an
// ActorValueInfo with the expected form ID.
[[nodiscard]] bool ReadRegisteredActorValueInfo(
    const ResolvedActorValueReader& reader,
    std::size_t registryOffset,
    std::uint32_t formId,
    std::uintptr_t& actorValueInfo) noexcept;

// Calls the owner's current-value getter: base value plus all modifiers,
// clamped to the ActorValueInfo limits. The getter uses game thread-local
// state, so only call this from a game thread.
[[nodiscard]] bool ReadCurrentActorValue(
    const ResolvedActorValueReader& reader,
    std::uintptr_t actorValueOwner,
    std::uintptr_t actorValueInfo,
    float& value) noexcept;

}
