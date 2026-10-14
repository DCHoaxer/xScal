#pragma once

#include "platform/windows_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace sf::platform {

[[nodiscard]] bool TryAddAddress(
    std::uintptr_t base,
    std::size_t offset,
    std::uintptr_t& address) noexcept;

struct RvaBinding final {
    std::uintptr_t rva;
    std::uintptr_t* address;
};

[[nodiscard]] bool ResolveRequiredRvas(
    std::uintptr_t moduleBase,
    std::initializer_list<RvaBinding> bindings) noexcept;

[[nodiscard]] bool TryCopyBytes(
    const void* address,
    void* destination,
    std::size_t size) noexcept;

[[nodiscard]] bool ReadBytes(
    VirtualQueryFn virtualQuery,
    const void* address,
    void* destination,
    std::size_t size) noexcept;

[[nodiscard]] bool CopyBoundedCString(
    const char* value,
    std::size_t maximumLength,
    bool allowEmpty,
    std::string& destination) noexcept;

[[nodiscard]] bool ValidateVtable(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    std::uintptr_t vtableRva) noexcept;

[[nodiscard]] bool ValidateVtables(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    std::initializer_list<std::uintptr_t> vtableRvas) noexcept;

[[nodiscard]] bool AreExecutableAddresses(
    VirtualQueryFn virtualQuery,
    std::initializer_list<std::uintptr_t> addresses) noexcept;

[[nodiscard]] bool AreReadableAddresses(
    VirtualQueryFn virtualQuery,
    std::initializer_list<std::uintptr_t> addresses,
    std::size_t size = sizeof(std::uintptr_t)) noexcept;

}
