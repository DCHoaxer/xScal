#pragma once

#include <cstddef>
#include <cstdint>

namespace sf::layout {

inline constexpr std::size_t kValueSize = 0x40;
inline constexpr std::size_t kValueInternalOneOffset = 0x10;
inline constexpr std::size_t kValueTypeOffset = 0x18;
inline constexpr std::size_t kValueInternalTwoOffset = 0x20;
inline constexpr std::size_t kFunctionArgumentSize = 0x30;
// Value arrays passed to MovieRoot::Invoke and ObjectInterface::Invoke use the
// same 0x30-byte element stride as native call arguments, not the 0x40 size
// of a single value.
inline constexpr std::size_t kValueArrayStride = kFunctionArgumentSize;
// The userData given to MovieRoot::CreateFunction is stored in the function
// object at +0x50 and passed to the handler at FunctionParams + 0x30 (Steam
// 1.7.26.22, function object execute 0x25A1DC0: mov rax,[rsi+0x50];
// mov [rbp+8],rax with the parameters at rbp-0x28).
inline constexpr std::size_t kFunctionParamsUserDataOffset = 0x30;
inline constexpr std::size_t kPrimaryArgumentTypeOffset = 0x18;
inline constexpr std::size_t kPrimaryArgumentPointerOffset = 0x20;
inline constexpr std::size_t kFallbackArgumentTypeOffset = 0x08;
inline constexpr std::size_t kFallbackArgumentPointerOffset = 0x10;
// MovieRoot::AdvanceFrame(movieRoot, nextFrame); the full-body plugin hooks
// the same slot.
inline constexpr std::size_t kAdvanceFrameVtableOffset = 0x10;
inline constexpr std::size_t kDestroyVtableOffset = 0x00;
inline constexpr std::size_t kCreateObjectVtableOffset = 0x170;
inline constexpr std::size_t kCreateFunctionVtableOffset = 0x180;
inline constexpr std::size_t kGetVariableVtableOffset = 0x190;
inline constexpr std::size_t kInvokeVtableOffset = 0x1C8;
inline constexpr std::uint8_t kTypeMask = 0x8F;
inline constexpr std::uint8_t kOwnedValueFlag = 0x40;
inline constexpr std::uint8_t kBooleanType = 2;
inline constexpr std::uint8_t kIntType = 3;
inline constexpr std::uint8_t kUIntType = 4;
inline constexpr std::uint8_t kNumberType = 5;
inline constexpr std::uint8_t kStringType = 6;
inline constexpr std::uint8_t kObjectType = 8;
inline constexpr std::uint8_t kDisplayObjectType = 10;
inline constexpr std::size_t kMaxScaleformStringLength = 0x1000000;
inline constexpr std::size_t kMaxReturnStringLength = 5 * 1024 * 1024;

}
