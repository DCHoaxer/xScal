#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"
#include "scaleform/abi.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace sf {

class NativeFunctionHandler;

struct ScaleformValueInfo final {
    std::uint8_t rawType{};
    void* objectInterface{};
    void* data{};
};

[[nodiscard]] ScaleformValueInfo InspectScaleformValue(
    const ScaleformValue& value) noexcept;
[[nodiscard]] bool IsObjectLikeScaleformValue(
    const ScaleformValue& value) noexcept;
// True when a GetVariable call succeeded and produced an object-typed value.
[[nodiscard]] bool IsObjectReadback(
    bool readable,
    const ScaleformValue& value) noexcept;

// Writes a type word and an 8-byte payload at the offsets Scaleform reads back.
// Any other bytes of the value are zeroed.
template <typename T>
void InitializeScaleformValue(
    ScaleformValue& value,
    std::uint64_t type,
    T payload) noexcept {
    static_assert(sizeof(T) <= sizeof(std::uint64_t));
    value = {};
    std::memcpy(value.storage + layout::kValueTypeOffset, &type, sizeof(type));
    std::memcpy(
        value.storage + layout::kValueInternalTwoOffset,
        &payload,
        sizeof(payload));
}

// Reads a routine pointer from the movie root's vtable. Returns nullptr when
// the movie root or its vtable pointer is null.
template <typename Routine>
[[nodiscard]] Routine GetMovieRootRoutine(
    void* movieRoot,
    std::size_t offset) noexcept {
    if (movieRoot == nullptr) {
        return nullptr;
    }
    void* const vtable = *reinterpret_cast<void**>(movieRoot);
    return vtable == nullptr
        ? nullptr
        : *reinterpret_cast<Routine*>(
            static_cast<std::byte*>(vtable) + offset);
}

// Decodes a string held by an owned or borrowed GFxValue. The returned view
// aliases Scaleform-owned storage and is only valid while the value remains
// alive.
[[nodiscard]] bool TryDecodeScaleformValueString(
    const ScaleformValue& value,
    std::string_view& decodedString,
    std::size_t maximumBytesToInspect =
        layout::kMaxScaleformStringLength) noexcept;

using ScaleformGetMemberRoutine = bool(__fastcall*)(
    void*, void*, const char*, ScaleformValue*, bool);
using ScaleformSetMemberRoutine = bool(__fastcall*)(
    void*, void*, const char*, ScaleformValue*, bool);
using ScaleformReleaseValueRoutine = void(__fastcall*)(
    void*, ScaleformValue*, void*);
// ObjectInterface::Invoke. result may be null; arguments use the
// layout::kValueArrayStride element stride.
using ScaleformInvokeRoutine = bool(__fastcall*)(
    void* objectInterface,
    void* data,
    ScaleformValue* result,
    const char* name,
    const void* arguments,
    std::uint64_t argumentCount,
    bool isDisplayObject);

struct ResolvedScaleformApi final {
    ScaleformGetMemberRoutine getMember{};
    ScaleformSetMemberRoutine setMember{};
    ScaleformReleaseValueRoutine releaseValue{};
    // Null when the target profile has no invoke offset.
    ScaleformInvokeRoutine invoke{};
};

// Most arguments one Invoke call can pass.
inline constexpr std::uint32_t kMaxInvokeArguments = 16;

// Value array in the layout the game reads: elements are
// layout::kValueArrayStride bytes apart.
struct alignas(16) PackedScaleformArguments final {
    std::byte records[kMaxInvokeArguments * layout::kValueArrayStride]{};
};

// Copies the first layout::kValueArrayStride bytes of each 0x40-byte value
// into packed. Returns false when count is above kMaxInvokeArguments or
// arguments is null with a nonzero count.
[[nodiscard]] bool PackScaleformArguments(
    const ScaleformValue* arguments,
    std::uint32_t argumentCount,
    PackedScaleformArguments& packed) noexcept;

enum class ResolveScaleformApiStatus {
    Resolved,
    AddressOverflow,
    TargetNotExecutable,
};

[[nodiscard]] ResolveScaleformApiStatus ResolveScaleformApi(
    const TargetProfile& profile,
    std::uintptr_t moduleBase,
    platform::VirtualQueryFn virtualQuery,
    ResolvedScaleformApi& resolved) noexcept;

struct MovieRootContext final {
    void* movieRoot{};
    MovieRootGetVariable originalGetVariable{};
    ResolvedScaleformApi api{};
    NativeFunctionHandler* functionHandler{};
};

[[nodiscard]] bool GetScaleformVariable(
    const MovieRootContext& context,
    const char* path,
    ScaleformValue& value) noexcept;

[[nodiscard]] bool HasScaleformObject(
    const MovieRootContext& context,
    const char* path) noexcept;

[[nodiscard]] bool GetScaleformBoolean(
    const MovieRootContext& context,
    const char* path,
    bool& result) noexcept;

[[nodiscard]] bool GetScaleformNumber(
    const MovieRootContext& context,
    const char* path,
    double& result) noexcept;

[[nodiscard]] bool GetScaleformString(
    const MovieRootContext& context,
    const char* path,
    std::string& result) noexcept;

[[nodiscard]] bool GetScaleformMember(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* memberName,
    ScaleformValue& outValue) noexcept;

// Calls object.memberName(arguments). Fails when the target has no invoke
// routine. arguments are 0x40-byte values and are packed for the game.
[[nodiscard]] bool InvokeScaleformMember(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* memberName,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount,
    ScaleformValue* outValue) noexcept;

[[nodiscard]] bool CreateScaleformObject(
    const MovieRootContext& context,
    const char* className,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount,
    ScaleformValue& outValue) noexcept;

[[nodiscard]] bool CreateUnnamedScaleformObject(
    const MovieRootContext& context,
    ScaleformValue& outValue) noexcept;

[[nodiscard]] bool InvokeScaleformMethod(
    const MovieRootContext& context,
    const char* methodPath,
    ScaleformValue& outValue,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount) noexcept;

[[nodiscard]] bool SetScaleformMember(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* memberName,
    ScaleformValue& memberValue) noexcept;

void InitializeBorrowedScaleformString(
    ScaleformValue& value,
    const char* text) noexcept;
void InitializeScaleformBoolean(
    ScaleformValue& value,
    bool boolean) noexcept;

void ReleaseValue(
    const MovieRootContext& context,
    ScaleformValue& value) noexcept;

// An owned value is a node of a doubly linked list through its first 16
// bytes: +0x00 points to the next node, whose +0x08 points back, and +0x08 to
// the previous node, whose +0x00 points forward. The release routine
// (Steam 0x2584610, Game Pass 0x25751A0) unlinks the node this way. After the
// bytes of an owned value are copied to a new address, call this on the copy
// so the neighbours point at it; the old address must not be used again.
void RelinkMovedScaleformValue(ScaleformValue& moved) noexcept;

// Releases an owned Scaleform value on every exit path unless ownership is transferred.
class ScopedScaleformValue final {
public:
    ScopedScaleformValue(const MovieRootContext& context, ScaleformValue& value) noexcept;
    ~ScopedScaleformValue();

    ScopedScaleformValue(const ScopedScaleformValue&) = delete;
    ScopedScaleformValue& operator=(const ScopedScaleformValue&) = delete;
    ScopedScaleformValue(ScopedScaleformValue&& other) noexcept;
    ScopedScaleformValue& operator=(ScopedScaleformValue&&) = delete;

private:
    const MovieRootContext* context_{};
    ScaleformValue* value_{};
};

}
