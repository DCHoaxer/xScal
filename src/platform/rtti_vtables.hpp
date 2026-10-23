#pragma once

#include "platform/windows_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sf::platform {

// One vtable to find through the MSVC RTTI data of a mapped x64 image.
struct RttiVtableQuery final {
    // Decorated type name, for example ".?AVActorValueInfo@@".
    std::string_view typeName;
    // Offset of the subobject the vtable belongs to; 0 for the primary vtable.
    std::uint32_t subobjectOffset;
    // Set to the vtable RVA, or 0 when the type has no unique match.
    std::uintptr_t vtableRva;
};

inline constexpr std::size_t kMaximumRttiVtableQueries = 8;

// Finds each vtable from its type name: the TypeDescriptor in a writable data
// section, then the CompleteObjectLocator and the vtable slot that points to it
// in read-only data sections. Code sections are never read, so this works
// while the game's .text is still encrypted. Returns true when every query
// has exactly one match.
[[nodiscard]] bool FindRttiVtables(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    RttiVtableQuery* queries,
    std::size_t count) noexcept;

}
