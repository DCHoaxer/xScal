#pragma once

#include "scaleform/abi.hpp"
#include "scaleform/value.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sf {

class ScaleformCallbackRouter;

enum class ScaleformResultKind {
    Undefined,
    Boolean,
    Int32,
    UInt32,
    Number,
    String,
    // An owned Scaleform value, such as an object built for the caller.
    OwnedValue,
};

struct ScaleformResult final {
    ScaleformResultKind kind{ScaleformResultKind::Undefined};
    bool booleanValue{};
    std::int32_t int32Value{};
    std::uint32_t uint32Value{};
    double numberValue{};
    std::string stringValue;
    ScaleformValue ownedValue{};
    // Releases ownedValue when the result is destroyed without being committed.
    ScaleformReleaseValueRoutine releaseOwnedValue{};

    ScaleformResult() noexcept = default;
    ScaleformResult(const ScaleformResult&) = delete;
    ScaleformResult& operator=(const ScaleformResult&) = delete;

    // Supports result = {} between calls. Releases the owned value this result
    // still holds and takes over the other result's.
    ScaleformResult& operator=(ScaleformResult&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        ReleaseOwnedValue();
        kind = other.kind;
        booleanValue = other.booleanValue;
        int32Value = other.int32Value;
        uint32Value = other.uint32Value;
        numberValue = other.numberValue;
        stringValue.swap(other.stringValue);
        if (other.kind == ScaleformResultKind::OwnedValue) {
            SetOwnedValue(other.ownedValue, other.releaseOwnedValue);
            other.ForgetOwnedValue();
        }
        return *this;
    }

    ~ScaleformResult() { ReleaseOwnedValue(); }

    void SetBoolean(bool value) noexcept {
        booleanValue = value;
        kind = ScaleformResultKind::Boolean;
    }

    void SetInt32(std::int32_t value) noexcept {
        int32Value = value;
        kind = ScaleformResultKind::Int32;
    }

    void SetUInt32(std::uint32_t value) noexcept {
        uint32Value = value;
        kind = ScaleformResultKind::UInt32;
    }

    void SetNumber(double value) noexcept {
        numberValue = value;
        kind = ScaleformResultKind::Number;
    }

    [[nodiscard]] bool SetString(std::string_view value) noexcept {
        if (value.size() > layout::kMaxReturnStringLength) {
            return false;
        }
        try {
            stringValue.assign(value);
            kind = ScaleformResultKind::String;
            return true;
        } catch (...) {
            return false;
        }
    }

    // Takes over value, an owned value of the movie whose release routine is
    // passed. value is left empty. Owned values are list nodes, so the copy is
    // relinked at its new address.
    void SetOwnedValue(
        ScaleformValue& value,
        ScaleformReleaseValueRoutine release) noexcept {
        std::memcpy(&ownedValue, &value, sizeof(value));
        RelinkMovedScaleformValue(ownedValue);
        value = {};
        releaseOwnedValue = release;
        kind = ScaleformResultKind::OwnedValue;
    }

    // Called after ownedValue was moved into the game's result value.
    void ForgetOwnedValue() noexcept {
        ownedValue = {};
        releaseOwnedValue = nullptr;
        kind = ScaleformResultKind::Undefined;
    }

private:
    void ReleaseOwnedValue() noexcept {
        if (kind != ScaleformResultKind::OwnedValue) {
            return;
        }
        MovieRootContext context{};
        context.api.releaseValue = releaseOwnedValue;
        ReleaseValue(context, ownedValue);
        ForgetOwnedValue();
    }
};

// Internal view of the ActionScript argument array.
struct ScaleformCall final {
    const void* arguments{};
    std::size_t argumentCount{};
    ScaleformResult* result{};
    void* movie{};
    void* thisObject{};
    void* argumentFrame{};
};

