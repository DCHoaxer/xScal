#include "platform/windows_memory.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace sf::platform {

bool IsReadableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xFFU) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

bool IsExecutableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xFFU) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

bool IsExecutableAddress(VirtualQueryFn virtualQuery, const void* address) noexcept {
    if (virtualQuery == nullptr || address == nullptr) return false;
    MEMORY_BASIC_INFORMATION region{};
    return virtualQuery(address, &region, sizeof(region)) != 0 &&
        region.State == MEM_COMMIT && IsExecutableProtection(region.Protect) &&
        RegionContains(region, address, 1);
}

bool RegionContains(const MEMORY_BASIC_INFORMATION& region, const void* address, std::size_t size) noexcept {
    if (address == nullptr || size == 0 || region.RegionSize < size) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= base && value - base <= region.RegionSize - size;
}

bool IsReadableRange(VirtualQueryFn virtualQuery, const void* address, std::size_t size) noexcept {
    if (virtualQuery == nullptr || address == nullptr || size == 0) return false;
    auto current = reinterpret_cast<std::uintptr_t>(address);
    if (size > (std::numeric_limits<std::uintptr_t>::max)() - current) return false;
    const auto end = current + size;
    while (current < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (virtualQuery(reinterpret_cast<const void*>(current), &region, sizeof(region)) == 0 ||
            region.State != MEM_COMMIT || !IsReadableProtection(region.Protect) ||
            !RegionContains(region, reinterpret_cast<const void*>(current), 1)) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
        const auto regionEnd = base + region.RegionSize;
        if (regionEnd <= current) return false;
        current = std::min(regionEnd, end);
    }
    return true;
}

}
