#include "scaleform/native_handler.hpp"

#include "api/scaleform_callbacks.hpp"
#include "platform/diagnostics.hpp"
#include "scaleform/memory_access.hpp"
#include "scaleform/scaleform_layout.hpp"
#include "scaleform/value.hpp"

#include <bit>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace sf {
namespace {

std::mutex callbackRouterMutex;
std::condition_variable callbackRouterIdle;
ScaleformCallbackRouter* activeCallbackRouter{};
unsigned callbackDispatchInFlight{};
thread_local std::string returnStringBuffer;

// Encodes a scalar or string payload as the GFx value written to a native call's result.
template <typename T>
[[nodiscard]] bool WriteResultValue(
    void* rawResult,
    std::uint64_t type,
    T payload) noexcept {
    if (rawResult == nullptr) {
        return false;
    }
    ScaleformValue value{};
    InitializeScaleformValue(value, type, payload);
    std::memcpy(rawResult, &value, sizeof(value));
    return true;
}

// The returned pointer refers to returnStringBuffer, which stays valid until the
// next string result on this thread.
[[nodiscard]] bool WriteStringResult(
    void* rawResult,
    const char* text,
    std::size_t textLength) noexcept {
    if (text == nullptr || textLength > layout::kMaxReturnStringLength) {
        return false;
    }

    try {
        returnStringBuffer.assign(text, textLength);
    }
    catch (...) {
        return false;
    }
    return WriteResultValue(rawResult, layout::kStringType, returnStringBuffer.c_str());
}


[[nodiscard]] bool IsScaleformStringByteAllowed(unsigned char value) noexcept {
    return value >= 0x20 || value == '\t' || value == '\n' || value == '\r';
}

[[nodiscard]] bool TryReadStringPointer(
    const void* address,
    const char*& text) noexcept {
    text = nullptr;
    __try {
        std::memcpy(&text, address, sizeof(text));
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        text = nullptr;
        return false;
    }
    return text != nullptr;
}

[[nodiscard]] bool IsUtf8Continuation(unsigned char value) noexcept {
    return value >= 0x80 && value <= 0xBF;
}

[[nodiscard]] bool TryValidateScaleformString(
    const char* text,
    std::size_t maximumBytesToInspect,
    std::size_t& length) noexcept {
    length = 0;
    if (text == nullptr || maximumBytesToInspect == 0) {
        return false;
    }

    __try {
        std::size_t index{};
        while (index < maximumBytesToInspect) {
            const auto first = static_cast<unsigned char>(text[index]);
            if (first == 0) {
                length = index;
                return true;
            }
            if (!IsScaleformStringByteAllowed(first)) {
                return false;
            }
            if (first <= 0x7F) {
                ++index;
                continue;
            }

            std::size_t sequenceLength{};
            if (first >= 0xC2 && first <= 0xDF) {
                sequenceLength = 2;
            } else if (first >= 0xE0 && first <= 0xEF) {
                sequenceLength = 3;
            } else if (first >= 0xF0 && first <= 0xF4) {
                sequenceLength = 4;
            } else {
                return false;
            }
            if (sequenceLength > maximumBytesToInspect - index) {
                return false;
            }

            const auto second =
                static_cast<unsigned char>(text[index + 1]);
            if (!IsUtf8Continuation(second) ||
                (first == 0xE0 && second < 0xA0) ||
                (first == 0xED && second > 0x9F) ||
                (first == 0xF0 && second < 0x90) ||
                (first == 0xF4 && second > 0x8F)) {
                return false;
            }
            for (std::size_t continuation = 2;
                 continuation < sequenceLength;
                 ++continuation) {
                if (!IsUtf8Continuation(
                        static_cast<unsigned char>(
                            text[index + continuation]))) {
                    return false;
                }
            }
            index += sequenceLength;
        }
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        length = 0;
        return false;
    }
    return false;
}

[[nodiscard]] bool DecodeStringCandidate(
    std::uint64_t encodedType,
    const void* encodedPointer,
    std::string_view& decodedString,
    std::size_t maximumBytesToInspect) noexcept {
    if ((static_cast<unsigned char>(encodedType) & layout::kTypeMask) != layout::kStringType ||
        encodedPointer == nullptr) {
        return false;
    }

    const char* text = static_cast<const char*>(encodedPointer);
    if ((static_cast<unsigned char>(encodedType) & layout::kOwnedValueFlag) != 0) {
        const auto pointerBits = reinterpret_cast<std::uintptr_t>(encodedPointer);
        if ((pointerBits & 7) != 0 ||
            !TryReadStringPointer(encodedPointer, text)) {
            return false;
        }
    }

    if (text == nullptr) {
        return false;
    }

    std::size_t length{};
    if (!TryValidateScaleformString(
            text, maximumBytesToInspect, length)) {
        return false;
    }
    decodedString = std::string_view{text, length};
    return true;
}

}  // namespace

