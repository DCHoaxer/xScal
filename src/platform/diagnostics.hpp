#pragma once

#include "hook/vtable_hook.hpp"

#include <Windows.h>

#include <string_view>

namespace sf {

class FileCommandService;

void SetDiagnosticModule(HMODULE module) noexcept;
void SetDiagnosticFileService(FileCommandService* service) noexcept;
void ClearDiagnosticFileService(FileCommandService* service) noexcept;
void ClearDiagnosticLog() noexcept;
void WriteDiagnosticLineToFile(std::string_view line) noexcept;
void DiagnosticLog(const char* message) noexcept;
void DiagnosticLogImmediate(const char* message) noexcept;
void DiagnosticLogFormat(const char* format, ...) noexcept;
void DiagnosticLogFormatImmediate(const char* format, ...) noexcept;
[[nodiscard]] const char* VtableHookStatusName(VtableHookStatus status) noexcept;

}
