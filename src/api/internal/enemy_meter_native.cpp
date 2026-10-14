#include "api/internal/enemy_meter_data.hpp"

#include "platform/diagnostics.hpp"
#include "platform/native_access.hpp"
#include "platform/windows_memory.hpp"

#include <Windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sf::enemy_meter {
namespace {

struct VatsArrayView final {
    std::uintptr_t allocator{};
    std::uintptr_t data{};
    std::uint32_t capacity{};
    std::uint32_t capacityPad{};
    std::uint32_t size{};
    std::uint32_t sizePad{};
};
static_assert(offsetof(VatsArrayView, data) == 0x08);
static_assert(offsetof(VatsArrayView, size) == 0x18);
static_assert(sizeof(VatsArrayView) == 0x20);

struct VatsResistanceEntry final {
    float value{};
    bool immune{};
    std::array<std::byte, 3> pad{};
};
static_assert(sizeof(VatsResistanceEntry) == 0x08);

struct VatsCollectorContext final {
    VatsArrayView* damageTypes{};
    VatsArrayView* resistances{};
    std::uintptr_t* target{};
};
static_assert(sizeof(VatsCollectorContext) == 0x18);

// Layout of the stock reference-control object and the form that the target
// handle resolves to. These values are fixed for the supported executable
// build and are not part of config::NativeEnemyMeterProfile.
struct NativeObjectLayout final {
    std::size_t referenceControlOffset{0x28};
    std::size_t referenceCountOffset{0x30};
    std::size_t formTypeOffset{0x26};
    std::uint8_t actorFormTypeA{0x55};
    std::uint8_t actorFormTypeB{0xB9};
};

constexpr NativeObjectLayout kObjectLayout{};

struct NativeRuntime final {
    config::NativeEnemyMeterProfile profile{};
    std::uintptr_t targetEventSourceGlobal{};
    std::uintptr_t targetModelVtable{};
    std::uintptr_t resolver{};
    std::uintptr_t vatsCollectResistances{};
    std::uintptr_t referenceControlVtable{};
    std::uintptr_t referenceDestroy{};
    bool active{};
};

NativeRuntime runtime;
std::mutex runtimeMutex;

constexpr std::uint32_t kMaxTargetEventSinks = 32;

struct EventSinkArray final {
    std::uintptr_t data;
    std::uint32_t capacity;
    std::uint32_t pad;
    std::uint32_t size;
    std::uint32_t pad2;
};
static_assert(sizeof(EventSinkArray) == 0x18);

struct TargetModelHandle final {
    std::uint32_t value;
    std::uint8_t valid;
};
static_assert(sizeof(TargetModelHandle) == 0x08);


[[nodiscard]] bool ReadPointer(
    std::uintptr_t address,
    std::uintptr_t& value) noexcept {
    value = 0;
    return address != 0 &&
        platform::TryCopyBytes(
            reinterpret_cast<const void*>(address),
            &value,
            sizeof(value));
}

[[nodiscard]] bool TryGetTargetHandle(
    const NativeRuntime& state,
    std::uint32_t& handle) noexcept {
    handle = 0;
    std::uintptr_t eventSource{};
    if (!ReadPointer(
            state.targetEventSourceGlobal, eventSource)) {
        return false;
    }

    EventSinkArray sinks{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(
                eventSource +
                    state.profile.targetEventSourceSinksOffset),
            &sinks,
            sizeof(sinks)) ||
        sinks.data == 0 ||
        sinks.size == 0 ||
        sinks.size > sinks.capacity ||
        sinks.size > kMaxTargetEventSinks) {
        return false;
    }

    for (std::uint32_t index = 0; index < sinks.size; ++index) {
        std::uintptr_t candidate{};
        std::uintptr_t vtable{};
        if (!ReadPointer(
                sinks.data +
                    index * sizeof(std::uintptr_t),
                candidate) ||
            !ReadPointer(candidate, vtable) ||
            vtable != state.targetModelVtable) {
            continue;
        }
        TargetModelHandle modelHandle{};
        if (!platform::TryCopyBytes(
                reinterpret_cast<const void*>(
                    candidate +
                        state.profile.targetModelHandleOffset),
                &modelHandle,
                sizeof(modelHandle)) ||
            modelHandle.valid == 0 ||
            modelHandle.value == 0) {
            continue;
        }
        handle = modelHandle.value;
        return true;
    }

    return false;
}

using ResolveObjectHandle = void*(__fastcall*)(
    std::uintptr_t* result,
    const std::uint32_t* handle);
using CollectVatsResistances = void(__fastcall*)(
    VatsCollectorContext* context);
using DestroyReference = void(__fastcall*)(void* referenceControl);

void ReleaseTarget(
    NativeRuntime& state,
    std::uintptr_t target) noexcept;

[[nodiscard]] int MemoryAccessExceptionFilter(
    unsigned long exceptionCode) noexcept {
    return exceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            exceptionCode == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH;
}

// Turns the native reads off after a fault in a game function. Without this,
// every frame would call the same faulting function again.
void DisableAfterFault(NativeRuntime& state, const char* site) noexcept {
    if (!state.active) {
        return;
    }
    state.active = false;
    DiagnosticLogFormat(
        "xScal enemy meter: access violation in native %s, reads disabled",
        site);
}

[[nodiscard]] bool TryResolveTarget(
    NativeRuntime& state,
    std::uint32_t handle,
    std::uintptr_t& target) noexcept {
    target = 0;
    __try {
        reinterpret_cast<ResolveObjectHandle>(state.resolver)(
            &target, &handle);
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        DisableAfterFault(state, "object handle resolver");
        target = 0;
        return false;
    }
    return target != 0;
}

