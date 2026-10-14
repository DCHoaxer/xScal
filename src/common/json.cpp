#include "common/json.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

namespace sf::json {
namespace {

class BoundedOutput final {
public:
    BoundedOutput(
        std::string& output,
        std::size_t maximumBytes) noexcept
        : output_{output}, maximumBytes_{maximumBytes} {}

    [[nodiscard]] bool Append(std::string_view value) {
        if (output_.size() > maximumBytes_ ||
            value.size() > maximumBytes_ - output_.size()) {
            return false;
        }
        output_.append(value);
        return true;
    }

    [[nodiscard]] bool Append(char value) {
        if (output_.size() >= maximumBytes_) {
            return false;
        }
        output_.push_back(value);
        return true;
    }

private:
    std::string& output_;
    std::size_t maximumBytes_{};
};

[[nodiscard]] bool AppendEscapedString(
    std::string_view value,
    BoundedOutput& output) {
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    if (!output.Append('"')) {
        return false;
    }
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                if (!output.Append("\\\"")) return false;
                break;
            case '\\':
                if (!output.Append("\\\\")) return false;
                break;
            case '\b':
                if (!output.Append("\\b")) return false;
                break;
            case '\f':
                if (!output.Append("\\f")) return false;
                break;
            case '\n':
                if (!output.Append("\\n")) return false;
                break;
            case '\r':
                if (!output.Append("\\r")) return false;
                break;
            case '\t':
                if (!output.Append("\\t")) return false;
                break;
            default:
                if (character < 0x20) {
                    const char escape[]{
                        '\\', 'u', '0', '0',
                        hexadecimal[(character >> 4) & 0x0F],
                        hexadecimal[character & 0x0F]};
                    if (!output.Append(std::string_view{escape, sizeof(escape)})) {
                        return false;
                    }
                } else if (!output.Append(static_cast<char>(character))) {
                    return false;
                }
                break;
        }
    }
    return output.Append('"');
}

[[nodiscard]] bool KeysEqual(
    yyjson_val* left,
    yyjson_val* right) noexcept {
    const auto leftLength = yyjson_get_len(left);
    return leftLength == yyjson_get_len(right) &&
        std::memcmp(
            yyjson_get_str(left),
            yyjson_get_str(right),
            leftLength) == 0;
}

