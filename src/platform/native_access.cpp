#include "platform/native_access.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <limits>

namespace sf::platform {
namespace {

[[nodiscard]] bool MeasureBoundedCString(
    const char* value,
    std::size_t maximumLength,
    std::size_t& length) noexcept {
    length = 0;
    __try {
        while (length < maximumLength && value[length] != 0) {
            ++length;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        length = 0;
        return false;
    }
    return length < maximumLength;
}

}

bool TryAddAddress(
    std::uintptr_t base,
    std::size_t offset,
    std::uintptr_t& address) noexcept {
    if (base > (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        return false;
    }
    address = base + offset;
    return true;
}

namespace {

[[nodiscard]] bool ResolveRequiredRva(
    std::uintptr_t moduleBase,
    std::uintptr_t rva,
    std::uintptr_t& address) noexcept {
    return moduleBase != 0 && rva != 0 &&
        TryAddAddress(moduleBase, rva, address);
}

}

bool ResolveRequiredRvas(
    std::uintptr_t moduleBase,
    std::initializer_list<RvaBinding> bindings) noexcept {
    for (const auto& binding : bindings) {
        if (binding.address == nullptr ||
            !ResolveRequiredRva(
                moduleBase, binding.rva, *binding.address)) {
            return false;
        }
    }
    return true;
}

bool ResolveAddressBindings(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const void* profile,
    void* runtime,
    const AddressBinding* bindings,
    std::size_t count) noexcept {
    const auto* const source = static_cast<const std::byte*>(profile);
    auto* const target = static_cast<std::byte*>(runtime);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& binding = bindings[index];
        std::uintptr_t rva{};
        std::uintptr_t address{};
        std::memcpy(&rva, source + binding.source, sizeof(rva));
        if (!ResolveRequiredRva(moduleBase, rva, address) ||
            (binding.check == AddressCheck::Executable &&
             !IsExecutableAddress(
                 virtualQuery, reinterpret_cast<const void*>(address))) ||
            (binding.check == AddressCheck::Readable &&
             !IsReadableRange(
                 virtualQuery,
                 reinterpret_cast<const void*>(address),
                 sizeof(std::uintptr_t)))) {
            return false;
        }
        std::memcpy(target + binding.target, &address, sizeof(address));
    }
    return true;
}

int MemoryAccessExceptionFilter(unsigned long exceptionCode) noexcept {
    return exceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            exceptionCode == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH;
}

bool TryCopyBytes(
    const void* address,
    void* destination,
    std::size_t size) noexcept {
    if (address == nullptr || destination == nullptr || size == 0) {
        return false;
    }
    __try {
        std::memcpy(destination, address, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool ReadBytes(
    VirtualQueryFn virtualQuery,
    const void* address,
    void* destination,
    std::size_t size) noexcept {
    return destination != nullptr &&
        IsReadableRange(virtualQuery, address, size) &&
        TryCopyBytes(address, destination, size);
}

bool CopyBoundedCString(
    const char* value,
    std::size_t maximumLength,
    bool allowEmpty,
    std::string& destination) noexcept {
    destination.clear();
    if (value == nullptr || maximumLength == 0) {
        return false;
    }

    std::size_t length{};
    if (!MeasureBoundedCString(value, maximumLength, length) ||
        (!allowEmpty && length == 0)) {
        return false;
    }

    try {
        destination.resize(length);
    } catch (...) {
        return false;
    }
    return length == 0 || TryCopyBytes(value, destination.data(), length);
}

bool ValidateVtable(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    std::uintptr_t vtableRva) noexcept {
    std::uintptr_t address{};
    if (virtualQuery == nullptr ||
        !ResolveRequiredRva(moduleBase, vtableRva, address)) {
        return false;
    }

    const auto* const vtable = reinterpret_cast<const void*>(address);
    MEMORY_BASIC_INFORMATION region{};
    if (virtualQuery(vtable, &region, sizeof(region)) == 0 ||
        region.State != MEM_COMMIT ||
        !IsReadableProtection(region.Protect) ||
        IsExecutableProtection(region.Protect) ||
        !RegionContains(region, vtable, sizeof(std::uintptr_t))) {
        return false;
    }

    std::uintptr_t firstVirtualFunction{};
    return ReadBytes(
               virtualQuery,
               vtable,
               &firstVirtualFunction,
               sizeof(firstVirtualFunction)) &&
        firstVirtualFunction != 0 &&
        IsExecutableAddress(
            virtualQuery,
            reinterpret_cast<const void*>(firstVirtualFunction));
}

bool ValidateVtables(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    std::initializer_list<std::uintptr_t> vtableRvas) noexcept {
    for (const auto rva : vtableRvas) {
        if (!ValidateVtable(virtualQuery, moduleBase, rva)) {
            return false;
        }
    }
    return true;
}

bool AreExecutableAddresses(
    VirtualQueryFn virtualQuery,
    std::initializer_list<std::uintptr_t> addresses) noexcept {
    for (const auto address : addresses) {
        if (!IsExecutableAddress(
                virtualQuery,
                reinterpret_cast<const void*>(address))) {
            return false;
        }
    }
    return true;
}

bool AreReadableAddresses(
    VirtualQueryFn virtualQuery,
    std::initializer_list<std::uintptr_t> addresses,
    std::size_t size) noexcept {
    for (const auto address : addresses) {
        if (!IsReadableRange(
                virtualQuery,
                reinterpret_cast<const void*>(address),
                size)) {
            return false;
        }
    }
    return true;
}

}
