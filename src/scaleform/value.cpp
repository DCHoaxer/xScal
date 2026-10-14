#include "scaleform/value.hpp"

#include "platform/native_access.hpp"
#include "scaleform/scaleform_layout.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace sf {
namespace {

using CreateObjectRoutine = void(__fastcall*)(
    void*, ScaleformValue*, const char*, const ScaleformValue*, unsigned int);

template <typename T>
[[nodiscard]] T ReadValue(
    const ScaleformValue& value,
    std::size_t offset) noexcept {
    T result{};
    std::memcpy(&result, value.storage + offset, sizeof(result));
    return result;
}

template <typename T>
[[nodiscard]] T ReadPayloadValue(const ScaleformValue& value) noexcept {
    static_assert(sizeof(T) <= sizeof(void*));
    return ReadValue<T>(value, layout::kValueInternalTwoOffset);
}

}

ScaleformValueInfo InspectScaleformValue(const ScaleformValue& value) noexcept {
    return {
        ReadValue<std::uint8_t>(value, layout::kValueTypeOffset),
        ReadValue<void*>(value, layout::kValueInternalOneOffset),
        ReadValue<void*>(value, layout::kValueInternalTwoOffset),
    };
}

bool IsObjectLikeScaleformValue(const ScaleformValue& value) noexcept {
    const auto info = InspectScaleformValue(value);
    const auto baseType = static_cast<std::uint8_t>(
        info.rawType & layout::kTypeMask);
    return baseType >= layout::kObjectType &&
        baseType <= layout::kDisplayObjectType &&
        info.objectInterface != nullptr && info.data != nullptr;
}

bool IsObjectReadback(bool readable, const ScaleformValue& value) noexcept {
    return readable && IsObjectLikeScaleformValue(value);
}

bool GetScaleformVariable(
    const MovieRootContext& context,
    const char* path,
    ScaleformValue& value) noexcept {
    value = {};
    return context.movieRoot != nullptr &&
        context.originalGetVariable != nullptr &&
        context.originalGetVariable(context.movieRoot, &value, path, 0U);
}

bool HasScaleformObject(
    const MovieRootContext& context,
    const char* path) noexcept {
    ScaleformValue value{};
    ScopedScaleformValue release{context, value};
    return GetScaleformVariable(context, path, value) &&
        IsObjectLikeScaleformValue(value);
}

bool GetScaleformBoolean(
    const MovieRootContext& context,
    const char* path,
    bool& result) noexcept {
    ScaleformValue value{};
    ScopedScaleformValue release{context, value};
    if (!GetScaleformVariable(context, path, value)) {
        return false;
    }
    const auto type = static_cast<std::uint8_t>(
        InspectScaleformValue(value).rawType & layout::kTypeMask);
    if (type != layout::kBooleanType) {
        return false;
    }
    result = ReadPayloadValue<std::uint64_t>(value) != 0;
    return true;
}

bool GetScaleformNumber(
    const MovieRootContext& context,
    const char* path,
    double& result) noexcept {
    ScaleformValue value{};
    ScopedScaleformValue release{context, value};
    if (!GetScaleformVariable(context, path, value)) {
        return false;
    }

    const auto type = static_cast<std::uint8_t>(
        InspectScaleformValue(value).rawType & layout::kTypeMask);
    switch (type) {
        case layout::kBooleanType:
            result = ReadPayloadValue<std::uint64_t>(value) != 0
                ? 1.0
                : 0.0;
            break;
        case layout::kIntType:
            result = static_cast<double>(
                ReadPayloadValue<std::int32_t>(value));
            break;
        case layout::kUIntType:
            result = static_cast<double>(
                ReadPayloadValue<std::uint32_t>(value));
            break;
        case layout::kNumberType:
            result = ReadPayloadValue<double>(value);
            break;
        default:
            return false;
    }
    return std::isfinite(result);
}

bool GetScaleformString(
    const MovieRootContext& context,
    const char* path,
    std::string& result) noexcept {
    result.clear();
    ScaleformValue value{};
    ScopedScaleformValue release{context, value};
    if (!GetScaleformVariable(context, path, value)) {
        return false;
    }
    std::string_view decoded;
    if (!TryDecodeScaleformValueString(value, decoded)) {
        return false;
    }
    try {
        result.assign(decoded);
        return true;
    } catch (...) {
        result.clear();
        return false;
    }
}

