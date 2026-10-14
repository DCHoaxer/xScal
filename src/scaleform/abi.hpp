#pragma once

#include "scaleform/scaleform_layout.hpp"

#include <cstddef>
#include <cstdint>

namespace sf {

// Scaleform values are passed by address and treated as opaque data.
// Their size and alignment are part of the target's x64 ABI.
struct alignas(16) ScaleformValue final {
    std::byte storage[layout::kValueSize]{};
};

// MovieRoot::GetVariable at vtable offset 0x190 in the supported builds.
using MovieRootGetVariable = bool(__fastcall*)(
    void* movieRoot,
    ScaleformValue* outValue,
    const char* path,
    unsigned int callerR9Scratch);

// MovieRoot::AdvanceFrame at vtable offset 0x10, called once per frame for
// every movie before it is drawn.
using MovieRootAdvanceFrame = void(__fastcall*)(void* movieRoot, bool nextFrame);

// MovieRoot scalar deleting destructor at vtable offset 0. Bit 0 of flags
// frees the object after destruction.
using MovieRootDestroy = void*(__fastcall*)(void* movieRoot, unsigned int flags);

// MovieRoot::Invoke at vtable offset 0x1C8 in the supported builds.
using MovieRootInvoke = bool(__fastcall*)(
    void* movieRoot,
    const char* methodPath,
    ScaleformValue* outValue,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount);

static_assert(layout::kValueSize == 0x40,
    "Fallout 76 GFxValue ABI must remain 0x40 bytes");
static_assert(sizeof(ScaleformValue) == layout::kValueSize);
static_assert(alignof(ScaleformValue) == 16);

}