[[nodiscard]] bool ValidateValue(
    yyjson_val* value,
    std::size_t depth,
    const ParseOptions& options,
    ParseError* error) noexcept {
    if (yyjson_is_num(value) &&
        options.numberSyntax == NumberSyntax::SignedInteger) {
        if (yyjson_is_real(value) ||
            (yyjson_is_uint(value) &&
             yyjson_get_uint(value) > static_cast<std::uint64_t>(
                 (std::numeric_limits<std::int64_t>::max)()))) {
            if (error != nullptr) {
                error->code = ParseErrorCode::UnsupportedNumber;
            }
            return false;
        }
    }

    if (!yyjson_is_obj(value) && !yyjson_is_arr(value)) {
        return true;
    }
    if (depth >= options.maximumDepth) {
        if (error != nullptr) {
            error->code = ParseErrorCode::DepthLimit;
        }
        return false;
    }

    if (yyjson_is_obj(value)) {
        yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
        yyjson_val* key{};
        while ((key = yyjson_obj_iter_next(&iterator)) != nullptr) {
            if (options.rejectDuplicateMembers) {
                yyjson_obj_iter later = iterator;
                yyjson_val* laterKey{};
                while ((laterKey = yyjson_obj_iter_next(&later)) != nullptr) {
                    if (KeysEqual(key, laterKey)) {
                        if (error != nullptr) {
                            error->code = ParseErrorCode::DuplicateMember;
                        }
                        return false;
                    }
                }
            }
            if (!ValidateValue(
                    yyjson_obj_iter_get_val(key),
                    depth + 1,
                    options,
                    error)) {
                return false;
            }
        }
        return true;
    }

    yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
    yyjson_val* item{};
    while ((item = yyjson_arr_iter_next(&iterator)) != nullptr) {
        if (!ValidateValue(item, depth + 1, options, error)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] MemberStatus FindTypedMember(
    const Value& object,
    std::string_view name,
    Type expectedType,
    Value& value) noexcept {
    value = {};
    if (object.GetType() != Type::Object) {
        return MemberStatus::WrongType;
    }
    value = object.FindMember(name);
    if (!value) {
        return MemberStatus::Missing;
    }
    if (value.GetType() != expectedType) {
        value = {};
        return MemberStatus::WrongType;
    }
    return MemberStatus::Found;
}

[[nodiscard]] ParseErrorCode MapReadError(
    const yyjson_read_err& error) noexcept {
    if (error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) {
        return ParseErrorCode::AllocationFailure;
    }
    if (error.code == YYJSON_READ_ERROR_INVALID_STRING &&
        error.msg != nullptr && std::strstr(error.msg, "UTF-8") != nullptr) {
        return ParseErrorCode::InvalidUtf8;
    }
    return ParseErrorCode::InvalidSyntax;
}

}  // namespace

Value::Value(yyjson_val* value) noexcept : value_{value} {}

Value::operator bool() const noexcept {
    return value_ != nullptr;
}

Type Value::GetType() const noexcept {
    if (value_ == nullptr) {
        return Type::Invalid;
    }
    if (yyjson_is_null(value_)) return Type::Null;
    if (yyjson_is_bool(value_)) return Type::Boolean;
    if (yyjson_is_sint(value_) || yyjson_is_uint(value_)) return Type::Integer;
    if (yyjson_is_real(value_)) return Type::Real;
    if (yyjson_is_str(value_)) return Type::String;
    if (yyjson_is_obj(value_)) return Type::Object;
    if (yyjson_is_arr(value_)) return Type::Array;
    return Type::Invalid;
}

bool Value::ReadBoolean(bool& value) const noexcept {
    value = false;
    if (value_ == nullptr || !yyjson_is_bool(value_)) {
        return false;
    }
    value = yyjson_get_bool(value_);
    return true;
}

bool Value::ReadInteger(std::int64_t& value) const noexcept {
    value = 0;
    if (value_ != nullptr && yyjson_is_sint(value_)) {
        value = yyjson_get_sint(value_);
        return true;
    }
    if (value_ == nullptr || !yyjson_is_uint(value_) ||
        yyjson_get_uint(value_) > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    value = static_cast<std::int64_t>(yyjson_get_uint(value_));
    return true;
}

bool Value::ReadString(std::string_view& value) const noexcept {
    value = {};
    if (value_ == nullptr || !yyjson_is_str(value_)) {
        return false;
    }
    value = std::string_view{yyjson_get_str(value_), yyjson_get_len(value_)};
    return true;
}

std::size_t Value::ArraySize() const noexcept {
    return value_ != nullptr && yyjson_is_arr(value_)
        ? yyjson_arr_size(value_)
        : 0;
}

Value Value::ArrayItem(std::size_t index) const noexcept {
    return Value{value_ != nullptr && yyjson_is_arr(value_)
        ? yyjson_arr_get(value_, index)
        : nullptr};
}

MemberIterator Value::Members() const noexcept {
    return MemberIterator{value_};
}

Value Value::FindMember(std::string_view name) const noexcept {
    if (value_ == nullptr || !yyjson_is_obj(value_)) {
        return {};
    }
    return Value{yyjson_obj_getn(value_, name.data(), name.size())};
}

MemberIterator::MemberIterator(yyjson_val* object) noexcept {
    if (object != nullptr && yyjson_is_obj(object)) {
        iterator_ = yyjson_obj_iter_with(object);
    }
}

bool MemberIterator::Next(
    std::string_view& name,
    Value& value) noexcept {
    name = {};
    value = {};
    yyjson_val* const key = yyjson_obj_iter_next(&iterator_);
    if (key == nullptr) {
        return false;
    }
    name = std::string_view{yyjson_get_str(key), yyjson_get_len(key)};
    value = Value{yyjson_obj_iter_get_val(key)};
    return true;
}

MemberStatus Value::ReadBooleanMember(
    std::string_view name,
    bool& value) const noexcept {
    value = false;
    Value member;
    const auto status = FindTypedMember(*this, name, Type::Boolean, member);
    return status == MemberStatus::Found && member.ReadBoolean(value)
        ? MemberStatus::Found
        : status;
}

MemberStatus Value::ReadIntegerMember(
    std::string_view name,
    std::int64_t& value) const noexcept {
    value = 0;
    Value member;
    const auto status = FindTypedMember(*this, name, Type::Integer, member);
    if (status != MemberStatus::Found) {
        return status;
    }
    // An unsigned member above INT64_MAX has the Integer type but cannot be
    // read as std::int64_t.
    return member.ReadInteger(value) ? MemberStatus::Found : MemberStatus::WrongType;
}

MemberStatus Value::ReadStringMember(
    std::string_view name,
    std::string_view& value) const noexcept {
    value = {};
    Value member;
    const auto status = FindTypedMember(*this, name, Type::String, member);
    return status == MemberStatus::Found && member.ReadString(value)
        ? MemberStatus::Found
        : status;
}

MemberStatus Value::ReadObjectMember(
    std::string_view name,
    Value& value) const noexcept {
    return FindTypedMember(*this, name, Type::Object, value);
}

MemberStatus Value::ReadArrayMember(
    std::string_view name,
    Value& value) const noexcept {
    return FindTypedMember(*this, name, Type::Array, value);
}

Document::~Document() {
    Reset();
}

Document::Document(Document&& other) noexcept
    : document_{std::exchange(other.document_, nullptr)} {}

Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) {
        Reset();
        document_ = std::exchange(other.document_, nullptr);
    }
    return *this;
}