ResolveScaleformApiStatus ResolveScaleformApi(
    const TargetProfile& profile,
    std::uintptr_t moduleBase,
    platform::VirtualQueryFn virtualQuery,
    ResolvedScaleformApi& resolved) noexcept {
    resolved = {};
    std::uintptr_t getMember{};
    std::uintptr_t setMember{};
    std::uintptr_t releaseValue{};
    std::uintptr_t invoke{};
    if (!platform::TryAddAddress(
            moduleBase, profile.getMemberOffset, getMember) ||
        !platform::TryAddAddress(
            moduleBase, profile.setMemberOffset, setMember) ||
        !platform::TryAddAddress(
            moduleBase, profile.releaseValueOffset, releaseValue) ||
        getMember == 0 || setMember == 0 || releaseValue == 0) {
        return ResolveScaleformApiStatus::AddressOverflow;
    }
    if (profile.invokeOffset != 0 &&
        (!platform::TryAddAddress(moduleBase, profile.invokeOffset, invoke) ||
         invoke == 0)) {
        return ResolveScaleformApiStatus::AddressOverflow;
    }
    if (!platform::AreExecutableAddresses(
            virtualQuery, {getMember, setMember, releaseValue}) ||
        (invoke != 0 &&
         !platform::AreExecutableAddresses(virtualQuery, {invoke}))) {
        return ResolveScaleformApiStatus::TargetNotExecutable;
    }
    resolved.getMember =
        reinterpret_cast<ScaleformGetMemberRoutine>(getMember);
    resolved.setMember =
        reinterpret_cast<ScaleformSetMemberRoutine>(setMember);
    resolved.releaseValue =
        reinterpret_cast<ScaleformReleaseValueRoutine>(releaseValue);
    resolved.invoke = reinterpret_cast<ScaleformInvokeRoutine>(invoke);
    return ResolveScaleformApiStatus::Resolved;
}

bool GetScaleformMember(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* memberName,
    ScaleformValue& outValue) noexcept {
    if (context.api.getMember == nullptr || memberName == nullptr) {
        return false;
    }
    const auto owner = InspectScaleformValue(object);
    if (owner.objectInterface == nullptr || owner.data == nullptr) {
        return false;
    }
    return context.api.getMember(
        owner.objectInterface,
        owner.data,
        memberName,
        &outValue,
        (owner.rawType & layout::kTypeMask) == layout::kDisplayObjectType);
}

bool PackScaleformArguments(
    const ScaleformValue* arguments,
    std::uint32_t argumentCount,
    PackedScaleformArguments& packed) noexcept {
    if (argumentCount > kMaxInvokeArguments ||
        (argumentCount != 0 && arguments == nullptr)) {
        return false;
    }
    for (std::uint32_t index = 0; index < argumentCount; ++index) {
        std::memcpy(
            packed.records + index * layout::kValueArrayStride,
            arguments[index].storage,
            layout::kValueArrayStride);
    }
    return true;
}

bool InvokeScaleformMember(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* memberName,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount,
    ScaleformValue* outValue) noexcept {
    if (outValue != nullptr) {
        *outValue = {};
    }
    if (context.api.invoke == nullptr || memberName == nullptr) {
        return false;
    }
    const auto owner = InspectScaleformValue(object);
    if (owner.objectInterface == nullptr || owner.data == nullptr) {
        return false;
    }
    PackedScaleformArguments packed;
    if (!PackScaleformArguments(arguments, argumentCount, packed)) {
        return false;
    }
    return context.api.invoke(
        owner.objectInterface,
        owner.data,
        outValue,
        memberName,
        packed.records,
        argumentCount,
        (owner.rawType & layout::kTypeMask) == layout::kDisplayObjectType);
}

namespace {

bool CreateObjectValue(
    const MovieRootContext& context,
    const char* className,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount,
    ScaleformValue& outValue) noexcept {
    outValue = {};
    if (argumentCount != 0 && arguments == nullptr) {
        return false;
    }
    const auto createObject = GetMovieRootRoutine<CreateObjectRoutine>(
        context.movieRoot,
        layout::kCreateObjectVtableOffset);
    if (createObject == nullptr) {
        return false;
    }

    createObject(
        context.movieRoot,
        &outValue,
        className,
        arguments,
        argumentCount);
    if (IsObjectLikeScaleformValue(outValue)) {
        return true;
    }
    ReleaseValue(context, outValue);
    outValue = {};
    return false;
}

}

