#include "datamanager/actor_value_reader.hpp"

#include "datamanager/local_player.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <cmath>

namespace sf::datamanager {
namespace {

using GetCurrentActorValue = float(__fastcall*)(
    void* actorValueOwner,
    void* actorValueInfo);

[[nodiscard]] bool HasCompleteActorValueProfile(
    const config::NativeActorValueProfile& profile) noexcept {
    return profile.localPlayerGlobal != 0 &&
        profile.localPlayerPrimaryVtable != 0 &&
        profile.localPlayerActorValueOwnerVtable != 0 &&
        profile.actorValueRegistry != 0 &&
        profile.actorValueInfoVtable != 0 &&
        profile.actorValueOwnerOffset != 0 &&
        profile.getCurrentActorValueVtableOffset != 0 &&
        profile.formIdOffset != 0;
}

[[nodiscard]] bool ReadPointer(
    std::uintptr_t address,
    std::uintptr_t& value) noexcept {
    value = 0;
    return platform::TryCopyBytes(
               reinterpret_cast<const void*>(address),
               &value,
               sizeof(value)) &&
        value != 0 &&
        value % alignof(std::uintptr_t) == 0;
}

[[nodiscard]] bool CallGetCurrentActorValue(
    std::uintptr_t getter,
    std::uintptr_t actorValueOwner,
    std::uintptr_t actorValueInfo,
    float& value) noexcept {
    __try {
        value = reinterpret_cast<GetCurrentActorValue>(getter)(
            reinterpret_cast<void*>(actorValueOwner),
            reinterpret_cast<void*>(actorValueInfo));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        value = 0.0F;
        return false;
    }
    return true;
}

}

bool ResolveActorValueReader(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActorValueProfile& profile,
    ResolvedActorValueReader& reader) noexcept {
    reader = {};
    if (virtualQuery == nullptr || moduleBase == 0 ||
        !HasCompleteActorValueProfile(profile) ||
        !platform::ValidateVtables(
            virtualQuery,
            moduleBase,
            {profile.localPlayerPrimaryVtable,
             profile.localPlayerActorValueOwnerVtable,
             profile.actorValueInfoVtable})) {
        return false;
    }

    ResolvedActorValueReader candidate{
        .layout = profile,
    };
    if (!platform::ResolveRequiredRvas(moduleBase, {
            {profile.localPlayerGlobal, &candidate.localPlayerGlobal},
            {profile.localPlayerPrimaryVtable,
             &candidate.localPlayerPrimaryVtable},
            {profile.localPlayerActorValueOwnerVtable,
             &candidate.actorValueOwnerVtable},
            {profile.actorValueRegistry, &candidate.actorValueRegistry},
            {profile.actorValueInfoVtable,
             &candidate.actorValueInfoVtable}})) {
        return false;
    }

    // The getter is resolved from the exact owner vtable once. Requests call
    // it only after the live owner's vtable matches the same address.
    std::uintptr_t getterSlot{};
    if (!platform::TryAddAddress(
            candidate.actorValueOwnerVtable,
            profile.getCurrentActorValueVtableOffset,
            getterSlot) ||
        !platform::ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(getterSlot),
            &candidate.getCurrentActorValue,
            sizeof(candidate.getCurrentActorValue)) ||
        !platform::AreExecutableAddresses(
            virtualQuery, {candidate.getCurrentActorValue}) ||
        !platform::AreReadableAddresses(
            virtualQuery,
            {candidate.localPlayerGlobal, candidate.actorValueRegistry})) {
        return false;
    }

    reader = candidate;
    return true;
}

bool ReadLocalPlayerActorValueOwner(
    const ResolvedActorValueReader& reader,
    std::uintptr_t& actorValueOwner) noexcept {
    actorValueOwner = 0;
    std::uintptr_t player{};
    std::uintptr_t owner{};
    std::uintptr_t ownerVtable{};
    if (!ReadLocalPlayer(
            reader.localPlayerGlobal,
            reader.localPlayerPrimaryVtable,
            player) ||
        !platform::TryAddAddress(
            player, reader.layout.actorValueOwnerOffset, owner) ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(owner),
            &ownerVtable,
            sizeof(ownerVtable)) ||
        ownerVtable != reader.actorValueOwnerVtable) {
        return false;
    }

    actorValueOwner = owner;
    return true;
}

bool ReadRegisteredActorValueInfo(
    const ResolvedActorValueReader& reader,
    std::size_t registryOffset,
    std::uint32_t formId,
    std::uintptr_t& actorValueInfo) noexcept {
    actorValueInfo = 0;
    if (reader.actorValueRegistry == 0) {
        return false;
    }

    std::uintptr_t slot{};
    std::uintptr_t info{};
    std::uintptr_t infoVtable{};
    std::uintptr_t formIdAddress{};
    std::uint32_t actualFormId{};
    if (!platform::TryAddAddress(
            reader.actorValueRegistry, registryOffset, slot) ||
        !ReadPointer(slot, info) ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(info),
            &infoVtable,
            sizeof(infoVtable)) ||
        infoVtable != reader.actorValueInfoVtable ||
        !platform::TryAddAddress(
            info, reader.layout.formIdOffset, formIdAddress) ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(formIdAddress),
            &actualFormId,
            sizeof(actualFormId)) ||
        actualFormId != formId) {
        return false;
    }

    actorValueInfo = info;
    return true;
}

bool ReadCurrentActorValue(
    const ResolvedActorValueReader& reader,
    std::uintptr_t actorValueOwner,
    std::uintptr_t actorValueInfo,
    float& value) noexcept {
    value = 0.0F;
    if (reader.getCurrentActorValue == 0 || actorValueOwner == 0 ||
        actorValueInfo == 0) {
        return false;
    }

    float result{};
    if (!CallGetCurrentActorValue(
            reader.getCurrentActorValue,
            actorValueOwner,
            actorValueInfo,
            result) ||
        !std::isfinite(result)) {
        return false;
    }
    value = result;
    return true;
}

}
