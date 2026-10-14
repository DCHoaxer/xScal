#include "modules/xscal_priority/xscal_priority.hpp"

#include "platform/diagnostics.hpp"

#include <bit>
#include <cstddef>
#include <limits>
#include <vector>

namespace sf::modules {
namespace {

[[nodiscard]] DWORD PriorityClassValue(
    config::ProcessPriorityClass priority) noexcept {
    switch (priority) {
        case config::ProcessPriorityClass::AboveNormal:
            return ABOVE_NORMAL_PRIORITY_CLASS;
        case config::ProcessPriorityClass::High:
            return HIGH_PRIORITY_CLASS;
        case config::ProcessPriorityClass::NoChange:
        default:
            return 0;
    }
}

[[nodiscard]] const char* PriorityClassName(DWORD priority) noexcept {
    switch (priority) {
        case IDLE_PRIORITY_CLASS: return "Idle";
        case BELOW_NORMAL_PRIORITY_CLASS: return "Below Normal";
        case NORMAL_PRIORITY_CLASS: return "Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS: return "Above Normal";
        case HIGH_PRIORITY_CLASS: return "High";
        case REALTIME_PRIORITY_CLASS: return "Realtime";
        default: return "Unknown";
    }
}

[[nodiscard]] bool ReadCoreTopology(
    const XScalPriorityWindowsApi& api,
    std::vector<std::uintptr_t>& coreMasks,
    std::uintptr_t& performanceCoreMask) noexcept {
    constexpr std::size_t kRecordHeaderSize =
        offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
    constexpr std::size_t kGroupMasksOffset =
        kRecordHeaderSize + offsetof(PROCESSOR_RELATIONSHIP, GroupMask);
    constexpr std::size_t kOneGroupRecordSize =
        kGroupMasksOffset + sizeof(GROUP_AFFINITY);

    DWORD byteCount{};
    if (api.getLogicalProcessorInformationEx(
            RelationProcessorCore, nullptr, &byteCount) != FALSE ||
        byteCount == 0) {
        DiagnosticLog("Module: xScalPriority affinity: CPU topology size query failed");
        return false;
    }

    const DWORD sizeError = api.getLastError();
    if (sizeError != ERROR_INSUFFICIENT_BUFFER) {
        DiagnosticLogFormat(
            "Module: xScalPriority affinity: CPU topology size query failed: %lu",
            static_cast<unsigned long>(sizeError));
        return false;
    }

    try {
        std::vector<std::byte> buffer(byteCount);
        std::vector<BYTE> efficiencyClasses;
        DWORD returnedBytes = byteCount;
        auto* first = reinterpret_cast<
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
        if (api.getLogicalProcessorInformationEx(
                RelationProcessorCore, first, &returnedBytes) == FALSE) {
            const DWORD error = api.getLastError();
            DiagnosticLogFormat(
                "Module: xScalPriority affinity: CPU topology query failed: %lu",
                static_cast<unsigned long>(error));
            return false;
        }
        if (returnedBytes == 0 || returnedBytes > buffer.size()) {
            DiagnosticLog("Module: xScalPriority affinity: invalid CPU topology size");
            return false;
        }

        std::size_t offset{};
        while (offset < returnedBytes) {
            const std::size_t remaining = returnedBytes - offset;
            if (remaining < kRecordHeaderSize) {
                DiagnosticLog("Module: xScalPriority affinity: truncated CPU topology record");
                return false;
            }

            const auto* record = reinterpret_cast<
                const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                    buffer.data() + offset);
            if (record->Size < kRecordHeaderSize || record->Size > remaining) {
                DiagnosticLog("Module: xScalPriority affinity: invalid CPU topology record");
                return false;
            }

            if (record->Relationship == RelationProcessorCore) {
                if (record->Size < kOneGroupRecordSize) {
                    DiagnosticLog("Module: xScalPriority affinity: truncated CPU core record");
                    return false;
                }
                const auto& processor = record->Processor;
                if (processor.GroupCount != 1 ||
                    processor.GroupMask[0].Group != 0 ||
                    processor.GroupMask[0].Mask == 0) {
                    DiagnosticLog("Module: xScalPriority affinity: unsupported CPU core record");
                    return false;
                }
                coreMasks.push_back(static_cast<std::uintptr_t>(
                    processor.GroupMask[0].Mask));
                efficiencyClasses.push_back(processor.EfficiencyClass);
            }
            offset += record->Size;
        }

        if (coreMasks.empty()) {
            return false;
        }

        BYTE lowestClass = efficiencyClasses.front();
        BYTE highestClass = efficiencyClasses.front();
        for (const BYTE efficiencyClass : efficiencyClasses) {
            if (efficiencyClass < lowestClass) {
                lowestClass = efficiencyClass;
            }
            if (efficiencyClass > highestClass) {
                highestClass = efficiencyClass;
            }
        }

        performanceCoreMask = 0;
        if (lowestClass != highestClass) {
            // Higher EfficiencyClass values identify faster, less-efficient cores.
            for (std::size_t index = 0; index < coreMasks.size(); ++index) {
                if (efficiencyClasses[index] == highestClass) {
                    performanceCoreMask |= coreMasks[index];
                }
            }
        }
        return true;
    } catch (...) {
        DiagnosticLog("Module: xScalPriority affinity: CPU topology allocation failed");
        return false;
    }
}

}

