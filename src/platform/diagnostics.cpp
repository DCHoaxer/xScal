#include "platform/diagnostics.hpp"

#include "api/file_command_service.hpp"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sf {
namespace {

HMODULE diagnosticModule = nullptr;
std::array<wchar_t, 32768> diagnosticPath{};
SRWLOCK diagnosticLock = SRWLOCK_INIT;
ULONGLONG diagnosticBaselineTick{};
ULONGLONG diagnosticBaselineElapsedMs{};
std::atomic<FileCommandService*> diagnosticFileService{};

bool BuildLogPath(std::array<wchar_t, 32768>& path) noexcept {
    if (diagnosticModule == nullptr) {
        return false;
    }
    const DWORD length = ::GetModuleFileNameW(
        diagnosticModule, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return false;
    }
    std::size_t separator = length;
    while (separator != 0 && path[separator - 1] != L'\\' && path[separator - 1] != L'/') {
        --separator;
    }
    constexpr wchar_t filename[] = L"xscal.log";
    constexpr std::size_t filenameLength = (sizeof(filename) / sizeof(filename[0])) - 1;
    if (separator + filenameLength >= path.size()) {
        return false;
    }
    std::memcpy(path.data() + separator, filename, sizeof(filename));
    return true;
}

}

void SetDiagnosticModule(HMODULE module) noexcept {
    diagnosticModule = module;
    diagnosticPath.fill(L'\0');
    if (!BuildLogPath(diagnosticPath)) {
        diagnosticPath.fill(L'\0');
    }
    diagnosticBaselineTick = ::GetTickCount64();
    diagnosticBaselineElapsedMs = 0;

    FILETIME creationTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    FILETIME currentTime{};
    if (::GetProcessTimes(
            ::GetCurrentProcess(),
            &creationTime,
            &exitTime,
            &kernelTime,
            &userTime) != FALSE) {
        ::GetSystemTimeAsFileTime(&currentTime);
        ULARGE_INTEGER creation{};
        creation.LowPart = creationTime.dwLowDateTime;
        creation.HighPart = creationTime.dwHighDateTime;
        ULARGE_INTEGER current{};
        current.LowPart = currentTime.dwLowDateTime;
        current.HighPart = currentTime.dwHighDateTime;
        if (current.QuadPart >= creation.QuadPart) {
            diagnosticBaselineElapsedMs =
                (current.QuadPart - creation.QuadPart) / 10000ULL;
        }
    }
}

void SetDiagnosticFileService(FileCommandService* service) noexcept {
    diagnosticFileService.store(service, std::memory_order_release);
}

