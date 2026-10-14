#pragma once

#include "config/config.h"

#include <Windows.h>

#include <cstdint>
#include <span>

namespace sf::modules {

struct XScalPriorityWindowsApi final {
    decltype(&::GetCurrentProcess) getCurrentProcess{};
    decltype(&::GetPriorityClass) getPriorityClass{};
    decltype(&::SetPriorityClass) setPriorityClass{};
    decltype(&::GetProcessAffinityMask) getProcessAffinityMask{};
    decltype(&::SetProcessAffinityMask) setProcessAffinityMask{};
    decltype(&::GetLogicalProcessorInformationEx)
        getLogicalProcessorInformationEx{};
    decltype(&::GetActiveProcessorGroupCount) getActiveProcessorGroupCount{};
    decltype(&::GetLastError) getLastError{};
};

[[nodiscard]] XScalPriorityWindowsApi
SystemXScalPriorityWindowsApi() noexcept;

[[nodiscard]] std::uintptr_t CalculateXScalPriorityAffinity(
    std::uintptr_t processMask,
    std::uintptr_t systemMask,
    std::span<const std::uintptr_t> coreMasks,
    std::uintptr_t performanceCoreMask,
    const config::XScalPriorityConfig& config) noexcept;

class XScalPriority final {
public:
    // Runtime default-constructs this module. It uses the compiled-in
    // settings in kXScalPriority and the Windows API from the system.
    XScalPriority() noexcept;
    XScalPriority(
        XScalPriorityWindowsApi api,
        config::XScalPriorityConfig config) noexcept;

    XScalPriority(const XScalPriority&) = delete;
    XScalPriority& operator=(const XScalPriority&) = delete;

    void Load() noexcept;
    void Unload() noexcept;

private:
    void ApplyPriority(HANDLE process) noexcept;
    void ApplyAffinity(HANDLE process) noexcept;
    void RestorePriority(HANDLE process) noexcept;
    void RestoreAffinity(HANDLE process) noexcept;

    XScalPriorityWindowsApi api_{};
    config::XScalPriorityConfig config_{};
    bool loaded_{};
    bool priorityChanged_{};
    DWORD previousPriority_{};
    DWORD appliedPriority_{};
    bool affinityChanged_{};
    DWORD_PTR previousAffinity_{};
    DWORD_PTR appliedAffinity_{};
};

}