XScalPriorityWindowsApi SystemXScalPriorityWindowsApi() noexcept {
    return {
        &::GetCurrentProcess,
        &::GetPriorityClass,
        &::SetPriorityClass,
        &::GetProcessAffinityMask,
        &::SetProcessAffinityMask,
        &::GetLogicalProcessorInformationEx,
        &::GetActiveProcessorGroupCount,
        &::GetLastError,
    };
}

std::uintptr_t CalculateXScalPriorityAffinity(
    std::uintptr_t processMask,
    std::uintptr_t systemMask,
    std::span<const std::uintptr_t> coreMasks,
    std::uintptr_t performanceCoreMask,
    const config::XScalPriorityConfig& config) noexcept {
    constexpr auto kMaskBits =
        std::numeric_limits<std::uintptr_t>::digits;
    if (processMask == 0 || systemMask == 0 ||
        config.minimumLogicalProcessors > kMaskBits) {
        return 0;
    }

    std::uintptr_t candidate = processMask & systemMask;
    if (candidate == 0) {
        return 0;
    }
    const std::uint32_t minimumLogicalProcessors =
        config.minimumLogicalProcessors == 0
            ? 1
            : config.minimumLogicalProcessors;

    std::uintptr_t covered{};
    for (const std::uintptr_t coreMask : coreMasks) {
        covered |= coreMask;
    }

    if (config.performanceCoresOnly && performanceCoreMask != 0 &&
        (covered & candidate) == candidate) {
        const std::uintptr_t allowedPerformanceCores =
            candidate & performanceCoreMask;
        const std::uintptr_t systemPerformanceCores =
            systemMask & performanceCoreMask;
        if ((candidate & ~performanceCoreMask) != 0 &&
            allowedPerformanceCores == systemPerformanceCores &&
            std::popcount(allowedPerformanceCores) >=
                minimumLogicalProcessors) {
            candidate = allowedPerformanceCores;
        }
    }

    if (config.oneLogicalProcessorPerCore && !coreMasks.empty()) {
        covered = 0;
        std::uintptr_t onePerCore{};
        for (const std::uintptr_t coreMask : coreMasks) {
            const std::uintptr_t eligible = coreMask & candidate;
            covered |= eligible;
            if (eligible != 0) {
                onePerCore |= std::uintptr_t{1} << std::countr_zero(eligible);
            }
        }
        if ((covered & candidate) == candidate && onePerCore != 0 &&
            std::popcount(onePerCore) >= minimumLogicalProcessors) {
            candidate = onePerCore;
        }
    }

    return candidate;
}

