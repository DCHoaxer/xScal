#pragma once

#include <Windows.h>

#include <cstddef>

namespace sf::platform {

using VirtualQueryFn = SIZE_T(WINAPI*)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);

[[nodiscard]] bool IsReadableProtection(DWORD protection) noexcept;
[[nodiscard]] bool IsExecutableProtection(DWORD protection) noexcept;
[[nodiscard]] bool IsExecutableAddress(VirtualQueryFn virtualQuery, const void* address) noexcept;
[[nodiscard]] bool RegionContains(const MEMORY_BASIC_INFORMATION& region, const void* address, std::size_t size) noexcept;
[[nodiscard]] bool IsReadableRange(VirtualQueryFn virtualQuery, const void* address, std::size_t size) noexcept;

}
