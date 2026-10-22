#pragma once

#include "platform/windows_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace sf::platform {

// __except filter for calls into game code: handles access violations and
// in-page errors, lets every other exception continue.
[[nodiscard]] int MemoryAccessExceptionFilter(
    unsigned long exceptionCode) noexcept;

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

enum class AddressCheck : std::uint8_t {
    None,
    Executable,
    Readable,
};

// One address of a resolved runtime: the RVA at profile+source becomes the
// absolute address at runtime+target, and check names the region test.
struct AddressBinding final {
    std::uint16_t source;
    std::uint16_t target;
    AddressCheck check;
};

[[nodiscard]] constexpr AddressBinding Bind(
    std::size_t source,
    std::size_t target,
    AddressCheck check) noexcept {
    return {
        static_cast<std::uint16_t>(source),
        static_cast<std::uint16_t>(target),
        check};
}

// Resolves every binding; fails on a zero RVA or a failed region test.
[[nodiscard]] bool ResolveAddressBindings(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const void* profile,
    void* runtime,
    const AddressBinding* bindings,
    std::size_t count) noexcept;

template <typename Profile, typename Runtime, std::size_t Count>
[[nodiscard]] bool ResolveAddressBindings(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const Profile* profile,
    Runtime* runtime,
    const std::array<AddressBinding, Count>& bindings) noexcept {
    return ResolveAddressBindings(
        virtualQuery, moduleBase, profile, runtime, bindings.data(), Count);
}

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