XScalPriority::XScalPriority() noexcept
    : XScalPriority(
          SystemXScalPriorityWindowsApi(),
          config::kXScalPriority) {}

XScalPriority::XScalPriority(
    XScalPriorityWindowsApi api,
    config::XScalPriorityConfig config) noexcept
    : api_(api),
      config_(config) {}

void XScalPriority::Load() noexcept {
    if (loaded_) {
        return;
    }
    loaded_ = true;

    if (!config_.enabled) {
        DiagnosticLog("Module: xScalPriority disabled");
        return;
    }

    const HANDLE process = api_.getCurrentProcess();
    if (process == nullptr) {
        DiagnosticLog("Module: xScalPriority current process handle unavailable");
        return;
    }

    ApplyPriority(process);
    if (config_.autoAffinity) {
        ApplyAffinity(process);
    } else {
        DiagnosticLog("Module: xScalPriority affinity: disabled");
    }
}

void XScalPriority::Unload() noexcept {
    if (!loaded_) {
        return;
    }

    const HANDLE process = api_.getCurrentProcess();
    if (process != nullptr) {
        RestoreAffinity(process);
        RestorePriority(process);
    }
    affinityChanged_ = false;
    priorityChanged_ = false;
    loaded_ = false;
}

void XScalPriority::ApplyPriority(HANDLE process) noexcept {
    if (config_.priority == config::ProcessPriorityClass::NoChange) {
        DiagnosticLog("Module: xScalPriority priority: unchanged by configuration");
        return;
    }

    const DWORD requestedValue = PriorityClassValue(config_.priority);
    const DWORD current = api_.getPriorityClass(process);
    if (current == 0) {
        const DWORD error = api_.getLastError();
        DiagnosticLogFormat(
            "Module: xScalPriority priority: query failed: %lu",
            static_cast<unsigned long>(error));
        return;
    }
    if (current == requestedValue) {
        DiagnosticLogFormat(
            "Module: xScalPriority priority: already %s",
            PriorityClassName(current));
        return;
    }
    if (api_.setPriorityClass(process, requestedValue) == FALSE) {
        const DWORD error = api_.getLastError();
        DiagnosticLogFormat(
            "Module: xScalPriority priority: could not set %s: %lu",
            PriorityClassName(requestedValue),
            static_cast<unsigned long>(error));
        return;
    }

    previousPriority_ = current;
    appliedPriority_ = requestedValue;
    priorityChanged_ = true;
    DiagnosticLogFormat(
        "Module: xScalPriority priority: %s -> %s",
        PriorityClassName(current),
        PriorityClassName(requestedValue));
}