void ClearDiagnosticFileService(FileCommandService* service) noexcept {
    FileCommandService* expected = service;
    (void)diagnosticFileService.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

void ClearDiagnosticLog() noexcept {
    if (diagnosticPath[0] == L'\0') {
        return;
    }

    if (::TryAcquireSRWLockExclusive(&diagnosticLock) == FALSE) {
        return;
    }
    HANDLE file = ::CreateFileW(
        diagnosticPath.data(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        (void)::CloseHandle(file);
    }
    ::ReleaseSRWLockExclusive(&diagnosticLock);
}

namespace {

void AppendDiagnosticLineToFile(
    std::string_view line,
    bool waitForLock) noexcept {
    if (line.empty() || diagnosticPath[0] == L'\0') {
        return;
    }

    if (waitForLock) {
        ::AcquireSRWLockExclusive(&diagnosticLock);
    } else if (::TryAcquireSRWLockExclusive(&diagnosticLock) == FALSE) {
        return;
    }
    HANDLE file = ::CreateFileW(
        diagnosticPath.data(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written{};
        (void)::WriteFile(
            file,
            line.data(),
            static_cast<DWORD>(line.size()),
            &written,
            nullptr);
        (void)::CloseHandle(file);
    }
    ::ReleaseSRWLockExclusive(&diagnosticLock);
}

}

void WriteDiagnosticLineToFile(std::string_view line) noexcept {
    AppendDiagnosticLineToFile(line, false);
}

namespace {

// Before the file service is attached, non-immediate lines wait for the log
// lock. Dropping them when the lock is busy would lose startup messages.
void WriteDiagnosticMessage(const char* message, bool immediate) noexcept {
    if (message == nullptr || diagnosticPath[0] == L'\0') {
        return;
    }

    char line[1024]{};
    const ULONGLONG elapsedMs = diagnosticBaselineElapsedMs +
        (::GetTickCount64() - diagnosticBaselineTick);
    const ULONGLONG hours = elapsedMs / 3600000ULL;
    const ULONGLONG minutes = (elapsedMs / 60000ULL) % 60ULL;
    const ULONGLONG seconds = (elapsedMs / 1000ULL) % 60ULL;
    const ULONGLONG milliseconds = elapsedMs % 1000ULL;
    const int prefixLength = std::snprintf(
        line,
        sizeof(line),
        "[%02llu:%02llu:%02llu.%03llu] ",
        static_cast<unsigned long long>(hours),
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds),
        static_cast<unsigned long long>(milliseconds));
    if (prefixLength <= 0 || static_cast<std::size_t>(prefixLength) >= sizeof(line)) {
        return;
    }
    const std::size_t available = sizeof(line) - static_cast<std::size_t>(prefixLength) - 3;
    const std::size_t messageLength = strnlen(message, available);
    std::memcpy(line + prefixLength, message, messageLength);
    std::size_t lineLength = static_cast<std::size_t>(prefixLength) + messageLength;
    line[lineLength++] = '\r';
    line[lineLength++] = '\n';

    if (!immediate) {
        FileCommandService* service =
            diagnosticFileService.load(std::memory_order_acquire);
        if (service != nullptr) {
            // A line that cannot be queued is dropped. Writing it here would
            // open and write the log file on the calling game thread.
            (void)service->SubmitDiagnosticLine(
                std::string_view{line, lineLength});
            return;
        }
    }
    AppendDiagnosticLineToFile(std::string_view{line, lineLength}, true);
}

void WriteDiagnosticFormat(
    bool immediate,
    const char* format,
    va_list arguments) noexcept {
    if (format == nullptr) {
        return;
    }
    char message[768]{};
    const int result = std::vsnprintf(message, sizeof(message), format, arguments);
    if (result >= 0) {
        WriteDiagnosticMessage(message, immediate);
    }
}

}

void DiagnosticLog(const char* message) noexcept {
    WriteDiagnosticMessage(message, false);
}

void DiagnosticLogImmediate(const char* message) noexcept {
    WriteDiagnosticMessage(message, true);
}

void DiagnosticLogFormat(const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    WriteDiagnosticFormat(false, format, arguments);
    va_end(arguments);
}

void DiagnosticLogFormatImmediate(const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    WriteDiagnosticFormat(true, format, arguments);
    va_end(arguments);
}

const char* VtableHookStatusName(VtableHookStatus status) noexcept {
    switch (status) {
    case VtableHookStatus::Installed: return "Installed";
    case VtableHookStatus::Restored: return "Restored";
    case VtableHookStatus::AlreadyActive: return "AlreadyActive";
    case VtableHookStatus::NotActive: return "NotActive";
    case VtableHookStatus::InvalidArgument: return "InvalidArgument";
    case VtableHookStatus::InvalidPlatform: return "InvalidPlatform";
    case VtableHookStatus::RuntimeVersionUnavailable: return "RuntimeVersionUnavailable";
    case VtableHookStatus::RuntimeVersionMismatch: return "RuntimeVersionMismatch";
    case VtableHookStatus::AddressLibraryMissing: return "AddressLibraryMissing";
    case VtableHookStatus::AddressLibraryRestartRequired: return "AddressLibraryRestartRequired";
    case VtableHookStatus::AddressLibraryRequiresNewerXScal: return "AddressLibraryRequiresNewerXScal";
    case VtableHookStatus::AddressLibraryInvalid: return "AddressLibraryInvalid";
    case VtableHookStatus::AddressLibraryIncomplete: return "AddressLibraryIncomplete";
    case VtableHookStatus::AddressOverflow: return "AddressOverflow";
    case VtableHookStatus::SlotUnaligned: return "SlotUnaligned";
    case VtableHookStatus::SlotNotReadable: return "SlotNotReadable";
    case VtableHookStatus::OriginalNotExecutable: return "OriginalNotExecutable";
    case VtableHookStatus::TargetRoutineNotExecutable: return "TargetRoutineNotExecutable";
    case VtableHookStatus::ProtectFailed: return "ProtectFailed";
    case VtableHookStatus::FlushFailed: return "FlushFailed";
    case VtableHookStatus::ProtectionRestoreFailed: return "ProtectionRestoreFailed";
    case VtableHookStatus::ReplacedByAnotherWriter: return "ReplacedByAnotherWriter";
    case VtableHookStatus::AllocationFailed: return "AllocationFailed";
    }
    return "Unknown";
}

}
