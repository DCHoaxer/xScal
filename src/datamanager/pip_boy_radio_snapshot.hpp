#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sf::datamanager {

struct ResolvedPipBoyRadioRuntime final {
    config::NativePipBoyRadioProfile layout{};
    std::uintptr_t radioLock{};
    std::uintptr_t radioStationMap{};
    std::uintptr_t radioManagerGlobal{};
    std::uintptr_t tryAcquireReadLock{};
    std::uintptr_t releaseReadLock{};
    std::uintptr_t getFixedStringCharacters{};
};

struct PipBoyRadioStation final {
    std::string name;
    float frequency{};
    // Powered and tuned to this frequency, as the radio page marks it.
    bool active{};
};

// Every station the game knows, sorted by frequency. The Pip-Boy lists only
// the stations a detection event reported in range; that set has no
// off-menu source.
struct PipBoyRadio final {
    // False until the game creates its radio manager.
    bool hasTuner{};
    bool powered{};
    float tunedFrequency{};
    std::vector<PipBoyRadioStation> stations;
};

enum class PipBoyRadioCaptureResult {
    Captured,
    // The game held the radio lock for writing; nothing was read.
    LockBusy,
    Unavailable,
};

[[nodiscard]] bool ResolvePipBoyRadioRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyRadioProfile& radio,
    ResolvedPipBoyRadioRuntime& runtime) noexcept;

[[nodiscard]] PipBoyRadioCaptureResult ReadPipBoyRadio(
    const ResolvedPipBoyRadioRuntime& runtime,
    PipBoyRadio& radio) noexcept;

[[nodiscard]] PipBoyRadioCaptureResult BuildPipBoyRadioJson(
    const ResolvedPipBoyRadioRuntime& runtime,
    std::string& json) noexcept;

}
