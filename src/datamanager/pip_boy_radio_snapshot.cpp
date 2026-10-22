#include "datamanager/pip_boy_radio_snapshot.hpp"

#include "common/json.hpp"
#include "datamanager/game_read_lock.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace sf::datamanager {
namespace {

constexpr std::uint32_t kEmptyChainIndex = 0xFFFFFFFF;
// The tuning event handler marks a station active when the radio is on and
// |station - tuned| <= 0.0001 (rva 0x4110100).
constexpr float kTunedFrequencyEpsilon = 0.0001F;
constexpr std::uint64_t kMaximumMapCapacity = 1ULL << 16;
constexpr std::size_t kMaximumStations = 1024;
constexpr std::size_t kMaximumNameLength = 256;
constexpr std::size_t kMaximumRadioJsonBytes = 256 * 1024;

using GetFixedStringCharacters =
    const char*(__fastcall*)(const void* fixedString);

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

[[nodiscard]] bool ReadTuner(
    const ResolvedPipBoyRadioRuntime& runtime,
    PipBoyRadio& radio) noexcept {
    const auto& layout = runtime.layout;
    std::uintptr_t manager{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.radioManagerGlobal),
            &manager,
            sizeof(manager))) {
        return false;
    }
    if (manager == 0) {
        return true;
    }
    std::uint8_t powered{};
    if (!ReadField(manager, layout.radioTunedFrequencyOffset, radio.tunedFrequency) ||
        !std::isfinite(radio.tunedFrequency) ||
        !ReadField(manager, layout.radioPoweredOffset, powered)) {
        return false;
    }
    radio.hasTuner = true;
    radio.powered = powered != 0;
    return true;
}

// Reads the stations while the caller holds the radio read lock.
[[nodiscard]] bool ReadLockedStations(
    const ResolvedPipBoyRadioRuntime& runtime,
    PipBoyRadio& radio) {
    const auto& layout = runtime.layout;
    std::uintptr_t entries{};
    std::uint64_t capacity{};
    if (!ReadField(runtime.radioStationMap, layout.radioStationMapEntriesOffset, entries) ||
        !ReadField(runtime.radioStationMap, layout.radioStationMapCapacityOffset, capacity)) {
        return false;
    }
    // The map is allocated with the first station.
    if (capacity == 0) {
        return true;
    }
    if (entries == 0 || capacity > kMaximumMapCapacity ||
        (capacity & (capacity - 1)) != 0) {
        return false;
    }

    for (std::uint64_t index = 0; index < capacity; ++index) {
        std::uintptr_t entry{};
        std::uint32_t chainIndex{};
        if (!platform::TryAddAddress(
                entries,
                static_cast<std::size_t>(index) * layout.radioStationEntrySize,
                entry) ||
            !ReadField(entry, layout.radioStationChainOffset, chainIndex)) {
            return false;
        }
        if (chainIndex == kEmptyChainIndex) {
            continue;
        }

        PipBoyRadioStation station;
        std::uintptr_t nameString{};
        const char* name{};
        if (radio.stations.size() >= kMaximumStations ||
            !ReadField(entry, 0, station.frequency) ||
            !std::isfinite(station.frequency) ||
            !platform::TryAddAddress(entry, layout.radioStationNameOffset, nameString) ||
            !CallGetFixedStringCharacters(
                runtime.getFixedStringCharacters, nameString, name) ||
            !platform::CopyBoundedCString(
                name, kMaximumNameLength, true, station.name)) {
            return false;
        }
        station.active = radio.powered &&
            std::fabs(station.frequency - radio.tunedFrequency) <=
                kTunedFrequencyEpsilon;
        radio.stations.push_back(std::move(station));
    }
    return true;
}

}

constexpr std::array kPipBoyRadioRuntimeBindings{
    platform::Bind(
        offsetof(config::NativePipBoyRadioProfile, radioLock),
        offsetof(ResolvedPipBoyRadioRuntime, radioLock),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyRadioProfile, radioStationMap),
        offsetof(ResolvedPipBoyRadioRuntime, radioStationMap),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyRadioProfile, radioManagerGlobal),
        offsetof(ResolvedPipBoyRadioRuntime, radioManagerGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyRadioProfile, tryAcquireReadLock),
        offsetof(ResolvedPipBoyRadioRuntime, tryAcquireReadLock),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyRadioProfile, releaseReadLock),
        offsetof(ResolvedPipBoyRadioRuntime, releaseReadLock),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyRadioProfile, getFixedStringCharacters),
        offsetof(ResolvedPipBoyRadioRuntime, getFixedStringCharacters),
        platform::AddressCheck::Executable),
};

bool ResolvePipBoyRadioRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyRadioProfile& radio,
    ResolvedPipBoyRadioRuntime& runtime) noexcept {
    runtime = {};
    if (virtualQuery == nullptr || moduleBase == 0 ||
        radio.radioStationMapEntriesOffset == 0 ||
        radio.radioStationMapCapacityOffset == 0 ||
        radio.radioStationEntrySize < sizeof(float) ||
        radio.radioStationNameOffset == 0 ||
        radio.radioStationNameOffset + sizeof(void*) > radio.radioStationEntrySize ||
        radio.radioStationChainOffset == 0 ||
        radio.radioStationChainOffset + sizeof(std::uint32_t) >
            radio.radioStationEntrySize ||
        radio.radioTunedFrequencyOffset == 0 ||
        radio.radioPoweredOffset == 0) {
        return false;
    }

    ResolvedPipBoyRadioRuntime candidate{
        .layout = radio,
    };
    if (!platform::ResolveAddressBindings(
            virtualQuery,
            moduleBase,
            &radio,
            &candidate,
            kPipBoyRadioRuntimeBindings)) {
        return false;
    }

    runtime = candidate;
    return true;
}

PipBoyRadioCaptureResult ReadPipBoyRadio(
    const ResolvedPipBoyRadioRuntime& runtime,
    PipBoyRadio& radio) noexcept {
    radio = {};
    bool acquired{};
    if (!TryAcquireGameReadLock(
            runtime.tryAcquireReadLock, runtime.radioLock, acquired)) {
        return PipBoyRadioCaptureResult::Unavailable;
    }
    // Waiting for a writer on the game thread could stall the frame, so a
    // busy lock is reported instead.
    if (!acquired) {
        return PipBoyRadioCaptureResult::LockBusy;
    }

    PipBoyRadio result;
    {
        // The game's radio getters read the tuner and the station map under
        // this lock.
        const GameReadLockGuard readLock{
            runtime.releaseReadLock, runtime.radioLock};
        bool read{};
        try {
            read = ReadTuner(runtime, result) &&
                ReadLockedStations(runtime, result);
        } catch (...) {
            read = false;
        }
        if (!read) {
            return PipBoyRadioCaptureResult::Unavailable;
        }
    }

    std::sort(
        result.stations.begin(),
        result.stations.end(),
        [](const PipBoyRadioStation& left, const PipBoyRadioStation& right) {
            return left.frequency < right.frequency;
        });
    radio = std::move(result);
    return PipBoyRadioCaptureResult::Captured;
}

PipBoyRadioCaptureResult BuildPipBoyRadioJson(
    const ResolvedPipBoyRadioRuntime& runtime,
    std::string& json) noexcept {
    json.clear();
    PipBoyRadio radio;
    const auto capture = ReadPipBoyRadio(runtime, radio);
    if (capture != PipBoyRadioCaptureResult::Captured) {
        return capture;
    }

    try {
        json.reserve(64 + radio.stations.size() * 96);
        if (radio.hasTuner) {
            json += radio.powered ? R"({"powered":true,"frequency":)"
                                  : R"({"powered":false,"frequency":)";
            if (!json::AppendFloat(radio.tunedFrequency, json)) {
                json.clear();
                return PipBoyRadioCaptureResult::Unavailable;
            }
        } else {
            json += R"({"powered":false,"frequency":null)";
        }
        json += R"(,"stations":[)";
        for (std::size_t index = 0; index < radio.stations.size(); ++index) {
            const auto& station = radio.stations[index];
            if (index != 0) {
                json.push_back(',');
            }
            json += R"({"name":)";
            if (!json::AppendString(station.name, json, kMaximumRadioJsonBytes)) {
                json.clear();
                return PipBoyRadioCaptureResult::Unavailable;
            }
            json += R"(,"frequency":)";
            if (!json::AppendFloat(station.frequency, json)) {
                json.clear();
                return PipBoyRadioCaptureResult::Unavailable;
            }
            json += station.active ? R"(,"active":true})" : R"(,"active":false})";
        }
        json += "]}";
    } catch (...) {
        json.clear();
        return PipBoyRadioCaptureResult::Unavailable;
    }
    if (json.size() > kMaximumRadioJsonBytes) {
        json.clear();
        return PipBoyRadioCaptureResult::Unavailable;
    }
    return PipBoyRadioCaptureResult::Captured;
}

}