NativeFunctionHandler::NativeFunctionHandler() noexcept
    : vtable_{nullptr},
      referenceCount_{1},
      referenceCountPadding_{0} {
    static_assert(offsetof(NativeFunctionHandler, vtable_) == 0x00);
    static_assert(offsetof(NativeFunctionHandler, referenceCount_) == 0x08);
    static_assert(offsetof(NativeFunctionHandler, referenceCountPadding_) == 0x0C);
    static_assert(sizeof(NativeFunctionHandler) == 0x10);
    static const HandlerVtableEntry kVtable[] = {
        reinterpret_cast<HandlerVtableEntry>(&NativeFunctionHandler::Identity),
        reinterpret_cast<HandlerVtableEntry>(&NativeFunctionHandler::Invoke),
    };
    vtable_ = kVtable;
}

bool PublishNativeCallbackRouter(
    ScaleformCallbackRouter& callbackRouter) noexcept {
    std::lock_guard lock{callbackRouterMutex};
    if (activeCallbackRouter != nullptr &&
        activeCallbackRouter != &callbackRouter) {
        return false;
    }
    activeCallbackRouter = &callbackRouter;
    return true;
}

void ClearNativeCallbackRouter(
    ScaleformCallbackRouter& callbackRouter) noexcept {
    std::unique_lock lock{callbackRouterMutex};
    if (activeCallbackRouter == &callbackRouter) {
        activeCallbackRouter = nullptr;
    }
    // Must not be called from inside a dispatch on this same thread.
    callbackRouterIdle.wait(lock, [] { return callbackDispatchInFlight == 0; });
}

bool CommitScaleformResult(
    const FunctionParams& params,
    const ScaleformResult& result) noexcept {
    switch (result.kind) {
        case ScaleformResultKind::Boolean:
            return WriteResultValue(
                params.result,
                layout::kBooleanType,
                std::uint64_t{result.booleanValue ? 1U : 0U});
        case ScaleformResultKind::Int32:
            return WriteResultValue(
                params.result,
                layout::kIntType,
                result.int32Value);
        case ScaleformResultKind::UInt32:
            return WriteResultValue(
                params.result,
                layout::kUIntType,
                result.uint32Value);
        case ScaleformResultKind::Number:
            return WriteResultValue(
                params.result,
                layout::kNumberType,
                result.numberValue);
        case ScaleformResultKind::String:
            return WriteStringResult(
                params.result,
                result.stringValue.data(),
                result.stringValue.size());
        default:
            return false;
    }
}

bool WriteScaleformStringResult(
    void* rawResult,
    std::string_view text) noexcept {
    return WriteStringResult(rawResult, text.data(), text.size());
}

void* __fastcall NativeFunctionHandler::Identity(void* handler) noexcept {
    return handler;
}

