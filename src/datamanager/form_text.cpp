#include "datamanager/form_text.hpp"

#include "platform/native_access.hpp"

#include <Windows.h>


namespace sf::datamanager {
namespace {

using GetFullName = const char*(__fastcall*)(const void* fullName);
using GetDescription = void(__fastcall*)(
    const void* description,
    GameString* text,
    std::uint32_t flags);

[[nodiscard]] bool ReadVirtualFunction(
    std::uintptr_t object,
    std::size_t vtableOffset,
    std::uintptr_t& function) noexcept {
    function = 0;
    std::uintptr_t vtable{};
    std::uintptr_t slot{};
    return platform::TryCopyBytes(
               reinterpret_cast<const void*>(object),
               &vtable,
               sizeof(vtable)) &&
        vtable != 0 &&
        platform::TryAddAddress(vtable, vtableOffset, slot) &&
        platform::TryCopyBytes(
            reinterpret_cast<const void*>(slot),
            &function,
            sizeof(function)) &&
        function != 0;
}

[[nodiscard]] bool CallGetFullName(
    std::uintptr_t function,
    std::uintptr_t fullName,
    const char*& name) noexcept {
    __try {
        name = reinterpret_cast<GetFullName>(function)(
            reinterpret_cast<const void*>(fullName));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        name = nullptr;
        return false;
    }
    return true;
}

[[nodiscard]] bool CallGetDescription(
    std::uintptr_t function,
    std::uintptr_t description,
    GameString& text) noexcept {
    __try {
        reinterpret_cast<GetDescription>(function)(
            reinterpret_cast<const void*>(description), &text, 0);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

}

bool ReadFullName(
    std::uintptr_t fullNameComponent,
    std::size_t getFullNameVtableOffset,
    std::size_t maximumLength,
    std::string& name) noexcept {
    name.clear();
    std::uintptr_t getFullName{};
    const char* text{};
    if (!ReadVirtualFunction(
            fullNameComponent, getFullNameVtableOffset, getFullName) ||
        !CallGetFullName(getFullName, fullNameComponent, text)) {
        return false;
    }
    return text == nullptr ||
        platform::CopyBoundedCString(text, maximumLength, true, name);
}

bool ReadDescription(
    std::uintptr_t descriptionComponent,
    std::size_t getDescriptionVtableOffset,
    GameDeallocate deallocate,
    std::size_t maximumLength,
    std::string& description) noexcept {
    description.clear();
    std::uintptr_t getDescription{};
    if (deallocate == nullptr ||
        !ReadVirtualFunction(
            descriptionComponent, getDescriptionVtableOffset, getDescription)) {
        return false;
    }
    GameString text;
    const GameStringOwner owner{deallocate, text};
    return CallGetDescription(getDescription, descriptionComponent, text) &&
        CopyGameString(text, maximumLength, description);
}

}
