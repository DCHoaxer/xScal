#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

struct XScalScaleformValue;
struct XScalAddressLibraryV1;
struct XScalScaleformMovieV1;
struct XScalScaleformMovieV2;

namespace sf {
enum class TargetKind;
struct MovieRootContext;
}

namespace sf::plugins {

// Set after host validation, before plugin descriptors are published.
void SetTargetKind(TargetKind kind) noexcept;
void SetAddressLibrary(
    const XScalAddressLibraryV1* addressLibrary) noexcept;
// Published only after the MovieRoot hook is installed.
void SetScaleformMovieInterface(
    const XScalScaleformMovieV2* movieInterface,
    const XScalScaleformMovieV1* movieInterfaceV1) noexcept;

// Called from DLL_PROCESS_ATTACH. Only scans filenames and patches named imports;
// plugin code is loaded later, on a normal call outside the loader lock.
void InstallCreationHooks(
    HMODULE executable, bool (*initializeHost)() noexcept = nullptr) noexcept;

// Called by the initialization worker and creation hooks; completes only once.
// Accepted DLLs and their callbacks remain resident until process exit.
void Load() noexcept;

[[nodiscard]] std::size_t ModuleCount() noexcept;
[[nodiscard]] std::size_t CopyActiveModuleNames(
    char* output,
    std::size_t capacity) noexcept;
[[nodiscard]] bool DispatchScaleform(
    std::string_view name,
    const XScalScaleformValue* arguments,
    std::uint32_t argumentCount,
    XScalScaleformValue& result) noexcept;
}