Value Document::Root() const noexcept {
    return Value{document_ != nullptr ? yyjson_doc_get_root(document_) : nullptr};
}

void Document::Reset() noexcept {
    yyjson_doc_free(document_);
    document_ = nullptr;
}

bool ParseDocument(
    std::string_view text,
    ParseOptions options,
    Document& document,
    ParseError* error) noexcept {
    document.Reset();
    if (error != nullptr) {
        *error = {};
    }
    if (options.maximumInputBytes == 0 || options.maximumDepth == 0 ||
        options.maximumDepth > kMaximumParserDepth) {
        if (error != nullptr) {
            error->code = ParseErrorCode::InvalidLimit;
        }
        return false;
    }
    if (text.size() > options.maximumInputBytes) {
        if (error != nullptr) {
            error->code = ParseErrorCode::InputTooLarge;
            error->offset = options.maximumInputBytes;
        }
        return false;
    }

    yyjson_read_err readError{};
    document.document_ = yyjson_read_opts(
        const_cast<char*>(text.data()),
        text.size(),
        YYJSON_READ_NOFLAG,
        nullptr,
        &readError);
    if (document.document_ == nullptr) {
        if (error != nullptr) {
            error->code = MapReadError(readError);
            error->offset = readError.pos;
        }
        return false;
    }
    if (!ValidateValue(
            yyjson_doc_get_root(document.document_),
            0,
            options,
            error)) {
        document.Reset();
        return false;
    }
    return true;
}

bool IsValidUtf8(std::string_view text) noexcept {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t continuationCount{};
        unsigned char minimumSecond = 0x80;
        unsigned char maximumSecond = 0xBF;
        if (first <= 0x7F) {
            ++index;
            continue;
        }
        if (first >= 0xC2 && first <= 0xDF) {
            continuationCount = 1;
        } else if (first >= 0xE0 && first <= 0xEF) {
            continuationCount = 2;
            if (first == 0xE0) minimumSecond = 0xA0;
            if (first == 0xED) maximumSecond = 0x9F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            continuationCount = 3;
            if (first == 0xF0) minimumSecond = 0x90;
            if (first == 0xF4) maximumSecond = 0x8F;
        } else {
            return false;
        }
        if (continuationCount > text.size() - index - 1) {
            return false;
        }
        const auto second = static_cast<unsigned char>(text[index + 1]);
        if (second < minimumSecond || second > maximumSecond) {
            return false;
        }
        for (std::size_t offset = 2; offset <= continuationCount; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if (next < 0x80 || next > 0xBF) {
                return false;
            }
        }
        index += continuationCount + 1;
    }
    return true;
}

void AppendString(std::string_view value, std::string& output) {
    BoundedOutput bounded{output, (std::numeric_limits<std::size_t>::max)()};
    (void)AppendEscapedString(value, bounded);
}

// Runs write on output and restores its original size if write fails or throws.
template <typename Write>
[[nodiscard]] bool AppendBounded(
    std::string& output,
    std::size_t maximumOutputBytes,
    Write write) noexcept {
    const std::size_t originalSize = output.size();
    if (originalSize > maximumOutputBytes) {
        return false;
    }
    try {
        BoundedOutput bounded{output, maximumOutputBytes};
        if (write(bounded)) {
            return true;
        }
    } catch (...) {
    }
    output.resize(originalSize);
    return false;
}

bool AppendString(
    std::string_view value,
    std::string& output,
    std::size_t maximumOutputBytes) noexcept {
    if (!IsValidUtf8(value)) {
        return false;
    }
    return AppendBounded(output, maximumOutputBytes, [value](BoundedOutput& bounded) {
        return AppendEscapedString(value, bounded);
    });
}

bool AppendInteger(
    std::int64_t value,
    std::string& output,
    std::size_t maximumOutputBytes) noexcept {
    return AppendBounded(output, maximumOutputBytes, [value](BoundedOutput& bounded) {
        char buffer[32]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return result.ec == std::errc{} &&
            bounded.Append(std::string_view{
                buffer,
                static_cast<std::size_t>(result.ptr - buffer)});
    });
}

bool AppendFloat(float value, std::string& output) {
    if (!std::isfinite(value)) {
        return false;
    }
    char buffer[64]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        return false;
    }
    output.append(buffer, result.ptr);
    return true;
}

}  // namespace sf::json