// The native FunctionHandler reads its parameter pointer from
// RDX, an argument-array pointer from +0x20, and a 32-bit count from +0x28.
// Arguments are 48-byte records; their string representation is decoded only
// by TryDecodeScaleformString().
struct FunctionParams final {
    void* result{};
    void* movie{};
    ScaleformValue* thisObject{};
    // Hidden argument-frame record immediately preceding the explicit args.
    void* argumentFrame{};
    const std::byte* arguments{};
    std::uint32_t argumentCount{};
};

static_assert(offsetof(FunctionParams, result) == 0x00);
static_assert(offsetof(FunctionParams, movie) == 0x08);
static_assert(offsetof(FunctionParams, thisObject) == 0x10);
static_assert(offsetof(FunctionParams, argumentFrame) == 0x18);
static_assert(offsetof(FunctionParams, arguments) == 0x20);
static_assert(offsetof(FunctionParams, argumentCount) == 0x28);
static_assert(sizeof(FunctionParams) == 0x30);

class NativeFunctionHandler final {
public:
    NativeFunctionHandler() noexcept;

private:
    using HandlerVtableEntry = void(__fastcall*)();

    [[nodiscard]] static void* __fastcall Identity(void* handler) noexcept;
    static void __fastcall Invoke(void* handler, const FunctionParams* params) noexcept;

    const HandlerVtableEntry* vtable_;
    // GFxFunctionHandler derives from GRefCountBase. Scaleform owns this
    // prefix and performs 32-bit interlocked ref-count operations at +0x08.
    volatile std::int32_t referenceCount_;
    std::uint32_t referenceCountPadding_;

};

// NativeFunctionHandler cannot store a router pointer because the
// GFx object is exactly 0x10 bytes. The active MovieRoot hook publishes the one
// process-wide router. Clearing it waits for callbacks already dispatching
// through that router to finish.
[[nodiscard]] bool PublishNativeCallbackRouter(
    ScaleformCallbackRouter& callbackRouter) noexcept;
void ClearNativeCallbackRouter(
    ScaleformCallbackRouter& callbackRouter) noexcept;

// Encodes one callback result into the GFx return value. Direct
// native methods reuse this path without going through __SFCodeObj.call.
// An OwnedValue result is moved into the game's value and left Undefined.
[[nodiscard]] bool CommitScaleformResult(
    const FunctionParams& params,
    ScaleformResult& result) noexcept;

// Writes a string value into a native call's result. The text is copied into a
// per-thread buffer that stays valid until the next string result on this
// thread. Fails when the text is longer than layout::kMaxReturnStringLength.
[[nodiscard]] bool WriteScaleformStringResult(
    void* rawResult,
    std::string_view text) noexcept;

// One type/payload pair of a 0x30-byte argument record. The payload is the
// raw 8 bytes: an immediate value or a pointer, depending on the type.
struct ArgumentCandidate final {
    std::uint64_t type;
    std::uint64_t payload;
};

struct ArgumentCandidates final {
    ArgumentCandidate primary;   // type at +0x18, payload at +0x20
    ArgumentCandidate fallback;  // type at +0x08, payload at +0x10
};

// Reads both pairs of an argument record. The game tries the primary pair
// first and the fallback pair when the primary one is not usable.
[[nodiscard]] ArgumentCandidates ReadArgumentCandidates(
    const std::byte* argumentRecord) noexcept;

// Parses one 48-byte FunctionParams argument using the two candidates
// above. The returned name aliases game-owned storage.
[[nodiscard]] bool TryDecodeScaleformString(
    const std::byte* argumentRecord,
    std::string_view& decodedString,
    std::size_t maximumBytesToInspect =
        layout::kMaxScaleformStringLength) noexcept;

// Safely addresses and decodes one argument from a callback invocation.
[[nodiscard]] bool TryGetScaleformStringArgument(
    const ScaleformCall& call,
    std::size_t argumentIndex,
    std::string_view& decodedString,
    std::size_t maximumBytesToInspect =
        layout::kMaxScaleformStringLength) noexcept;

}
