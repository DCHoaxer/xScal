#include "platform/native_access.hpp"

#include <Windows.h>

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