void XScalPriority::ApplyAffinity(HANDLE process) noexcept {
    const WORD groupCount = api_.getActiveProcessorGroupCount();
    if (groupCount != 1) {
        DiagnosticLogFormat(
            "Module: xScalPriority affinity: skipped for %hu processor groups",
            groupCount);
        return;
    }

    DWORD_PTR processMask{};
    DWORD_PTR systemMask{};
    if (api_.getProcessAffinityMask(
            process, &processMask, &systemMask) == FALSE) {
        const DWORD error = api_.getLastError();
        DiagnosticLogFormat(
            "Module: xScalPriority affinity: query failed: %lu",
            static_cast<unsigned long>(error));
        return;
    }
    DiagnosticLogFormat(
        "Module: xScalPriority affinity: process 0x%llX, system 0x%llX",
        static_cast<unsigned long long>(processMask),
        static_cast<unsigned long long>(systemMask));

    std::vector<std::uintptr_t> coreMasks;
    std::uintptr_t performanceCoreMask{};
    if ((config_.performanceCoresOnly ||
            config_.oneLogicalProcessorPerCore) &&
        !ReadCoreTopology(api_, coreMasks, performanceCoreMask)) {
        coreMasks.clear();
        DiagnosticLog("Module: xScalPriority affinity: CPU topology filters skipped");
    }
    if (config_.performanceCoresOnly && performanceCoreMask != 0) {
        DiagnosticLogFormat(
            "Module: xScalPriority affinity: performance cores 0x%llX",
            static_cast<unsigned long long>(performanceCoreMask));
    } else if (config_.performanceCoresOnly && !coreMasks.empty()) {
        DiagnosticLog(
            "Module: xScalPriority affinity: performance-core classification unavailable");
    }

    const std::uintptr_t requested = CalculateXScalPriorityAffinity(
        static_cast<std::uintptr_t>(processMask),
        static_cast<std::uintptr_t>(systemMask),
        coreMasks,
        performanceCoreMask,
        config_);
    if (requested == 0) {
        DiagnosticLog("Module: xScalPriority affinity: invalid mask or minimum");
        return;
    }
    if (requested == processMask) {
        DiagnosticLog("Module: xScalPriority affinity: unchanged");
        return;
    }
    if (api_.setProcessAffinityMask(
            process, static_cast<DWORD_PTR>(requested)) == FALSE) {
        const DWORD error = api_.getLastError();
        DiagnosticLogFormat(
            "Module: xScalPriority affinity: could not set mask 0x%llX: %lu",
            static_cast<unsigned long long>(requested),
            static_cast<unsigned long>(error));
        return;
    }

    previousAffinity_ = processMask;
    appliedAffinity_ = static_cast<DWORD_PTR>(requested);
    affinityChanged_ = true;
    DiagnosticLogFormat(
        "Module: xScalPriority affinity: 0x%llX -> 0x%llX",
        static_cast<unsigned long long>(processMask),
        static_cast<unsigned long long>(requested));
}

void XScalPriority::RestorePriority(HANDLE process) noexcept {
    if (!priorityChanged_) {
        return;
    }

    const DWORD current = api_.getPriorityClass(process);
    if (current == 0) {
        const DWORD error = api_.getLastError();
        DiagnosticLogFormat(
            "Module: xScalPriority priority: restore query failed: %lu",
            static_cast<unsigned long>(error));
    } else if (current != appliedPriority_) {
        DiagnosticLog("Module: xScalPriority priority: restore skipped after external change");
    } else if (api_.setPriorityClass(process, previousPriority_) == FALSE) {
        const DWORD error = api_.getLastError();
        DiagnosticLogFormat(
            "Module: xScalPriority priority: restore failed: %lu",
            static_cast<unsigned long>(error));
    } else {
        DiagnosticLogFormat(
            "Module: xScalPriority priority: restored %s",
            PriorityClassName(previousPriority_));
    }
    priorityChanged_ = false;
}

void XScalPriority::RestoreAffinity(HANDLE process) noexcept {
    if (!affinityChanged_) {
        return;
    }

    DWORD_PTR current{};
    DWORD_PTR systemMask{};
    if (api_.getProcessAffinityMask(
            process, &current, &systemMask) == FALSE) {
        const DWORD error = api_.getLastError();
        DiagnosticLogFormat(
            "Module: xScalPriority affinity: restore query failed: %lu",
            static_cast<unsigned long>(error));
    } else if (current != appliedAffinity_) {
        DiagnosticLog("Module: xScalPriority affinity: restore skipped after external change");
    } else if (api_.setProcessAffinityMask(
                   process, previousAffinity_) == FALSE) {
        const DWORD error = api_.getLastError();
        DiagnosticLogFormat(
            "Module: xScalPriority affinity: restore failed: %lu",
            static_cast<unsigned long>(error));
    } else {
        DiagnosticLogFormat(
            "Module: xScalPriority affinity: restored 0x%llX",
            static_cast<unsigned long long>(previousAffinity_));
    }
    affinityChanged_ = false;
}

}