[[nodiscard]] bool ReadStockVatsResistances(
    NativeRuntime& state,
    std::uintptr_t target,
    Resistances& values) noexcept {
    std::array<std::uint32_t, 6> damageTypes{1, 2, 3, 4, 5, 6};
    std::array<VatsResistanceEntry, 10> entries{};
    VatsArrayView damageTypeView{
        0,
        reinterpret_cast<std::uintptr_t>(damageTypes.data()),
        static_cast<std::uint32_t>(damageTypes.size()),
        0,
        static_cast<std::uint32_t>(damageTypes.size()),
        0};
    VatsArrayView resistanceView{
        0,
        reinterpret_cast<std::uintptr_t>(entries.data()),
        static_cast<std::uint32_t>(entries.size()),
        0,
        static_cast<std::uint32_t>(entries.size()),
        0};
    auto targetSlot = target;
    VatsCollectorContext context{
        &damageTypeView, &resistanceView, &targetSlot};

    __try {
        reinterpret_cast<CollectVatsResistances>(
            state.vatsCollectResistances)(&context);
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        DisableAfterFault(state, "VATS resistance collector");
        return false;
    }

    // Entries are indexed by stock damage type: 1 damage, 2 poison, 3 fire,
    // 4 energy, 5 cryo, 6 radiation. Resistances stores damage, energy,
    // radiation, fire, poison, cryo.
    constexpr std::array<std::size_t, 6> kEntryIndices{1, 4, 6, 3, 2, 5};
    for (const std::size_t entryIndex : kEntryIndices) {
        if (!std::isfinite(entries[entryIndex].value)) {
            return false;
        }
    }

    // Match the stock VATS producer's CVTTSS2SI display conversion.
    for (std::size_t index = 0; index < kEntryIndices.size(); ++index) {
        const auto& entry = entries[kEntryIndices[index]];
        values.values[index] = std::trunc(entry.value);
        values.immune[index] = entry.immune;
    }

    return true;
}

// Always decrements the reference count. The destroy function runs only when
// this call drops the last reference. If its address is not executable, the
// destroy call is skipped and the object is not freed.
void ReleaseTarget(
    NativeRuntime& state,
    std::uintptr_t target) noexcept {
    __try {
        const auto control = target + kObjectLayout.referenceControlOffset;
        const auto vtable =
            *reinterpret_cast<const std::uintptr_t*>(control);
        const auto destroy =
            *reinterpret_cast<const std::uintptr_t*>(
                vtable + sizeof(std::uintptr_t));
        const auto previousCount = InterlockedExchangeAdd(
            reinterpret_cast<volatile LONG*>(
                target + kObjectLayout.referenceCountOffset),
            -1);
        if (previousCount != 1) {
            return;
        }

        const bool destroyCached =
            vtable == state.referenceControlVtable &&
            destroy == state.referenceDestroy;
        if (!destroyCached) {
            if (!platform::IsExecutableAddress(
                    &::VirtualQuery,
                    reinterpret_cast<const void*>(destroy))) {
                return;
            }
            state.referenceControlVtable = vtable;
            state.referenceDestroy = destroy;
        }
        reinterpret_cast<DestroyReference>(destroy)(
            reinterpret_cast<void*>(control));
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        DisableAfterFault(state, "reference release");
    }
}

[[nodiscard]] bool ReadValues(
    NativeRuntime& state,
    Resistances& values) noexcept {
    std::uint32_t handle{};
    if (!TryGetTargetHandle(state, handle)) {
        return false;
    }

    std::uintptr_t target{};
    if (!TryResolveTarget(state, handle, target)) {
        return false;
    }

    std::uint8_t formType{};
    const bool actor =
        platform::TryCopyBytes(
            reinterpret_cast<const void*>(
                target + kObjectLayout.formTypeOffset),
            &formType,
            sizeof(formType)) &&
        (formType == kObjectLayout.actorFormTypeA ||
         formType == kObjectLayout.actorFormTypeB);

    if (!actor) {
        ReleaseTarget(state, target);
        return false;
    }

    const bool success = ReadStockVatsResistances(state, target, values);
    ReleaseTarget(state, target);
    return success;
}

}  // namespace

void ConfigureNativeRuntime(
    const config::NativeEnemyMeterProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    std::scoped_lock lock{runtimeMutex};
    runtime = {};
    runtime.profile = profile;
    if (!platform::ResolveRequiredRvas(moduleBase, {
            {profile.targetEventSourceGlobal,
             &runtime.targetEventSourceGlobal},
            {profile.targetModelVtable, &runtime.targetModelVtable},
            {profile.resolveObjectHandle, &runtime.resolver},
            {profile.vatsCollectResistances,
             &runtime.vatsCollectResistances}})) {
        runtime = {};
        return;
    }
    runtime.active = platform::AreExecutableAddresses(
        &::VirtualQuery,
        {runtime.vatsCollectResistances, runtime.resolver});
}

void ResetNativeRuntime() noexcept {
    std::scoped_lock lock{runtimeMutex};
    runtime = {};
}

bool ReadNativeResistances(
    std::optional<Resistances>& resistances) noexcept {
    resistances.reset();
    std::scoped_lock lock{runtimeMutex};
    if (!runtime.active) {
        return false;
    }
    Resistances values{};
    if (!ReadValues(runtime, values)) {
        return false;
    }
    resistances = values;
    return true;
}

}  // namespace sf::enemy_meter
