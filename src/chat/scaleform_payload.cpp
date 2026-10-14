#include "chat/scaleform_payload.hpp"

#include "chat/chat_types.hpp"
#include "hook/movie_root_hook.hpp"
#include "scaleform/native_handler.hpp"
#include "scaleform/scaleform_layout.hpp"
#include "scaleform/value.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace sf::chat {
namespace {

enum class RequestedType {
    String,
    Boolean,
    Integer,
};

struct MemberRead final {
    const ScaleformValue* object{};
    const char* name{};
    RequestedType type{};
    PayloadMemberStatus status{PayloadMemberStatus::Missing};
    std::string* stringValue{};
    bool* booleanValue{};
    std::int64_t* integerValue{};
};

template <typename Type>
[[nodiscard]] Type ReadStoredValue(
    const ScaleformValue& value,
    std::size_t offset) noexcept {
    Type output{};
    std::memcpy(&output, value.storage + offset, sizeof(output));
    return output;
}

[[nodiscard]] bool ReadMemberValue(
    const ScaleformValue& value,
    MemberRead& read) noexcept {
    const auto info = InspectScaleformValue(value);
    const auto type = static_cast<std::uint8_t>(
        info.rawType & layout::kTypeMask);
    if (read.type == RequestedType::String) {
        std::string_view text;
        if (type != layout::kStringType ||
            !TryDecodeScaleformValueString(
                value, text, kMaximumRelayMessageBytes + 1)) {
            return false;
        }
        try {
            read.stringValue->assign(text);
            return true;
        } catch (...) {
            return false;
        }
    }
    if (read.type == RequestedType::Boolean) {
        if (type != layout::kBooleanType) {
            return false;
        }
        *read.booleanValue =
            ReadStoredValue<std::uint64_t>(
                value, layout::kValueInternalTwoOffset) != 0;
        return true;
    }
    if (type == layout::kIntType) {
        *read.integerValue = ReadStoredValue<std::int32_t>(
            value, layout::kValueInternalTwoOffset);
        return true;
    }
    if (type == layout::kUIntType) {
        *read.integerValue = ReadStoredValue<std::uint32_t>(
            value, layout::kValueInternalTwoOffset);
        return true;
    }
    if (type != layout::kNumberType) {
        return false;
    }
    const double number = ReadStoredValue<double>(
        value, layout::kValueInternalTwoOffset);
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < static_cast<double>(
            (std::numeric_limits<std::int64_t>::min)()) ||
        number >= static_cast<double>(
            (std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    *read.integerValue = static_cast<std::int64_t>(number);
    return true;
}

[[nodiscard]] bool VisitMember(
    const MovieRootContext& context,
    void* userData) noexcept {
    auto& read = *static_cast<MemberRead*>(userData);
    ScaleformValue value{};
    if (!GetScaleformMember(
            context,
            const_cast<ScaleformValue&>(*read.object),
            read.name,
            value)) {
        return false;
    }
    ScopedScaleformValue release{context, value};
    read.status = ReadMemberValue(value, read)
        ? PayloadMemberStatus::Found
        : PayloadMemberStatus::WrongType;
    return true;
}

// Reads one member from a ScaleformObject argument. The member name is copied
// because the GetMember call needs a NUL-terminated name.
[[nodiscard]] PayloadMemberStatus ReadObjectMember(
    const ScaleformCall& call,
    const ScaleformValue& object,
    std::string_view name,
    MemberRead& read) noexcept {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        return PayloadMemberStatus::Missing;
    }
    std::string memberName;
    try {
        memberName.assign(name);
    } catch (...) {
        return PayloadMemberStatus::WrongType;
    }
    read.object = &object;
    read.name = memberName.c_str();
    read.status = PayloadMemberStatus::Missing;
    (void)VisitTrackedMovieRoots(call.movie, &VisitMember, &read);
    return read.status;
}

[[nodiscard]] PayloadMemberStatus JsonStatus(
    json::MemberStatus status) noexcept {
    switch (status) {
    case json::MemberStatus::Found:
        return PayloadMemberStatus::Found;
    case json::MemberStatus::WrongType:
        return PayloadMemberStatus::WrongType;
    case json::MemberStatus::Missing:
        return PayloadMemberStatus::Missing;
    }
    return PayloadMemberStatus::Missing;
}

}  // namespace

ScaleformPayload::ScaleformPayload(
    const ScaleformCall& call,
    std::size_t argumentIndex) noexcept
    : call_{&call} {
    if (call.arguments == nullptr || argumentIndex >= call.argumentCount) {
        return;
    }
    std::string_view jsonText;
    if (TryGetScaleformStringArgument(
            call,
            argumentIndex,
            jsonText,
            kMaximumRelayMessageBytes + 1)) {
        if (json::ParseDocument(
                jsonText,
                {json::NumberSyntax::SignedInteger,
                 kMaximumRelayMessageBytes,
                 32,
                 true},
                json_) &&
            json_.Root().GetType() == json::Type::Object) {
            source_ = Source::Json;
        }
        return;
    }
    const auto* arguments = static_cast<const std::byte*>(call.arguments);
    std::memcpy(
        object_.storage,
        arguments + argumentIndex * layout::kFunctionArgumentSize,
        layout::kFunctionArgumentSize);
    if (IsObjectLikeScaleformValue(object_)) {
        source_ = Source::ScaleformObject;
    }
}

bool ScaleformPayload::IsObject() const noexcept {
    return source_ != Source::None;
}

PayloadMemberStatus ScaleformPayload::ReadString(
    std::string_view name,
    std::string& value) const noexcept {
    value.clear();
    if (source_ == Source::Json) {
        std::string_view text;
        const auto status = json_.Root().ReadStringMember(name, text);
        if (status == json::MemberStatus::Found) {
            try {
                value.assign(text);
            } catch (...) {
                return PayloadMemberStatus::WrongType;
            }
        }
        return JsonStatus(status);
    }
    if (source_ != Source::ScaleformObject) {
        return PayloadMemberStatus::Missing;
    }
    MemberRead read{.type = RequestedType::String, .stringValue = &value};
    return ReadObjectMember(*call_, object_, name, read);
}

PayloadMemberStatus ScaleformPayload::ReadBoolean(
    std::string_view name,
    bool& value) const noexcept {
    value = false;
    if (source_ == Source::Json) {
        return JsonStatus(json_.Root().ReadBooleanMember(name, value));
    }
    if (source_ != Source::ScaleformObject) {
        return PayloadMemberStatus::Missing;
    }
    MemberRead read{.type = RequestedType::Boolean, .booleanValue = &value};
    return ReadObjectMember(*call_, object_, name, read);
}

PayloadMemberStatus ScaleformPayload::ReadInteger(
    std::string_view name,
    std::int64_t& value) const noexcept {
    value = 0;
    if (source_ == Source::Json) {
        return JsonStatus(json_.Root().ReadIntegerMember(name, value));
    }
    if (source_ != Source::ScaleformObject) {
        return PayloadMemberStatus::Missing;
    }
    MemberRead read{.type = RequestedType::Integer, .integerValue = &value};
    return ReadObjectMember(*call_, object_, name, read);
}

}  // namespace sf::chat