void __fastcall NativeFunctionHandler::Invoke(
    void* handler,
    const FunctionParams* params) noexcept {
    if (handler == nullptr || params == nullptr ||
        params->arguments == nullptr || params->argumentCount == 0) {
        return;
    }

    std::string_view callbackName;
    if (!TryDecodeScaleformString(
            params->arguments,
            callbackName,
            ScaleformCallbackRouter::kMaximumNameLength + 1)) {
        DiagnosticLogFormat(
            "Scaleform call: callback name is not a decodable string (arguments=%zu)",
            params->argumentCount);
        return;
    }
    ScaleformResult callbackResult;
    const ScaleformCall call{
        params->arguments,
        params->argumentCount,
        &callbackResult,
        params->movie,
        params->thisObject,
        params->argumentFrame,
    };
    ScaleformCallbackRouter* router{};
    {
        std::lock_guard lock{callbackRouterMutex};
        router = activeCallbackRouter;
        if (router == nullptr) {
            DiagnosticLogFormat(
                "Scaleform call: no active router (name=%.*s, arguments=%zu)",
                static_cast<int>(callbackName.size()),
                callbackName.data(),
                params->argumentCount);
            return;
        }
        ++callbackDispatchInFlight;
    }

    const bool dispatched = router->Dispatch(callbackName, call);

    {
        std::lock_guard lock{callbackRouterMutex};
        if (--callbackDispatchInFlight == 0) {
            callbackRouterIdle.notify_all();
        }
    }

    if (!dispatched) {
        DiagnosticLogFormat(
            "Scaleform call: dispatch failed (name=%.*s, arguments=%zu)",
            static_cast<int>(callbackName.size()),
            callbackName.data(),
            params->argumentCount);
        return;
    }
    (void)CommitScaleformResult(*params, callbackResult);
}

ArgumentCandidates ReadArgumentCandidates(
    const std::byte* argumentRecord) noexcept {
    ArgumentCandidates candidates{};
    std::memcpy(
        &candidates.primary.type,
        argumentRecord + layout::kPrimaryArgumentTypeOffset,
        sizeof(candidates.primary.type));
    std::memcpy(
        &candidates.primary.payload,
        argumentRecord + layout::kPrimaryArgumentPointerOffset,
        sizeof(candidates.primary.payload));
    std::memcpy(
        &candidates.fallback.type,
        argumentRecord + layout::kFallbackArgumentTypeOffset,
        sizeof(candidates.fallback.type));
    std::memcpy(
        &candidates.fallback.payload,
        argumentRecord + layout::kFallbackArgumentPointerOffset,
        sizeof(candidates.fallback.payload));
    return candidates;
}

bool TryDecodeScaleformString(
    const std::byte* argumentRecord,
    std::string_view& decodedString,
    std::size_t maximumBytesToInspect) noexcept {
    decodedString = {};
    if (argumentRecord == nullptr || maximumBytesToInspect == 0) {
        return false;
    }

    // +0x18/+0x20 is the preferred pair; the game retries +0x08/+0x10 when
    // the preferred pair is not a string.
    const auto candidates = ReadArgumentCandidates(argumentRecord);
    if (DecodeStringCandidate(
            candidates.primary.type,
            std::bit_cast<const void*>(candidates.primary.payload),
            decodedString,
            maximumBytesToInspect)) {
        return true;
    }

    return DecodeStringCandidate(
        candidates.fallback.type,
        std::bit_cast<const void*>(candidates.fallback.payload),
        decodedString,
        maximumBytesToInspect);
}

bool TryDecodeScaleformValueString(
    const ScaleformValue& value,
    std::string_view& decodedString,
    std::size_t maximumBytesToInspect) noexcept {
    decodedString = {};
    const auto info = InspectScaleformValue(value);
    return DecodeStringCandidate(
        info.rawType,
        info.data,
        decodedString,
        maximumBytesToInspect);
}

bool TryGetScaleformStringArgument(
    const ScaleformCall& call,
    std::size_t argumentIndex,
    std::string_view& decodedString,
    std::size_t maximumBytesToInspect) noexcept {
    decodedString = {};
    if (call.arguments == nullptr || argumentIndex >= call.argumentCount ||
        argumentIndex > (std::numeric_limits<std::size_t>::max)() / layout::kFunctionArgumentSize) {
        return false;
    }
    const auto* arguments = static_cast<const std::byte*>(call.arguments);
    return TryDecodeScaleformString(
        arguments + argumentIndex * layout::kFunctionArgumentSize,
        decodedString,
        maximumBytesToInspect);
}

}  // namespace sf