bool CreateScaleformObject(
    const MovieRootContext& context,
    const char* className,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount,
    ScaleformValue& outValue) noexcept {
    if (className == nullptr) {
        outValue = {};
        return false;
    }
    return CreateObjectValue(
        context,
        className,
        arguments,
        argumentCount,
        outValue);
}

bool CreateUnnamedScaleformObject(
    const MovieRootContext& context,
    ScaleformValue& outValue) noexcept {
    // Bridge objects (version, modStorage, chatInterface) are created without a class name.
    return CreateObjectValue(context, nullptr, nullptr, 0, outValue);
}

bool InvokeScaleformMethod(
    const MovieRootContext& context,
    const char* methodPath,
    ScaleformValue& outValue,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount) noexcept {
    outValue = {};
    const auto invoke = GetMovieRootRoutine<MovieRootInvoke>(
        context.movieRoot,
        layout::kInvokeVtableOffset);
    PackedScaleformArguments packed;
    return invoke != nullptr && methodPath != nullptr &&
        PackScaleformArguments(arguments, argumentCount, packed) &&
        invoke(
            context.movieRoot,
            methodPath,
            &outValue,
            reinterpret_cast<const ScaleformValue*>(packed.records),
            argumentCount);
}

bool SetScaleformMember(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* memberName,
    ScaleformValue& memberValue) noexcept {
    if (context.api.setMember == nullptr || memberName == nullptr) {
        return false;
    }
    const auto owner = InspectScaleformValue(object);
    if (owner.objectInterface == nullptr || owner.data == nullptr) {
        return false;
    }
    const bool isDisplayObject =
        (owner.rawType & layout::kTypeMask) == layout::kDisplayObjectType;
    const auto setMember = [&](bool displayObject) {
        return context.api.setMember(
            owner.objectInterface,
            owner.data,
            memberName,
            &memberValue,
            displayObject);
    };
    if (setMember(isDisplayObject)) {
        return true;
    }
    // Display objects are set with the display-object flag first and retried without it.
    return isDisplayObject && setMember(false);
}

void InitializeBorrowedScaleformString(
    ScaleformValue& value,
    const char* text) noexcept {
    InitializeScaleformValue(value, layout::kStringType, text);
}

void InitializeScaleformBoolean(
    ScaleformValue& value,
    bool boolean) noexcept {
    InitializeScaleformValue(
        value,
        layout::kBooleanType,
        std::uint64_t{boolean ? 1U : 0U});
}

void ReleaseValue(
    const MovieRootContext& context,
    ScaleformValue& value) noexcept {
    if (context.api.releaseValue == nullptr) {
        return;
    }
    const auto info = InspectScaleformValue(value);
    if ((info.rawType & layout::kOwnedValueFlag) != 0 &&
        info.objectInterface != nullptr && info.data != nullptr) {
        context.api.releaseValue(info.objectInterface, &value, info.data);
    }
}

void RelinkMovedScaleformValue(ScaleformValue& moved) noexcept {
    if ((InspectScaleformValue(moved).rawType & layout::kOwnedValueFlag) == 0) {
        return;
    }
    const auto next = ReadValue<std::byte*>(moved, 0x00);
    const auto previous = ReadValue<std::byte*>(moved, 0x08);
    // The release routine writes -1 into both links of a node it unlinked.
    const auto unlinked = reinterpret_cast<std::byte*>(~std::uintptr_t{0});
    if (next == nullptr || previous == nullptr ||
        next == unlinked || previous == unlinked) {
        return;
    }
    std::byte* const self = moved.storage;
    std::memcpy(next + 0x08, &self, sizeof(self));
    std::memcpy(previous + 0x00, &self, sizeof(self));
}

ScopedScaleformValue::ScopedScaleformValue(
    const MovieRootContext& context,
    ScaleformValue& value) noexcept
    : context_{&context}, value_{&value} {}

ScopedScaleformValue::~ScopedScaleformValue() {
    if (context_ != nullptr && value_ != nullptr) {
        ReleaseValue(*context_, *value_);
    }
}

ScopedScaleformValue::ScopedScaleformValue(
    ScopedScaleformValue&& other) noexcept
    : context_{std::exchange(other.context_, nullptr)},
      value_{std::exchange(other.value_, nullptr)} {}

}
